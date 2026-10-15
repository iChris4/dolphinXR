// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/OpenXRUtilitySession.h"

#ifdef ENABLE_VR

// VulkanLoader.h must come first -- it defines the platform Vulkan types and VK_NO_PROTOTYPES.
#include "VideoBackends/Vulkan/VulkanLoader.h"

#define XR_USE_GRAPHICS_API_VULKAN
#if defined(ANDROID)
#include <jni.h>
#define XR_USE_PLATFORM_ANDROID
#endif

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <optional>
#include <ranges>
#include <sstream>
#include <thread>
#include <utility>

#include <fmt/format.h>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <openxr/openxr_platform.h>

// On Linux, VulkanLoader.h defines VK_USE_PLATFORM_XLIB_KHR, so <vulkan/vulkan.h> pulls in
// <X11/Xlib.h>, which #defines None/Bool/Status/etc. as macros. Undo them before including the
// Dolphin headers below that use those names as identifiers (e.g. WiimoteSource::None,
// SettingType::Bool), matching the #undef pattern already used in BPMemory.h/XFMemory.h.
#ifdef None
#undef None
#endif
#ifdef Bool
#undef Bool
#endif
#ifdef Status
#undef Status
#endif
#ifdef Success
#undef Success
#endif
#ifdef Always
#undef Always
#endif

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/VR/OpenXRInputState.h"
#include "Core/Core.h"
#include "Core/HotkeyManager.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/Wiimote.h"
#include "Core/System.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/MappingCommon.h"
#include "InputCommon/InputConfig.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/VR/OpenXRBindingWizardModel.h"
#include "VideoCommon/VR/OpenXRManager.h"
#include "VideoCommon/VR/VRPanelMath.h"
#include "VideoCommon/VideoConfig.h"

namespace VR
{
namespace
{
using namespace std::chrono_literals;

constexpr char OPENXR_DEVICE[] = "OpenXR/0/OpenXR Controller";
constexpr uint32_t UI_WIDTH = 1600;
constexpr uint32_t UI_HEIGHT = 1100;
constexpr float UI_HEIGHT_METERS = 1.2f;
constexpr float UI_DISTANCE_METERS = 1.35f;
constexpr float UI_VERTICAL_OFFSET_METERS = 0.12f;
constexpr float INPUT_NEUTRAL_THRESHOLD = 0.20f;

std::atomic<bool> s_utility_session_active{false};
thread_local VkResult s_imgui_vulkan_error = VK_SUCCESS;

void RecordImGuiVulkanResult(VkResult result)
{
  if (result != VK_SUCCESS)
  {
    s_imgui_vulkan_error = result;
    ERROR_LOG_FMT(OPENXR, "OpenXR controller mapper: ImGui Vulkan operation failed ({})",
                  static_cast<int>(result));
  }
}

struct VulkanRequirements
{
  std::vector<std::string> instance_extensions;
  std::vector<std::string> device_extensions;
  uint32_t min_api_version = 0;
  uint32_t max_api_version = 0;
};

std::vector<std::string> ParseExtensionList(const std::string& list)
{
  std::vector<std::string> extensions;
  std::istringstream stream(list);
  std::string extension;
  while (stream >> extension)
    extensions.emplace_back(std::move(extension));
  return extensions;
}

template <typename T>
T GetXRFunction(XrInstance instance, const char* name)
{
  PFN_xrVoidFunction function = nullptr;
  if (XR_FAILED(xrGetInstanceProcAddr(instance, name, &function)))
    return nullptr;
  return reinterpret_cast<T>(function);
}

bool QueryVulkanRequirements(OpenXRManager& manager, VulkanRequirements* requirements)
{
  const XrInstance instance = manager.GetInstance();
  const XrSystemId system = manager.GetSystemId();

  const auto get_requirements = GetXRFunction<PFN_xrGetVulkanGraphicsRequirementsKHR>(
      instance, "xrGetVulkanGraphicsRequirementsKHR");
  const auto get_instance_extensions = GetXRFunction<PFN_xrGetVulkanInstanceExtensionsKHR>(
      instance, "xrGetVulkanInstanceExtensionsKHR");
  const auto get_device_extensions =
      GetXRFunction<PFN_xrGetVulkanDeviceExtensionsKHR>(instance, "xrGetVulkanDeviceExtensionsKHR");
  if (!get_requirements || !get_instance_extensions || !get_device_extensions)
    return false;

  XrGraphicsRequirementsVulkanKHR graphics_requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
  if (XR_FAILED(get_requirements(instance, system, &graphics_requirements)))
    return false;

  requirements->min_api_version =
      VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(graphics_requirements.minApiVersionSupported),
                          XR_VERSION_MINOR(graphics_requirements.minApiVersionSupported),
                          XR_VERSION_PATCH(graphics_requirements.minApiVersionSupported));
  const uint32_t reported_max =
      VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(graphics_requirements.maxApiVersionSupported),
                          XR_VERSION_MINOR(graphics_requirements.maxApiVersionSupported),
                          XR_VERSION_PATCH(graphics_requirements.maxApiVersionSupported));
  // XR_KHR_vulkan_enable revision 1 runtimes commonly report 1.0.0 as an unspecified ceiling.
  requirements->max_api_version = reported_max > VK_API_VERSION_1_0 ? reported_max : 0;

  const auto query_list = [instance, system](auto function, std::vector<std::string>* output) {
    uint32_t length = 0;
    if (XR_FAILED(function(instance, system, 0, &length, nullptr)))
      return false;
    if (length == 0)
      return true;
    std::string text(length, '\0');
    if (XR_FAILED(function(instance, system, length, &length, text.data())))
      return false;
    *output = ParseExtensionList(text);
    return true;
  };

  if (!query_list(get_instance_extensions, &requirements->instance_extensions) ||
      !query_list(get_device_extensions, &requirements->device_extensions))
  {
    return false;
  }

