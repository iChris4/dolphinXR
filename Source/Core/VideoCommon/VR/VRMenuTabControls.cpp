// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include "Common/Config/Config.h"
#include "Common/FileSearch.h"
#include "Common/IniFile.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Timer.h"
#include "Common/VR/OpenXRInputState.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/System.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/ControllerInterface/MappingCommon.h"
#include "InputCommon/InputConfig.h"

#include "VideoCommon/VR/OpenXRBindingWizardModel.h"
#include "VideoCommon/VR/OpenXRManager.h"
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

// Bindings are detected on the VR controllers only, like the stopped-emulation mapper.
constexpr char OPENXR_DEVICE[] = "OpenXR/0/OpenXR Controller";
constexpr float INPUT_NEUTRAL_THRESHOLD = 0.2f;
constexpr u64 NEUTRAL_DEBOUNCE_US = 150'000;
constexpr u64 RELEASE_TIMEOUT_US = 10'000'000;

struct Profile
{
  std::string name;
  std::string path;
};

struct Binding
{
  std::string group;
  std::string name;
  ControlReference* reference = nullptr;
  std::string expression;
};

// The bindable inputs of one emulated controller (port 1).
struct ControllerMapping
{
  InputConfig* config = nullptr;
  std::string default_device;
  std::vector<Binding> bindings;
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
  ControllerMapping wiimote_mapping;
  ControllerMapping gcpad_mapping;
};

bool InputsAreNeutral(const Common::VR::OpenXRInputSnapshot& snapshot)
{
  for (const auto& controller : snapshot.controllers)
  {
    if (controller.primary_button || controller.secondary_button || controller.menu_button ||
        controller.trigger_button || controller.squeeze_button || controller.thumbstick_button ||
        controller.trigger_value > INPUT_NEUTRAL_THRESHOLD ||
        controller.squeeze_value > INPUT_NEUTRAL_THRESHOLD ||
        std::abs(controller.thumbstick_x) > INPUT_NEUTRAL_THRESHOLD ||
        std::abs(controller.thumbstick_y) > INPUT_NEUTRAL_THRESHOLD)
    {
      return false;
    }
  }
  return true;
}

void AddBindings(ControllerEmu::ControlGroupContainer& container, std::string_view prefix,
                 std::vector<Binding>* bindings)
{
  for (const auto& group : container.groups)
  {
    // Motion and pointer groups are driven by the VR controllers' poses, not by bindings.
    if (!IsOpenXRMapperMappableGroup(group->type))
      continue;
    std::string group_name = Common::GetStringT(group->ui_name.c_str());
    if (!prefix.empty())
      group_name = fmt::format("{} - {}", prefix, group_name);
    for (const auto& control : group->controls)
    {
      if (!control->control_ref->IsInput())
        continue;
      bindings->push_back({group_name, Common::GetStringT(control->ui_name.c_str()),
                           control->control_ref.get(), control->control_ref->GetExpression()});
    }
  }
}

// Host thread. With `with_extension`, adds the selected Wii Remote extension's controls.
ControllerMapping ReadMapping(InputConfig* config, bool with_extension)
{
  ControllerMapping mapping;
  mapping.config = config;
  ControllerEmu::EmulatedController* controller = config->GetController(0);
  const auto lock = ControllerEmu::EmulatedController::GetStateLock();
  mapping.default_device = controller->GetDefaultDevice().ToString();
  AddBindings(*controller, {}, &mapping.bindings);
  if (!with_extension)
    return mapping;
  for (const auto& group : controller->groups)
  {
    if (group->type != ControllerEmu::GroupType::Attachments)
      continue;
    const auto* attachments = static_cast<ControllerEmu::Attachments*>(group.get());
    const auto& list = attachments->GetAttachmentList();
    const u32 selected = attachments->GetSelectedAttachment();
    if (selected < list.size())
      AddBindings(*list[selected], Common::GetStringT(list[selected]->GetDisplayName().c_str()),
                  &mapping.bindings);
    break;
  }
  return mapping;
}

