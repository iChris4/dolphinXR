// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include "Common/Config/Config.h"
#include "Common/FileSearch.h"
#include "Common/IniFile.h"
#include "Common/StringUtil.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/System.h"
#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/InputConfig.h"

#include "VideoCommon/VR/VRMenuActions.h"
#include "VideoCommon/VR/VRMenuTabs.h"
#include "VideoCommon/VR/VRMenuWidgets.h"

namespace VR
{
namespace
{
// Same key the OpenXR Wii Remote override reads (Core/HW/Wiimote.cpp).
const Config::Info<bool> LEFT_HANDED{{Config::System::Main, "Android", "QuestLeftHanded"}, false};

constexpr std::array<std::pair<WiimoteSource, const char*>, 4> WIIMOTE_SOURCES{{
    {WiimoteSource::OpenXR, "VR Controllers (motion + pointer)"},
    {WiimoteSource::Emulated, "Emulated (button mapping only)"},
    {WiimoteSource::Real, "Real Wii Remote"},
    {WiimoteSource::None, "None"},
}};

struct Profile
{
  std::string name;
  std::string path;
};

// Controller state read on the host thread, where it is safe to touch.
struct ControlsInfo
{
  bool valid = false;
  bool is_wii = false;
  WiimoteSource wiimote_source = WiimoteSource::None;
  std::vector<std::string> extensions;
  u32 extension = 0;
  std::vector<Profile> wiimote_profiles;
  std::vector<Profile> gcpad_profiles;
};

std::vector<Profile> FindProfiles(const InputConfig& config)
{
  const std::string user_dir = config.GetUserProfileDirectoryPath();
  const std::string sys_dir = config.GetSysProfileDirectoryPath();
  const std::array<std::string_view, 2> directories{user_dir, sys_dir};
  std::vector<Profile> profiles;
  for (const std::string& path : Common::DoFileSearch(directories, ".ini", true))
  {
    std::string name;
    SplitPath(path, nullptr, &name, nullptr);
    // A user profile shadows the system profile of the same name.
    if (std::ranges::none_of(profiles, [&](const Profile& p) { return p.name == name; }))
      profiles.push_back({std::move(name), path});
  }
  return profiles;
}

ControllerEmu::Attachments* GetWiimoteAttachments()
{
  return static_cast<ControllerEmu::Attachments*>(
      Wiimote::GetWiimoteGroup(0, WiimoteEmu::WiimoteGroup::Attachments));
}

// Host thread.
std::string LoadProfile(InputConfig* config, const Profile& profile)
{
  Common::IniFile ini;
  if (!ini.Load(profile.path))
    return fmt::format("Could not read profile \"{}\"", profile.name);

  ControllerEmu::EmulatedController* controller = config->GetController(0);
  controller->LoadConfig(ini.GetOrCreateSection("Profile"));
  controller->UpdateReferences(g_controller_interface);
  config->GenerateControllerTextures(ini);
  config->SaveConfig();
  return fmt::format("Loaded profile \"{}\"", profile.name);
}

class ControlsTab final : public VRMenuTab
{
public:
  const char* GetName() const override { return "Controls"; }

  void OnOpen(VRMenuContext& context) override { Refresh(context); }

  void Draw(VRMenuContext& context) override
  {
    ControlsInfo info;
    {
      std::lock_guard lock(m_info->mutex);
      info = m_info->info;
    }
    if (!info.valid)
    {
      ImGui::TextDisabled("Reading controller settings...");
      return;
    }

    if (info.is_wii)
      DrawWiiRemote(context, info);
    DrawGameCube(context, info);
  }

private:
  struct SharedInfo
  {
    std::mutex mutex;
    ControlsInfo info;
  };

  void Refresh(VRMenuContext& context)
  {
    context.actions.Run({}, [shared = m_info](Core::System& system) {
      ControlsInfo info;
      info.valid = true;
      info.is_wii = system.IsWii();
      info.wiimote_source = Config::Get(Config::GetInfoForWiimoteSource(0));
      if (const ControllerEmu::Attachments* attachments = GetWiimoteAttachments())
      {
        for (const auto& attachment : attachments->GetAttachmentList())
          info.extensions.push_back(attachment->GetDisplayName());
        info.extension = attachments->GetSelectedAttachment();
      }
      info.wiimote_profiles = FindProfiles(*Wiimote::GetConfig());
      info.gcpad_profiles = FindProfiles(*Pad::GetConfig());

      std::lock_guard lock(shared->mutex);
      shared->info = std::move(info);
      return std::string();
    });
  }

