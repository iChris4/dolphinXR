// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// VR overrides of the game's profile: list, try live (session), save, discard.

#include <algorithm>
#include <map>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Core/AgentServer/AgentServer.h"
#include "Core/AgentServer/OverrideSession.h"
#include "Core/AgentServer/ToolHelpers.h"
#include "Core/ConfigManager.h"
#include "VideoCommon/ElementsGroupManager.h"
#include "VideoCommon/HideObjectEngine.h"
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/TextureElementManager.h"
#include "VideoCommon/VR/AgentVideoJobs.h"

namespace AgentServer
{
namespace
{
OverrideSession& Session()
{
  return Server::GetInstance().GetOverrideSession();
}

OverrideKind RequireKind(const ToolArgs& args)
{
  const std::string name = args.RequireString("kind");
  const std::optional<OverrideKind> kind = ParseOverrideKind(name);
  if (!kind)
    throw ToolError("kind is shader, elements, texture or hideobject.");
  return *kind;
}

std::vector<OverrideKind> KindsArg(const ToolArgs& args)
{
  std::vector<OverrideKind> kinds;
  for (const std::string& name : args.GetStringArray("kinds"))
  {
    if (name == "all")
      return {ALL_OVERRIDE_KINDS.begin(), ALL_OVERRIDE_KINDS.end()};
    const std::optional<OverrideKind> kind = ParseOverrideKind(name);
    if (!kind)
      throw ToolError("kinds are shader, elements, texture, hideobject or all.");
    kinds.push_back(*kind);
  }
  if (kinds.empty())
    kinds.assign(ALL_OVERRIDE_KINDS.begin(), ALL_OVERRIDE_KINDS.end());
  return kinds;
}

void AdvanceIfPaused(const ToolArgs& args)
{
  if (IsPaused() && args.GetBool("advance", true))
    AdvanceFrames(1);
}

ToolResult ListOverrides(const ToolArgs& args)
{
  RequireGame();
  std::vector<OverrideKind> kinds;
  const std::string kind_name = args.GetString("kind", "all");
  if (kind_name == "all")
    kinds.assign(ALL_OVERRIDE_KINDS.begin(), ALL_OVERRIDE_KINDS.end());
  else
    kinds.push_back(RequireKind(args));

  std::string text = fmt::format("VR overrides of {} (origin: sys = ships with DolphinXR, user = "
                                 "the user's file, session = tried by you, not saved):\n",
                                 SConfig::GetInstance().GetGameID());
  for (const OverrideKind kind : kinds)
  {
    const std::vector<OverrideRow> rows = Session().List(kind);
    text += fmt::format("[{}] {} override(s)\n", GetOverrideKindName(kind), rows.size());
    for (const OverrideRow& row : rows)
    {
      text += fmt::format("  {} \"{}\" ({}{}): {}\n", row.enabled ? "on " : "off", row.name,
                          row.origin, row.removed ? ", removed" : "", row.summary);
    }
  }
  return ToolResult::Text(std::move(text));
}

ToolResult GetOverride(const ToolArgs& args)
{
  RequireGame();
  const OverrideKind kind = RequireKind(args);
  const std::string name = args.RequireString("name");
  return ToolResult::Text(
      fmt::format("[{}]\n{}", GetOverrideSectionName(kind), Session().GetEntryText(kind, name)));
}

// One real entry of each kind from the shipped profiles, as an example of the format.
std::string FindSysExample(OverrideKind kind)
{
  static std::map<OverrideKind, std::string> s_cache;
  if (const auto it = s_cache.find(kind); it != s_cache.end())
    return it->second;

  const std::string directory = File::GetSysDirectory() + GAMESETTINGSVR_DIR DIR_SEP;
  std::vector<std::string> files = {directory + "D43E01.ini", directory + "G9SE8P.ini",
                                    directory + "GSAE01.ini"};
  for (const File::FSTEntry& entry : File::ScanDirectoryTree(directory, false).children)
  {
    if (!entry.isDirectory)
      files.push_back(entry.physicalName);
  }

  const std::string header = fmt::format("[{}]", GetOverrideSectionName(kind));
  for (const std::string& path : files)
  {
    std::string contents;
    if (!File::ReadFileToString(path, contents))
      continue;
    const size_t section = contents.find(header);
    if (section == std::string::npos)
      continue;
    const size_t dollar = contents.find("\n$", section);
    if (dollar == std::string::npos)
      continue;
    const size_t line_end = contents.find('\n', dollar + 1);
    std::string name = contents.substr(dollar + 2, line_end - dollar - 2);
    while (!name.empty() && (name.back() == '\r' || name.back() == ' '))
      name.pop_back();
    if (auto text = ExtractOverrideEntryText(contents, GetOverrideSectionName(kind), name))
    {
      std::string example = fmt::format("Example from Sys/GameSettingsVR/{}:\n{}\n",
                                        PathToString(StringToPath(path).filename()), *text);
      s_cache[kind] = example;
      return example;
    }
  }
  return "";
}

constexpr std::string_view HANDLINGS_DOC =
    "Handlings: skip (not drawn); screen (on the world-locked 2D virtual screen: HUDs, menus, "
    "FMVs); fullscreen (drawn natively per eye, not reprojected: post effects, fades, bloom); "
    "headlocked (follows the head: HUDs that should stay in view); units_per_meter (this draw at "
    "another world scale; units_per_meter=<float>); flag (no change, sets a named flag that "
    "conditions other overrides); passthrough (a camera passthrough window; "
    "passthrough_opacity=0..1); camera_anchor (the element's origin becomes the VR camera, e.g. "
    "first person; anchor_right/anchor_up/anchor_forward meters, anchor_hide=1, "
    "anchor_rotation=off|yaw|full, anchor_yaw, anchor_upm); controller_anchor (the element follows "
    "a controller; anchor_hand=left|right, offsets, anchor_rotation=full, anchor_yaw/pitch/roll). "
    "element_depth=<0..1> pushes a screen/headlocked element into the screen. Common to Shader, "
    "Elements and Texture overrides: flag=<name>, condition=<flag> with "
    "condition_mode=activate|deactivate, texture=<16 hex> (repeatable) with "
    "texture_mode=include|exclude, min_depth=<game units> / max_depth=<game units>: the entry "
    "only matches draws whose vertices are all at least / at most that far from the game camera "
    "(a HUD drawn in perspective just in front of the camera that shares its textures or state "
    "with world objects; get_draw and list_draws print a draw's camera depth; a draw whose depth "
    "cannot be read does not match).";

std::string FormatDoc(OverrideKind kind)
{
  switch (kind)
  {
  case OverrideKind::Shader:
    return fmt::format(
        "[ShaderOverride] entries apply a handling to every draw using one shader.\n"
        "$Name\nHash=<16 hex digits: the draw's vs/ps/gs hash zero-padded>\nType=PS|VS|GS\n"
        "match_mode=exact_hash|shader_family (family: every shader of the same family; add "
        "family_signature=<16 hex, the draw's family>)\nhandling=<handling>\n{}",
        HANDLINGS_DOC);
  case OverrideKind::Elements:
    return fmt::format(
        "[ElementsGroupOverride] entries match draws by their state: projection, viewport, "
        "scissor, render state (get_draw prints a ready entry for a draw).\n"
        "$Name\nformat=element_only_v6\nhandling=<handling, or screen_pane>\n"
        "sig_use_projection=0|1 (exact projection values) / sig_use_projection_type=0|1 "
        "(perspective vs orthographic) / sig_use_viewport / sig_use_scissor / "
        "sig_use_render_state (alpha test, z, blend) / sig_use_layer: which groups must match "
        "exactly.\nsig_perspective, sig_persp_hfov/vfov/far (x100), sig_persp_near (x1000), "
        "sig_ortho_left/right/top/bottom (x100), sig_vp_x/y (viewport center), sig_vp_w/h "
        "(half size), sig_sc_l/t/r/b (scissor), sig_alpha (8 hex), sig_ztest, sig_zupdate, "
        "sig_zfunc, sig_blend_color, sig_blend_alpha, sig_layer=0.\n"
        "Elements only: handling=screen_pane keeps a perspective sub-region (picture-in-picture) "
        "on the virtual screen with depth: screen_pane_depth=game|vr|flat, "
        "screen_pane_reference_depth=<game W>, screen_pane_depth_strength=<0..1>; "
        "preserve_stereo_efb=1 (fullscreen effects reading a stereo EFB copy); clear_efb=1 with "
        "clear_efb_min/clear_efb_max (EFB copy widths) clears the EFB after the element.\n{}",
        HANDLINGS_DOC);
  case OverrideKind::Texture:
    return fmt::format(
        "[TextureElementOverride] entries apply a handling to every draw binding one of the "
        "textures.\n$Name\nhandling=<handling>\ntexture=<16 hex digits> (one line per "
        "texture)\n{}",
        HANDLINGS_DOC);
  case OverrideKind::HideObject:
    return "[HideObjectCodes] entries hide every vertex batch whose raw vertex data starts with "
           "the given bytes (8 to 128 bits). capture_hide_object_prefixes builds one from draws.\n"
           "$Name\nType=128bits\nUpper=0x<16 hex>\nLower=0x<16 hex>\n(Type/Upper/Lower "
           "repeat for more patterns; shorter types use the low bytes of Lower.)";
  }
  return "";
}

ToolResult DescribeOverrideFormat(const ToolArgs& args)
{
  const OverrideKind kind = RequireKind(args);
  std::string text = FormatDoc(kind) + "\n\n" + FindSysExample(kind);
  text += "\nPass one entry (from its \"$Name\" line, without the section header) to "
          "set_override. The enable lists are handled for you.";
  return ToolResult::Text(std::move(text));
}

ToolResult SetOverride(const ToolArgs& args)
{
  const OverrideKind kind = RequireKind(args);
  std::string text =
      Session().SetOverride(kind, args.RequireString("ini_text"), args.GetBool("enabled", true));
  AdvanceIfPaused(args);
  text += "\nApplied for this session only. Check it with a screenshot, keep it with "
          "save_overrides once the user agrees.";
  return ToolResult::Text(std::move(text));
}

ToolResult RemoveOverride(const ToolArgs& args)
{
  const OverrideKind kind = RequireKind(args);
  const std::string name = args.RequireString("name");
  Session().RemoveOverride(kind, name);
  AdvanceIfPaused(args);
  return ToolResult::Text(fmt::format("Removed \"{}\" for this session (save_overrides makes it "
                                      "permanent).",
                                      name));
}

ToolResult EnableOverride(const ToolArgs& args)
{
  const OverrideKind kind = RequireKind(args);
  const std::string name = args.RequireString("name");
  const bool enabled = args.GetBool("enabled", true);
  Session().EnableOverride(kind, name, enabled);
  AdvanceIfPaused(args);
  return ToolResult::Text(
      fmt::format("\"{}\" {} for this session.", name, enabled ? "enabled" : "disabled"));
}

ToolResult SaveOverrides(const ToolArgs& args)
{
  RequireGame();
  std::string text;
  for (const OverrideKind kind : KindsArg(args))
  {
    const size_t edits = Session().CountEdits(kind);
    if (edits == 0)
      continue;
    const std::string path = Session().Save(kind);
    text += fmt::format("Saved {} {} edit(s) to {}.\n", edits, GetOverrideKindName(kind), path);
  }
  if (text.empty())
    text = "Nothing to save: no override was changed in this session.";
  return ToolResult::Text(std::move(text));
}

ToolResult DiscardOverrides(const ToolArgs& args)
{
  RequireGame();
  size_t discarded = 0;
  for (const OverrideKind kind : KindsArg(args))
  {
    discarded += Session().CountEdits(kind);
    Session().Discard(kind);
  }
  AdvanceIfPaused(args);
  return ToolResult::Text(fmt::format(
      "Discarded {} session edit(s); the game's saved overrides are active again.", discarded));
}
}  // namespace

void RegisterOverrideTools(ToolRegistry& registry, Server& server)
{
  // Server stopped with unsaved edits: put the managers back to the game's files.
  server.AddServerStoppedCallback([&server] {
    bool had_edits = false;
    for (const OverrideKind kind : ALL_OVERRIDE_KINDS)
      had_edits |= server.GetOverrideSession().CountEdits(kind) != 0;
    server.GetOverrideSession().Forget();
    if (!had_edits)
      return;
    VR::AgentVideoJobs::Post([] {
      const std::string game_id = SConfig::GetInstance().GetGameID();
      ShaderHunter::GetInstance().LoadOverrides(game_id);
      ElementsGroupManager::GetInstance().LoadOverrides(game_id);
      TextureElementManager::GetInstance().LoadOverrides(game_id);
      HideObjectEngine::Engine::GetInstance().LoadCodes(game_id);
    });
  });

  const auto kind_schema = [](Schema schema) {
    schema.String("kind", "Override kind.", {"shader", "elements", "texture", "hideobject"});
    return schema;
  };

  registry.Add({.name = "list_overrides",
                .title = "List VR overrides",
                .description = "The game's VR overrides (Sys, User and this session's edits) "
                               "with whether they are on and what they do.",
                .input_schema = Schema()
                                    .String("kind", "Default all.",
                                            {"all", "shader", "elements", "texture", "hideobject"})
                                    .Build(),
                .read_only = true,
                .idempotent = true,
                .handler = ListOverrides});

  registry.Add({.name = "get_override",
                .title = "One override's INI text",
                .description = "The INI entry of one override, to read or edit and pass back to "
                               "set_override.",
                .input_schema = kind_schema(Schema())
                                    .String("name", "Override name (without $).")
                                    .Required({"kind", "name"})
                                    .Build(),
                .read_only = true,
                .idempotent = true,
                .handler = GetOverride});

  registry.Add({.name = "describe_override_format",
                .title = "Override format reference",
                .description = "The keys of an override kind, what each handling does, and a "
                               "real example from the shipped profiles.",
                .input_schema = kind_schema(Schema()).Required({"kind"}).Build(),
                .read_only = true,
                .idempotent = true,
                .handler = DescribeOverrideFormat});

  registry.Add(
      {.name = "set_override",
       .title = "Try an override",
       .description =
           "Adds or replaces (by name) one override for this session and applies it at once; "
           "nothing is saved. ini_text is one GameSettingsVR entry: a \"$Name\" line, then "
           "key=value lines (see describe_override_format, get_draw). Returns how Dolphin parsed "
           "it. When paused, one frame is stepped so it is drawn.",
       .input_schema = kind_schema(Schema())
                           .String("ini_text", "The entry, \"$Name\" first.")
                           .Boolean("enabled", "Default true.")
                           .Boolean("advance", "When paused, step one frame (default true).")
                           .Required({"kind", "ini_text"})
                           .Build(),
       .handler = SetOverride});

  registry.Add({.name = "remove_override",
                .title = "Remove an override",
                .description = "Removes an override for this session (saved or not).",
                .input_schema = kind_schema(Schema())
                                    .String("name", "Override name.")
                                    .Boolean("advance", "When paused, step one frame.")
                                    .Required({"kind", "name"})
                                    .Build(),
                .handler = RemoveOverride});

  registry.Add({.name = "enable_override",
                .title = "Turn an override on or off",
                .description = "Enables or disables an existing override for this session.",
                .input_schema = kind_schema(Schema())
                                    .String("name", "Override name.")
                                    .Boolean("enabled", "Default true.")
                                    .Boolean("advance", "When paused, step one frame.")
                                    .Required({"kind", "name"})
                                    .Build(),
                .idempotent = true,
                .handler = EnableOverride});

  registry.Add(
      {.name = "save_overrides",
       .title = "Save overrides",
       .description = "Writes this session's override edits to User/GameSettingsVR/<GameID>.ini "
                      "(the same file the Qt dialogs and the VR menu edit). Ask the user to "
                      "confirm in the headset first.",
       .input_schema = Schema()
                           .StringArray("kinds", "Default all.",
                                        {"all", "shader", "elements", "texture", "hideobject"})
                           .Build(),
       .destructive = true,
       .handler = SaveOverrides});

  registry.Add(
      {.name = "discard_overrides",
       .title = "Discard override edits",
       .description = "Drops this session's unsaved override edits and goes back to the "
                      "game's saved overrides.",
       .input_schema = Schema()
                           .StringArray("kinds", "Default all.",
                                        {"all", "shader", "elements", "texture", "hideobject"})
                           .Boolean("advance", "When paused, step one frame.")
                           .Build(),
       .idempotent = true,
       .handler = DiscardOverrides});
}
}  // namespace AgentServer