  // The bundled ImGui Vulkan renderer loads its optional ImGui_ImplVulkanH desktop swapchain
  // helpers together with the renderer entry points. The mapper does not call those helpers, but
  // some Vulkan loaders return null for them unless these generic extensions are enabled.
  // Neither extension creates a window surface, so the utility remains headless.
  if (std::ranges::find(requirements->instance_extensions, VK_KHR_SURFACE_EXTENSION_NAME) ==
      requirements->instance_extensions.end())
  {
    requirements->instance_extensions.emplace_back(VK_KHR_SURFACE_EXTENSION_NAME);
  }
  if (std::ranges::find(requirements->device_extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME) ==
      requirements->device_extensions.end())
  {
    requirements->device_extensions.emplace_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  }
  return true;
}

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

struct MappableControl
{
  std::string group_name;
  std::string control_name;
  ControlReference* reference = nullptr;
};

struct MappableGroup
{
  std::string name;
  size_t first_control = 0;
  size_t control_count = 0;
};

void AddMappableControls(ControllerEmu::ControlGroupContainer& container,
                         std::vector<MappableControl>* controls, std::string_view prefix = {},
                         const ControllerEmu::ControlGroup* excluded_group = nullptr,
                         const ControllerEmu::ControlGroup* renamed_group = nullptr,
                         const char* renamed_group_name = nullptr)
{
  for (const auto& group : container.groups)
  {
    if (group.get() == excluded_group)
      continue;
    if (!IsOpenXRMapperMappableGroup(group->type))
      continue;

    std::string group_name = Common::GetStringT(group->ui_name.c_str());
    if (group.get() == renamed_group && renamed_group_name)
      group_name = renamed_group_name;
    if (!prefix.empty())
      group_name = fmt::format("{} - {}", prefix, group_name);
    for (const auto& control : group->controls)
    {
      if (!control->control_ref->IsInput())
        continue;
      controls->push_back(
          {group_name, Common::GetStringT(control->ui_name.c_str()), control->control_ref.get()});
    }
  }
}

class BindingWizard
{
public:
  using Result = OpenXRBindingWizardModel::Result;

  explicit BindingWizard(OpenXRUtilitySessionTarget target) : m_target(target)
  {
    InputConfig* config = nullptr;
    switch (target.type)
    {
    case OpenXRUtilitySessionTargetType::WiiRemote:
      config = Wiimote::GetConfig();
      break;
    case OpenXRUtilitySessionTargetType::GameCubeController:
      config = Pad::GetConfig();
      break;
    case OpenXRUtilitySessionTargetType::Hotkeys:
      config = HotkeyManagerEmu::GetConfig();
      break;
    }
    m_controller = config && target.port >= 0 && target.port < config->GetControllerCount() ?
                       config->GetController(target.port) :
                       nullptr;
    if (!m_controller)
      return;

    const ControllerEmu::ControlGroup* android_hotkey_group =
        target.type == OpenXRUtilitySessionTargetType::Hotkeys ?
            HotkeyManagerEmu::GetHotkeyGroup(HKGP_ANDROID) :
            nullptr;
#if defined(ANDROID)
    AddMappableControls(*m_controller, &m_controls, {}, nullptr, android_hotkey_group,
                        "Android / Quest");
#else
    AddMappableControls(*m_controller, &m_controls, {}, android_hotkey_group);
#endif
    if (target.type == OpenXRUtilitySessionTargetType::WiiRemote)
    {
      for (const auto& group : m_controller->groups)
      {
        if (group->type != ControllerEmu::GroupType::Attachments)
          continue;
        auto* attachments = static_cast<ControllerEmu::Attachments*>(group.get());
        const auto selected = attachments->GetSelectedAttachment();
        const auto& list = attachments->GetAttachmentList();
        if (selected < list.size())
        {
          AddMappableControls(*list[selected], &m_controls,
                              Common::GetStringT(list[selected]->GetDisplayName().c_str()));
        }
        break;
      }
    }

    BuildGroupColumns();

    std::vector<std::string> expressions;
    expressions.reserve(m_controls.size());
    for (const auto& control : m_controls)
      expressions.emplace_back(control.reference->GetExpression());
    m_model = std::make_unique<OpenXRBindingWizardModel>(std::move(expressions));
  }

  bool IsValid() const { return m_controller != nullptr && m_model && m_model->IsValid(); }
  Result GetResult() const { return m_model->GetResult(); }

