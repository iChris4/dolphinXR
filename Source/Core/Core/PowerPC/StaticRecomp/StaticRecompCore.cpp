// RecompCore: StaticRecomp CPU core.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "Common/Config/Config.h"
#include "Common/DynamicLibrary.h"
#include "Common/FileUtil.h"
#include "Common/GekkoDisassembler.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/StaticRecompSettings.h"
#include "Core/ConfigManager.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"

StaticRecompCore* g_static_recomp_core = nullptr;

u32 StaticRecompShouldYieldAt(u32 address)
{
  return g_static_recomp_core && g_static_recomp_core->ShouldYieldAt(address);
}

namespace
{
bool RangesAreSorted(const StaticRecompRange* ranges, u32 count)
{
  if (!ranges || count == 0)
    return false;
  for (u32 i = 0; i < count; ++i)
  {
    if (ranges[i].start >= ranges[i].end ||
        (i != 0 && ranges[i - 1].end > ranges[i].start))
      return false;
  }
  return true;
}

bool AddressIsCovered(const StaticRecompRange* ranges, u32 count, u32 address)
{
  for (u32 i = 0; i < count; ++i)
  {
    if (address >= ranges[i].start && address < ranges[i].end)
      return true;
  }
  return false;
}

bool ChunksTileCode(const StaticRecompModuleDesc& desc)
{
  if (!RangesAreSorted(desc.chunk_ranges, desc.num_chunk_ranges) || !desc.chunk_hashes)
    return false;
  u32 chunk = 0;
  for (u32 code = 0; code < desc.num_code_ranges; ++code)
  {
    u32 cursor = desc.code_ranges[code].start;
    while (chunk < desc.num_chunk_ranges && desc.chunk_ranges[chunk].start < desc.code_ranges[code].end)
    {
      if (desc.chunk_ranges[chunk].start != cursor ||
          desc.chunk_ranges[chunk].end > desc.code_ranges[code].end)
        return false;
      cursor = desc.chunk_ranges[chunk++].end;
    }
    if (cursor != desc.code_ranges[code].end)
      return false;
  }
  return chunk == desc.num_chunk_ranges;
}

bool RelModulesValid(const StaticRecompModuleDesc& desc)
{
  if (desc.num_rel_modules == 0)
    return desc.rel_modules == nullptr;
  if (!desc.rel_modules)
    return false;
  for (u32 i = 0; i < desc.num_rel_modules; ++i)
  {
    const StaticRecompRelModule& module = desc.rel_modules[i];
    if (module.module_id == 0 || module.section_count == 0 ||
        module.section_info_offset < 0x40 || module.file_size < 0x40 ||
        !module.sections || module.num_sections == 0)
      return false;
    for (u32 j = 0; j < module.num_sections; ++j)
    {
      const StaticRecompRelSection& section = module.sections[j];
      const u64 end = static_cast<u64>(section.linked_start) + section.size;
      if (section.module_id != module.module_id || section.section_index >= module.section_count ||
          section.size == 0 || end > 0x100000000ull ||
          !AddressIsCovered(desc.code_ranges, desc.num_code_ranges, section.linked_start) ||
          !AddressIsCovered(desc.code_ranges, desc.num_code_ranges, static_cast<u32>(end - 1)))
        return false;
    }
  }
  return true;
}
}  // namespace

bool StaticRecompCore::IsModuleActive() const
{
  return m_module_active;
}

bool StaticRecompCore::IsHostCallAddress(u32 address) const
{
  if (!m_module_source.host_call_contains)
    return false;
  if (m_module_source.host_call_contains(address, m_module_source.host_call_user))
    return true;
  return address < m_guest.ram_size &&
         m_module_source.host_call_contains(address | 0x80000000u,
                                            m_module_source.host_call_user);
}

bool StaticRecompCore::ShouldYieldAt(u32 address)
{
  if (m_host_call_passthrough && m_host_call_passthrough_pc == address)
  {
    m_host_call_passthrough = false;
    return false;
  }
  if (m_module_active && DispatchableAt(address))
    return true;
  return IsHostCallAddress(address);
}

StaticRecompCore::StaticRecompCore(Core::System& system, StaticRecompModuleSource module_source)
    : JitBase(system), m_module_source(std::move(module_source))
{
}

StaticRecompCore::~StaticRecompCore() = default;

