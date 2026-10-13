// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>

#if defined(_M_X86_64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#elif defined(_MSC_VER) && defined(_M_ARM64)
#include <intrin.h>
#endif

#include "Common/CommonTypes.h"

namespace Vulkan
{
namespace detail
{
// A cheap hint for a short spin: no syscall, and on ARM it lowers the core's issue rate.
inline void SpinPause()
{
#if defined(_MSC_VER) && defined(_M_ARM64)
  __yield();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#elif defined(_M_X86_64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
  _mm_pause();
#else
  std::this_thread::yield();
#endif
}
}  // namespace detail

// Bounded single-producer/single-consumer ring of trivially copyable commands with blocking
// waits on both sides. The producer pushes, waits for the ring to empty and shuts it down; the
// consumer peeks, pops and waits for work. Capacity must be a power of two. Indices are free
// running u32 values, so the ring wraps correctly after 2^32 commands.
//
// The consumer waits with std::atomic::wait on the write index, so every wake-up coincides with
// a value change. Shutdown therefore pushes a caller-supplied no-op command: the consumer drains
// it like any other and then sees the shutdown flag on an empty ring.
//
// The producer's waits (ring empty, free slot) need a timeout, which std::atomic::wait lacks, so
// they sleep on a condition variable for a target read index instead. Only the Pop that reaches
// that target notifies, so a drain wakes exactly once, as soon as the ring empties, and the
// consumer never takes the mutex for the other Pops. See SleepUntilRead for why no wake-up is
// lost.
//
// Before sleeping, each side spins with a cheap pause for a while. The consumer's spin is long
// enough to bridge the gaps between one draw's commands and the next, so during a frame it stays
// awake and the producer never pays a futex wake per command.
template <typename T, size_t Capacity>
class RecordingQueue
{
  static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0);

public:
  RecordingQueue() : m_slots(std::make_unique<T[]>(Capacity)) {}

  static constexpr size_t GetCapacity() { return Capacity; }

  // Number of queued commands. Exact on the producer thread, a snapshot elsewhere.
  size_t GetDepth() const
  {
    return m_write.load(std::memory_order_relaxed) - m_read.load(std::memory_order_acquire);
  }

  // Producer: blocks while the ring is full. Returns false if it had to wait.
  bool Push(const T& command)
  {
    const u32 write = m_write.load(std::memory_order_relaxed);
    bool waited = false;
    // The consumer's index lives on another core; only re-read it when the cached value says
    // the ring might be full, which keeps the common push free of cross-core traffic.
    if (write - m_cached_read >= Capacity)
    {
      m_cached_read = m_read.load(std::memory_order_acquire);
      while (write - m_cached_read >= Capacity)
      {
        waited = true;
        // Sleep until the consumer frees the oldest slot of this lap.
        SleepUntilRead(write - static_cast<u32>(Capacity) + 1, nullptr);
        m_cached_read = m_read.load(std::memory_order_acquire);
      }
    }
#if defined(__GNUC__) || defined(__clang__)
    // The consumer pulled this line into its own cache one lap ago; start reclaiming it early.
    __builtin_prefetch(&m_slots[(write + 8) & MASK], 1, 3);
#endif
    m_slots[write & MASK] = command;
    m_write.store(write + 1, std::memory_order_seq_cst);
    if (m_consumer_sleeping.load(std::memory_order_seq_cst))
      m_write.notify_one();
    return !waited;
  }

  // Producer: blocks until every pushed command has been popped.
  void WaitForEmpty()
  {
    const u32 write = m_write.load(std::memory_order_relaxed);
    if (!SpinUntilRead(write))
      SleepUntilRead(write, nullptr);
  }