  Result DrawAndUpdate()
  {
    g_controller_interface.UpdateInput();
    UpdateDetection();

    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize({static_cast<float>(UI_WIDTH), static_cast<float>(UI_HEIGHT)});
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("OpenXR Controller Mapper", nullptr, flags);

    ImGui::SetCursorPos({45.0f, 28.0f});
    if (m_target.type == OpenXRUtilitySessionTargetType::WiiRemote)
      ImGui::Text("OpenXR Wii Remote %d", m_target.port + 1);
    else if (m_target.type == OpenXRUtilitySessionTargetType::GameCubeController)
      ImGui::Text("OpenXR GameCube Standard Controller %d", m_target.port + 1);
    else
      ImGui::Text("OpenXR Hotkey Settings");
    ImGui::SetCursorPos({45.0f, 68.0f});
    ImGui::TextDisabled("Select any control to listen for a new OpenXR input");

    if (m_controls.empty())
    {
      ImGui::SetCursorPos({55.0f, 170.0f});
      ImGui::Text("No bindable controls were found.");
    }
    else
    {
      const size_t index = m_model->GetIndex();
      const auto& control = m_controls[index];
      ImGui::SetCursorPos({45.0f, 108.0f});
      if (m_detection_phase == DetectionPhase::WaitingForNeutral)
      {
        ImGui::TextColored({1.0f, 0.78f, 0.2f, 1.0f}, "(%s): release all inputs...",
                           control.control_name.c_str());
      }
      else if (m_detection_phase == DetectionPhase::Detecting)
      {
        ImGui::TextColored({0.35f, 0.85f, 1.0f, 1.0f}, "(%s): press the input to bind now...",
                           control.control_name.c_str());
      }
      else if (!m_status.empty())
        ImGui::Text("%s", m_status.c_str());
      else
        ImGui::TextDisabled("Current bindings are staged until Apply and Finish");

      const auto snapshot = Common::VR::OpenXRInputState::GetSnapshot();
      ImGui::SetCursorPos({45.0f, 145.0f});
      ImGui::SetWindowFontScale(0.75f);
      for (size_t hand = 0; hand < snapshot.controllers.size(); ++hand)
      {
        const auto& state = snapshot.controllers[hand];
        ImGui::SetCursorPosX(45.0f);
        ImGui::TextDisabled("%s: trigger %.2f  grip %.2f  stick %.2f, %.2f  %s",
                            hand == 0 ? "Left" : "Right", state.trigger_value, state.squeeze_value,
                            state.thumbstick_x, state.thumbstick_y,
                            state.connected ? "active" : "waiting");
      }
      ImGui::SetWindowFontScale(1.0f);

      ImGui::SetCursorPos({35.0f, 220.0f});
      ImGui::BeginChild("BindingDashboard", {1530.0f, 725.0f}, true);
      constexpr ImGuiTableFlags table_flags =
          ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV;
      if (ImGui::BeginTable("BindingColumns", static_cast<int>(m_group_columns.size()),
                            table_flags))
      {
        for (size_t column = 0; column < m_group_columns.size(); ++column)
        {
          ImGui::TableNextColumn();
          DrawBindingColumn(column);
        }
        ImGui::EndTable();
      }
      ImGui::EndChild();
    }

    const bool detecting = m_detection_phase != DetectionPhase::Idle || m_suppress_ui_clicks;
    ImGui::SetCursorPos({45.0f, 985.0f});
    ImGui::BeginDisabled(detecting);
    if (ImGui::Button("Cancel", {220.0f, 70.0f}))
      m_model->Cancel();
    ImGui::SameLine(0.0f, 25.0f);
    if (ImGui::Button("Clear Selected", {260.0f, 70.0f}))
    {
      const std::string name = m_controls[m_model->GetIndex()].control_name;
      m_model->Clear();
      m_status = fmt::format("({}) cleared (pending Apply)", name);
    }
    ImGui::SameLine(0.0f, 765.0f);
    if (ImGui::Button("Apply", {250.0f, 70.0f}))
      m_model->Finish();
    ImGui::EndDisabled();

    ImGui::End();
    m_suppress_ui_clicks = false;
    return m_model->GetResult();
  }

  OpenXRPendingBindings BuildPendingBindings() const
  {
    OpenXRPendingBindings pending;
    pending.target = m_target;
    pending.default_device = OPENXR_DEVICE;
    pending.bindings.reserve(m_controls.size());
    const auto& expressions = m_model->GetExpressions();
    for (size_t i = 0; i < m_controls.size(); ++i)
      pending.bindings.push_back({m_controls[i].reference, expressions[i]});
    return pending;
  }

private:
  enum class DetectionPhase
  {
    Idle,
    WaitingForNeutral,
    Detecting,
  };

  void BuildGroupColumns()
  {
    std::vector<MappableGroup> groups;
    for (size_t i = 0; i < m_controls.size();)
    {
      const size_t first = i;
      while (i < m_controls.size() && m_controls[i].group_name == m_controls[first].group_name)
        ++i;
      groups.push_back({m_controls[first].group_name, first, i - first});
    }

    std::array<size_t, 3> column_rows{};
    for (const auto& group : groups)
    {
      const size_t column = static_cast<size_t>(
          std::distance(column_rows.begin(), std::ranges::min_element(column_rows)));
      m_group_columns[column].push_back(group);
      column_rows[column] += group.control_count + 1;
    }
  }

  void BeginDetection(size_t index)
  {
    if (!m_model->Select(index))
      return;
    m_detector.reset();
    m_detection_phase = DetectionPhase::WaitingForNeutral;
    m_model->BeginBinding();
    m_status.clear();
  }

  void DrawBindingColumn(size_t column)
  {
    ImGui::SetWindowFontScale(0.68f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.0f, 4.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, {0.5f, 0.5f});
    const bool detecting = m_detection_phase != DetectionPhase::Idle || m_suppress_ui_clicks;
    ImGui::BeginDisabled(detecting);
    for (const auto& group : m_group_columns[column])
    {
      ImGui::TextColored({0.45f, 0.82f, 1.0f, 1.0f}, "%s", group.name.c_str());
      ImGui::Separator();
      for (size_t offset = 0; offset < group.control_count; ++offset)
      {
        const size_t index = group.first_control + offset;
        const auto& control = m_controls[index];
        const float row_y = ImGui::GetCursorPosY();
        ImGui::PushID(static_cast<int>(index));
        const bool selected = index == m_model->GetIndex();
        if (selected)
        {
          ImGui::PushStyleColor(ImGuiCol_Button, {0.18f, 0.48f, 0.68f, 1.0f});
          ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.25f, 0.62f, 0.82f, 1.0f});
        }
        if (ImGui::Button(control.control_name.c_str(), {220.0f, 34.0f}))
          BeginDetection(index);
        if (selected)
          ImGui::PopStyleColor(2);
        ImGui::SameLine(0.0f, 14.0f);
        const std::string& expression = m_model->GetExpressions()[index];
        if (expression.empty())
          ImGui::TextDisabled("Not bound");
        else
          ImGui::TextUnformatted(expression.c_str());
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), row_y + 38.0f));
        ImGui::PopID();
      }
      ImGui::Spacing();
    }
    ImGui::EndDisabled();
    ImGui::PopStyleVar(2);
    ImGui::SetWindowFontScale(1.0f);
  }

  void UpdateDetection()
  {
    if (m_detection_phase == DetectionPhase::WaitingForNeutral)
    {
      const auto now = std::chrono::steady_clock::now();
      if (!m_model->UpdateNeutralGate(InputsAreNeutral(Common::VR::OpenXRInputState::GetSnapshot()),
                                      now))
        return;

      m_detector = std::make_unique<ciface::Core::InputDetector>();
      const std::array<std::string, 1> devices{OPENXR_DEVICE};
      m_detector->Start(g_controller_interface, devices);
      if (m_detector->IsComplete())
      {
        m_detector.reset();
        m_detection_phase = DetectionPhase::Idle;
        m_status = "OpenXR controller device is unavailable";
      }
      else
      {
        m_detection_phase = DetectionPhase::Detecting;
      }
      return;
    }

    if (m_detection_phase != DetectionPhase::Detecting || !m_detector)
      return;

    m_detector->Update(5s, 350ms, 8s);
    if (!m_detector->IsComplete())
      return;

    auto detections = m_detector->TakeResults();
    ciface::MappingCommon::RemoveSpuriousTriggerCombinations(&detections);
    if (!detections.empty())
    {
      ciface::Core::DeviceQualifier default_device;
      default_device.FromString(OPENXR_DEVICE);
      const std::string expression = ciface::MappingCommon::BuildExpression(
          detections, default_device, ciface::MappingCommon::Quote::On);
      m_model->AcceptBinding(expression);
      m_status =
          fmt::format("({}) bound to {}", m_controls[m_model->GetIndex()].control_name, expression);
    }
    else
    {
      m_status = "No input detected";
    }
    m_detector.reset();
    m_detection_phase = DetectionPhase::Idle;
    m_suppress_ui_clicks = true;
  }

  OpenXRUtilitySessionTarget m_target;
  ControllerEmu::EmulatedController* m_controller = nullptr;
  std::vector<MappableControl> m_controls;
  std::array<std::vector<MappableGroup>, 3> m_group_columns;
  std::unique_ptr<OpenXRBindingWizardModel> m_model;
  DetectionPhase m_detection_phase = DetectionPhase::Idle;
  std::unique_ptr<ciface::Core::InputDetector> m_detector;
  bool m_suppress_ui_clicks = false;
  std::string m_status;
};

