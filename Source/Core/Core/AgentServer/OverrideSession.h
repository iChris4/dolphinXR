// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AgentServer
{
// The four kinds of VR overrides of a GameSettingsVR/<GameID>.ini profile.
enum class OverrideKind
{
  Shader,
  Elements,
  Texture,
  HideObject,
};

constexpr std::array<OverrideKind, 4> ALL_OVERRIDE_KINDS = {
    OverrideKind::Shader, OverrideKind::Elements, OverrideKind::Texture, OverrideKind::HideObject};

std::optional<OverrideKind> ParseOverrideKind(std::string_view name);
std::string_view GetOverrideKindName(OverrideKind kind);
// The INI sections: "ShaderOverride" and "ShaderOverride_Enable", etc.
std::string_view GetOverrideSectionName(OverrideKind kind);
std::string_view GetOverrideEnableSectionName(OverrideKind kind);

// The raw text of one entry ("$Name" and its key lines) in an INI file's section.
std::optional<std::string> ExtractOverrideEntryText(const std::string& file_contents,
                                                    std::string_view section,
                                                    std::string_view name);

struct OverrideRow
{
  std::string name;
  // "sys" (ships with DolphinXR), "user" or "session" (tried by the agent, not saved).
  std::string origin;
  bool enabled = false;
  bool removed = false;
  std::string summary;
};

// The overrides the agent tries before anything is saved. Edits (added, replaced, removed or
// toggled entries, by name) are kept here and merged over the game's Sys + User lists; the merged
// list is applied live to the override managers on the video thread. Save writes it to
// User/GameSettingsVR/<GameID>.ini with the managers' own writers; Discard reapplies the files.
//
// Server thread, except Forget() (emulation stopped, any thread). A save from the Qt override
// dialogs or the VR menu reloads the managers from the files and drops unsaved edits from what is
// drawn (they stay listed here until saved or discarded).
class OverrideSession final
{
public:
  // Parses one entry ("$Name" then key=value lines, the GameSettingsVR format) with the
  // manager's own parser, records it and applies it. Returns how Dolphin understood it.
  std::string SetOverride(OverrideKind kind, std::string_view ini_text, bool enabled);
  void RemoveOverride(OverrideKind kind, const std::string& name);
  void EnableOverride(OverrideKind kind, const std::string& name, bool enabled);

  std::vector<OverrideRow> List(OverrideKind kind) const;
  std::string GetEntryText(OverrideKind kind, const std::string& name) const;
  size_t CountEdits(OverrideKind kind) const;

  // Returns the file written.
  std::string Save(OverrideKind kind);
  void Discard(OverrideKind kind);
  // Drops all edits without touching the managers.
  void Forget();

private:
  struct Edit
  {
    // nullopt: removed.
    std::optional<std::string> text;
    bool enabled = true;
  };
  using EditMap = std::map<std::string, Edit>;

  // Applies the merged list of a kind on the video thread.
  void Apply(OverrideKind kind) const;

  mutable std::mutex m_mutex;
  std::array<EditMap, 4> m_edits;
};
}  // namespace AgentServer