std::string DisplayExpression(std::string_view expression)
{
  std::string display;
  for (const char c : expression)
  {
    if (c != '`')
      display += c;
  }
  return display;
}

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

  void OnClose(VRMenuContext& context) override { EndCapture(); }

  void Draw(VRMenuContext& context) override
  {
    UpdateCapture(context);

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

  enum class CapturePhase
  {
    Idle,
    // The click that started the capture (and anything else) must be released first.
    WaitingForNeutral,
    Detecting,
    // The bound input must be released before the menu takes button input again.
    WaitingForRelease,
  };

  void StartCapture(const ControllerMapping& mapping, const Binding& binding)
  {
    if (!g_openxr)
      return;
    m_capture_phase = CapturePhase::WaitingForNeutral;
    m_capture_config = mapping.config;
    m_capture_reference = binding.reference;
    m_capture_label = binding.name;
    m_capture_default_device = mapping.default_device;
    m_capture_phase_start_us = Common::Timer::NowUs();
    m_capture_neutral_since_us = 0;
    // Suspends the menu's buttons, opens the OpenXR device to the detector and gates the game.
    g_openxr->SetMenuBindingCapture(true);
  }

  void EndCapture()
  {
    m_detector.reset();
    m_capture_phase = CapturePhase::Idle;
    m_capture_reference = nullptr;
    if (g_openxr)
      g_openxr->SetMenuBindingCapture(false);
  }

  void UpdateCapture(VRMenuContext& context)
  {
    if (m_capture_phase == CapturePhase::Idle)
      return;

    const u64 now_us = Common::Timer::NowUs();
    const bool neutral = InputsAreNeutral(Common::VR::OpenXRInputState::GetSnapshot());
    switch (m_capture_phase)
    {
    case CapturePhase::WaitingForNeutral:
      if (!neutral)
      {
        m_capture_neutral_since_us = 0;
        if (now_us - m_capture_phase_start_us > RELEASE_TIMEOUT_US)
        {
          context.actions.SetStatus("Binding cancelled: release all buttons to start");
          EndCapture();
        }
        return;
      }
      if (m_capture_neutral_since_us == 0)
        m_capture_neutral_since_us = now_us;
      if (now_us - m_capture_neutral_since_us < NEUTRAL_DEBOUNCE_US)
        return;

      m_detector = std::make_unique<ciface::Core::InputDetector>();
      {
        const std::array<std::string, 1> devices{OPENXR_DEVICE};
        m_detector->Start(g_controller_interface, devices);
      }
      if (m_detector->IsComplete())
      {
        context.actions.SetStatus("The OpenXR controller device is unavailable");
        EndCapture();
        return;
      }
      m_capture_phase = CapturePhase::Detecting;
      m_capture_phase_start_us = now_us;
      return;

    case CapturePhase::Detecting:
    {
      using namespace std::chrono_literals;
      // Emulation does this too while it runs; keep the device current while paused.
      g_controller_interface.UpdateInput();
      m_detector->Update(5s, 350ms, 8s);
      if (!m_detector->IsComplete())
        return;

      auto detections = m_detector->TakeResults();
      m_detector.reset();
      ciface::MappingCommon::RemoveSpuriousTriggerCombinations(&detections);
      if (detections.empty())
      {
        context.actions.SetStatus(fmt::format("{}: no input detected", m_capture_label));
      }
      else
      {
        ciface::Core::DeviceQualifier default_device;
        default_device.FromString(m_capture_default_device);
        ApplyBinding(context, m_capture_config, m_capture_reference, m_capture_label,
                     ciface::MappingCommon::BuildExpression(detections, default_device,
                                                            ciface::MappingCommon::Quote::On));
      }
      m_capture_phase = CapturePhase::WaitingForRelease;
      m_capture_phase_start_us = now_us;
      return;
    }

    case CapturePhase::WaitingForRelease:
      if (neutral || now_us - m_capture_phase_start_us > RELEASE_TIMEOUT_US)
        EndCapture();
      return;

    case CapturePhase::Idle:
      return;
    }
  }

  // Changes a binding on the host thread (the CPU thread reads bindings under the state lock)
  // and saves the controller configuration.
  void ApplyBinding(VRMenuContext& context, InputConfig* config, ControlReference* reference,
                    std::string label, std::string expression)
  {
    context.actions.Run({}, [config, reference, label = std::move(label),
                             expression = std::move(expression)](Core::System&) {
      ControllerEmu::EmulatedController* controller = config->GetController(0);
      {
        const auto lock = ControllerEmu::EmulatedController::GetStateLock();
        reference->SetExpression(expression);
        controller->UpdateReferences(g_controller_interface);
      }
      config->SaveConfig();
      return expression.empty() ?
                 fmt::format("{} cleared", label) :
                 fmt::format("{} bound to {}", label, DisplayExpression(expression));
    });
    Refresh(context);
  }

  void DrawMapping(VRMenuContext& context, const char* id, const ControllerMapping& mapping)
  {
    ImGui::PushID(id);
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Button mapping: select a binding, release every button, then press the "
                       "VR controller input to use for it.");
    ImGui::PopStyleColor();

    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float clear_width = ImGui::GetFontSize() * 4.0f;
    const bool capturing = m_capture_phase != CapturePhase::Idle;
    std::string_view group;
    for (size_t i = 0; i < mapping.bindings.size(); ++i)
    {
      const Binding& binding = mapping.bindings[i];
      if (binding.group != group)
      {
        group = binding.group;
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.45f, 0.82f, 1.0f, 1.0f), "%s", binding.group.c_str());
      }

      ImGui::PushID(static_cast<int>(i));
      MenuWidgets::BeginRow(binding.name.c_str(), nullptr, Config::LayerType::Base);
      const bool capturing_this = capturing && m_capture_reference == binding.reference;
      std::string label;
      if (capturing_this)
      {
        switch (m_capture_phase)
        {
        case CapturePhase::WaitingForNeutral:
          label = "Release all buttons...";
          break;
        case CapturePhase::Detecting:
          label = "Press an input now...";
          break;
        default:
          label = "Release the input...";
          break;
        }
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.48f, 0.68f, 1.0f));
      }
      else
      {
        label = binding.expression.empty() ? "(none)" : DisplayExpression(binding.expression);
      }

      ImGui::BeginDisabled(capturing && !capturing_this);
      if (ImGui::Button(fmt::format("{}###bind", label).c_str(),
                        ImVec2(MenuWidgets::ControlWidth() - clear_width - spacing, 0.0f)) &&
          !capturing)
      {
        StartCapture(mapping, binding);
      }
      if (capturing_this)
        ImGui::PopStyleColor();
      ImGui::SameLine();
      if (ImGui::Button("Clear", ImVec2(clear_width, 0.0f)) && !capturing &&
          !binding.expression.empty())
      {
        ApplyBinding(context, mapping.config, binding.reference, binding.name, std::string());
      }
      ImGui::EndDisabled();
      ImGui::PopID();
    }
    ImGui::PopID();
  }

  void Refresh(VRMenuContext& context)
  {
    context.actions.RunInBackground([shared = m_info](Core::System& system) {
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
      if (info.is_wii)
        info.wiimote_mapping = ReadMapping(Wiimote::GetConfig(), true);
      info.gcpad_mapping = ReadMapping(Pad::GetConfig(), false);

      std::lock_guard lock(shared->mutex);
      shared->info = std::move(info);
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

    DrawMapping(context, "##wiimote_mapping", info.wiimote_mapping);
  }

  void DrawGameCube(VRMenuContext& context, const ControlsInfo& info)
  {
    MenuWidgets::SectionHeader("GameCube Controller 1");
    DrawProfileRow(context, "##gcpad_profile", info.gcpad_profiles, &m_gcpad_profile,
                   Pad::GetConfig());
    DrawMapping(context, "##gcpad_mapping", info.gcpad_mapping);
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

  // Binding capture (video thread).
  CapturePhase m_capture_phase = CapturePhase::Idle;
  InputConfig* m_capture_config = nullptr;
  ControlReference* m_capture_reference = nullptr;
  std::string m_capture_label;
  std::string m_capture_default_device;
  u64 m_capture_phase_start_us = 0;
  u64 m_capture_neutral_since_us = 0;
  std::unique_ptr<ciface::Core::InputDetector> m_detector;
};
}  // namespace

std::unique_ptr<VRMenuTab> CreateControlsTab()
{
  return std::make_unique<ControlsTab>();
}
}  // namespace VR