struct RenderImage
{
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool initialized = false;
};

VkFormat SelectSwapchainFormat(XrSession session)
{
  uint32_t count = 0;
  if (XR_FAILED(xrEnumerateSwapchainFormats(session, 0, &count, nullptr)) || count == 0)
    return VK_FORMAT_UNDEFINED;
  std::vector<int64_t> formats(count);
  if (XR_FAILED(xrEnumerateSwapchainFormats(session, count, &count, formats.data())))
    return VK_FORMAT_UNDEFINED;
  constexpr std::array preferred = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
                                    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
  for (VkFormat wanted : preferred)
  {
    if (std::ranges::find(formats, static_cast<int64_t>(wanted)) != formats.end())
      return wanted;
  }
  return VK_FORMAT_UNDEFINED;
}

}  // namespace

struct OpenXRUtilitySession::Impl
{
  explicit Impl(OpenXRUtilitySession& owner_) : owner(owner_) {}

  OpenXRUtilitySession& owner;
  std::thread thread;
  std::atomic<bool> stop_requested{false};
  OpenXRUtilitySessionTarget target;

  std::unique_ptr<OpenXRManager> manager;
  std::unique_ptr<Vulkan::VulkanContext> vulkan;
  bool vulkan_library_loaded = false;
  BackendInfo previous_backend_info;
  bool backend_info_saved = false;
  XrSwapchain swapchain = XR_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  std::vector<XrSwapchainImageVulkanKHR> xr_images;
  std::vector<RenderImage> render_images;
  ImGuiContext* imgui_context = nullptr;
  ImGuiContext* previous_imgui_context = nullptr;
  bool imgui_backend_initialized = false;
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  bool quad_pose_initialized = false;
  std::array<ImVec2, 2> pointer_positions{};
  std::array<bool, 2> pointer_visible{};
  std::array<bool, 2> pointer_pressed{};
  size_t active_pointer_hand = 0;
  bool previous_pointer_down = false;