void StaticRecompCore::Init()
{
  g_static_recomp_core = this;
  RefreshConfig();
  m_collect_dispatch_samples = std::getenv("STATICRECOMP_DISPATCH_SAMPLES") != nullptr;
  const char* fallback_override = std::getenv("STATICRECOMP_FALLBACK_RANGES");
  std::istringstream fallback_ranges(fallback_override ? fallback_override :
                                                         Config::Get(Config::MAIN_STATICRECOMP_FALLBACK_RANGES));
  std::string fallback_range;
  while (std::getline(fallback_ranges, fallback_range, ','))
  {
    StaticRecompRange range{};
    if (std::sscanf(fallback_range.c_str(), "%x-%x", &range.start, &range.end) == 2 &&
        range.start < range.end)
      m_forced_fallback_ranges.push_back(range);
  }
  jo.enableBlocklink = false;
  jo.fastmem = false;
  jo.fastmem_arena = false;

  m_block_cache.Init();

  m_guest = CPUState{};
  m_guest.external_read = HookExternalRead;
  m_guest.external_write = HookExternalWrite;
  m_guest.external_read32 = HookExternalRead32;
  m_guest.external_write32 = HookExternalWrite32;
  m_guest.external_pointer = HookExternalPointer;
  m_guest.spr_read = HookSPRRead;
  m_guest.spr_write = HookSPRWrite;
  m_guest.cache_control = HookCacheControl;
  m_guest.instruction_fallback = HookInstructionFallback;
  m_guest.host_call = m_module_source.host_call ? HookHostCall : nullptr;
  m_guest.external_user_data = this;

  std::fprintf(stderr, "[staticrecomp] core init\n");
  if (const char* stats = std::getenv("STATICRECOMP_FALLBACK_STATS"); stats && *stats == '1')
  {
    m_fallback_stats = true;
    m_stats_init_time = std::chrono::steady_clock::now();
  }
  LoadModule();
  m_idle_pc = Config::Get(Config::MAIN_STATICRECOMP_IDLE_PC);
  // On unless a game's settings turn it off, or STATICRECOMP_NO_IDLE_SKIP=1 (comparisons).
  m_busy_wait_skipping = Config::Get(Config::MAIN_STATICRECOMP_IDLE_SKIP);
  if (const char* no_idle = std::getenv("STATICRECOMP_NO_IDLE_SKIP"); no_idle && *no_idle == '1')
    m_busy_wait_skipping = false;
  if (!m_busy_wait_skipping)
    NOTICE_LOG_FMT(POWERPC, "StaticRecomp: busy-wait loop skipping is off");
  m_lockstep_verifier = std::make_unique<StaticRecompLockstep::StaticRecompLockstepVerifier>(*this);
  m_lockstep_verifier->Init();

  UpdateModuleDirectCalls();

  // DolphinXR: no fallback JIT on any platform. visionOS forbids runtime code generation, and
  // running the same module + interpreter contract on desktop keeps Mac testing representative of
  // the headset. (The upstream fallback JIT also needs yield hooks in the JIT dispatchers, which
  // are not ported.) m_fallback_jit stays null; every use of it is null-checked.
}

// DolphinXR: a module built with DolRecomp --direct-calls calls other chunks directly, but only
// chunks whose state (m_chunk_state, which never reallocates after LoadModule) says verified.
// Not with lockstep, forced fallback ranges or host calls: they need every chunk entry here. Nor
// while a REL runs away from where it was recompiled: generated code calls chunks by their linked
// addresses, and only the dispatcher translates.
void StaticRecompCore::UpdateModuleDirectCalls()
{
  if (!m_module || !m_library.IsOpen())
    return;
  const auto set_chunk_states = reinterpret_cast<StaticRecompSetChunkStatesFn>(
      m_library.GetSymbolAddress(STATICRECOMP_SET_CHUNK_STATES_SYMBOL));
  if (!set_chunk_states)
    return;
  const bool direct_calls = !m_lockstep_verifier->IsEnabled() &&
                            m_forced_fallback_ranges.empty() && !m_guest.host_call &&
                            !m_rel_translation;
  set_chunk_states(direct_calls ? m_chunk_state.data() : nullptr,
                   direct_calls ? static_cast<u32>(m_chunk_state.size()) : 0u);
  NOTICE_LOG_FMT(POWERPC, "StaticRecomp: direct calls between chunks {}",
                 direct_calls ? "on" :
                                "off (lockstep, forced fallback, host calls or a moved REL)");
}

