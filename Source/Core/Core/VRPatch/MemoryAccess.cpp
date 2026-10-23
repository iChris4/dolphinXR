// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/VRPatch/MemoryAccess.h"

#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace VRPatch
{
MemoryAccess::~MemoryAccess() = default;

namespace
{
template <typename T>
std::optional<T> TryRead(const Core::CPUThreadGuard& guard, const MemoryAccess& memory, u32 address)
{
  if (!memory.IsRAM(address, sizeof(T)))
    return std::nullopt;
  const auto result = PowerPC::MMU::HostTryRead<T>(guard, address);
  if (!result)
    return std::nullopt;
  return result->value;
}

template <typename T>
bool TryWrite(const Core::CPUThreadGuard& guard, const MemoryAccess& memory, u32 address, T value)
{
  if (!memory.IsRAM(address, sizeof(T)))
    return false;
  return PowerPC::MMU::HostTryWrite<T>(guard, value, address).has_value();
}
}  // namespace

bool GuardMemoryAccess::IsRAM(u32 address, u32 size) const
{
  if (size == 0)
    return false;
  const u32 last = address + (size - 1);
  if (last < address)
    return false;
  return PowerPC::MMU::HostIsRAMAddress(m_guard, address) &&
         PowerPC::MMU::HostIsRAMAddress(m_guard, last);
}

std::optional<u8> GuardMemoryAccess::Read8(u32 address) const
{
  return TryRead<u8>(m_guard, *this, address);
}

std::optional<u16> GuardMemoryAccess::Read16(u32 address) const
{
  return TryRead<u16>(m_guard, *this, address);
}

std::optional<u32> GuardMemoryAccess::Read32(u32 address) const
{
  return TryRead<u32>(m_guard, *this, address);
}

std::optional<u64> GuardMemoryAccess::Read64(u32 address) const
{
  return TryRead<u64>(m_guard, *this, address);
}

bool GuardMemoryAccess::Write8(u32 address, u8 value)
{
  return TryWrite<u8>(m_guard, *this, address, value);
}

bool GuardMemoryAccess::Write16(u32 address, u16 value)
{
  return TryWrite<u16>(m_guard, *this, address, value);
}

bool GuardMemoryAccess::Write32(u32 address, u32 value)
{
  return TryWrite<u32>(m_guard, *this, address, value);
}

bool GuardMemoryAccess::Write64(u32 address, u64 value)
{
  return TryWrite<u64>(m_guard, *this, address, value);
}

void GuardMemoryAccess::InvalidateCode(u32 address, u32 size)
{
  if (size == 0)
    return;
  Core::System& system = m_guard.GetSystem();
  auto& ppc_state = system.GetPPCState();
  auto& memory = system.GetMemory();
  auto& jit_interface = system.GetJitInterface();
  const u32 end = address + size;
  for (u32 line = address & ~31u; line < end && line >= (address & ~31u); line += 32)
    ppc_state.iCache.Invalidate(memory, jit_interface, line);
}
}  // namespace VRPatch