  void Fail(OpenXRUtilitySessionFailure reason, std::string_view message)
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR controller mapper: {}", message);
    {
      std::lock_guard lock(owner.m_result_mutex);
      owner.m_failure_message = std::string(message);
    }
    owner.m_failure.store(reason, std::memory_order_release);
    owner.m_state.store(OpenXRUtilitySessionState::Failed, std::memory_order_release);
  }

  bool InitializeOpenXR()
  {
    manager = std::make_unique<OpenXRManager>();
    std::vector<const char*> extensions{XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
#if defined(ANDROID)
    extensions.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
#endif
    const auto controller_extensions = OpenXRManager::GetAvailableControllerExtensions();
    extensions.insert(extensions.end(), controller_extensions.begin(), controller_extensions.end());
    return manager->CreateInstance(extensions) && manager->InitializeSystem() &&
           manager->EnumerateViewConfigurations();
  }

  bool InitializeVulkan()
  {
    VulkanRequirements requirements;
    if (!QueryVulkanRequirements(*manager, &requirements))
      return false;
    if (!Vulkan::LoadVulkanLibrary())
      return false;
    vulkan_library_loaded = true;

    uint32_t api_version = 0;
    VkInstance instance = Vulkan::VulkanContext::CreateVulkanInstance(
        WindowSystemType::Headless, false, false, &api_version, requirements.instance_extensions,
        requirements.max_api_version);
    if (instance == VK_NULL_HANDLE)
      return false;
    if (!Vulkan::LoadVulkanInstanceFunctions(instance))
    {
      vkDestroyInstance(instance, nullptr);
      return false;
    }
    if (api_version < requirements.min_api_version)
    {
      vkDestroyInstance(instance, nullptr);
      return false;
    }

    const auto get_graphics_device = GetXRFunction<PFN_xrGetVulkanGraphicsDeviceKHR>(
        manager->GetInstance(), "xrGetVulkanGraphicsDeviceKHR");
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    if (!get_graphics_device ||
        XR_FAILED(get_graphics_device(manager->GetInstance(), manager->GetSystemId(), instance,
                                      &physical_device)) ||
        physical_device == VK_NULL_HANDLE)
    {
      vkDestroyInstance(instance, nullptr);
      return false;
    }

    previous_backend_info = g_backend_info;
    backend_info_saved = true;
    Vulkan::VulkanContext::PopulateBackendInfo(&g_backend_info);
    vulkan = Vulkan::VulkanContext::Create(instance, physical_device, VK_NULL_HANDLE, false, false,
                                           api_version, requirements.device_extensions);
    return vulkan != nullptr;
  }

  bool CreateXRSession()
  {
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = vulkan->GetVulkanInstance();
    binding.physicalDevice = vulkan->GetPhysicalDevice();
    binding.device = vulkan->GetDevice();
    binding.queueFamilyIndex = vulkan->GetGraphicsQueueFamilyIndex();
    binding.queueIndex = 0;

    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    info.systemId = manager->GetSystemId();
    XrSession session = XR_NULL_HANDLE;
    const XrResult create_result = xrCreateSession(manager->GetInstance(), &info, &session);
    if (XR_FAILED(create_result))
    {
      ERROR_LOG_FMT(OPENXR, "OpenXR controller mapper: xrCreateSession failed ({})",
                    static_cast<int>(create_result));
      return false;
    }
    manager->SetSession(session);
    if (!manager->CreateReferenceSpace())
    {
      ERROR_LOG_FMT(OPENXR, "OpenXR controller mapper: reference-space creation failed");
      return false;
    }
    return true;
  }

  bool CreateRenderPass()
  {
    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &attachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    const VkResult result = vkCreateRenderPass(vulkan->GetDevice(), &info, nullptr, &render_pass);
    if (result != VK_SUCCESS)
    {
      ERROR_LOG_FMT(OPENXR, "OpenXR controller mapper: vkCreateRenderPass failed ({})",
                    static_cast<int>(result));
      return false;
    }
    return true;
  }

  bool CreateSwapchainAndImages()
  {
    format = SelectSwapchainFormat(manager->GetSession());
    if (format == VK_FORMAT_UNDEFINED)
    {
      ERROR_LOG_FMT(OPENXR, "OpenXR controller mapper: no supported RGBA swapchain format");
      return false;
    }

    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    info.format = static_cast<int64_t>(format);
    info.sampleCount = 1;
    info.width = UI_WIDTH;
    info.height = UI_HEIGHT;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    const XrResult create_result = xrCreateSwapchain(manager->GetSession(), &info, &swapchain);
    if (XR_FAILED(create_result))
    {
      ERROR_LOG_FMT(OPENXR, "OpenXR controller mapper: xrCreateSwapchain failed ({})",
                    static_cast<int>(create_result));
      return false;
    }

    uint32_t count = 0;
    const XrResult count_result = xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr);
    if (XR_FAILED(count_result) || count == 0)
    {
      ERROR_LOG_FMT(OPENXR,
                    "OpenXR controller mapper: swapchain image count query failed ({}, count {})",
                    static_cast<int>(count_result), count);
      return false;
    }
    INFO_LOG_FMT(OPENXR, "OpenXR controller mapper: swapchain format {} with {} image(s)",
                 static_cast<int>(format), count);
    xr_images.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    const XrResult images_result = xrEnumerateSwapchainImages(
        swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(xr_images.data()));
    if (XR_FAILED(images_result))
    {
      ERROR_LOG_FMT(OPENXR, "OpenXR controller mapper: swapchain image enumeration failed ({})",
                    static_cast<int>(images_result));
      return false;
    }
    if (!CreateRenderPass())
      return false;

    render_images.resize(count);
    for (uint32_t i = 0; i < count; ++i)
    {
      auto& image = render_images[i];
      image.image = xr_images[i].image;

      VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view_info.image = image.image;
      view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view_info.format = format;
      view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      view_info.subresourceRange.levelCount = 1;
      view_info.subresourceRange.layerCount = 1;
      const VkResult view_result =
          vkCreateImageView(vulkan->GetDevice(), &view_info, nullptr, &image.view);
      if (view_result != VK_SUCCESS)
      {
        ERROR_LOG_FMT(OPENXR, "OpenXR controller mapper: vkCreateImageView failed ({})",
                      static_cast<int>(view_result));
        return false;
      }

      VkFramebufferCreateInfo framebuffer_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      framebuffer_info.renderPass = render_pass;
      framebuffer_info.attachmentCount = 1;
      framebuffer_info.pAttachments = &image.view;
      framebuffer_info.width = UI_WIDTH;
      framebuffer_info.height = UI_HEIGHT;
      framebuffer_info.layers = 1;
      const VkResult framebuffer_result =
          vkCreateFramebuffer(vulkan->GetDevice(), &framebuffer_info, nullptr, &image.framebuffer);
      if (framebuffer_result != VK_SUCCESS)
      {
        ERROR_LOG_FMT(OPENXR, "OpenXR controller mapper: vkCreateFramebuffer failed ({})",
                      static_cast<int>(framebuffer_result));
        return false;
      }

      VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      pool_info.queueFamilyIndex = vulkan->GetGraphicsQueueFamilyIndex();
      const VkResult pool_result =
          vkCreateCommandPool(vulkan->GetDevice(), &pool_info, nullptr, &image.command_pool);
      if (pool_result != VK_SUCCESS)
      {
        ERROR_LOG_FMT(OPENXR, "OpenXR controller mapper: vkCreateCommandPool failed ({})",
                      static_cast<int>(pool_result));
        return false;
      }

      VkCommandBufferAllocateInfo allocate_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      allocate_info.commandPool = image.command_pool;
      allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      allocate_info.commandBufferCount = 1;
      const VkResult command_result =
          vkAllocateCommandBuffers(vulkan->GetDevice(), &allocate_info, &image.command_buffer);
      if (command_result != VK_SUCCESS)
      {
        ERROR_LOG_FMT(OPENXR, "OpenXR controller mapper: command-buffer allocation failed ({})",
                      static_cast<int>(command_result));
        return false;
      }

      VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
      const VkResult fence_result =
          vkCreateFence(vulkan->GetDevice(), &fence_info, nullptr, &image.fence);
      if (fence_result != VK_SUCCESS)
      {
        ERROR_LOG_FMT(OPENXR, "OpenXR controller mapper: vkCreateFence failed ({})",
                      static_cast<int>(fence_result));
        return false;
      }
    }

    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = manager->GetReferenceSpace();
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    quad.pose.position = {0.0f, UI_VERTICAL_OFFSET_METERS, -UI_DISTANCE_METERS};
    quad.size.height = UI_HEIGHT_METERS;
    quad.size.width =
        UI_HEIGHT_METERS * static_cast<float>(UI_WIDTH) / static_cast<float>(UI_HEIGHT);
    quad.subImage.swapchain = swapchain;
    quad.subImage.imageRect.extent = {static_cast<int32_t>(UI_WIDTH),
                                      static_cast<int32_t>(UI_HEIGHT)};
    return true;
  }

  bool InitializeImGui()
  {
    previous_imgui_context = ImGui::GetCurrentContext();
    imgui_context = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui_context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = {static_cast<float>(UI_WIDTH), static_cast<float>(UI_HEIGHT)};
    std::string font_path = File::GetUserPath(D_LOAD_IDX) + "OSD_Font.ttf";
    if (!File::Exists(font_path))
      font_path = File::GetSysDirectory() + DIR_SEP + RESOURCES_DIR + DIR_SEP + "OSD_Font.ttf";
    if (File::Exists(font_path))
      io.Fonts->AddFontFromFileTTF(font_path.c_str(), 30.0f);
    else
      io.Fonts->AddFontDefault();

    if (!ImGui_ImplVulkan_LoadFunctions(
            VK_API_VERSION_1_1,
            [](const char* name, void* user_data) {
              return vkGetInstanceProcAddr(reinterpret_cast<VkInstance>(user_data), name);
            },
            vulkan->GetVulkanInstance()))
    {
      ERROR_LOG_FMT(
          OPENXR,
          "OpenXR controller mapper: ImGui Vulkan function loading failed (surface/swapchain "
          "entry points may be unavailable)");
      return false;
    }

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = vulkan->GetDeviceInfo().apiVersion;
    info.Instance = vulkan->GetVulkanInstance();
    info.PhysicalDevice = vulkan->GetPhysicalDevice();
    info.Device = vulkan->GetDevice();
    info.QueueFamily = vulkan->GetGraphicsQueueFamilyIndex();
    info.Queue = vulkan->GetGraphicsQueue();
    info.RenderPass = render_pass;
    // ImGui uses this count only for its rotating transient buffers. OpenXR is allowed to expose
    // a one-image swapchain, while the renderer backend requires at least two transient slots.
    info.MinImageCount = std::max(2u, static_cast<uint32_t>(render_images.size()));
    info.ImageCount = info.MinImageCount;
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE;
    info.CheckVkResultFn = RecordImGuiVulkanResult;
    s_imgui_vulkan_error = VK_SUCCESS;
    if (!ImGui_ImplVulkan_Init(&info))
    {
      ERROR_LOG_FMT(OPENXR, "OpenXR controller mapper: ImGui Vulkan initialization failed");
      return false;
    }
    if (s_imgui_vulkan_error != VK_SUCCESS)
      return false;
    imgui_backend_initialized = true;

    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);
    style.WindowPadding = {0.0f, 0.0f};
    style.FrameRounding = 10.0f;
    style.FramePadding = {18.0f, 14.0f};
    style.ItemSpacing = {18.0f, 18.0f};
    style.ScrollbarSize = 24.0f;
    style.ScrollbarRounding = 10.0f;
    style.Colors[ImGuiCol_WindowBg] = {0.035f, 0.045f, 0.06f, 0.97f};
    return true;
  }

  void UpdatePointer()
  {
    ImGuiIO& io = ImGui::GetIO();
    const auto snapshot = Common::VR::OpenXRInputState::GetSnapshot();
    pointer_visible.fill(false);
    pointer_pressed.fill(false);

    for (size_t hand = 0; hand < snapshot.controllers.size(); ++hand)
    {
      const auto& controller = snapshot.controllers[hand];
      if (!controller.connected || !controller.aim_pose.valid)
        continue;
      const std::optional<PanelMath::QuadHit> hit =
          PanelMath::IntersectRayQuad(PanelMath::ToXrPose(controller.aim_pose), quad.pose);
      if (!hit)
        continue;
      const float u = hit->x / quad.size.width + 0.5f;
      const float v = 0.5f - hit->y / quad.size.height;
      if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
        continue;
      pointer_visible[hand] = true;
      pointer_positions[hand] = {u * UI_WIDTH, v * UI_HEIGHT};
      pointer_pressed[hand] = controller.trigger_value > 0.55f || controller.trigger_button;
    }

    std::optional<size_t> selected_hand;
    for (size_t hand = 0; hand < pointer_pressed.size(); ++hand)
    {
      if (pointer_visible[hand] && pointer_pressed[hand])
      {
        selected_hand = hand;
        break;
      }
    }
    if (!selected_hand && pointer_visible[active_pointer_hand])
      selected_hand = active_pointer_hand;
    if (!selected_hand)
    {
      for (size_t hand = 0; hand < pointer_visible.size(); ++hand)
      {
        if (pointer_visible[hand])
        {
          selected_hand = hand;
          break;
        }
      }
    }

    const bool pointer_down = selected_hand && pointer_pressed[*selected_hand];
    if (selected_hand)
    {
      active_pointer_hand = *selected_hand;
      io.AddMousePosEvent(pointer_positions[*selected_hand].x, pointer_positions[*selected_hand].y);
    }
    else
    {
      io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    if (pointer_down != previous_pointer_down)
    {
      io.AddMouseButtonEvent(0, pointer_down);
      previous_pointer_down = pointer_down;
    }
  }

  void DrawPointers() const
  {
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    constexpr std::array<ImU32, 2> colors{IM_COL32(35, 210, 255, 255), IM_COL32(255, 170, 35, 255)};
    for (size_t hand = 0; hand < pointer_visible.size(); ++hand)
    {
      if (!pointer_visible[hand])
        continue;

      const float radius = pointer_pressed[hand] ? 15.0f : 11.0f;
      const ImVec2 position = pointer_positions[hand];
      draw_list->AddCircleFilled(position, radius, colors[hand]);
      draw_list->AddCircle(position, radius + 4.0f, IM_COL32(255, 255, 255, 255), 0, 3.0f);
      draw_list->AddCircleFilled(position, 3.0f, IM_COL32(255, 255, 255, 255));
    }
  }

  bool Render(uint32_t index, BindingWizard& wizard)
  {
    auto& image = render_images[index];
    const VkDevice device = vulkan->GetDevice();
    if (vkWaitForFences(device, 1, &image.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS ||
        vkResetFences(device, 1, &image.fence) != VK_SUCCESS ||
        vkResetCommandPool(device, image.command_pool, 0) != VK_SUCCESS)
      return false;

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(image.command_buffer, &begin) != VK_SUCCESS)
      return false;

    if (!image.initialized)
    {
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.srcAccessMask = 0;
      barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = image.image;
      barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      barrier.subresourceRange.levelCount = 1;
      barrier.subresourceRange.layerCount = 1;
      vkCmdPipelineBarrier(image.command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                           1, &barrier);
      image.initialized = true;
    }

    ImGui::SetCurrentContext(imgui_context);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {static_cast<float>(UI_WIDTH), static_cast<float>(UI_HEIGHT)};
    io.DeltaTime = 1.0f / 72.0f;
    UpdatePointer();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    wizard.DrawAndUpdate();
    DrawPointers();
    ImGui::Render();

    VkClearValue clear{};
    clear.color = {{0.035f, 0.045f, 0.06f, 0.97f}};
    VkRenderPassBeginInfo render_info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    render_info.renderPass = render_pass;
    render_info.framebuffer = image.framebuffer;
    render_info.renderArea.extent = {UI_WIDTH, UI_HEIGHT};
    render_info.clearValueCount = 1;
    render_info.pClearValues = &clear;
    vkCmdBeginRenderPass(image.command_buffer, &render_info, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), image.command_buffer);
    vkCmdEndRenderPass(image.command_buffer);
    if (vkEndCommandBuffer(image.command_buffer) != VK_SUCCESS)
      return false;

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &image.command_buffer;
    if (vkQueueSubmit(vulkan->GetGraphicsQueue(), 1, &submit, image.fence) != VK_SUCCESS ||
        vkWaitForFences(device, 1, &image.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
      return false;
    return true;
  }

  bool RunFrame(BindingWizard& wizard)
  {
    if (!manager->WaitFrame() || !manager->BeginFrame())
      return false;
    if (manager->ShouldRender())
    {
      manager->LocateViews();
      if (!quad_pose_initialized)
      {
        const auto& views = manager->GetEyeViews();
        quad.pose.position.y = 0.5f * (views[0].pose.position.y + views[1].pose.position.y) +
                               UI_VERTICAL_OFFSET_METERS;
        quad_pose_initialized = true;
      }
    }

    std::vector<XrCompositionLayerBaseHeader*> layers;
    if (manager->ShouldRender())
    {
      uint32_t index = 0;
      XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
      if (XR_FAILED(xrAcquireSwapchainImage(swapchain, &acquire, &index)))
        return false;
      XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
      wait.timeout = XR_INFINITE_DURATION;
      const bool rendered = XR_SUCCEEDED(xrWaitSwapchainImage(swapchain, &wait)) &&
                            index < render_images.size() && Render(index, wizard);
      XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      if (XR_FAILED(xrReleaseSwapchainImage(swapchain, &release)) || !rendered)
        return false;
      layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad));
    }
    return manager->EndFrame(layers);
  }

  void Cleanup()
  {
    if (vulkan)
      vkDeviceWaitIdle(vulkan->GetDevice());
    if (imgui_context)
    {
      ImGui::SetCurrentContext(imgui_context);
      if (imgui_backend_initialized)
        ImGui_ImplVulkan_Shutdown();
      ImGui::DestroyContext(imgui_context);
      imgui_context = nullptr;
      ImGui::SetCurrentContext(previous_imgui_context);
      previous_imgui_context = nullptr;
      imgui_backend_initialized = false;
    }
    if (vulkan)
    {
      for (auto& image : render_images)
      {
        if (image.fence)
          vkDestroyFence(vulkan->GetDevice(), image.fence, nullptr);
        if (image.command_pool)
          vkDestroyCommandPool(vulkan->GetDevice(), image.command_pool, nullptr);
        if (image.framebuffer)
          vkDestroyFramebuffer(vulkan->GetDevice(), image.framebuffer, nullptr);
        if (image.view)
          vkDestroyImageView(vulkan->GetDevice(), image.view, nullptr);
      }
      if (render_pass)
        vkDestroyRenderPass(vulkan->GetDevice(), render_pass, nullptr);
    }
    render_images.clear();
    xr_images.clear();
    if (swapchain != XR_NULL_HANDLE)
    {
      xrDestroySwapchain(swapchain);
      swapchain = XR_NULL_HANDLE;
    }
    manager.reset();
    vulkan.reset();
    if (vulkan_library_loaded)
    {
      Vulkan::UnloadVulkanLibrary();
      vulkan_library_loaded = false;
    }
    if (backend_info_saved)
    {
      g_backend_info = std::move(previous_backend_info);
      backend_info_saved = false;
    }
    Common::VR::OpenXRInputState::Reset();
    s_utility_session_active.store(false, std::memory_order_release);
  }

  void ThreadMain()
  {
    Common::VR::OpenXRInputState::Reset();
    BindingWizard wizard(target);
    if (!wizard.IsValid())
    {
      Fail(OpenXRUtilitySessionFailure::InitializationFailed,
           "selected controller has no bindable controls");
      Cleanup();
      return;
    }
    if (!InitializeOpenXR())
    {
      Fail(OpenXRUtilitySessionFailure::RuntimeUnavailable, "OpenXR runtime initialization failed");
      Cleanup();
      return;
    }
    if (!InitializeVulkan())
    {
      Fail(OpenXRUtilitySessionFailure::VulkanUnavailable,
           "Vulkan initialization or runtime GPU selection failed");
      Cleanup();
      return;
    }
    if (!CreateXRSession())
    {
      Fail(OpenXRUtilitySessionFailure::InitializationFailed,
           "OpenXR Vulkan session creation failed. See the OpenXR log for the runtime result.");
      Cleanup();
      return;
    }
    if (!CreateSwapchainAndImages())
    {
      Fail(OpenXRUtilitySessionFailure::InitializationFailed,
           "OpenXR mapper quad swapchain creation failed. See the OpenXR log for the exact "
           "Vulkan or runtime result.");
      Cleanup();
      return;
    }
    if (!InitializeImGui())
    {
      Fail(OpenXRUtilitySessionFailure::InitializationFailed,
           "OpenXR mapper UI initialization failed. See the OpenXR log for the exact Vulkan "
           "result.");
      Cleanup();
      return;
    }

    owner.m_state.store(OpenXRUtilitySessionState::Running, std::memory_order_release);
    bool session_failed = false;
    bool session_was_running = false;
    while (!stop_requested.load(std::memory_order_acquire))
    {
      if (!manager->PollEvents())
      {
        session_failed = true;
        break;
      }
      if (!manager->IsSessionRunning())
      {
        // READY is asynchronous, so an initially non-running session is expected. Once a
        // session has begun, however, STOPPING must end the utility rather than leaving it
        // indefinitely in the Running state without a frame loop.
        if (session_was_running)
        {
          session_failed = true;
          break;
        }
        std::this_thread::sleep_for(10ms);
        continue;
      }
      session_was_running = true;
      if (!RunFrame(wizard))
      {
        session_failed = true;
        break;
      }

      const BindingWizard::Result result = wizard.GetResult();
      if (result == BindingWizard::Result::Apply)
      {
        std::lock_guard lock(owner.m_result_mutex);
        owner.m_pending_bindings = wizard.BuildPendingBindings();
        owner.m_state.store(OpenXRUtilitySessionState::ApplyPending, std::memory_order_release);
        break;
      }
      if (result == BindingWizard::Result::Cancel)
      {
        owner.m_state.store(OpenXRUtilitySessionState::Cancelled, std::memory_order_release);
        break;
      }
    }

    if (session_failed && !stop_requested.load(std::memory_order_acquire))
      Fail(OpenXRUtilitySessionFailure::SessionLost, "OpenXR session ended unexpectedly");
    else if (stop_requested.load(std::memory_order_acquire) &&
             owner.m_state.load(std::memory_order_acquire) == OpenXRUtilitySessionState::Running)
      owner.m_state.store(OpenXRUtilitySessionState::Cancelled, std::memory_order_release);
    Cleanup();
  }
};