void StaticRecompCore::Shutdown()
{
  g_static_recomp_core = nullptr;
  m_sampler.reset();
  std::fprintf(stderr,
               "[staticrecomp] shutdown: native=%llu fallback=%llu native_exc=%llu hook_fb=%llu "
               "smc_failed=%u verifications=%llu reverify_events=%llu bursts=%llu cycles=%llu\n",
               (unsigned long long)m_native_dispatches, (unsigned long long)m_fallback_steps,
               (unsigned long long)m_native_exceptions,
               (unsigned long long)m_hook_fallback_instructions, m_failed_chunks,
               (unsigned long long)m_verifications, (unsigned long long)m_reverify_events,
               (unsigned long long)m_bursts, (unsigned long long)m_charged_cycles);
  std::vector<std::pair<u32, u64>> dispatch_samples(m_dispatch_samples.begin(),
                                                    m_dispatch_samples.end());
  std::sort(dispatch_samples.begin(), dispatch_samples.end(),
            [](const auto& left, const auto& right) { return left.second > right.second; });
  for (std::size_t i = 0; i < std::min<std::size_t>(dispatch_samples.size(), 16); ++i)
  {
    std::fprintf(stderr, "[staticrecomp] dispatch-site pc=%08x samples=%llu\n",
                 dispatch_samples[i].first,
                 static_cast<unsigned long long>(dispatch_samples[i].second));
  }
  if (m_fallback_stats && m_stats_ref_taken)
  {
    const double wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - m_stats_ref_time).count();
    const double emulated =
        static_cast<double>(m_system.GetCoreTiming().GetTicks() - m_stats_ref_ticks) /
        static_cast<double>(m_system.GetSystemTimers().GetTicksPerSecond());
    std::fprintf(stderr, "[staticrecomp] speed: %.1f s emulated in %.1f s = %.0f%%\n", emulated,
                 wall, wall > 0 ? 100.0 * emulated / wall : 0.0);
  }
  if (m_fallback_stats)
  {
    std::vector<std::pair<u32, u64>> by_opcode(m_fallback_by_opcode.begin(),
                                               m_fallback_by_opcode.end());
    std::sort(by_opcode.begin(), by_opcode.end(),
              [](const auto& left, const auto& right) { return left.second > right.second; });
    for (std::size_t i = 0; i < std::min<std::size_t>(by_opcode.size(), 24); ++i)
    {
      std::fprintf(stderr, "[staticrecomp] fallback opcode %u/%u: %llu\n", by_opcode[i].first >> 16,
                   by_opcode[i].first & 0xFFFFu, static_cast<unsigned long long>(by_opcode[i].second));
    }
    std::vector<std::pair<u32, std::pair<u32, u64>>> by_address(m_fallback_by_address.begin(),
                                                                m_fallback_by_address.end());
    std::sort(by_address.begin(), by_address.end(), [](const auto& left, const auto& right) {
      return left.second.second > right.second.second;
    });
    for (std::size_t i = 0; i < std::min<std::size_t>(by_address.size(), 40); ++i)
    {
      const auto& [address, site] = by_address[i];
      std::fprintf(stderr, "[staticrecomp] fallback at %08x (%08x %s): %llu\n", address, site.first,
                   Common::GekkoDisassembler::Disassemble(site.first, address).c_str(),
                   static_cast<unsigned long long>(site.second));
    }
  }
  NOTICE_LOG_FMT(POWERPC,
                 "StaticRecomp: shutdown. native_dispatches={} fallback_steps={} "
                 "native_exceptions={} hook_fallback_instructions={} smc_failed_chunks={} "
                 "verifications={} reverify_events={}",
                 m_native_dispatches, m_fallback_steps, m_native_exceptions,
                 m_hook_fallback_instructions, m_failed_chunks, m_verifications,
                 m_reverify_events);
  m_lockstep_verifier.reset();
  m_block_cache.Shutdown();
  m_module = nullptr;
  if (m_library.IsOpen())
  {
    // dlclose need not unload the module: don't leave it pointing at m_chunk_state.
    if (const auto set_chunk_states = reinterpret_cast<StaticRecompSetChunkStatesFn>(
            m_library.GetSymbolAddress(STATICRECOMP_SET_CHUNK_STATES_SYMBOL)))
    {
      set_chunk_states(nullptr, 0u);
    }
    m_library.Close();
  }

  if (m_fallback_jit)
  {
    m_fallback_jit->Shutdown();
    m_fallback_jit.reset();
  }
}

