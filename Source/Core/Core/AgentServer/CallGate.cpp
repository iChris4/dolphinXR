// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/AgentServer/CallGate.h"

#include <atomic>
#include <utility>

namespace AgentServer
{
struct CallGate::Call
{
  CallGate* gate = nullptr;
  std::string tool;
  std::string request_id;
  std::chrono::steady_clock::time_point start;
  std::atomic<bool> runs_emulation{false};
  std::atomic<bool> cancelled{false};
};

namespace
{
// The gated call running on this thread.
thread_local CallGate::Call* t_call = nullptr;

double SecondsSince(std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
}  // namespace

CallGate::Ticket::Ticket(CallGate* gate, std::shared_ptr<Call> call)
    : m_gate(gate), m_call(std::move(call))
{
  t_call = m_call.get();
}

CallGate::Ticket::Ticket(Ticket&& other) noexcept
    : m_gate(std::exchange(other.m_gate, nullptr)), m_call(std::move(other.m_call))
{
}

CallGate::Ticket& CallGate::Ticket::operator=(Ticket&& other) noexcept
{
  if (this != &other)
  {
    Release();
    m_gate = std::exchange(other.m_gate, nullptr);
    m_call = std::move(other.m_call);
  }
  return *this;
}

CallGate::Ticket::~Ticket()
{
  Release();
}

void CallGate::Ticket::Release()
{
  if (!m_gate || !m_call)
    return;
  if (t_call == m_call.get())
    t_call = nullptr;
  m_gate->Release(m_call);
  m_gate = nullptr;
  m_call.reset();
}

bool CallGate::Ticket::WasCancelled() const
{
  return m_call && m_call->cancelled.load();
}

double CallGate::Ticket::GetSeconds() const
{
  return m_call ? SecondsSince(m_call->start) : 0.0;
}

std::optional<CallGate::Ticket> CallGate::Enter(std::string tool, std::string request_id,
                                                RunningCall* busy,
                                                std::chrono::milliseconds max_wait)
{
  std::unique_lock lock(m_mutex);
  const auto deadline = std::chrono::steady_clock::now() + max_wait;
  while (m_running)
  {
    if (m_running->runs_emulation.load() || std::chrono::steady_clock::now() >= deadline)
    {
      if (busy)
        *busy = Describe(*m_running);
      return std::nullopt;
    }
    m_changed.wait_until(lock, deadline);
  }

  auto call = std::make_shared<Call>();
  call->gate = this;
  call->tool = std::move(tool);
  call->request_id = std::move(request_id);
  call->start = std::chrono::steady_clock::now();
  m_running = call;
  return Ticket(this, std::move(call));
}

std::optional<CallGate::RunningCall> CallGate::GetRunning() const
{
  std::lock_guard lock(m_mutex);
  if (!m_running)
    return std::nullopt;
  return Describe(*m_running);
}

bool CallGate::Cancel(const std::string* request_id)
{
  std::lock_guard lock(m_mutex);
  if (!m_running || (request_id && m_running->request_id != *request_id))
    return false;
  m_running->cancelled.store(true);
  return true;
}

void CallGate::Release(const std::shared_ptr<Call>& call)
{
  {
    std::lock_guard lock(m_mutex);
    if (m_running == call)
      m_running.reset();
  }
  m_changed.notify_all();
}

void CallGate::NotifyWaiters()
{
  // Taking the lock orders this with a waiter between its check and its wait.
  {
    std::lock_guard lock(m_mutex);
  }
  m_changed.notify_all();
}

CallGate::RunningCall CallGate::Describe(const Call& call)
{
  return {.tool = call.tool,
          .seconds = SecondsSince(call.start),
          .runs_emulation = call.runs_emulation.load(),
          .cancel_requested = call.cancelled.load()};
}

void MarkCallRunsEmulation()
{
  if (t_call && !t_call->runs_emulation.exchange(true))
    t_call->gate->NotifyWaiters();
}

bool IsCallCancelled()
{
  return t_call && t_call->cancelled.load();
}
}  // namespace AgentServer
