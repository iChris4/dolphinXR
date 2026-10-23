// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/GeckoCode.h"

#include <algorithm>
#include <iterator>
#include <mutex>
#include <tuple>
#include <vector>

#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"

#include "Core/AchievementManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/Host.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#ifdef ENABLE_VR
#include <fmt/format.h>

#include "VideoCommon/OnScreenDisplay.h"
#endif

namespace Gecko
{
static constexpr u32 CODE_SIZE = 8;

bool operator==(const GeckoCode& lhs, const GeckoCode& rhs)
{
  return lhs.codes == rhs.codes;
}

bool operator==(const GeckoCode::Code& lhs, const GeckoCode::Code& rhs)
{
  return std::tie(lhs.address, lhs.data) == std::tie(rhs.address, rhs.data);
}

enum class Installation
{
  Uninstalled,
  Installed,
  Failed
};

static Installation s_code_handler_installed = Installation::Uninstalled;
// the currently active codes
static std::vector<GeckoCode> s_active_codes;
static std::vector<GeckoCode> s_synced_codes;
static std::mutex s_active_codes_lock;
#ifdef ENABLE_VR
static VRPatchRegion s_vr_patch_region;
// Whether the installed code list includes the user's codes (cheats enabled at install time).
static bool s_installed_user_codes = false;

void SetVRPatchRegion(VRPatchRegion region)
{
  std::lock_guard lk(s_active_codes_lock);
  if (region == s_vr_patch_region)
    return;
  s_vr_patch_region = std::move(region);
  s_code_handler_installed = Installation::Uninstalled;
}

bool HasActiveUserCodes()
{
  if (!Config::AreCheatsEnabled())
    return false;
  std::lock_guard lk(s_active_codes_lock);
  return !s_active_codes.empty();
}
#endif

size_t CountEnabledCodes()
{
  if (!Config::AreCheatsEnabled())
    return 0;

  std::lock_guard guard(s_active_codes_lock);
  return s_active_codes.size();
}

void SetActiveCodes(std::span<const GeckoCode> gcodes, const std::string& game_id, u16 revision)
{
  std::lock_guard lk(s_active_codes_lock);

  s_active_codes.clear();
  if (Config::AreCheatsEnabled())
  {
    s_active_codes.reserve(gcodes.size());

    const auto should_be_activated = [&game_id, &revision](const GeckoCode& code) {
      return AchievementManager::GetInstance().ShouldGeckoCodeBeActivated(code, game_id, revision);
    };
    std::copy_if(gcodes.begin(), gcodes.end(), std::back_inserter(s_active_codes),
                 should_be_activated);
  }
  s_active_codes.shrink_to_fit();

  s_code_handler_installed = Installation::Uninstalled;
}

void SetSyncedCodesAsActive()
{
  s_active_codes.clear();
  s_active_codes.reserve(s_synced_codes.size());
  s_active_codes = s_synced_codes;
}

void UpdateSyncedCodes(std::span<const GeckoCode> gcodes)
{
  s_synced_codes.clear();
  s_synced_codes.reserve(gcodes.size());
  std::copy_if(gcodes.begin(), gcodes.end(), std::back_inserter(s_synced_codes),
               [](const GeckoCode& code) { return code.enabled; });
  s_synced_codes.shrink_to_fit();
}

std::vector<GeckoCode> SetAndReturnActiveCodes(std::span<const GeckoCode> gcodes)
{
  std::lock_guard lk(s_active_codes_lock);

  s_active_codes.clear();
  if (Config::AreCheatsEnabled())
  {
    s_active_codes.reserve(gcodes.size());
    std::copy_if(gcodes.begin(), gcodes.end(), std::back_inserter(s_active_codes),
                 [](const GeckoCode& code) { return code.enabled; });
  }
  s_active_codes.shrink_to_fit();

  s_code_handler_installed = Installation::Uninstalled;

  return s_active_codes;
}

// Requires s_active_codes_lock
// NOTE: Refer to "codehandleronly.s" from Gecko OS.
static Installation InstallCodeHandlerLocked(const Core::CPUThreadGuard& guard,
                                             [[maybe_unused]] bool include_user_codes)
{
  std::string data;
  if (!File::ReadFileToString(File::GetSysDirectory() + GECKO_CODE_HANDLER, data))
  {
    ERROR_LOG_FMT(ACTIONREPLAY,
                  "Could not enable cheats because " GECKO_CODE_HANDLER " was missing.");
    return Installation::Failed;
  }

  if (data.size() > INSTALLER_END_ADDRESS - INSTALLER_BASE_ADDRESS - CODE_SIZE)
  {
    ERROR_LOG_FMT(ACTIONREPLAY, GECKO_CODE_HANDLER " is too big. The file may be corrupt.");
    return Installation::Failed;
  }

#ifdef ENABLE_VR
  // VR patches' scratch memory sits at the top of the region: the code list ends below it.
  const u32 codelist_end_address =
      std::clamp(s_vr_patch_region.code_list_end,
                 INSTALLER_BASE_ADDRESS + static_cast<u32>(data.size()) + 2 * CODE_SIZE,
                 INSTALLER_END_ADDRESS);
  for (const auto& [start, end] : s_vr_patch_region.reserved)
  {
    if (start < codelist_end_address && INSTALLER_BASE_ADDRESS < end)
    {
      ERROR_LOG_FMT(ACTIONREPLAY,
                    "Gecko codes are off: the VR patch {} writes {:08x}-{:08x}, inside the code "
                    "handler's memory.",
                    s_vr_patch_region.owners, start, end);
      OSD::AddMessage(fmt::format("Gecko codes are off while the VR patch {} runs (both use the "
                                  "memory at {:08x})",
                                  s_vr_patch_region.owners, start),
                      OSD::Duration::VERY_LONG, OSD::Color::RED);
      return Installation::Failed;
    }
  }
#else
  const u32 codelist_end_address = INSTALLER_END_ADDRESS;
#endif

  u8 mmio_addr = 0xCC;
  if (guard.GetSystem().IsWii())
  {
    mmio_addr = 0xCD;
  }

  // Install code handler
  for (u32 i = 0; i < data.size(); ++i)
    PowerPC::MMU::HostWrite<u8>(guard, data[i], INSTALLER_BASE_ADDRESS + i);

  // Patch the code handler to the current system type (Gamecube/Wii)
  for (u32 h = 0; h < data.length(); h += 4)
  {
    // Patch MMIO address
    if (PowerPC::MMU::HostRead<u32>(guard, INSTALLER_BASE_ADDRESS + h) ==
        (0x3f000000u | ((mmio_addr ^ 1) << 8)))
    {
      NOTICE_LOG_FMT(ACTIONREPLAY, "Patching MMIO access at {:08x}", INSTALLER_BASE_ADDRESS + h);
      PowerPC::MMU::HostWrite<u32>(guard, 0x3f000000u | mmio_addr << 8, INSTALLER_BASE_ADDRESS + h);
    }
  }

  const u32 codelist_base_address =
      INSTALLER_BASE_ADDRESS + static_cast<u32>(data.size()) - CODE_SIZE;

  // Write a magic value to 'gameid' (codehandleronly does not actually read this).
  // This value will be read back and modified over time by HLE_Misc::GeckoCodeHandlerICacheFlush.
  PowerPC::MMU::HostWrite<u32>(guard, MAGIC_GAMEID, INSTALLER_BASE_ADDRESS);

  // Create GCT in memory
  PowerPC::MMU::HostWrite<u32>(guard, 0x00d0c0de, codelist_base_address);
  PowerPC::MMU::HostWrite<u32>(guard, 0x00d0c0de, codelist_base_address + 4);

  // Each code is 8 bytes (2 words) wide. There is a starter code and an end code.
  const u32 start_address = codelist_base_address + CODE_SIZE;
  const u32 end_address = codelist_end_address - CODE_SIZE;
  u32 next_address = start_address;

  const auto write_codes = [&](std::span<const GeckoCode> codes) {
    for (const GeckoCode& active_code : codes)
    {
      // If the code is not going to fit in the space we have left then we have to skip it
      if (next_address + active_code.codes.size() * CODE_SIZE > end_address)
      {
        NOTICE_LOG_FMT(ACTIONREPLAY,
                       "Too many GeckoCodes! Ran out of storage space in Game RAM. Could "
                       "not write: \"{}\". Need {} bytes, only {} remain.",
                       active_code.name, active_code.codes.size() * CODE_SIZE,
                       end_address - next_address);
        continue;
      }

      for (const GeckoCode::Code& code : active_code.codes)
      {
        PowerPC::MMU::HostWrite<u32>(guard, code.address, next_address);
        PowerPC::MMU::HostWrite<u32>(guard, code.data, next_address + 4);
        next_address += CODE_SIZE;
      }
    }
  };

  // NOTE: Only active codes are in the list
#ifdef ENABLE_VR
  if (include_user_codes)
    write_codes(s_active_codes);
  write_codes(s_vr_patch_region.codes);
#else
  write_codes(s_active_codes);
#endif

  WARN_LOG_FMT(ACTIONREPLAY, "GeckoCodes: Using {} of {} bytes", next_address - start_address,
               end_address - start_address);

  // Stop code. Tells the handler that this is the end of the list.
  PowerPC::MMU::HostWrite<u32>(guard, 0xF0000000, next_address);
  PowerPC::MMU::HostWrite<u32>(guard, 0x00000000, next_address + 4);
  // With VR patch scratch memory above the list, the trampoline word is left alone (it is an HLE
  // hook address; only a max-length list reaches it).
  if (codelist_end_address == INSTALLER_END_ADDRESS)
    PowerPC::MMU::HostWrite<u32>(guard, 0, HLE_TRAMPOLINE_ADDRESS);

  // Turn on codes
  PowerPC::MMU::HostWrite<u8>(guard, 1, INSTALLER_BASE_ADDRESS + 7);

  // Invalidate the icache and any asm codes
  auto& ppc_state = guard.GetSystem().GetPPCState();
  auto& memory = guard.GetSystem().GetMemory();
  auto& jit_interface = guard.GetSystem().GetJitInterface();
  for (u32 j = 0; j < (INSTALLER_END_ADDRESS - INSTALLER_BASE_ADDRESS); j += 32)
  {
    ppc_state.iCache.Invalidate(memory, jit_interface, INSTALLER_BASE_ADDRESS + j);
  }
  Host_JitCacheInvalidation();
#ifdef ENABLE_VR
  s_installed_user_codes = include_user_codes;
#endif
  return Installation::Installed;
}

// Gecko needs to participate in the savestate system because the handler is embedded within the
// game directly. The PC may be inside the code handler in the save state and the codehandler.bin
// on the disk may be different resulting in the PC pointing at a different instruction and then
// the game malfunctions or crashes. [Also, self-modifying codes will break since the
// modifications will be reset]
void DoState(PointerWrap& p)
{
  std::lock_guard codes_lock(s_active_codes_lock);
  p.Do(s_code_handler_installed);
  // The loaded RAM holds the code list that was active when the state was saved, which can disagree
  // with the active codes (a code edited, enabled or disabled since). Reinstall the handler with
  // the active codes at the next frame, or stop running it if there are none. RunCodeHandler only
  // runs at a VI, so a handler run the state caught midway has returned by then.
  if (p.IsReadMode() && s_code_handler_installed == Installation::Installed)
    s_code_handler_installed = Installation::Uninstalled;
}

void Shutdown()
{
  std::lock_guard codes_lock(s_active_codes_lock);
  s_active_codes.clear();
#ifdef ENABLE_VR
  s_vr_patch_region = {};
#endif
  s_code_handler_installed = Installation::Uninstalled;
}

void RunCodeHandler(const Core::CPUThreadGuard& guard)
{
  const bool cheats_enabled = Config::AreCheatsEnabled();
#ifdef ENABLE_VR
  // VR patches bring their own codes, which run whether or not cheats are enabled.
  {
    std::lock_guard codes_lock(s_active_codes_lock);
    if (!cheats_enabled && s_vr_patch_region.codes.empty())
      return;
  }
#else
  if (!cheats_enabled)
    return;
#endif

  // NOTE: Need to release the lock because of GUI deadlocks with PanicAlert in HostWrite_*
  {
    std::lock_guard codes_lock(s_active_codes_lock);
#ifdef ENABLE_VR
    const bool include_user_codes = cheats_enabled && !s_active_codes.empty();
    // Cheats were turned on or off since the install: the list must gain or lose the user's codes.
    if (s_code_handler_installed == Installation::Installed &&
        include_user_codes != s_installed_user_codes)
    {
      s_code_handler_installed = Installation::Uninstalled;
    }
    const bool has_codes = include_user_codes || !s_vr_patch_region.codes.empty();
#else
    constexpr bool include_user_codes = true;
    const bool has_codes = !s_active_codes.empty();
#endif
    if (s_code_handler_installed != Installation::Installed)
    {
      // Don't spam retry if the install failed. The corrupt / missing disk file is not likely to be
      // fixed within 1 frame of the last error.
      if (!has_codes || s_code_handler_installed == Installation::Failed)
        return;
      s_code_handler_installed = InstallCodeHandlerLocked(guard, include_user_codes);

      // A warning was already issued for the install failing
      if (s_code_handler_installed != Installation::Installed)
        return;
    }
  }

  auto& ppc_state = guard.GetSystem().GetPPCState();

  // We always do this to avoid problems with the stack since we're branching in random locations.
  // Even with function call return hooks (PC == LR), hand coded assembler won't necessarily
  // follow the ABI. [Volatile FPR, GPR, CR may not be volatile]
  // The codehandler will STMW all of the GPR registers, but we need to fix the Stack's Red
  // Zone, the LR, PC (return address) and the volatile floating point registers.
  // Build a function call stack frame.
  u32 SFP = ppc_state.gpr[1];                     // Stack Frame Pointer
  ppc_state.gpr[1] -= 256;                        // Stack's Red Zone
  ppc_state.gpr[1] -= 16 + 2 * 14 * sizeof(u64);  // Our stack frame
                                                  // (HLE_Misc::GeckoReturnTrampoline)
  ppc_state.gpr[1] -= 8;                          // Fake stack frame for codehandler
  ppc_state.gpr[1] &= 0xFFFFFFF0;                 // Align stack to 16bytes
  u32 SP = ppc_state.gpr[1];                      // Stack Pointer
  PowerPC::MMU::HostWrite<u32>(guard, SP + 8, SP);
  // SP + 4 is reserved for the codehandler to save LR to the stack.
  PowerPC::MMU::HostWrite<u32>(guard, SFP, SP + 8);  // Real stack frame
  PowerPC::MMU::HostWrite<u32>(guard, ppc_state.pc, SP + 12);
  PowerPC::MMU::HostWrite<u32>(guard, LR(ppc_state), SP + 16);
  PowerPC::MMU::HostWrite<u32>(guard, ppc_state.cr.Get(), SP + 20);
  // Registers FPR0->13 are volatile
  for (u32 i = 0; i < 14; ++i)
  {
    PowerPC::MMU::HostWrite<u64>(guard, ppc_state.ps[i].PS0AsU64(), SP + 24 + 2 * i * sizeof(u64));
    PowerPC::MMU::HostWrite<u64>(guard, ppc_state.ps[i].PS1AsU64(),
                                 SP + 24 + (2 * i + 1) * sizeof(u64));
  }
  DEBUG_LOG_FMT(ACTIONREPLAY,
                "GeckoCodes: Initiating phantom branch-and-link. "
                "PC = {:#010x}, SP = {:#010x}, SFP = {:#010x}",
                ppc_state.pc, SP, SFP);
  LR(ppc_state) = HLE_TRAMPOLINE_ADDRESS;
  ppc_state.pc = ppc_state.npc = ENTRY_POINT;
}

}  // namespace Gecko
