// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Core/AgentServer/AgentInput.h"

using namespace AgentServer::AgentInput;

namespace
{
// The groups of a GameCube controller as DescribeController reports them.
std::vector<GroupInfo> MakeGCPad()
{
  return {
      {"Buttons", GroupKind::Buttons, {"A", "B", "X", "Y", "Z", "Start"}, 0},
      {"D-Pad", GroupKind::Buttons, {"Up", "Down", "Left", "Right"}, 0},
      {"Main Stick", GroupKind::Stick, {"X", "Y"}, 0},
      {"C-Stick", GroupKind::Stick, {"X", "Y"}, 0},
      {"Triggers", GroupKind::Buttons, {"L", "R", "L-Analog", "R-Analog"}, 2},
  };
}
}  // namespace

TEST(AgentInput, ResolvesButtons)
{
  const std::vector<GroupInfo> pad = MakeGCPad();
  std::string error;

  std::vector<HeldControl> held = ResolvePress(pad, "a", &error);
  ASSERT_EQ(held.size(), 1u) << error;
  EXPECT_EQ(held[0].group, "Buttons");
  EXPECT_EQ(held[0].control, "A");
  EXPECT_EQ(held[0].value, 1.0);

  held = ResolvePress(pad, "D-Pad/Down", &error);
  ASSERT_EQ(held.size(), 1u) << error;
  EXPECT_EQ(held[0].group, "D-Pad");

  // A digital trigger also presses its analog value.
  held = ResolvePress(pad, "L", &error);
  ASSERT_EQ(held.size(), 2u) << error;
  EXPECT_EQ(held[1].control, "L-Analog");

  EXPECT_TRUE(ResolvePress(pad, "Home", &error).empty());
  EXPECT_NE(error.find("Buttons/A"), std::string::npos) << error;
}

TEST(AgentInput, ReportsAmbiguousButtons)
{
  std::vector<GroupInfo> classic = {
      {"Buttons", GroupKind::Buttons, {"A", "B", "Home"}, 0},
      {"Extra", GroupKind::Buttons, {"A"}, 0},
  };
  std::string error;
  EXPECT_TRUE(ResolvePress(classic, "A", &error).empty());
  EXPECT_NE(error.find("ambiguous"), std::string::npos) << error;
  EXPECT_EQ(ResolvePress(classic, "Extra/A", &error).size(), 1u) << error;
}

TEST(AgentInput, ResolvesAnalogAndGroups)
{
  const std::vector<GroupInfo> pad = MakeGCPad();
  std::string error;

  const std::optional<HeldControl> analog = ResolveAnalog(pad, "R-Analog", 0.5, &error);
  ASSERT_TRUE(analog) << error;
  EXPECT_EQ(analog->group, "Triggers");
  EXPECT_EQ(analog->value, 0.5);
  // The digital half is not an analog value.
  EXPECT_FALSE(ResolveAnalog(pad, "R", 0.5, &error));

  EXPECT_EQ(ResolveGroup(pad, GroupKind::Stick, "", &error), "Main Stick");
  EXPECT_EQ(ResolveGroup(pad, GroupKind::Stick, "c-stick", &error), "C-Stick");
  EXPECT_FALSE(ResolveGroup(pad, GroupKind::Stick, "Nope", &error));
  EXPECT_FALSE(ResolveGroup(pad, GroupKind::Pointer, "", &error));
  EXPECT_NE(error.find("no pointer"), std::string::npos) << error;
}

TEST(AgentInput, HoldsAndReleases)
{
  EXPECT_FALSE(Lookup(Device::GCPad, 0, "Buttons", "A"));
  Hold(Device::GCPad, 0, {{"Buttons", "A", 1.0}, {"Main Stick", "Y", -1.0}});
  EXPECT_EQ(Lookup(Device::GCPad, 0, "Buttons", "A"), 1.0);
  EXPECT_EQ(Lookup(Device::GCPad, 0, "Main Stick", "Y"), -1.0);
  // Other ports, devices and controls keep the user's input.
  EXPECT_FALSE(Lookup(Device::GCPad, 1, "Buttons", "A"));
  EXPECT_FALSE(Lookup(Device::Wiimote, 0, "Buttons", "A"));
  EXPECT_FALSE(Lookup(Device::GCPad, 0, "Buttons", "B"));

  // A new hold replaces the previous one on that controller.
  Hold(Device::GCPad, 0, {{"Buttons", "B", 1.0}});
  EXPECT_FALSE(Lookup(Device::GCPad, 0, "Buttons", "A"));
  EXPECT_EQ(Lookup(Device::GCPad, 0, "Buttons", "B"), 1.0);

  ReleaseAll();
  EXPECT_FALSE(Lookup(Device::GCPad, 0, "Buttons", "B"));
}