  void DrawWiiRemote(VRMenuContext& context, const ControlsInfo& info)
  {
    MenuWidgets::SectionHeader("Wii Remote 1");

    MenuWidgets::BeginRow("Source", "Where the emulated Wii Remote's input comes from.",
                          Config::GetActiveLayerForConfig(Config::GetInfoForWiimoteSource(0)));
    const char* source_name = "?";
    for (const auto& [source, name] : WIIMOTE_SOURCES)
    {
      if (source == info.wiimote_source)
        source_name = name;
    }
    ImGui::SetNextItemWidth(MenuWidgets::ControlWidth());
    if (ImGui::BeginCombo("##source", source_name, ImGuiComboFlags_HeightLargest))
    {
      for (const auto& [source, name] : WIIMOTE_SOURCES)
      {
#ifdef ANDROID
        if (source == WiimoteSource::Real)
          continue;
#endif
        if (ImGui::Selectable(name, source == info.wiimote_source) &&
            source != info.wiimote_source)
        {
          // Changing the source reconnects the remote, which synchronizes with the CPU thread.
          context.actions.Run({}, [source, name](Core::System&) {
            Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(0), source);
            return fmt::format("Wii Remote 1: {}", name);
          });
          Refresh(context);
        }
      }
      ImGui::EndCombo();
    }

    if (!info.extensions.empty())
    {
      MenuWidgets::BeginRow("Extension", nullptr, Config::LayerType::Base);
      const char* extension_name =
          info.extension < info.extensions.size() ? info.extensions[info.extension].c_str() : "?";
      ImGui::SetNextItemWidth(MenuWidgets::ControlWidth());
      if (ImGui::BeginCombo("##extension", extension_name, ImGuiComboFlags_HeightLargest))
      {
        for (u32 i = 0; i < info.extensions.size(); ++i)
        {
          if (ImGui::Selectable(info.extensions[i].c_str(), i == info.extension) &&
              i != info.extension)
          {
            context.actions.Run({}, [i, name = info.extensions[i]](Core::System&) {
              {
                const auto lock = ControllerEmu::EmulatedController::GetStateLock();
                if (ControllerEmu::Attachments* attachments = GetWiimoteAttachments())
                  attachments->SetSelectedAttachment(i);
              }
              Wiimote::GetConfig()->SaveConfig();
              return fmt::format("Wii Remote 1 extension: {}", name);
            });
            Refresh(context);
          }
        }
        ImGui::EndCombo();
      }
    }

    DrawProfileRow(context, "##wiimote_profile", info.wiimote_profiles, &m_wiimote_profile,
                   Wiimote::GetConfig());

    MenuWidgets::Checkbox("Left-Handed",
                          "Point with the left controller; the Nunchuk follows the right one.",
                          LEFT_HANDED, nullptr);
  }

  void DrawGameCube(VRMenuContext& context, const ControlsInfo& info)
  {
    MenuWidgets::SectionHeader("GameCube Controller 1");
    DrawProfileRow(context, "##gcpad_profile", info.gcpad_profiles, &m_gcpad_profile,
                   Pad::GetConfig());
  }

  void DrawProfileRow(VRMenuContext& context, const char* id,
                      const std::vector<Profile>& profiles, std::string* selected,
                      InputConfig* config)
  {
    MenuWidgets::BeginRow("Profile", "Load a saved controller mapping.",
                          Config::LayerType::Base);
    if (profiles.empty())
    {
      ImGui::AlignTextToFramePadding();
      ImGui::TextDisabled("No profiles found");
      return;
    }

    const auto it = std::ranges::find(profiles, *selected, &Profile::name);
    const Profile* profile = it != profiles.end() ? &*it : &profiles.front();

    ImGui::PushID(id);
    const float load_width = ImGui::GetFontSize() * 4.0f;
    ImGui::SetNextItemWidth(MenuWidgets::ControlWidth() - load_width -
                            ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::BeginCombo("##profile", profile->name.c_str(), ImGuiComboFlags_HeightLargest))
    {
      for (const Profile& candidate : profiles)
      {
        if (ImGui::Selectable(candidate.name.c_str(), &candidate == profile))
          *selected = candidate.name;
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Load", ImVec2(load_width, 0.0f)))
    {
      context.actions.Run(fmt::format("Loading profile \"{}\"...", profile->name),
                          [config, chosen = *profile](Core::System&) {
                            return LoadProfile(config, chosen);
                          });
      Refresh(context);
    }
    ImGui::PopID();
  }

  std::shared_ptr<SharedInfo> m_info = std::make_shared<SharedInfo>();
  std::string m_wiimote_profile;
  std::string m_gcpad_profile;
};
}  // namespace

std::unique_ptr<VRMenuTab> CreateControlsTab()
{
  return std::make_unique<ControlsTab>();
}
}  // namespace VR
