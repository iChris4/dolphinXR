// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/VRPatch/VRPatch.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <utility>
#include <variant>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/VR/OpenXRInputState.h"
#include "Core/AchievementManager.h"
#include "Core/AgentServer/AgentInput.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "Core/Core.h"
#include "Core/GeckoCode.h"
#include "Core/GeckoCodeConfig.h"
#include "Core/Movie.h"
#include "Core/NetPlayProto.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "Core/VRPatch/GeckoRegion.h"
#include "Core/VRPatch/MemoryAccess.h"
#include "Core/VRPatch/PatchIni.h"
#include "Core/VRPatch/PatchScript.h"
#include "Core/VRPatch/VRSettingsTable.h"
#include "VideoCommon/VR/OverrideSuppression.h"

namespace VRPatch
{
namespace
{
constexpr int MAX_CONSECUTIVE_ERRORS = 3;
constexpr size_t MAX_LOG_LINES = 300;
constexpr const char* PATCH_EXTENSION = ".lua";

// ---- Files ----

struct FileEntry
{
  std::string id;
  std::string path;
  PatchOrigin origin = PatchOrigin::User;
  bool overrides_sys = false;
};

// The folder names a game's patches can be in: GM8, GM8E01, GM8E01r0 (and G for every GameCube
// title), least specific first.
std::vector<std::string> FolderNames(std::string_view game_id, std::optional<u16> revision)
{
  std::vector<std::string> names;
  for (std::string name : ConfigLoaders::GetGameIniFilenames(game_id, revision))
  {
    if (name.ends_with(".ini"))
      name.resize(name.size() - 4);
    names.push_back(std::move(name));
  }
  return names;
}

std::string IdFromPath(const std::string& path)
{
  std::string name;
  std::string extension;
  SplitPath(path, nullptr, &name, &extension);
  return name;
}

std::vector<FileEntry> FindPatchFiles(std::string_view game_id, std::optional<u16> revision)
{
  std::map<std::string, FileEntry> found;
  const std::vector<std::string> folders = FolderNames(game_id, revision);
  for (const PatchOrigin origin : {PatchOrigin::Sys, PatchOrigin::User})
  {
    const std::string root =
        origin == PatchOrigin::Sys ? GetSysPatchDirectory() : File::GetUserPath(D_VRPATCHES_IDX);
    for (const std::string& folder : folders)
    {
      const std::string directory = root + folder;
      if (!File::IsDirectory(directory))
        continue;
      for (const std::string& path :
           Common::DoFileSearch(directory, std::string_view(PATCH_EXTENSION), false))
      {
        const std::string id = IdFromPath(path);
        if (!IsValidPatchId(id))
          continue;
        FileEntry entry{id, path, origin, false};
        const auto existing = found.find(id);
        if (existing != found.end() && origin == PatchOrigin::User &&
            (existing->second.origin == PatchOrigin::Sys || existing->second.overrides_sys))
        {
          entry.overrides_sys = true;
        }
        found[id] = std::move(entry);
      }
    }
  }
  std::vector<FileEntry> files;
  for (auto& [id, entry] : found)
    files.push_back(std::move(entry));
  return files;
}

// Manifests by file, re-read when the file's text changes.
struct CachedManifest
{
  std::string source;
  std::variant<Manifest, std::string> manifest;
};

std::mutex s_manifest_cache_mutex;
std::map<std::string, CachedManifest> s_manifest_cache;

std::variant<Manifest, std::string> ManifestOf(const std::string& key, const std::string& id,
                                               const std::string& source)
{
  {
    std::lock_guard lock(s_manifest_cache_mutex);
    const auto it = s_manifest_cache.find(key);
    if (it != s_manifest_cache.end() && it->second.source == source)
      return it->second.manifest;
  }
  std::variant<Manifest, std::string> manifest = ReadManifest(source, id);
  std::lock_guard lock(s_manifest_cache_mutex);
  s_manifest_cache[key] = {source, manifest};
  return manifest;
}

// Why the manifest's games or revision exclude the game; "" when they match. An unknown revision
// matches.
std::string GameMismatch(const Manifest& manifest, std::string_view game_id,
                         std::optional<u16> revision)
{
  if (!manifest.games.empty() &&
      std::ranges::none_of(manifest.games,
                           [&](const std::string& prefix) { return game_id.starts_with(prefix); }))
  {
    return fmt::format("for {} only", fmt::join(manifest.games, ", "));
  }
  if (manifest.revision && revision && *manifest.revision != *revision)
    return fmt::format("for revision {} only (the disc is revision {})", *manifest.revision,
                       *revision);
  return {};
}

std::map<std::string, double> SettingValues(const Manifest& manifest, const PatchValues& values,
                                            const std::string& id)
{
  std::map<std::string, double> result;
  for (const SettingDef& def : manifest.settings)
    result[def.name] = def.Normalize(values.GetSetting(id, def.name).value_or(def.default_value));
  return result;
}

// ---- Controller input for pad.* ----

AgentServer::AgentInput::Device ToAgentDevice(PadDevice device)
{
  switch (device)
  {
  case PadDevice::GCPad:
    return AgentServer::AgentInput::Device::GCPad;
  case PadDevice::Wiimote:
    return AgentServer::AgentInput::Device::Wiimote;
  case PadDevice::Attachment:
  default:
    return AgentServer::AgentInput::Device::Attachment;
  }
}

class EmulatedPadResolver final : public PadResolver
{
public:
  std::vector<PadControl> Resolve(PadDevice device, u8 port, PadControlKind kind,
                                  std::string_view name, std::string* error) override
  {
    namespace AI = AgentServer::AgentInput;
    auto* emulated = AI::GetController(ToAgentDevice(device), port, error);
    if (!emulated)
      return {};
    const std::vector<AI::GroupInfo> groups = AI::DescribeController(*emulated);
    std::vector<PadControl> controls;
    switch (kind)
    {
    case PadControlKind::Button:
      for (const AI::HeldControl& held : AI::ResolvePress(groups, name, error))
        controls.push_back({held.group, held.control});
      break;
    case PadControlKind::Analog:
      if (const auto held = AI::ResolveAnalog(groups, name, 0.0, error))
        controls.push_back({held->group, held->control});
      break;
    case PadControlKind::Stick:
      if (const auto group = AI::ResolveGroup(groups, AI::GroupKind::Stick, name, error))
      {
        for (const AI::GroupInfo& info : groups)
        {
          if (info.name != *group)
            continue;
          for (const std::string& control : info.controls)
            controls.push_back({info.name, control});
        }
      }
      break;
    }
    return controls;
  }
};

// ---- The running patches ----

struct RunningPatch
{
  std::string id;
  FileEntry file;
  bool session_source = false;
  std::string source;
  Manifest manifest;
  std::unique_ptr<PatchScript> script;
  PatchState state = PatchState::Waiting;
  std::string status;
  int consecutive_errors = 0;
  bool needs_on_load = true;
  std::map<std::string, double> values;
  AddressRange scratch;
  double average_us = 0;
  double max_us = 0;
  double window_max_us = 0;
  u64 window_start_field = 0;
  u64 frames = 0;
};

// What the UIs see of a patch of the running game.
struct Published
{
  PatchState state = PatchState::Off;
  std::string status;
  std::map<std::string, double> values;
  AddressRange scratch;
  double average_us = 0;
  double max_us = 0;
  size_t memory_bytes = 0;
  std::deque<LogLine> log;
};

struct Command
{
  enum class Type
  {
    // Start, stop or restart a patch so that it matches its enabled state and source.
    Sync,
    SyncAll,
    Setting,
  };
  Type type = Type::Sync;
  std::string id;
  bool reload = false;
  std::string name;
  double value = 0;
};

class Manager
{
public:
  // Commands, the published state, the game and the live sources.
  std::mutex mutex;
  // The Lua states. Held by the CPU thread while the patches run.
  std::recursive_mutex run_mutex;
  std::atomic<bool> has_work{false};
  std::atomic<bool> state_loaded{false};
  // VI fields since the game started (every RunFrame, with or without patches).
  std::atomic<u64> field{0};

