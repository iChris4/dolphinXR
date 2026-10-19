// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace ControllerEmu
{
class ControlGroupContainer;
}

// Controller input held by the agent server (press_input): values for chosen controls of an
// emulated GameCube controller, Wii Remote or Wii Remote attachment, returned by input override
// functions in place of what the user's controller reports. The other controls keep the user's
// input.
//
// Threads: the CPU thread reads the held values from the override functions (Lookup). The server
// thread changes them, and installs or removes its override functions under
// ControllerEmu::EmulatedController::GetStateLock().
namespace AgentServer::AgentInput
{
enum class Device : u8
{
  GCPad,
  Wiimote,
  // The extension plugged in the Wii Remote (Nunchuk, Classic Controller...).
  Attachment,
};

// A control by its ControllerEmu names, e.g. "Buttons"/"A" or "Main Stick"/"X".
struct HeldControl
{
  std::string group;
  std::string control;
  double value = 0.0;
};

// Any thread; the CPU thread calls it for every control it reads. Costs one atomic load while
// the agent holds nothing.
std::optional<double> Lookup(Device device, unsigned int port, std::string_view group,
                             std::string_view control);

// Replaces what the agent holds on that controller.
void Hold(Device device, unsigned int port, std::vector<HeldControl> controls);
void ReleaseAll();

// The emulated controller, or nullptr with `error` set (nothing plugged in, a real Wii Remote, no
// attachment, a GameCube Adapter for Wii U port...).
ControllerEmu::ControlGroupContainer* GetController(Device device, unsigned int port,
                                                    std::string* error);
// Makes the controller return the held values: installs the agent's override function, unless
// the VR (OpenXR) override of the Wii Remote returns them already.
// Fails when another override owns the controller (a TAS input window).
bool Attach(Device device, unsigned int port, std::string* error);
// Releases everything and removes the agent's override functions (never the VR one).
void DetachAll();

// ---- Control names (pure) ----

enum class GroupKind : u8
{
  // Buttons, D-Pad, and the digital and analog halves of mixed triggers.
  Buttons,
  // Analog-only triggers.
  Analog,
  Stick,
  // The Wii Remote's IR pointer.
  Pointer,
};

struct GroupInfo
{
  std::string name;
  GroupKind kind = GroupKind::Buttons;
  // Overridable controls: "X" and "Y" for sticks and the pointer.
  std::vector<std::string> controls;
  // Mixed triggers: the first `digital_count` controls are digital, the rest their analog
  // values in the same order. 0 otherwise.
  size_t digital_count = 0;
};

// The groups of a controller that press_input can drive.
std::vector<GroupInfo> DescribeController(const ControllerEmu::ControlGroupContainer& controller);

// A button by name ("A", "Start") or as "Group/Control" ("D-Pad/Up"), case-insensitive. Pressing
// a digital trigger also presses its analog value fully. Empty with `error` set when the name is
// unknown or ambiguous.
std::vector<HeldControl> ResolvePress(const std::vector<GroupInfo>& groups, std::string_view name,
                                      std::string* error);
// An analog trigger value, e.g. "L-Analog" or "Triggers/L-Analog".
std::optional<HeldControl> ResolveAnalog(const std::vector<GroupInfo>& groups,
                                         std::string_view name, double value, std::string* error);
// A stick or pointer group by name, or the first one of that kind when `name` is empty.
std::optional<std::string> ResolveGroup(const std::vector<GroupInfo>& groups, GroupKind kind,
                                        std::string_view name, std::string* error);
}  // namespace AgentServer::AgentInput
