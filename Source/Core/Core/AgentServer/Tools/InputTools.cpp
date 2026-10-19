// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Controller input from the agent: press buttons, move sticks and the Wii Remote pointer, to
// navigate menus and reach the screens to check.

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "Core/AgentServer/AgentInput.h"
#include "Core/AgentServer/AgentServer.h"
#include "Core/AgentServer/ToolHelpers.h"
#include "Core/AgentServer/Tools/Tools.h"
#include "Core/Core.h"
#include "Core/Movie.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"

namespace AgentServer
{
namespace
{
using AgentInput::Device;
using AgentInput::GroupInfo;
using AgentInput::GroupKind;
using AgentInput::HeldControl;

constexpr size_t MAX_STEPS = 64;
constexpr u32 MAX_STEP_FIELDS = 600;

struct Target
{
  Device device = Device::GCPad;
  unsigned int port = 0;
};

const char* GetDeviceName(Device device)
{
  switch (device)
  {
  case Device::GCPad:
    return "GameCube controller";
  case Device::Wiimote:
    return "Wii Remote";
  case Device::Attachment:
    return "Wii Remote attachment";
  }
  return "?";
}

Target ParseTarget(const ToolArgs& args)
{
  Target target;
  const std::string controller =
      args.GetString("controller", GetSystem().IsWii() ? "wiimote" : "gc");
  if (controller == "gc")
    target.device = Device::GCPad;
  else if (controller == "wiimote")
    target.device = Device::Wiimote;
  else if (controller == "attachment")
    target.device = Device::Attachment;
  else
    throw ToolError("controller is gc, wiimote or attachment.");
  const s64 port = args.GetInt("port", 1);
  if (port < 1 || port > 4)
    throw ToolError("port is 1 to 4.");
  target.port = static_cast<unsigned int>(port - 1);
  return target;
}

std::vector<GroupInfo> DescribeTarget(const Target& target)
{
  std::string error;
  const ControllerEmu::ControlGroupContainer* controller =
      AgentInput::GetController(target.device, target.port, &error);
  if (!controller)
    throw ToolError(error);
  return AgentInput::DescribeController(*controller);
}

double GetUnit(const ToolArgs& args, const char* key)
{
  return std::clamp(args.GetNumber(key, 0.0), -1.0, 1.0);
}

struct Step
{
  std::vector<HeldControl> held;
  u32 fields = 0;
  std::string description;
};

Step ParseStep(const std::vector<GroupInfo>& groups, const picojson::object& object)
{
  const ToolArgs args(object);
  Step step;
  std::vector<std::string> parts;
  std::string error;

  for (const std::string& name : args.GetStringArray("press"))
  {
    std::vector<HeldControl> held = AgentInput::ResolvePress(groups, name, &error);
    if (held.empty())
      throw ToolError(error);
    parts.push_back(fmt::format("{}/{}", held.front().group, held.front().control));
    for (HeldControl& control : held)
      step.held.push_back(std::move(control));
  }

  if (const picojson::array* sticks = args.GetArray("sticks"))
  {
    for (const picojson::value& value : *sticks)
    {
      if (!value.is<picojson::object>())
        throw ToolError("Each stick is {group, x, y}.");
      const ToolArgs stick(value.get<picojson::object>());
      const std::optional<std::string> group =
          AgentInput::ResolveGroup(groups, GroupKind::Stick, stick.GetString("group"), &error);
      if (!group)
        throw ToolError(error);
      const double x = GetUnit(stick, "x");
      const double y = GetUnit(stick, "y");
      step.held.push_back({*group, "X", x});
      step.held.push_back({*group, "Y", y});
      parts.push_back(fmt::format("{} ({:.2f}, {:.2f})", *group, x, y));
    }
  }

  if (const picojson::array* analogs = args.GetArray("analog"))
  {
    for (const picojson::value& value : *analogs)
    {
      if (!value.is<picojson::object>())
        throw ToolError("Each analog value is {control, value}.");
      const ToolArgs analog(value.get<picojson::object>());
      const double amount = std::clamp(analog.GetNumber("value", 1.0), 0.0, 1.0);
      const std::optional<HeldControl> control =
          AgentInput::ResolveAnalog(groups, analog.RequireString("control"), amount, &error);
      if (!control)
        throw ToolError(error);
      parts.push_back(fmt::format("{}/{} {:.2f}", control->group, control->control, amount));
      step.held.push_back(*control);
    }
  }

  if (const picojson::object* pointer = args.GetObject("pointer"))
  {
    const ToolArgs point(*pointer);
    const std::optional<std::string> group =
        AgentInput::ResolveGroup(groups, GroupKind::Pointer, {}, &error);
    if (!group)
      throw ToolError(error);
    const double x = GetUnit(point, "x");
    const double y = GetUnit(point, "y");
    step.held.push_back({*group, "X", x});
    step.held.push_back({*group, "Y", y});
    parts.push_back(fmt::format("pointer ({:.2f}, {:.2f})", x, y));
  }

  const s64 default_fields = step.held.empty() ? 10 : 4;
  step.fields = static_cast<u32>(
      std::clamp<s64>(args.GetInt("frames", default_fields), 1, static_cast<s64>(MAX_STEP_FIELDS)));

  std::string description;
  for (const std::string& part : parts)
    description += (description.empty() ? "" : " + ") + part;
  step.description =
      fmt::format("{} for {} field(s)", description.empty() ? "wait" : description, step.fields);
  return step;
}

// Waits for `fields` VI fields while emulation runs. False when no field came for a while (the
// game froze).
bool WaitForFields(u32 fields)
{
  FrameCounters& counters = Server::GetInstance().GetFrameCounters();
  u64 last = counters.GetSnapshot().fields;
  const u64 target = last + fields;
  auto last_progress = std::chrono::steady_clock::now();
  while (true)
  {
    const u64 now_fields = counters.GetSnapshot().fields;
    if (now_fields >= target)
      return true;
    const auto now = std::chrono::steady_clock::now();
    if (now_fields != last)
    {
      last = now_fields;
      last_progress = now;
    }
    if (now - last_progress > 3s || Server::GetInstance().IsStopping())
      return false;
    std::this_thread::sleep_for(2ms);
  }
}

// Releases the agent's input and removes its override functions however the tool ends.
class ScopedAgentInput final
{
public:
  ScopedAgentInput() = default;
  ~ScopedAgentInput() { AgentInput::DetachAll(); }
  ScopedAgentInput(const ScopedAgentInput&) = delete;
  ScopedAgentInput& operator=(const ScopedAgentInput&) = delete;
};

void RequireInputAllowed()
{
  RequireGame();
  if (NetPlay::IsNetPlayRunning())
    throw ToolError("Controller input cannot be injected during NetPlay.");
  if (GetSystem().GetMovie().IsPlayingInput())
    throw ToolError("A recorded movie is playing back its own input.");
}

ToolResult ListInputControls(const ToolArgs& args)
{
  RequireGame();
  const Target target = ParseTarget(args);
  const std::vector<GroupInfo> groups = DescribeTarget(target);

  std::string text = fmt::format("{} {}:\n", GetDeviceName(target.device), target.port + 1);
  for (const GroupInfo& group : groups)
  {
    switch (group.kind)
    {
    case GroupKind::Buttons:
    {
      std::string buttons;
      std::string analog;
      for (size_t i = 0; i < group.controls.size(); ++i)
      {
        std::string& list = group.digital_count != 0 && i >= group.digital_count ? analog : buttons;
        list += fmt::format("{}{}", list.empty() ? "" : ", ", group.controls[i]);
      }
      text += fmt::format("  {} (press): {}\n", group.name, buttons);
      if (!analog.empty())
        text += fmt::format("  {} (analog, 0 to 1): {}\n", group.name, analog);
      break;
    }
    case GroupKind::Analog:
    {
      std::string analog;
      for (const std::string& control : group.controls)
        analog += fmt::format("{}{}", analog.empty() ? "" : ", ", control);
      text += fmt::format("  {} (analog, 0 to 1): {}\n", group.name, analog);
      break;
    }
    case GroupKind::Stick:
      text += fmt::format("  {} (stick: x right, y up, -1 to 1)\n", group.name);
      break;
    case GroupKind::Pointer:
      text += fmt::format("  {} (pointer: x right, y up, -1 to 1; 0,0 is the screen center)\n",
                          group.name);
      break;
    }
  }
  text += "Press with press_input; a name can be written Group/Control when it is ambiguous.";
  return ToolResult::Text(std::move(text));
}

ToolResult PressInput(const ToolArgs& args)
{
  RequireInputAllowed();
  const Target target = ParseTarget(args);
  const std::vector<GroupInfo> groups = DescribeTarget(target);

  const picojson::array* step_values = args.GetArray("steps");
  if (!step_values || step_values->empty() || step_values->size() > MAX_STEPS)
    throw ToolError(fmt::format("steps is 1 to {} steps.", MAX_STEPS));
  std::vector<Step> steps;
  for (const picojson::value& value : *step_values)
  {
    if (!value.is<picojson::object>())
      throw ToolError("Each step is an object {press, sticks, analog, pointer, frames}.");
    steps.push_back(ParseStep(groups, value.get<picojson::object>()));
  }
  const u32 settle = static_cast<u32>(std::clamp<s64>(args.GetInt("settle_frames", 10), 0, 600));

  const ScopedAgentInput scoped_input;
  std::string error;
  if (!AgentInput::Attach(target.device, target.port, &error))
    throw ToolError(error);

  const bool was_paused = IsPaused();
  ResumeEmulation();
  std::string text = fmt::format("{} {}:\n", GetDeviceName(target.device), target.port + 1);
  bool hung = false;
  for (const Step& step : steps)
  {
    AgentInput::Hold(target.device, target.port, step.held);
    text += "  " + step.description + "\n";
    if (!WaitForFields(step.fields))
    {
      hung = true;
      break;
    }
  }
  AgentInput::ReleaseAll();
  if (!hung && settle != 0)
    hung = !WaitForFields(settle);

  if (hung)
  {
    text += "The game stopped producing fields (it froze or was paused): input released.\n";
  }
  else
  {
    text += fmt::format("Released, then {} field(s) to settle.", settle);
  }

  ToolResult images;
  if (args.GetBool("screenshot", true) && !hung)
  {
    const u32 max_width =
        static_cast<u32>(std::clamp<s64>(args.GetInt("max_width", 480), 64, 1280));
    try
    {
      text += " Screenshot: " + AppendScreenshots(images, max_width, true, true) + ".";
    }
    catch (const ToolError& e)
    {
      text += fmt::format(" No screenshot: {}.", e.what());
    }
  }
  if (was_paused)
    PauseEmulation();

  ToolResult output = ToolResult::Text(std::move(text));
  for (picojson::value& block : images.content)
    output.content.push_back(std::move(block));
  return output;
}
}  // namespace

void RegisterInputTools(ToolRegistry& registry, Server& server)
{
  server.AddEmulationStoppedCallback([] { AgentInput::DetachAll(); });
  server.AddServerStoppedCallback([] { AgentInput::DetachAll(); });

  const auto target_schema = [](Schema schema) {
    schema
        .String("controller",
                "gc (GameCube controller), wiimote, or attachment (the Wii Remote's Nunchuk, "
                "Classic Controller...). Default: wiimote for Wii games, gc otherwise.",
                {"gc", "wiimote", "attachment"})
        .Integer("port", "Controller port, 1 to 4 (default 1).", 1, 4);
    return schema;
  };

  registry.Add({.name = "list_input_controls",
                .title = "List controller controls",
                .description = "Lists the buttons, analog triggers, sticks and pointer of an "
                               "emulated controller, with the names press_input takes.",
                .input_schema = target_schema(Schema()).Build(),
                .read_only = true,
                .idempotent = true,
                .handler = ListInputControls});

  const picojson::object stick_schema = Schema()
                                            .String("group", "Stick group (default the first, "
                                                             "e.g. Main Stick).")
                                            .Number("x", "-1 (left) to 1 (right).")
                                            .Number("y", "-1 (down) to 1 (up).")
                                            .Build();
  const picojson::object analog_schema = Schema()
                                             .String("control", "e.g. L-Analog.")
                                             .Number("value", "0 to 1 (default 1).")
                                             .Required({"control"})
                                             .Build();
  const picojson::object pointer_schema = Schema()
                                              .Number("x", "-1 (left) to 1 (right).")
                                              .Number("y", "-1 (bottom) to 1 (top).")
                                              .Build();
  const picojson::object step_schema =
      Schema()
          .StringArray("press", "Buttons held during the step, e.g. [\"A\"] or [\"D-Pad/Down\"].")
          .ArrayOf("sticks", "Sticks held at a position.", stick_schema)
          .ArrayOf("analog", "Analog trigger values.", analog_schema)
          .Object("pointer", "Wii Remote pointer position.", pointer_schema)
          .Integer("frames",
                   "VI fields the step lasts (default 4 with input, 10 for a wait). A game reads "
                   "input once per frame: hold at least 2 to 4.",
                   1, MAX_STEP_FIELDS)
          .Build();

  registry.Add(
      {.name = "press_input",
       .title = "Press controller input",
       .description =
           "Plays a sequence of controller input on an emulated controller to navigate menus: "
           "each step holds its buttons, sticks, triggers and pointer for some VI fields; a step "
           "with only frames is a wait with everything released. Between two presses of the "
           "same button, add a wait step, or the game sees one long press. Runs emulation for "
           "the duration, releases everything at the end, waits settle_frames and takes a "
           "screenshot. The user's own controller keeps working for the other controls.",
       .input_schema = target_schema(Schema())
                           .ArrayOf("steps", "1 to 64 steps.", step_schema)
                           .Integer("settle_frames",
                                    "Fields after the last step, before the screenshot "
                                    "(default 10).",
                                    0, 600)
                           .Boolean("screenshot", "Screenshot both eyes at the end (default true).")
                           .Integer("max_width", "Screenshot width (default 480).", 64, 1280)
                           .Required({"steps"})
                           .Build(),
       .handler = PressInput});
}
}  // namespace AgentServer