  // Producer: like WaitForEmpty but gives up after the timeout. Returns true when empty. Wakes
  // on the Pop that empties the ring, not on a polling interval: a drain used to oversleep by up
  // to a 50 us poll slice each time, and the video thread drains many times per frame.
  template <typename Rep, typename Period>
  bool WaitForEmptyFor(const std::chrono::duration<Rep, Period>& timeout)
  {
    const u32 write = m_write.load(std::memory_order_relaxed);
    if (SpinUntilRead(write))
      return true;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout);
    return SleepUntilRead(write, &deadline);
  }

  // Producer: after this, WaitForCommand returns false once the ring (including the wake-up
  // command pushed here) has been consumed.
  void Shutdown(const T& wake_command)
  {
    m_shutdown.store(true, std::memory_order_seq_cst);
    Push(wake_command);
  }

  // Consumer: blocks until a command is available. Returns false after Shutdown() when empty.
  bool WaitForCommand()
  {
    const u32 read = m_read.load(std::memory_order_relaxed);
    for (int spin = 0; spin < CONSUMER_SPIN_ITERATIONS; ++spin)
    {
      if (m_write.load(std::memory_order_acquire) != read)
        return true;
      if ((spin & 255) == 255 && m_shutdown.load(std::memory_order_acquire))
        return false;
      detail::SpinPause();
    }
    while (true)
    {
      m_consumer_sleeping.store(true, std::memory_order_seq_cst);
      const u32 write = m_write.load(std::memory_order_seq_cst);
      if (write != read)
        break;
      if (m_shutdown.load(std::memory_order_seq_cst))
      {
        m_consumer_sleeping.store(false, std::memory_order_seq_cst);
        return false;
      }
      m_write.wait(write, std::memory_order_acquire);
    }
    m_consumer_sleeping.store(false, std::memory_order_seq_cst);
    return true;
  }

  // Consumer: the oldest command, or nullptr when the ring is empty.
  const T* Front() const
  {
    const u32 read = m_read.load(std::memory_order_relaxed);
    if (read == m_write.load(std::memory_order_acquire))
      return nullptr;
    return &m_slots[read & MASK];
  }

  // Consumer: releases the front slot back to the producer.
  void Pop()
  {
    const u32 read = m_read.load(std::memory_order_relaxed) + 1;
    m_read.store(read, std::memory_order_seq_cst);
    // The hot path is this one flag load. Only the Pop that reaches the sleeping producer's
    // target takes the mutex; the target is published before the flag, so the acquire half of
    // the flag load makes it visible here. The index moves by exactly one per Pop, so an exact
    // match fires once per wait.
    if (m_producer_waiting.load(std::memory_order_seq_cst)) [[unlikely]]
    {
      if (read == m_wait_target.load(std::memory_order_relaxed))
      {
        std::lock_guard lock(m_wait_mutex);
        m_wait_cv.notify_one();
      }
    }
  }

private:
  static constexpr u32 MASK = static_cast<u32>(Capacity - 1);
  // Roughly 100-200 us of pause hints on a Quest-class core before the consumer sleeps: longer
  // than the gap between draws, shorter than the gap between frames.
  static constexpr int CONSUMER_SPIN_ITERATIONS = 16384;
  static constexpr int PRODUCER_SPIN_ITERATIONS = 4096;

  // True once the read index has reached target. The index only moves forward and a wait never
  // targets more than Capacity ahead of it, so the signed distance is exact across wraparound.
  static bool HasReached(u32 read, u32 target) { return static_cast<s32>(read - target) >= 0; }

  // Producer: short pre-sleep spin. Returns true once the read index reaches target.
  bool SpinUntilRead(u32 target) const
  {
    for (int spin = 0; spin < PRODUCER_SPIN_ITERATIONS; ++spin)
    {
      if (HasReached(m_read.load(std::memory_order_acquire), target))
        return true;
      detail::SpinPause();
    }
    return false;
  }

  // Producer: sleeps until the read index reaches target, or until *deadline when one is given.
  // Returns true when the target was reached.
  //
  // Why no wake-up is lost: with the mutex held, the producer publishes the target, sets
  // m_producer_waiting (seq_cst) and only then loads m_read (seq_cst) in the predicate. Pop
  // stores m_read (seq_cst) and then loads m_producer_waiting (seq_cst). All four are seq_cst,
  // so they fall in one total order consistent with each thread's program order: either the
  // consumer's flag load comes after the producer's flag store, and the consumer sees the flag
  // and the target and notifies; or it comes before it, and then the consumer's m_read store
  // also precedes the producer's predicate load, which sees the new index and does not sleep.
  // The notify itself is issued with the mutex held, so it cannot land between the producer's
  // predicate check and its wait: it runs either before the producer takes the mutex (the
  // predicate then sees the index stored before the notify) or after wait() has atomically
  // released the mutex and is asleep. Spurious or extra wake-ups only re-run the predicate.
  bool SleepUntilRead(u32 target, const std::chrono::steady_clock::time_point* deadline)
  {
    const auto reached = [this, target] {
      return HasReached(m_read.load(std::memory_order_seq_cst), target);
    };
    std::unique_lock lock(m_wait_mutex);
    m_wait_target.store(target, std::memory_order_relaxed);
    m_producer_waiting.store(true, std::memory_order_seq_cst);
    bool result = true;
    if (deadline)
      result = m_wait_cv.wait_until(lock, *deadline, reached);
    else
      m_wait_cv.wait(lock, reached);
    m_producer_waiting.store(false, std::memory_order_seq_cst);
    return result;
  }

  std::unique_ptr<T[]> m_slots;
  alignas(64) std::atomic<u32> m_write{0};
  u32 m_cached_read = 0;  // producer-private
  alignas(64) std::atomic<u32> m_read{0};
  alignas(64) std::atomic<bool> m_consumer_sleeping{false};
  std::atomic<bool> m_producer_waiting{false};
  std::atomic<bool> m_shutdown{false};
  // Read index the sleeping producer waits for; written by the producer before it sets
  // m_producer_waiting.
  std::atomic<u32> m_wait_target{0};
  // Only touched while the producer sleeps, and by the one Pop that wakes it.
  alignas(64) std::mutex m_wait_mutex;
  std::condition_variable m_wait_cv;
};
}  // namespace Vulkan