OpenXRUtilitySession::OpenXRUtilitySession() : m_impl(std::make_unique<Impl>(*this))
{
}

OpenXRUtilitySession::~OpenXRUtilitySession()
{
  Stop();
}

bool OpenXRUtilitySession::Start(OpenXRUtilitySessionTarget target)
{
  Stop();
  {
    std::lock_guard lock(m_result_mutex);
    m_failure_message.clear();
    m_pending_bindings = {};
  }
  m_failure.store(OpenXRUtilitySessionFailure::None, std::memory_order_release);
  m_state.store(OpenXRUtilitySessionState::Starting, std::memory_order_release);

  if (target.port < 0 || target.port >= 4 ||
      (target.type == OpenXRUtilitySessionTargetType::Hotkeys && target.port != 0) ||
      Core::GetState(Core::System::GetInstance()) != Core::State::Uninitialized || g_openxr)
  {
    m_failure.store(OpenXRUtilitySessionFailure::SessionBusy, std::memory_order_release);
    m_state.store(OpenXRUtilitySessionState::Failed, std::memory_order_release);
    return false;
  }

  bool expected = false;
  if (!s_utility_session_active.compare_exchange_strong(expected, true))
  {
    m_failure.store(OpenXRUtilitySessionFailure::SessionBusy, std::memory_order_release);
    m_state.store(OpenXRUtilitySessionState::Failed, std::memory_order_release);
    return false;
  }

  m_impl->target = target;
  m_impl->stop_requested.store(false, std::memory_order_release);
  m_impl->thread = std::thread(&Impl::ThreadMain, m_impl.get());
  return true;
}