  // Under `mutex`.
  bool game_running = false;
  std::string game_id;
  u16 revision = 0;
  bool is_wii = false;
  std::vector<Command> commands;
  std::map<std::string, Published> published;
  // (game ID, patch ID) -> source.
  std::map<std::pair<std::string, std::string>, std::string> session_sources;

  // Under `run_mutex`.
  std::vector<std::unique_ptr<RunningPatch>> patches;
  std::string block_reason;
  std::map<std::pair<PadDevice, u8>, std::vector<AgentServer::AgentInput::HeldControl>> pad_holds;
  u64 pad_signature = 0;
  std::set<std::pair<PadDevice, u8>> attach_failures;
  EmulatedPadResolver pad_resolver;
};

Manager& GetManager()
{
  static Manager manager;
  return manager;
}

void Queue(Command command)
{
  Manager& m = GetManager();
  std::lock_guard lock(m.mutex);
  if (!m.game_running)
    return;
  m.commands.push_back(std::move(command));
  m.has_work.store(true, std::memory_order_release);
}

std::string FirstLine(std::string_view text)
{
  const size_t newline = text.find('\n');
  return std::string(newline == std::string_view::npos ? text : text.substr(0, newline));
}

void PublishLog(const std::string& id, std::vector<LogLine> lines)
{
  if (lines.empty())
    return;
  for (const LogLine& line : lines)
  {
    switch (line.level)
    {
    case 0:
      INFO_LOG_FMT(OPENXR, "VR patch {}: {}", id, line.text);
      break;
    case 1:
      WARN_LOG_FMT(OPENXR, "VR patch {}: {}", id, line.text);
      break;
    default:
      ERROR_LOG_FMT(OPENXR, "VR patch {}: {}", id, line.text);
      break;
    }
  }
  Manager& m = GetManager();
  std::lock_guard lock(m.mutex);
  std::deque<LogLine>& log = m.published[id].log;
  for (LogLine& line : lines)
    log.push_back(std::move(line));
  while (log.size() > MAX_LOG_LINES)
    log.pop_front();
}

void LogLineFor(const std::string& id, u8 level, std::string text, u64 field)
{
  PublishLog(id, {LogLine{level, field, std::move(text)}});
}

void Publish(const RunningPatch& patch)
{
  Manager& m = GetManager();
  std::lock_guard lock(m.mutex);
  Published& published = m.published[patch.id];
  published.state = patch.state;
  published.status = patch.status;
  published.values = patch.values;
  published.scratch = patch.scratch;
  published.average_us = patch.average_us;
  published.max_us = patch.max_us;
  published.memory_bytes = patch.script ? patch.script->GetMemoryUsed() : 0;
}

void PublishStopped(const std::string& id, PatchState state, std::string status)
{
  Manager& m = GetManager();
  std::lock_guard lock(m.mutex);
  Published& published = m.published[id];
  published.state = state;
  published.status = std::move(status);
  published.scratch = {};
  published.average_us = 0;
  published.memory_bytes = 0;
}

// Why no patch may run now ("" = they may).
std::string BlockReason(Core::System& system)
{
  if (NetPlay::IsNetPlayRunning())
    return "netplay is running";
#ifdef USE_RETRO_ACHIEVEMENTS
  if (AchievementManager::GetInstance().IsHardcoreModeActive())
    return "RetroAchievements hardcore mode is on";
#endif
  if (system.GetMovie().IsMovieActive())
    return "a movie (TAS input) is recording or playing";
  return {};
}

VRInputFrame CaptureInput()
{
  const Common::VR::OpenXRInputSnapshot snapshot = Common::VR::OpenXRInputState::GetGameSnapshot();
  VRInputFrame input;
  input.runtime_active = snapshot.runtime_active;
  input.session_focused = snapshot.session_focused;
  input.menu_open = snapshot.menu.input_withheld;
  const auto copy_pose = [](const Common::VR::OpenXRPoseState& from, PoseInput* to) {
    to->valid = from.valid;
    to->position = from.position;
    to->orientation = from.orientation;
  };
  copy_pose(snapshot.head_pose, &input.head);
  for (size_t i = 0; i < input.hands.size(); ++i)
  {
    const Common::VR::OpenXRControllerState& from = snapshot.controllers[i];
    HandInput& to = input.hands[i];
    to.connected = from.connected;
    to.primary = from.primary_button;
    to.secondary = from.secondary_button;
    to.button_3 = from.button_3;
    to.button_4 = from.button_4;
    to.menu = from.menu_button;
    to.shoulder = from.shoulder_button;
    to.trigger_button = from.trigger_button;
    to.squeeze_button = from.squeeze_button;
    to.thumbstick_button = from.thumbstick_button;
    to.trigger = from.trigger_value;
    to.squeeze = from.squeeze_value;
    to.squeeze_force = from.squeeze_force;
    to.thumbstick_x = from.thumbstick_x;
    to.thumbstick_y = from.thumbstick_y;
    copy_pose(from.aim_pose, &to.aim);
    copy_pose(from.grip_pose, &to.grip);
    copy_pose(from.pointer_pose, &to.pointer);
    copy_pose(from.view_pose, &to.view);
    to.linear_velocity_valid = from.grip_velocity.linear_valid;
    to.angular_velocity_valid = from.grip_velocity.angular_valid;
    to.linear_velocity = from.grip_velocity.linear;
    to.angular_velocity = from.grip_velocity.angular;
    to.screen_hit_valid = from.screen_hit.valid;
    to.screen_u = from.screen_hit.u;
    to.screen_v = from.screen_hit.v;
    to.screen_distance = from.screen_hit.distance_m;
  }
  return input;
}

// The environment of one batch of calls; `memory` may be null.
struct Environment
{
  ScriptEnvironment env;
  VRInputFrame input;
};

void FillEnvironment(Environment* environment, MemoryAccess* memory)
{
  Manager& m = GetManager();
  {
    std::lock_guard lock(m.mutex);
    environment->env.game_id = m.game_id;
    environment->env.revision = m.revision;
    environment->env.is_wii = m.is_wii;
  }
  environment->env.memory = memory;
  environment->env.input = &environment->input;
  environment->env.pad = &m.pad_resolver;
  environment->env.field = m.field.load(std::memory_order_relaxed);
  environment->env.units_per_meter = Config::Get(Config::GFX_VR_UNITS_PER_METER);
}

// ---- Gecko region, overrides, controller holds ----

void UpdateSharedState()
{
  Manager& m = GetManager();
  Gecko::VRPatchRegion region;
  std::vector<AddressRange> scratch_blocks;
  std::vector<std::pair<VR::OverrideSuppression::Kind, std::string>> suppressed;
  for (const auto& patch : m.patches)
  {
    if (patch->state != PatchState::Running)
      continue;
    const Manifest& manifest = patch->manifest;
    if (!manifest.gecko_lines.empty())
    {
      Gecko::GeckoCode code;
      code.name = "VR patch " + patch->id;
      code.enabled = true;
      for (const std::string& line : manifest.gecko_lines)
      {
        if (const auto parsed = Gecko::DeserializeLine(line))
          code.codes.push_back(*parsed);
      }
      region.codes.push_back(std::move(code));
    }
    if (patch->scratch.end > patch->scratch.start)
      scratch_blocks.push_back(patch->scratch);
    bool reserves_region = false;
    for (const AddressRange& range : manifest.reserve)
    {
      if (InGeckoRegion(range))
      {
        region.reserved.emplace_back(range.start, range.end);
        reserves_region = true;
      }
    }
    if (reserves_region)
      region.owners += (region.owners.empty() ? "" : ", ") + patch->id;
    for (const OverrideRef& conflict : manifest.conflicts)
    {
      const auto kind =
          conflict.kind == OverrideKind::Shader   ? VR::OverrideSuppression::Kind::Shader :
          conflict.kind == OverrideKind::Elements ? VR::OverrideSuppression::Kind::Elements :
                                                    VR::OverrideSuppression::Kind::Texture;
      suppressed.emplace_back(kind, conflict.name);
    }
  }
  region.code_list_end = GeckoCodeListEnd(scratch_blocks);
  Gecko::SetVRPatchRegion(std::move(region));
  VR::OverrideSuppression::Set(std::move(suppressed));
}

void UpdatePadHolds()
{
  namespace AI = AgentServer::AgentInput;
  Manager& m = GetManager();
  u64 signature = 0;
  for (const auto& patch : m.patches)
  {
    if (patch->script)
      signature = signature * 31 + patch->script->GetPadHoldsGeneration() + 1;
  }
  signature = signature * 31 + m.patches.size();
  if (signature == m.pad_signature)
    return;
  m.pad_signature = signature;

  std::map<std::pair<PadDevice, u8>, std::vector<AI::HeldControl>> holds;
  for (const auto& patch : m.patches)
  {
    if (!patch->script || patch->state != PatchState::Running)
      continue;
    for (const auto& [key, value] : patch->script->GetPadHolds())
    {
      const auto& [device, port, group, control] = key;
      holds[{device, port}].push_back({group, control, value});
    }
  }
  for (const auto& [target, controls] : m.pad_holds)
  {
    if (!holds.contains(target))
      AI::Hold(AI::Layer::Patch, ToAgentDevice(target.first), target.second, {});
  }
  for (const auto& [target, controls] : holds)
  {
    std::string error;
    if (!AI::Attach(ToAgentDevice(target.first), target.second, &error))
    {
      if (m.attach_failures.insert(target).second)
      {
        WARN_LOG_FMT(OPENXR, "VR patches: controller input on port {} is not available: {}",
                     target.second + 1, error);
      }
      continue;
    }
    AI::Hold(AI::Layer::Patch, ToAgentDevice(target.first), target.second, controls);
  }
  m.pad_holds = std::move(holds);
}

void ApplySettingRequests(RunningPatch& patch)
{
  for (SettingRequest& request : patch.script->TakeSettingRequests())
  {
    Core::QueueHostJob([key = std::move(request.key),
                        value = std::move(request.value)](Core::System&) {
      const VRSetting* setting = FindVRSetting(key);
      if (setting && Config::GetLayer(Config::LayerType::CurrentRun)->Set(setting->location, value))
        Config::OnConfigChanged();
    });
  }
}

// ---- Starting and stopping ----

void StopPatch(RunningPatch& patch, const ScriptEnvironment& env, PatchState state,
               std::string status)
{
  if (patch.script && patch.state == PatchState::Running && patch.script->HasHook(Hook::OnUnload))
  {
    if (!patch.script->Call(Hook::OnUnload, env))
      LogLineFor(patch.id, 2, "on_unload: " + patch.script->Error(), env.field);
    PublishLog(patch.id, patch.script->TakeLog());
  }
  patch.script.reset();
  patch.state = state;
  patch.status = std::move(status);
  PublishStopped(patch.id, patch.state, patch.status);
}

void RemovePatch(const std::string& id, const ScriptEnvironment& env, PatchState state,
                 std::string status)
{
  Manager& m = GetManager();
  const auto it = std::ranges::find_if(m.patches, [&](const auto& p) { return p->id == id; });
  if (it == m.patches.end())
  {
    PublishStopped(id, state, std::move(status));
    return;
  }
  StopPatch(**it, env, state, std::move(status));
  m.patches.erase(it);
}

// Where the patch's source is: a live agent source, else its file when it is enabled.
struct SourceChoice
{
  bool enabled = false;
  bool session = false;
  FileEntry file;
  std::string source;
};

SourceChoice ChooseSource(const std::string& id, const PatchValues& values)
{
  Manager& m = GetManager();
  SourceChoice choice;
  std::string game_id;
  u16 revision = 0;
  {
    std::lock_guard lock(m.mutex);
    game_id = m.game_id;
    revision = m.revision;
    const auto it = m.session_sources.find({game_id, id});
    if (it != m.session_sources.end())
    {
      choice.enabled = true;
      choice.session = true;
      choice.source = it->second;
      choice.file = {id, "", PatchOrigin::Session, false};
      return choice;
    }
  }
  if (!values.IsEnabled(id))
    return choice;
  for (FileEntry& file : FindPatchFiles(game_id, revision))
  {
    if (file.id != id)
      continue;
    if (!File::ReadFileToString(file.path, choice.source))
      return choice;
    choice.enabled = true;
    choice.file = std::move(file);
    return choice;
  }
  return choice;
}

void StartPatch(const std::string& id, SourceChoice choice, const PatchValues& values,
                const ScriptEnvironment& base_env)
{
  Manager& m = GetManager();
  auto patch = std::make_unique<RunningPatch>();
  patch->id = id;
  patch->file = choice.file;
  patch->session_source = choice.session;
  patch->source = std::move(choice.source);

  const auto fail = [&](PatchState state, std::string status) {
    if (state == PatchState::Error)
      LogLineFor(id, 2, status, base_env.field);
    PublishStopped(id, state, std::move(status));
  };

  // The manifest first: the scratch block must exist before the top level runs (mem.scratch).
  const std::variant<Manifest, std::string> read =
      choice.session ? ReadManifest(patch->source, id) :
                       ManifestOf(patch->file.path, id, patch->source);
  if (const std::string* error = std::get_if<std::string>(&read))
  {
    fail(PatchState::Error, *error);
    return;
  }
  patch->manifest = std::get<Manifest>(read);
  const Manifest& manifest = patch->manifest;

  const std::string mismatch = GameMismatch(manifest, base_env.game_id, base_env.revision);
  if (!mismatch.empty())
  {
    fail(PatchState::Error, "not for this game: " + mismatch);
    return;
  }
  if (!m.block_reason.empty())
  {
    fail(PatchState::Blocked, "paused while " + m.block_reason);
    return;
  }

  // Memory: the patch's fixed ranges and its scratch must not meet another running patch's.
  std::vector<AddressRange> used;
  std::map<u32, std::string> owner_of;
  for (const auto& other : m.patches)
  {
    if (other->state != PatchState::Running)
      continue;
    for (const AddressRange& range : other->manifest.reserve)
    {
      used.push_back(range);
      owner_of[range.start] = other->id;
    }
    if (other->scratch.end > other->scratch.start)
    {
      used.push_back(other->scratch);
      owner_of[other->scratch.start] = other->id;
    }
  }
  if (const auto overlap = FindOverlap(manifest.reserve, used))
  {
    std::string owner;
    for (const AddressRange& range : used)
    {
      if (overlap->Overlaps(range))
        owner = owner_of[range.start];
    }
    fail(PatchState::Blocked, fmt::format("uses {:08X}-{:08X}, like the running patch {}",
                                          overlap->start, overlap->end, owner));
    return;
  }
  const bool reserves_gecko_region =
      std::ranges::any_of(manifest.reserve, [](const AddressRange& r) { return InGeckoRegion(r); });
  if (reserves_gecko_region && !manifest.gecko_lines.empty())
  {
    fail(PatchState::Error, "its reserve ranges are in the Gecko code handler's memory, where its "
                            "own Gecko codes would run");
    return;
  }
  if (reserves_gecko_region && Gecko::HasActiveUserCodes())
  {
    fail(PatchState::Blocked,
         "it writes in the Gecko code handler's memory (0x80001800-0x80003000), and Gecko codes "
         "are enabled for this game: turn them off (or Enable Cheats), then reload the patches");
    return;
  }
  if (manifest.scratch_bytes != 0)
  {
    std::vector<AddressRange> occupied = used;
    for (const AddressRange& range : manifest.reserve)
      occupied.push_back(range);
    const std::optional<u32> base = AllocateScratch(occupied, manifest.scratch_bytes);
    if (!base)
    {
      fail(PatchState::Blocked,
           fmt::format("no room for {} bytes of scratch memory next to the running patches",
                       manifest.scratch_bytes));
      return;
    }
    patch->scratch = {*base, *base + manifest.scratch_bytes};
  }

  ScriptEnvironment env = base_env;
  // The top level does not touch memory: it gets none.
  env.memory = nullptr;
  env.scratch_base = patch->scratch.start;
  env.scratch_size = patch->scratch.end - patch->scratch.start;
  patch->script = std::make_unique<PatchScript>(id);
  if (!patch->script->Load(patch->source, env))
  {
    PublishLog(id, patch->script->TakeLog());
    fail(PatchState::Error, patch->script->Error());
    return;
  }
  PublishLog(id, patch->script->TakeLog());

  patch->values = SettingValues(manifest, values, id);
  for (const SettingDef& def : manifest.settings)
    patch->script->SetSettingValue(def, patch->values[def.name]);

  patch->state = PatchState::Running;
  patch->needs_on_load = true;
  patch->status = "starting";
  patch->window_start_field = base_env.field;
  LogLineFor(id, 0,
             fmt::format("started ({}{})",
                         choice.session ? "live source from the agent" : patch->file.path,
                         patch->scratch.end > patch->scratch.start ?
                             fmt::format(", scratch {:08X}", patch->scratch.start) :
                             ""),
             base_env.field);
  Publish(*patch);
  m.patches.push_back(std::move(patch));
}

void SyncPatch(const std::string& id, bool reload, const ScriptEnvironment& env,
               const PatchValues& values)
{
  Manager& m = GetManager();
  SourceChoice choice = ChooseSource(id, values);
  const auto it = std::ranges::find_if(m.patches, [&](const auto& p) { return p->id == id; });
  RunningPatch* running = it == m.patches.end() ? nullptr : it->get();
  if (!choice.enabled)
  {
    RemovePatch(id, env, PatchState::Off, "");
    return;
  }
  if (running && running->state == PatchState::Running && !reload &&
      running->source == choice.source)
  {
    // Same code (e.g. a live source just saved to a file): keep it running.
    running->file = choice.file;
    running->session_source = choice.session;
    return;
  }
  if (running)
    RemovePatch(id, env, PatchState::Off, "");
  StartPatch(id, std::move(choice), values, env);
}

std::set<std::string> AllPatchIds()
{
  Manager& m = GetManager();
  std::set<std::string> ids;
  std::string game_id;
  u16 revision = 0;
  {
    std::lock_guard lock(m.mutex);
    game_id = m.game_id;
    revision = m.revision;
    for (const auto& [key, source] : m.session_sources)
    {
      if (key.first == game_id)
        ids.insert(key.second);
    }
    for (const auto& [id, published] : m.published)
      ids.insert(id);
  }
  for (const FileEntry& file : FindPatchFiles(game_id, revision))
    ids.insert(file.id);
  for (const auto& patch : m.patches)
    ids.insert(patch->id);
  return ids;
}

// ---- Calls ----

void RecordCallTime(RunningPatch& patch, u64 microseconds, u64 field)
{
  const double us = static_cast<double>(microseconds);
  patch.average_us = patch.frames == 0 ? us : patch.average_us * 0.95 + us * 0.05;
  ++patch.frames;
  patch.window_max_us = std::max(patch.window_max_us, us);
  if (field - patch.window_start_field >= 300)
  {
    patch.max_us = patch.window_max_us;
    patch.window_max_us = 0;
    patch.window_start_field = field;
  }
  patch.max_us = std::max(patch.max_us, patch.window_max_us);
}

// Runs one hook; on an error, logs it and stops the patch after three in a row. False when the
// call failed.
bool RunHook(RunningPatch& patch, Hook hook, const ScriptEnvironment& env, u64* total_us)
{
  const bool ok = patch.script->Call(hook, env);
  *total_us += patch.script->GetLastCallMicroseconds();
  PublishLog(patch.id, patch.script->TakeLog());
  ApplySettingRequests(patch);
  if (ok)
  {
    patch.consecutive_errors = 0;
    return true;
  }
  const std::string error = patch.script->Error();
  LogLineFor(patch.id, 2, fmt::format("{}: {}", HookName(hook), error), env.field);
  if (++patch.consecutive_errors >= MAX_CONSECUTIVE_ERRORS)
  {
    StopPatch(patch, env, PatchState::Error,
              fmt::format("stopped after {} errors in a row: {}", MAX_CONSECUTIVE_ERRORS,
                          FirstLine(error)));
  }
  return false;
}

void ApplySetting(RunningPatch& patch, const std::string& name, double value,
                  const ScriptEnvironment& env)
{
  const SettingDef* def = patch.manifest.FindSetting(name);
  if (!def || !patch.script)
    return;
  const double normalized = def->Normalize(value);
  if (patch.values[name] == normalized)
    return;
  patch.values[name] = normalized;
  patch.script->SetSettingValue(*def, normalized);
  if (patch.script->HasSettingChangedHook() && patch.state == PatchState::Running &&
      !patch.needs_on_load)
  {
    const bool ok = patch.script->CallSettingChanged(name, normalized, env);
    PublishLog(patch.id, patch.script->TakeLog());
    if (!ok)
      LogLineFor(patch.id, 2, "on_setting_changed: " + patch.script->Error(), env.field);
  }
  Publish(patch);
}

// Applies the queued commands. `env.memory` may be null (no hooks that need memory then).
void ApplyCommands(const ScriptEnvironment& env)
{
  Manager& m = GetManager();
  std::vector<Command> commands;
  {
    std::lock_guard lock(m.mutex);
    commands = std::exchange(m.commands, {});
  }
  if (commands.empty())
    return;

  std::string game_id;
  u16 revision = 0;
  {
    std::lock_guard lock(m.mutex);
    game_id = m.game_id;
    revision = m.revision;
  }
  const PatchValues values = LoadPatchValues(game_id, revision);
  bool changed = false;
  for (const Command& command : commands)
  {
    switch (command.type)
    {
    case Command::Type::Sync:
      SyncPatch(command.id, command.reload, env, values);
      changed = true;
      break;
    case Command::Type::SyncAll:
      for (const std::string& id : AllPatchIds())
        SyncPatch(id, command.reload, env, values);
      changed = true;
      break;
    case Command::Type::Setting:
      for (const auto& patch : m.patches)
      {
        if (patch->id == command.id)
          ApplySetting(*patch, command.name, command.value, env);
      }
      break;
    }
  }
  if (changed)
    UpdateSharedState();
}

// Removes the patches that stopped with an error (their published state stays).
void DropStopped()
{
  Manager& m = GetManager();
  const size_t before = m.patches.size();
  std::erase_if(m.patches, [](const auto& p) { return p->state != PatchState::Running; });
  if (m.patches.size() != before)
    UpdateSharedState();
}

void RunHooks(const ScriptEnvironment& env, bool frame_hooks)
{
  Manager& m = GetManager();
  const bool state_loaded = frame_hooks && m.state_loaded.exchange(false);
  for (const auto& patch_ptr : m.patches)
  {
    RunningPatch& patch = *patch_ptr;
    if (patch.state != PatchState::Running || !patch.script)
      continue;
    u64 total_us = 0;
    if (patch.needs_on_load)
    {
      if (!RunHook(patch, Hook::OnLoad, env, &total_us))
        continue;
      patch.needs_on_load = false;
      patch.status = "running";
    }
    else if (state_loaded && patch.script->HasHook(Hook::OnStateLoaded))
    {
      if (!RunHook(patch, Hook::OnStateLoaded, env, &total_us))
        continue;
    }
    if (frame_hooks && patch.script->HasHook(Hook::OnFrame))
      RunHook(patch, Hook::OnFrame, env, &total_us);
    if (frame_hooks)
      RecordCallTime(patch, total_us, env.field);
    if (patch.state == PatchState::Running && (env.field % 30 == 0 || !frame_hooks))
      Publish(patch);
  }
  DropStopped();
  UpdatePadHolds();
}

void HandleBlockReason(Core::System& system, const ScriptEnvironment& env)
{
  Manager& m = GetManager();
  std::string reason = BlockReason(system);
  if (reason == m.block_reason)
    return;
  m.block_reason = std::move(reason);
  if (!m.block_reason.empty())
  {
    for (const auto& patch : m.patches)
      StopPatch(*patch, env, PatchState::Blocked, "paused while " + m.block_reason);
    m.patches.clear();
    UpdateSharedState();
    UpdatePadHolds();
    return;
  }
  Queue({Command::Type::SyncAll, {}, true});
}
}  // namespace

// ---- Emulation hooks ----

void OnGameLoaded(const std::string& game_id, u16 revision, bool is_wii)
{
  Manager& m = GetManager();
  std::lock_guard run_lock(m.run_mutex);
  if (!m.patches.empty())
    OnGameUnloaded(nullptr);
  {
    std::lock_guard lock(m.mutex);
    m.game_running = !game_id.empty();
    m.game_id = game_id;
    m.revision = revision;
    m.is_wii = is_wii;
    m.commands.clear();
    m.published.clear();
  }
  m.field.store(0);
  m.block_reason.clear();
  m.state_loaded.store(false);
  m.pad_signature = 0;
  m.attach_failures.clear();
  if (!game_id.empty())
    Queue({Command::Type::SyncAll, {}, true});
}

void RunFrame(const Core::CPUThreadGuard& guard)
{
  Manager& m = GetManager();
  m.field.fetch_add(1, std::memory_order_relaxed);
  if (!m.has_work.load(std::memory_order_acquire))
  {
    m.state_loaded.store(false, std::memory_order_relaxed);
    return;
  }
  std::lock_guard run_lock(m.run_mutex);
  GuardMemoryAccess memory(guard);
  Environment environment;
  FillEnvironment(&environment, &memory);
  HandleBlockReason(guard.GetSystem(), environment.env);
  ApplyCommands(environment.env);
  if (!m.patches.empty())
  {
    environment.input = CaptureInput();
    RunHooks(environment.env, true);
  }
  else
  {
    // The last patch stopped: its controller holds go.
    UpdatePadHolds();
  }
  std::lock_guard lock(m.mutex);
  m.has_work.store(!m.patches.empty() || !m.commands.empty(), std::memory_order_release);
}

void OnGameUnloaded(const Core::CPUThreadGuard* guard)
{
  Manager& m = GetManager();
  std::lock_guard run_lock(m.run_mutex);
  std::optional<GuardMemoryAccess> memory;
  if (guard)
    memory.emplace(*guard);
  Environment environment;
  FillEnvironment(&environment, memory ? &*memory : nullptr);
  for (const auto& patch : m.patches)
    StopPatch(*patch, environment.env, PatchState::Off, "");
  const bool had_patches = !m.patches.empty() || !m.pad_holds.empty();
  m.patches.clear();
  m.pad_holds.clear();
  m.pad_signature = 0;
  if (had_patches)
    AgentServer::AgentInput::Detach(AgentServer::AgentInput::Layer::Patch);
  Gecko::SetVRPatchRegion({});
  VR::OverrideSuppression::Set({});
  std::lock_guard lock(m.mutex);
  m.game_running = false;
  m.commands.clear();
  for (auto& [id, published] : m.published)
  {
    published.state = PatchState::Off;
    published.status.clear();
  }
  m.has_work.store(false, std::memory_order_release);
}

void DoState(PointerWrap& p)
{
  if (p.IsReadMode())
    GetManager().state_loaded.store(true, std::memory_order_release);
}

// ---- UIs ----

const char* PatchStateName(PatchState state)
{
  switch (state)
  {
  case PatchState::Off:
    return "off";
  case PatchState::Waiting:
    return "waiting";
  case PatchState::Running:
    return "running";
  case PatchState::Error:
    return "error";
  case PatchState::Blocked:
  default:
    return "blocked";
  }
}

std::vector<PatchInfo> ListPatches(const std::string& game_id, std::optional<u16> revision)
{
  Manager& m = GetManager();
  std::vector<PatchInfo> list;
  if (game_id.empty())
    return list;
  const PatchValues values = LoadPatchValues(game_id, revision);

  std::map<std::string, std::string> session;
  std::map<std::string, Published> published;
  bool running_game = false;
  {
    std::lock_guard lock(m.mutex);
    running_game = m.game_running && m.game_id == game_id;
    for (const auto& [key, source] : m.session_sources)
    {
      if (key.first == game_id)
        session[key.second] = source;
    }
    if (running_game)
    {
      for (const auto& [id, state] : m.published)
      {
        // Everything but the log.
        Published& copy = published[id];
        copy.state = state.state;
        copy.status = state.status;
        copy.values = state.values;
        copy.scratch = state.scratch;
        copy.average_us = state.average_us;
        copy.max_us = state.max_us;
        copy.memory_bytes = state.memory_bytes;
      }
    }
  }

  const auto fill = [&](PatchInfo& info, const std::variant<Manifest, std::string>& read) {
    if (const Manifest* manifest = std::get_if<Manifest>(&read))
    {
      info.manifest = *manifest;
      info.for_this_game = GameMismatch(*manifest, game_id, revision).empty();
      info.values = SettingValues(*manifest, values, info.id);
    }
    else
    {
      info.manifest_error = std::get<std::string>(read);
    }
    info.enabled = info.enabled || values.IsEnabled(info.id);
    const auto it = published.find(info.id);
    if (it != published.end())
    {
      info.state = it->second.state;
      info.status = it->second.status;
      if (!it->second.values.empty())
        info.values = it->second.values;
      info.scratch_base = it->second.scratch.start;
      info.scratch_size = it->second.scratch.end - it->second.scratch.start;
      info.average_us = it->second.average_us;
      info.max_us = it->second.max_us;
      info.memory_bytes = it->second.memory_bytes;
    }
    else if (info.enabled)
    {
      info.state = running_game ? PatchState::Waiting : PatchState::Off;
    }
  };

  std::set<std::string> seen;
  for (const FileEntry& file : FindPatchFiles(game_id, revision))
  {
    PatchInfo info;
    info.id = file.id;
    info.path = file.path;
    info.origin = file.origin;
    info.overrides_sys = file.overrides_sys;
    std::string source;
    std::variant<Manifest, std::string> read = std::string("the file cannot be read");
    if (const auto it = session.find(file.id); it != session.end())
    {
      info.origin = PatchOrigin::Session;
      info.enabled = true;
      read = ReadManifest(it->second, file.id);
    }
    else if (File::ReadFileToString(file.path, source))
    {
      read = ManifestOf(file.path, file.id, source);
    }
    fill(info, read);
    seen.insert(info.id);
    list.push_back(std::move(info));
  }
  for (const auto& [id, source] : session)
  {
    if (seen.contains(id))
      continue;
    PatchInfo info;
    info.id = id;
    info.origin = PatchOrigin::Session;
    info.enabled = true;
    fill(info, ReadManifest(source, id));
    list.push_back(std::move(info));
  }
  std::ranges::sort(list, [](const PatchInfo& a, const PatchInfo& b) { return a.id < b.id; });
  return list;
}

std::optional<PatchInfo> FindPatch(const std::string& game_id, std::optional<u16> revision,
                                   const std::string& id)
{
  for (PatchInfo& info : ListPatches(game_id, revision))
  {
    if (info.id == id)
      return std::move(info);
  }
  return std::nullopt;
}

std::string ReadPatchSource(const std::string& game_id, std::optional<u16> revision,
                            const std::string& id)
{
  Manager& m = GetManager();
  {
    std::lock_guard lock(m.mutex);
    const auto it = m.session_sources.find({game_id, id});
    if (it != m.session_sources.end())
      return it->second;
  }
  for (const FileEntry& file : FindPatchFiles(game_id, revision))
  {
    std::string source;
    if (file.id == id && File::ReadFileToString(file.path, source))
      return source;
  }
  return {};
}

bool SetPatchEnabled(const std::string& game_id, const std::string& id, bool enabled)
{
  if (game_id.empty() || !IsValidPatchId(id))
    return false;
  PatchValues values = LoadUserPatchValues(game_id);
  values.enabled[id] = enabled;
  const bool saved = SaveUserPatchValues(game_id, values);
  if (GetRunningGameId() == game_id)
    Queue({Command::Type::Sync, id, false});
  return saved;
}

bool SetPatchSetting(const std::string& game_id, const std::string& id, const std::string& name,
                     double value, bool save)
{
  if (game_id.empty() || !IsValidPatchId(id) || name.empty())
    return false;
  bool saved = true;
  if (save)
  {
    // The running patch normalizes live values itself; the file gets the normalized one.
    if (const std::optional<PatchInfo> info = FindPatch(game_id, GetRunningRevision(), id))
    {
      if (info->manifest)
      {
        const SettingDef* def = info->manifest->FindSetting(name);
        if (!def)
          return false;
        value = def->Normalize(value);
      }
    }
    PatchValues values = LoadUserPatchValues(game_id);
    values.settings[id][name] = value;
    saved = SaveUserPatchValues(game_id, values);
  }
  if (GetRunningGameId() == game_id)
    Queue({Command::Type::Setting, id, false, name, value});
  return saved;
}

void ReloadPatches()
{
  {
    std::lock_guard lock(s_manifest_cache_mutex);
    s_manifest_cache.clear();
  }
  Queue({Command::Type::SyncAll, {}, true});
}

std::string GetRunningGameId()
{
  Manager& m = GetManager();
  std::lock_guard lock(m.mutex);
  return m.game_running ? m.game_id : std::string();
}

std::optional<u16> GetRunningRevision()
{
  Manager& m = GetManager();
  std::lock_guard lock(m.mutex);
  if (!m.game_running)
    return std::nullopt;
  return m.revision;
}

bool HasPendingChanges()
{
  Manager& m = GetManager();
  std::lock_guard lock(m.mutex);
  return !m.commands.empty();
}

void ApplyPendingNow(Core::System& system)
{
  if (!HasPendingChanges() || Core::GetState(system) != Core::State::Paused)
    return;
  Core::CPUThreadGuard guard(system);
  const auto& ppc_state = system.GetPPCState();
  // The patches' on_load reads and writes memory at the game's effective addresses.
  if (!ppc_state.msr.DR || !ppc_state.msr.IR)
    return;
  Manager& m = GetManager();
  std::lock_guard run_lock(m.run_mutex);
  GuardMemoryAccess memory(guard);
  Environment environment;
  FillEnvironment(&environment, &memory);
  environment.input = CaptureInput();
  ApplyCommands(environment.env);
  RunHooks(environment.env, false);
  std::lock_guard lock(m.mutex);
  m.has_work.store(!m.patches.empty() || !m.commands.empty(), std::memory_order_release);
}

std::vector<LogLine> GetLog(const std::string& id, size_t max_lines)
{
  Manager& m = GetManager();
  std::lock_guard lock(m.mutex);
  const auto it = m.published.find(id);
  if (it == m.published.end())
    return {};
  const std::deque<LogLine>& log = it->second.log;
  const size_t start = log.size() > max_lines ? log.size() - max_lines : 0;
  return {log.begin() + static_cast<std::ptrdiff_t>(start), log.end()};
}

std::string GetUserPatchDirectory(const std::string& game_id)
{
  return File::GetUserPath(D_VRPATCHES_IDX) + game_id + DIR_SEP;
}

std::string GetSysPatchDirectory()
{
  return File::GetSysDirectory() + VRPATCHES_DIR DIR_SEP;
}

// ---- The agent server ----

std::string SetSessionSource(const std::string& id, std::string source)
{
  if (!IsValidPatchId(id))
    return "the id must be 1 to 64 letters, digits, '-', '_' or '.' (it is the file name)";
  const std::string game_id = GetRunningGameId();
  if (game_id.empty())
    return "no game is running";
  const std::variant<Manifest, std::string> read = ReadManifest(source, id);
  if (const std::string* error = std::get_if<std::string>(&read))
    return *error;
  Manager& m = GetManager();
  {
    std::lock_guard lock(m.mutex);
    m.session_sources[{game_id, id}] = std::move(source);
  }
  Queue({Command::Type::Sync, id, true});
  return {};
}

bool HasSessionSource(const std::string& id)
{
  Manager& m = GetManager();
  std::lock_guard lock(m.mutex);
  return m.session_sources.contains({m.game_id, id});
}

void ClearSessionSource(const std::string& id)
{
  Manager& m = GetManager();
  bool erased = false;
  {
    std::lock_guard lock(m.mutex);
    erased = m.session_sources.erase({m.game_id, id}) != 0;
  }
  if (erased)
    Queue({Command::Type::Sync, id, true});
}

void ClearSessionSources()
{
  Manager& m = GetManager();
  std::vector<std::string> ids;
  {
    std::lock_guard lock(m.mutex);
    for (const auto& [key, source] : m.session_sources)
    {
      if (key.first == m.game_id)
        ids.push_back(key.second);
    }
    m.session_sources.clear();
  }
  for (const std::string& id : ids)
    Queue({Command::Type::Sync, id, true});
}

std::string SaveSessionSource(const std::string& id, bool enable, std::string* path)
{
  Manager& m = GetManager();
  std::string game_id;
  std::string source;
  {
    std::lock_guard lock(m.mutex);
    game_id = m.game_id;
    const auto it = m.session_sources.find({game_id, id});
    if (it == m.session_sources.end())
      return "there is no live source for this patch (set_vr_patch_source)";
    source = it->second;
  }
  const std::string file = GetUserPatchDirectory(game_id) + id + PATCH_EXTENSION;
  if (!File::CreateFullPath(file) || !File::WriteStringToFile(file, source))
    return "could not write " + file;
  if (path)
    *path = file;
  {
    std::lock_guard lock(m.mutex);
    m.session_sources.erase({game_id, id});
  }
  {
    std::lock_guard lock(s_manifest_cache_mutex);
    s_manifest_cache.erase(file);
  }
  if (enable)
  {
    PatchValues values = LoadUserPatchValues(game_id);
    values.enabled[id] = true;
    SaveUserPatchValues(game_id, values);
  }
  Queue({Command::Type::Sync, id, false});
  return {};
}
}  // namespace VRPatch