// DolphinXR: where to look for a module when the host did not pass one explicitly. The
// STATICRECOMP_MODULE environment variable names a module file; otherwise
// <UserDir>/StaticRecompModules/g<GAMEID>_recomp.<dylib|so|dll>. On Apple embedded platforms
// (visionOS) apps may only load code signed inside their bundle, so the app's Frameworks folder
// comes first. Declared in StaticRecompModuleSource.h for front ends.
std::string FindStaticRecompModule(const std::string& game_id)
{
  if (const char* env = std::getenv("STATICRECOMP_MODULE"); env && *env)
    return env;
  if (game_id.empty())
    return {};
#if defined(_WIN32)
  constexpr const char* suffix = ".dll";
#elif defined(__APPLE__)
  constexpr const char* suffix = ".dylib";
#else
  constexpr const char* suffix = ".so";
#endif
  const std::string file_name = "g" + game_id + "_recomp" + suffix;
#if defined(__APPLE__) && defined(IPHONEOS)
  const std::string bundled = File::GetBundleDirectory() + "/Frameworks/" + file_name;
  if (File::Exists(bundled))
    return bundled;
#endif
  const std::string path = File::GetUserPath(D_USER_IDX) + "StaticRecompModules/" + file_name;
  return File::Exists(path) ? path : std::string{};
}

void StaticRecompCore::LoadModule()
{
  const std::string game_id = SConfig::GetInstance().GetGameID();
  if (m_module_source.kind == StaticRecompModuleSource::Kind::None)
  {
    const std::string default_path = FindStaticRecompModule(game_id);
    if (default_path.empty())
    {
      NOTICE_LOG_FMT(POWERPC, "StaticRecomp: no module for '{}'; interpreter-only.", game_id);
      return;
    }
    NOTICE_LOG_FMT(POWERPC, "StaticRecomp: using module '{}'.", default_path);
    m_module_source = StaticRecompModuleSource::Dynamic(default_path);
  }

  std::string path = m_module_source.path;
  const StaticRecompModuleDesc* desc = nullptr;
  if (m_module_source.kind == StaticRecompModuleSource::Kind::AttachedDescriptor)
  {
    desc = m_module_source.descriptor;
  }
  else
  {
    if (path.empty() || !File::Exists(path) || !m_library.Open(path.c_str()))
    {
      ERROR_LOG_FMT(POWERPC, "StaticRecomp: failed to open explicit module '{}'.", path);
      return;
    }
    const auto get_module = reinterpret_cast<StaticRecompGetModuleFn>(
        m_library.GetSymbolAddress(STATICRECOMP_GET_MODULE_SYMBOL));
    desc = get_module ? get_module() : nullptr;
  }

  const auto reject = [&](const std::string& why) {
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: rejecting module '{}': {}. Interpreter-only.", path, why);
    m_module = nullptr;
    if (m_library.IsOpen())
      m_library.Close();
  };

  if (!desc)
    return reject("missing or null " STATICRECOMP_GET_MODULE_SYMBOL);
  if (desc->abi_version != STATICRECOMP_ABI_VERSION)
    return reject(fmt::format("abi_version {} != {}", desc->abi_version, STATICRECOMP_ABI_VERSION));
  if (desc->cpu_abi_version != GXRUNTIME_CPU_ABI_VERSION)
    return reject(fmt::format("cpu_abi_version {} != {}", desc->cpu_abi_version,
                              GXRUNTIME_CPU_ABI_VERSION));
  if (desc->cpu_state_size != sizeof(CPUState))
    return reject(fmt::format("cpu_state_size {} != sizeof(CPUState) {}", desc->cpu_state_size,
                              sizeof(CPUState)));
  if (!desc->dispatch || !desc->code_ranges || desc->num_code_ranges == 0)
    return reject("no dispatch entry or empty code ranges");
  if (!std::memchr(desc->game_id, '\0', sizeof(desc->game_id)) || desc->game_id[0] == '\0')
    return reject("invalid game_id");
  if (!RangesAreSorted(desc->code_ranges, desc->num_code_ranges))
    return reject("malformed or overlapping code ranges");
  if (desc->num_smc_ranges != 0 && !RangesAreSorted(desc->smc_ranges, desc->num_smc_ranges))
    return reject("malformed or overlapping SMC ranges");
  if (!desc->chunk_ranges || desc->num_chunk_ranges == 0 || !desc->chunk_hashes)
    return reject("no chunk ranges/hashes (required for the SMC guard)");
  if (!ChunksTileCode(*desc))
    return reject("chunk ranges do not exactly tile code ranges");
  if (!RelModulesValid(*desc))
    return reject("malformed REL module metadata");
  if (!AddressIsCovered(desc->code_ranges, desc->num_code_ranges, desc->entry_point))
    return reject("entry point is not covered by the module");
  if (!game_id.empty() && game_id != desc->game_id)
    return reject(fmt::format("module game_id '{}' != running game '{}'", desc->game_id, game_id));

  m_module = desc;
  m_module_active = (desc != nullptr);
  m_has_rel_modules = desc->num_rel_modules != 0;
  m_chunk_state.assign(desc->num_chunk_ranges, CHUNK_UNVERIFIED);
  m_chunk_host_call_state.assign(desc->num_chunk_ranges, 0);
  m_effective_chunk_hashes.assign(desc->chunk_hashes,
                                  desc->chunk_hashes + desc->num_chunk_ranges);
  m_chunk_rel_sections.assign(desc->num_chunk_ranges, -1);
  for (u32 chunk_index = 0; chunk_index < desc->num_chunk_ranges; ++chunk_index)
  {
    const StaticRecompRange& chunk = desc->chunk_ranges[chunk_index];
    u32 flat_section = 0;
    for (u32 module_index = 0; module_index < desc->num_rel_modules; ++module_index)
    {
      const StaticRecompRelModule& module = desc->rel_modules[module_index];
      for (u32 section_index = 0; section_index < module.num_sections; ++section_index, ++flat_section)
      {
        const StaticRecompRelSection& section = module.sections[section_index];
        const u64 section_end = static_cast<u64>(section.linked_start) + section.size;
        if (chunk.start >= section.linked_start && chunk.end <= section_end)
          m_chunk_rel_sections[chunk_index] = static_cast<int>(flat_section);
      }
    }
  }
  m_active_rel_sections.clear();
  m_rel_mapping_generation = 0;
  m_failed_chunks = 0;
  m_lookup_ram_size = 0;
  m_lookup_exram_size = 0;
  m_chunk_lookup_table.clear();

  if (m_module_source.host_call_range_contains)
  {
    for (u32 i = 0; i < desc->num_chunk_ranges; ++i)
      ChunkContainsHostCall(i);
  }

  std::fprintf(stderr, "[staticrecomp] module loaded: %s entry=0x%08X\n", path.c_str(),
               desc->entry_point);
  NOTICE_LOG_FMT(POWERPC,
                 "StaticRecomp: loaded module '{}' (game_id={} entry=0x{:08X} "
                 "code_ranges={} smc_ranges={})",
                 path, desc->game_id, desc->entry_point, desc->num_code_ranges,
                 desc->num_smc_ranges);
}