bool OpenXRUtilitySession::Start(int wiimote_port)
{
  return Start({OpenXRUtilitySessionTargetType::WiiRemote, wiimote_port});
}

void OpenXRUtilitySession::RequestStop()
{
  m_impl->stop_requested.store(true, std::memory_order_release);
}

void OpenXRUtilitySession::Stop()
{
  RequestStop();
  if (m_impl->thread.joinable())
    m_impl->thread.join();
  if (m_state.load(std::memory_order_acquire) == OpenXRUtilitySessionState::Starting)
    m_state.store(OpenXRUtilitySessionState::Idle, std::memory_order_release);
}

OpenXRUtilitySessionState OpenXRUtilitySession::GetState() const
{
  return m_state.load(std::memory_order_acquire);
}

OpenXRUtilitySessionFailure OpenXRUtilitySession::GetFailureReason() const
{
  return m_failure.load(std::memory_order_acquire);
}

std::string OpenXRUtilitySession::GetFailureMessage() const
{
  {
    std::lock_guard lock(m_result_mutex);
    if (!m_failure_message.empty())
      return m_failure_message;
  }

  switch (GetFailureReason())
  {
  case OpenXRUtilitySessionFailure::None:
    return {};
  case OpenXRUtilitySessionFailure::RuntimeUnavailable:
    return Common::GetStringT("No usable OpenXR runtime was found.");
  case OpenXRUtilitySessionFailure::VulkanUnavailable:
    return Common::GetStringT("The OpenXR runtime could not create a compatible Vulkan device.");
  case OpenXRUtilitySessionFailure::SessionBusy:
    return Common::GetStringT("Another game or OpenXR session is already running.");
  case OpenXRUtilitySessionFailure::InitializationFailed:
    return Common::GetStringT("The OpenXR controller mapper could not be initialized.");
  case OpenXRUtilitySessionFailure::SessionLost:
    return Common::GetStringT("The OpenXR controller mapper session was lost.");
  }
  return {};
}

OpenXRPendingBindings OpenXRUtilitySession::TakePendingBindings()
{
  std::lock_guard lock(m_result_mutex);
  return std::exchange(m_pending_bindings, {});
}

void OpenXRUtilitySession::MarkApplied()
{
  if (GetState() == OpenXRUtilitySessionState::ApplyPending)
    m_state.store(OpenXRUtilitySessionState::Applied, std::memory_order_release);
}
}  // namespace VR

#endif
