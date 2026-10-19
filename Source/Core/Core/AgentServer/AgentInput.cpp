// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/AgentServer/AgentInput.h"

#include <atomic>
#include <mutex>
#include <utility>

#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/StringUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/ExtensionPort.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/StickGate.h"
#include "InputCommon/InputConfig.h"

namespace AgentServer::AgentInput
{
namespace
{
constexpr unsigned int GC_PORTS = 4;

struct Entry
{
  Device device;
  unsigned int port;
  HeldControl control;
};

std::atomic<bool> s_holding{false};
std::mutex s_mutex;
std::vector<Entry> s_held;

// The agent's override function, recognizable by type (InputOverrideFunction::target).
struct AgentOverride
{
  Device device;
  unsigned int port;

  std::optional<ControlState> operator()(std::string_view group, std::string_view control,
                                         ControlState) const
  {
    return Lookup(device, port, group, control);
  }
};

bool IsAgentOverride(const ControllerEmu::ControlGroupContainer& controller)
{
  return controller.GetInputOverrideFunction().target<AgentOverride>() != nullptr;
}

ControllerEmu::Attachments* GetAttachments(unsigned int port)
{
  return static_cast<ControllerEmu::Attachments*>(
      Wiimote::GetWiimoteGroup(static_cast<int>(port), WiimoteEmu::WiimoteGroup::Attachments));
}

std::string ListButtons(const std::vector<GroupInfo>& groups)
{
  std::string list;
  for (const GroupInfo& group : groups)
  {
    if (group.kind != GroupKind::Buttons)
      continue;
    for (const std::string& control : group.controls)
      list += fmt::format("{}{}/{}", list.empty() ? "" : ", ", group.name, control);
  }
  return list;
}

// "Group/Control" or "Control".
std::pair<std::string_view, std::string_view> SplitName(std::string_view name)
{
  const size_t slash = name.find('/');
  if (slash == std::string_view::npos)
    return {{}, name};
  return {name.substr(0, slash), name.substr(slash + 1)};
}
}  // namespace

std::optional<double> Lookup(Device device, unsigned int port, std::string_view group,
                             std::string_view control)
{
  if (!s_holding.load(std::memory_order_acquire))
    return std::nullopt;
  std::lock_guard lock(s_mutex);
  for (const Entry& entry : s_held)
  {
    if (entry.device == device && entry.port == port && entry.control.group == group &&
        entry.control.control == control)
    {
      return entry.control.value;
    }
  }
  return std::nullopt;
}

void Hold(Device device, unsigned int port, std::vector<HeldControl> controls)
{
  std::lock_guard lock(s_mutex);
  std::erase_if(s_held,
                [&](const Entry& entry) { return entry.device == device && entry.port == port; });
  for (HeldControl& control : controls)
    s_held.push_back({device, port, std::move(control)});
  s_holding.store(!s_held.empty(), std::memory_order_release);
}

void ReleaseAll()
{
  std::lock_guard lock(s_mutex);
  s_held.clear();
  s_holding.store(false, std::memory_order_release);
}

ControllerEmu::ControlGroupContainer* GetController(Device device, unsigned int port,
                                                    std::string* error)
{
  if (device == Device::GCPad)
  {
    if (port >= GC_PORTS)
    {
      *error = "GameCube ports are 1 to 4.";
      return nullptr;
    }
    const SerialInterface::SIDevices si_device =
        Config::Get(Config::GetInfoForSIDevice(static_cast<int>(port)));
    if (si_device == SerialInterface::SIDEVICE_WIIU_ADAPTER)
    {
      // The adapter reads real controllers without ControllerEmu, so no override reaches it.
      *error = fmt::format("GameCube port {} uses the GameCube Adapter for Wii U, which the agent "
                           "cannot drive: ask the user to set the port to Standard Controller.",
                           port + 1);
      return nullptr;
    }
    if (si_device != SerialInterface::SIDEVICE_GC_CONTROLLER)
    {
      *error = fmt::format("GameCube port {} has no emulated Standard Controller (Controllers "
                           "settings).",
                           port + 1);
      return nullptr;
    }
    return Pad::GetConfig()->GetController(static_cast<int>(port));
  }

  if (port >= MAX_WIIMOTES)
  {
    *error = "Wii Remotes are 1 to 4.";
    return nullptr;
  }
  const WiimoteSource source = Config::Get(Config::GetInfoForWiimoteSource(static_cast<int>(port)));
  if (source != WiimoteSource::Emulated && source != WiimoteSource::OpenXR)
  {
    *error = fmt::format("Wii Remote {} is not emulated (its source is {}), so it cannot be "
                         "driven.",
                         port + 1, source == WiimoteSource::Real ? "a real Wii Remote" : "none");
    return nullptr;
  }
  if (device == Device::Wiimote)
    return Wiimote::GetConfig()->GetController(static_cast<int>(port));

  ControllerEmu::Attachments* attachments = GetAttachments(port);
  const u32 selected = attachments ? attachments->GetSelectedAttachment() : 0;
  if (!attachments || selected == WiimoteEmu::ExtensionNumber::NONE ||
      selected >= attachments->GetAttachmentList().size())
  {
    *error = fmt::format("Wii Remote {} has no attachment plugged in.", port + 1);
    return nullptr;
  }
  return attachments->GetAttachmentList()[selected].get();
}

bool Attach(Device device, unsigned int port, std::string* error)
{
  ControllerEmu::ControlGroupContainer* controller = GetController(device, port, error);
  if (!controller)
    return false;

  // The CPU thread calls the override functions under this lock.
  const auto lock = ControllerEmu::EmulatedController::GetStateLock();
  if (device != Device::GCPad && Wiimote::IsOpenXRInputOverrideInstalled(port))
    return true;
  if (controller->HasInputOverrideFunction())
  {
    if (IsAgentOverride(*controller))
      return true;
    *error = "Another input override controls this controller (a TAS input window): ask the user "
             "to close it.";
    return false;
  }
  controller->SetInputOverrideFunction(AgentOverride{device, port});
  return true;
}

void DetachAll()
{
  ReleaseAll();

  const auto lock = ControllerEmu::EmulatedController::GetStateLock();
  const auto detach = [](ControllerEmu::ControlGroupContainer* controller) {
    if (controller && IsAgentOverride(*controller))
      controller->ClearInputOverrideFunction();
  };
  for (unsigned int port = 0; port < GC_PORTS; ++port)
    detach(Pad::GetConfig()->GetController(static_cast<int>(port)));
  for (unsigned int port = 0; port < MAX_WIIMOTES; ++port)
  {
    detach(Wiimote::GetConfig()->GetController(static_cast<int>(port)));
    if (ControllerEmu::Attachments* attachments = GetAttachments(port))
    {
      for (const auto& attachment : attachments->GetAttachmentList())
        detach(attachment.get());
    }
  }
}

std::vector<GroupInfo> DescribeController(const ControllerEmu::ControlGroupContainer& controller)
{
  std::vector<GroupInfo> groups;
  for (const auto& group : controller.groups)
  {
    GroupInfo info;
    info.name = group->name;
    switch (group->type)
    {
    case ControllerEmu::GroupType::Buttons:
      info.kind = GroupKind::Buttons;
      break;
    case ControllerEmu::GroupType::MixedTriggers:
      info.kind = GroupKind::Buttons;
      info.digital_count = group->controls.size() / 2;
      break;
    case ControllerEmu::GroupType::Triggers:
      info.kind = GroupKind::Analog;
      break;
    case ControllerEmu::GroupType::Stick:
      info.kind = GroupKind::Stick;
      break;
    case ControllerEmu::GroupType::Cursor:
      info.kind = GroupKind::Pointer;
      break;
    default:
      continue;
    }
    if (info.kind == GroupKind::Stick || info.kind == GroupKind::Pointer)
    {
      info.controls = {ControllerEmu::ReshapableInput::X_INPUT_OVERRIDE,
                       ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE};
    }
    else
    {
      for (const auto& control : group->controls)
        info.controls.push_back(control->name);
    }
    groups.push_back(std::move(info));
  }
  return groups;
}

std::vector<HeldControl> ResolvePress(const std::vector<GroupInfo>& groups, std::string_view name,
                                      std::string* error)
{
  const auto [group_name, control_name] = SplitName(name);
  std::vector<std::pair<const GroupInfo*, size_t>> matches;
  for (const GroupInfo& group : groups)
  {
    if (group.kind != GroupKind::Buttons && group.kind != GroupKind::Analog)
      continue;
    if (!group_name.empty() && !Common::CaseInsensitiveEquals(group.name, group_name))
      continue;
    for (size_t i = 0; i < group.controls.size(); ++i)
    {
      if (Common::CaseInsensitiveEquals(group.controls[i], control_name))
        matches.emplace_back(&group, i);
    }
  }

  if (matches.size() != 1)
  {
    if (matches.empty())
    {
      *error = fmt::format("Unknown button \"{}\". Buttons: {}.", name, ListButtons(groups));
    }
    else
    {
      std::string candidates;
      for (const auto& [group, index] : matches)
      {
        candidates += fmt::format("{}{}/{}", candidates.empty() ? "" : ", ", group->name,
                                  group->controls[index]);
      }
      *error = fmt::format("\"{}\" is ambiguous: write one of {}.", name, candidates);
    }
    return {};
  }

  const auto& [group, index] = matches.front();
  std::vector<HeldControl> held = {{group->name, group->controls[index], 1.0}};
  // A digital trigger pressed by a hand also reaches the end of its analog travel.
  if (index < group->digital_count && group->digital_count + index < group->controls.size())
    held.push_back({group->name, group->controls[group->digital_count + index], 1.0});
  return held;
}

std::optional<HeldControl> ResolveAnalog(const std::vector<GroupInfo>& groups,
                                         std::string_view name, double value, std::string* error)
{
  const auto [group_name, control_name] = SplitName(name);
  for (const GroupInfo& group : groups)
  {
    if (group.kind != GroupKind::Analog && group.digital_count == 0)
      continue;
    if (!group_name.empty() && !Common::CaseInsensitiveEquals(group.name, group_name))
      continue;
    const size_t first = group.kind == GroupKind::Analog ? 0 : group.digital_count;
    for (size_t i = first; i < group.controls.size(); ++i)
    {
      if (Common::CaseInsensitiveEquals(group.controls[i], control_name))
        return HeldControl{group.name, group.controls[i], value};
    }
  }

  std::string list;
  for (const GroupInfo& group : groups)
  {
    if (group.kind != GroupKind::Analog && group.digital_count == 0)
      continue;
    const size_t first = group.kind == GroupKind::Analog ? 0 : group.digital_count;
    for (size_t i = first; i < group.controls.size(); ++i)
      list += fmt::format("{}{}/{}", list.empty() ? "" : ", ", group.name, group.controls[i]);
  }
  *error = list.empty() ?
               std::string("This controller has no analog triggers.") :
               fmt::format("Unknown analog control \"{}\". Analog controls: {}.", name, list);
  return std::nullopt;
}

std::optional<std::string> ResolveGroup(const std::vector<GroupInfo>& groups, GroupKind kind,
                                        std::string_view name, std::string* error)
{
  std::string list;
  for (const GroupInfo& group : groups)
  {
    if (group.kind != kind)
      continue;
    if (name.empty() || Common::CaseInsensitiveEquals(group.name, name))
      return group.name;
    list += fmt::format("{}{}", list.empty() ? "" : ", ", group.name);
  }
  const char* what = kind == GroupKind::Pointer ? "pointer" : "stick";
  *error = list.empty() ? fmt::format("This controller has no {}.", what) :
                          fmt::format("Unknown {} \"{}\". Choices: {}.", what, name, list);
  return std::nullopt;
}
}  // namespace AgentServer::AgentInput