bool StaticRecompCore::IsBusyWaitLoop(u32 address)
{
  if (!m_busy_wait_skipping)
    return false;
  if (const auto it = m_busy_wait_loops.find(address); it != m_busy_wait_loops.end())
    return it->second;

  // Busy-wait loops are a few instructions; a short block reaches the branch back.
  constexpr std::size_t MAX_LOOP_INSTRUCTIONS = 32;
  if (m_busy_wait_code_buffer.size() < MAX_LOOP_INSTRUCTIONS)
    m_busy_wait_code_buffer.resize(MAX_LOOP_INSTRUCTIONS);
  // The analyzer fills these as it goes, as for a JIT block.
  PPCAnalyst::BlockStats stats{};
  PPCAnalyst::BlockRegStats gpa{};
  PPCAnalyst::BlockRegStats fpa{};
  PPCAnalyst::CodeBlock block;
  block.m_stats = &stats;
  block.m_gpa = &gpa;
  block.m_fpa = &fpa;
  analyzer.Analyze(address, &block, &m_busy_wait_code_buffer, MAX_LOOP_INSTRUCTIONS);
  bool busy_wait = false;
  for (u32 i = 0; i < block.m_num_instructions; ++i)
    busy_wait = busy_wait || m_busy_wait_code_buffer[i].branchIsIdleLoop;
  m_busy_wait_loops.emplace(address, busy_wait);
  if (busy_wait)
    NOTICE_LOG_FMT(POWERPC, "StaticRecomp: busy-wait loop at {:08x}: idle skipping", address);
  return busy_wait;
}

void StaticRecompCore::ClearCache()
{
  if (m_fallback_jit)
    m_fallback_jit->ClearCache();
  m_busy_wait_loops.clear();
  m_rel_list_seen = false;  // RAM may hold other modules now (a state load): read the list again.

  if (!m_module)
    return;
  std::fill(m_chunk_state.begin(), m_chunk_state.end(), u8{CHUNK_UNVERIFIED});
  m_failed_chunks = 0;
  ++m_reverify_events;
}
