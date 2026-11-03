// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// One VR game patch: a sandboxed Lua state running a patch file, with the engine's bindings (mem,
// vr, pad, asm, game, log). See docs/VR/VR-Patches.md for the file format and the API.

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/VRPatch/PatchTypes.h"
#include "VideoCommon/VR/CameraAnchorMath.h"

namespace VRPatch
{
class MemoryAccess;

enum class PadDevice : u8
{
  GCPad,
  Wiimote,
  // The extension plugged in the Wii Remote (Nunchuk, Classic Controller...).
  Attachment,
};

enum class PadControlKind : u8
{
  // A button, or an analog trigger value.
  Button,
  Analog,
  Stick,
  // The Wii Remote's IR pointer ("IR"): its X and Y controls. A NaN value takes it off the screen.
  Pointer,
};

struct PadControl
{
  std::string group;
  std::string control;
};

// Resolves controller control names for pad.* (the emulated controllers in production, a fake in
// the tests). Called on the thread that runs the patch.
class PadResolver
{
public:
  virtual ~PadResolver();
  // A button ("A", "D-Pad/Up"), an analog trigger ("L-Analog"), a stick group ("Main Stick", ""
  // = the first stick: its X and Y controls) or the pointer group ("IR": its X and Y controls).
  // Empty with `error` set when unknown.
  virtual std::vector<PadControl> Resolve(PadDevice device, u8 port, PadControlKind kind,
                                          std::string_view name, std::string* error) = 0;
};

// What a patch can reach while one of its functions runs.
struct ScriptEnvironment
{
  // Null when there is no emulated memory (the game stopped): mem.* reads return nil.
  MemoryAccess* memory = nullptr;
  const VRInputFrame* input = nullptr;
  PadResolver* pad = nullptr;
  std::string_view game_id;
  u16 revision = 0;
  bool is_wii = false;
  // VI fields since the game started.
  u64 field = 0;
  u32 scratch_base = 0;
  u32 scratch_size = 0;
  // Game units per meter of the VR profile.
  double units_per_meter = 1.0;
  // The headset's refresh rate (Hz) as the VR session reported it; 0 without a session or before
  // the runtime reports it.
  double display_refresh_rate = 0.0;
  // A Camera Anchor applies: the setting is on and a VR session runs in stereo.
  bool camera_anchor_active = false;
  // The Camera Anchor Smoothing setting (0 to 0.95).
  double camera_anchor_smoothing = 0.0;
};

// Per call of a patch function.
struct ScriptLimits
{
  u64 max_instructions = 50'000'000;
  u32 max_milliseconds = 40;
  size_t max_memory_bytes = 32 * 1024 * 1024;
};

enum class Hook : u8
{
  OnLoad,
  OnFrame,
  OnStateLoaded,
  OnUnload,
  Count,
};

const char* HookName(Hook hook);

// pad.* holds: (device, port, group, control) -> value, kept until the patch releases them.
using PadHoldKey = std::tuple<PadDevice, u8, std::string, std::string>;
using PadHolds = std::map<PadHoldKey, double>;

// A VR setting changed by vr.set_setting, applied by the engine on the host thread.
struct SettingRequest
{
  std::string key;
  std::string value;
};

struct ScriptContext;

class PatchScript final
{
public:
  explicit PatchScript(std::string id, ScriptLimits limits = {});
  ~PatchScript();
  PatchScript(const PatchScript&) = delete;
  PatchScript& operator=(const PatchScript&) = delete;

  // Compiles `source`, runs its top level and reads the `patch` table. False with Error() set on a
  // syntax error, a runtime error or an invalid manifest.
  bool Load(std::string_view source, const ScriptEnvironment& env);

  const std::string& GetId() const;
  const Manifest& GetManifest() const;
  bool HasHook(Hook hook) const;
  bool HasSettingChangedHook() const;

  // Calls a hook if the patch defines it. False with Error() set when it raised an error or ran
  // past its limits.
  bool Call(Hook hook, const ScriptEnvironment& env);
  bool CallSettingChanged(const std::string& name, double value, const ScriptEnvironment& env);

  // settings.<name> = value (a boolean for Bool settings).
  bool SetSettingValue(const SettingDef& def, double value);

  const std::string& Error() const;
  // The log lines written since the last call.
  std::vector<LogLine> TakeLog();

  const PadHolds& GetPadHolds() const;
  // Changes each time the holds change.
  u64 GetPadHoldsGeneration() const;
  // Requests made by vr.set_setting since the last call.
  std::vector<SettingRequest> TakeSettingRequests();
  // The override flags raised with vr.set_flag, and a counter that changes with them.
  const std::set<std::string>& GetFlags() const;
  u64 GetFlagsGeneration() const;
  // Where the game keeps its camera pitch's sine (vr.set_camera_pitch_source), or nothing.
  std::optional<u32> GetCameraPitchSource() const;
  // The patch keeps the VR pointer on the screen (vr.keep_pointer_on_screen).
  bool KeepsPointerOnScreen() const;
  // The Camera Anchor the patch gave since the last call (vr.set_camera_anchor*), then none, and
  // the frame pairs it was composed from (vr.set_camera_anchor_frames), if any.
  std::optional<VR::CameraAnchorMath::Anchor>
  TakeCameraAnchor(std::optional<VR::CameraAnchorMath::FramePairs>* pairs = nullptr);

  size_t GetMemoryUsed() const;
  // Microseconds the last call took.
  u64 GetLastCallMicroseconds() const;

private:
  std::unique_ptr<ScriptContext> m_context;
};

// The manifest of a patch file, read without the engine: the top level runs in a sandbox with only
// the Lua libraries and the math helpers (vec3, quat, mat34...). An error message otherwise.
std::variant<Manifest, std::string> ReadManifest(std::string_view source, std::string_view id);

// Patch IDs are file names: letters, digits, '-', '_' and '.', 1 to 64 characters.
bool IsValidPatchId(std::string_view id);

// The API reference shown to agents (describe_vr_patch_api) and in the docs.
std::string_view GetApiReference();
}  // namespace VRPatch
