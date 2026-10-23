// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

namespace VRPatch
{
// Emulated memory as a patch sees it: big-endian values at effective addresses, RAM only (never
// MMIO), with failures reported instead of alerts. An interface so the tests can use fake memory.
class MemoryAccess
{
public:
  virtual ~MemoryAccess();

  // The `size` bytes at `address` are RAM.
  virtual bool IsRAM(u32 address, u32 size) const = 0;

  virtual std::optional<u8> Read8(u32 address) const = 0;
  virtual std::optional<u16> Read16(u32 address) const = 0;
  virtual std::optional<u32> Read32(u32 address) const = 0;
  virtual std::optional<u64> Read64(u32 address) const = 0;

  virtual bool Write8(u32 address, u8 value) = 0;
  virtual bool Write16(u32 address, u16 value) = 0;
  virtual bool Write32(u32 address, u32 value) = 0;
  virtual bool Write64(u32 address, u64 value) = 0;

  // The code at [address, address + size) changed: drop the emulated instruction cache lines and
  // the JIT blocks that cover it.
  virtual void InvalidateCode(u32 address, u32 size) = 0;
};

// The running game's memory, through PowerPC::MMU's host accessors. Only valid while the guard is.
class GuardMemoryAccess final : public MemoryAccess
{
public:
  explicit GuardMemoryAccess(const Core::CPUThreadGuard& guard) : m_guard(guard) {}

  bool IsRAM(u32 address, u32 size) const override;
  std::optional<u8> Read8(u32 address) const override;
  std::optional<u16> Read16(u32 address) const override;
  std::optional<u32> Read32(u32 address) const override;
  std::optional<u64> Read64(u32 address) const override;
  bool Write8(u32 address, u8 value) override;
  bool Write16(u32 address, u16 value) override;
  bool Write32(u32 address, u32 value) override;
  bool Write64(u32 address, u64 value) override;
  void InvalidateCode(u32 address, u32 size) override;

private:
  const Core::CPUThreadGuard& m_guard;
};
}  // namespace VRPatch
