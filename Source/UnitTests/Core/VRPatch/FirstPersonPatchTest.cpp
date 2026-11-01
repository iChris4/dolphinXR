// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The shipped first-person patch of Twilight Princess (Data/Sys/VRPatches/GZ2E01/first-person.lua)
// against fake memory laid out like the game's: Link's actor and J3DModel, his joints' matrices
// and the camera's view matrix. The anchor it publishes is level and turned with Link, whatever the
// camera.

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <optional>
#include <string>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Core/VRPatch/MemoryAccess.h"
#include "Core/VRPatch/PatchScript.h"
#include "VideoCommon/VR/CameraAnchorMath.h"

using namespace VRPatch;
using VR::CameraAnchorMath::Mat33;
using VR::CameraAnchorMath::Mtx34;

namespace
{
class Memory final : public MemoryAccess
{
public:
  bool IsRAM(u32 address, u32 size) const override
  {
    return size != 0 && address >= 0x80000000 && static_cast<u64>(address) + size <= 0x81800000;
  }
  std::optional<u8> Read8(u32 a) const override { return Read<u8>(a); }
  std::optional<u16> Read16(u32 a) const override { return Read<u16>(a); }
  std::optional<u32> Read32(u32 a) const override { return Read<u32>(a); }
  std::optional<u64> Read64(u32 a) const override { return Read<u64>(a); }
  bool Write8(u32 a, u8 v) override { return Write<u8>(a, v); }
  bool Write16(u32 a, u16 v) override { return Write<u16>(a, v); }
  bool Write32(u32 a, u32 v) override { return Write<u32>(a, v); }
  bool Write64(u32 a, u64 v) override { return Write<u64>(a, v); }
  void InvalidateCode(u32, u32) override {}

  void SetMtx(u32 a, const Mtx34& m)
  {
    for (u32 i = 0; i < 12; ++i)
      Write32(a + i * 4, std::bit_cast<u32>(m[i]));
  }

private:
  template <typename T>
  std::optional<T> Read(u32 address) const
  {
    if (!IsRAM(address, sizeof(T)))
      return std::nullopt;
    u64 value = 0;
    for (u32 i = 0; i < sizeof(T); ++i)
    {
      const auto it = bytes.find(address + i);
      value = value << 8 | (it == bytes.end() ? 0 : it->second);
    }
    return static_cast<T>(value);
  }
  template <typename T>
  bool Write(u32 address, T value)
  {
    if (!IsRAM(address, sizeof(T)))
      return false;
    for (u32 i = 0; i < sizeof(T); ++i)
      bytes[address + i] = static_cast<u8>(static_cast<u64>(value) >> (8 * (sizeof(T) - 1 - i)));
    return true;
  }
  std::map<u32, u8> bytes;
};

constexpr float DEG = 0.01745329252f;
constexpr u32 PLAYER = 0x8040BF6C;
constexpr u32 OTHER_PLAYER = 0x8040B098;
constexpr u32 ACTOR = 0x80A220A0;
constexpr u32 MODEL = 0x81000000;
constexpr u32 MODEL_DATA = 0x81100000;
constexpr u32 MTX_BUFFER = 0x81200000;
constexpr u32 JOINTS = 0x81300000;
constexpr u32 CAMERA = 0x803DCDCC;

Mat33 RotX(float deg)
{
  const float c = std::cos(deg * DEG), s = std::sin(deg * DEG);
  return {1, 0, 0, 0, c, -s, 0, s, c};
}

Mat33 RotY(float deg)
{
  const float c = std::cos(deg * DEG), s = std::sin(deg * DEG);
  return {c, 0, s, 0, 1, 0, -s, 0, c};
}

Mat33 Transpose(const Mat33& m)
{
  return {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]};
}

Mtx34 Make(const Mat33& r, const std::array<float, 3>& t)
{
  return {r[0], r[1], r[2], t[0], r[3], r[4], r[5], t[1], r[6], r[7], r[8], t[2]};
}

template <size_t N>
void ExpectNear(const std::array<float, N>& actual, const std::array<float, N>& expected)
{
  for (size_t i = 0; i < N; ++i)
  {
    SCOPED_TRACE(i);
    EXPECT_NEAR(actual[i], expected[i], 1e-3f);
  }
}

// The camera looks down 20 degrees from behind Link; Link faces 40 degrees from +Z, his head
// (whose +X faces forward) facing the same way.
const Mtx34 VIEW = Make(VR::CameraAnchorMath::Multiply33(RotX(20), RotY(-30)), {10, -300, -900});
const std::array<float, 3> HEAD_POSITION = {100, 942, -200};

struct TwilightPrincess
{
  TwilightPrincess()
  {
    memory.Write32(PLAYER, ACTOR);
    memory.Write32(OTHER_PLAYER, ACTOR);
    memory.Write32(ACTOR + 0x524, MODEL);
    memory.Write32(MODEL, 0x803CED20);
    memory.Write32(MODEL + 4, MODEL_DATA);
    memory.Write16(MODEL_DATA + 0x2C, 35);
    memory.SetMtx(MODEL + 0x24, Make(RotY(40), {100, 800, -200}));
    memory.Write32(MODEL + 0x84, MTX_BUFFER);
    memory.Write32(MTX_BUFFER + 0xC, JOINTS);
    memory.SetMtx(JOINTS + 4 * 0x30, Make(RotY(40 - 90), HEAD_POSITION));
    memory.SetMtx(CAMERA, VIEW);
    env.memory = &memory;
    env.input = &input;
    env.game_id = "GZ2E01";
    env.field = 100;
    env.units_per_meter = 50;
  }

  ::testing::AssertionResult Load()
  {
    std::string source;
    File::ReadFileToString(File::GetSysDirectory() + VRPATCHES_DIR "/GZ2E01/first-person.lua",
                           source);
    if (!script.Load(source, env) || !script.Call(Hook::OnLoad, env))
      return ::testing::AssertionFailure() << script.Error();
    return ::testing::AssertionSuccess();
  }

  std::optional<VR::CameraAnchorMath::Anchor> Frame()
  {
    EXPECT_TRUE(script.Call(Hook::OnFrame, env)) << script.Error();
    return script.TakeCameraAnchor();
  }

  void SetSetting(const std::string& name, double value)
  {
    ASSERT_TRUE(script.SetSettingValue(*script.GetManifest().FindSetting(name), value));
    ASSERT_TRUE(script.CallSettingChanged(name, value, env)) << script.Error();
  }

  // The rig in the world: the anchor's rotation with the camera's turned back out.
  Mat33 WorldRig(const VR::CameraAnchorMath::Anchor& anchor) const
  {
    return VR::CameraAnchorMath::Multiply33(Transpose(VR::CameraAnchorMath::Basis(VIEW)),
                                            anchor.rotation);
  }

  Memory memory;
  VRInputFrame input;
  ScriptEnvironment env;
  PatchScript script{"first-person"};
};
}  // namespace

TEST(FirstPersonPatch, ManifestIsValid)
{
  TwilightPrincess game;
  ASSERT_TRUE(game.Load());
  const Manifest& manifest = game.script.GetManifest();
  EXPECT_EQ(manifest.games, std::vector<std::string>{"GZ2E01"});
  EXPECT_EQ(manifest.revision, 0);
  ASSERT_EQ(manifest.conflicts.size(), 1u);
  EXPECT_EQ(manifest.conflicts[0].name, "Links Head");
  EXPECT_EQ(manifest.hide_textures.size(), 4u);
  EXPECT_EQ(manifest.hide_textures[0], 0xd7f168afaf5c673aULL);
  EXPECT_TRUE(manifest.FindSetting("heading"));
}

// In the head, level, facing where Link's body faces: the camera's pitch and yaw are gone.
TEST(FirstPersonPatch, LevelInTheHeadWithTheBodysHeading)
{
  TwilightPrincess game;
  ASSERT_TRUE(game.Load());
  const auto anchor = game.Frame();
  ASSERT_TRUE(anchor);
  ExpectNear(anchor->position, VR::CameraAnchorMath::TransformPoint(VIEW, HEAD_POSITION));
  // The body faces RotY(40) * +Z: the rig is turned 40 + 180 degrees.
  ExpectNear(game.WorldRig(*anchor), RotY(220));
  EXPECT_EQ(anchor->units_per_meter, 0.0f);

  // The head's heading: its +X, which faces where the body faces.
  game.SetSetting("heading", 1);
  const auto from_head = game.Frame();
  ASSERT_TRUE(from_head);
  ExpectNear(game.WorldRig(*from_head), RotY(220));
  // Link looks 30 degrees to his left: the head's heading follows, the body's does not.
  game.memory.SetMtx(JOINTS + 4 * 0x30, Make(RotY(40 - 90 + 30), HEAD_POSITION));
  const auto looking = game.Frame();
  ASSERT_TRUE(looking);
  ExpectNear(game.WorldRig(*looking), RotY(250));
  game.SetSetting("heading", 0);
  const auto body = game.Frame();
  ASSERT_TRUE(body);
  ExpectNear(game.WorldRig(*body), RotY(220));
}

// The old behaviour on request: the game camera's orientation.
TEST(FirstPersonPatch, CameraRotation)
{
  TwilightPrincess game;
  ASSERT_TRUE(game.Load());
  game.SetSetting("rotation", 3);
  game.SetSetting("upm", 90);
  const auto anchor = game.Frame();
  ASSERT_TRUE(anchor);
  ExpectNear(anchor->rotation, RotY(0));
  EXPECT_EQ(anchor->units_per_meter, 90.0f);
}

// No anchor without Link's 35-joint model (the wolf, loading) or without his actor.
TEST(FirstPersonPatch, OnlyLinksModel)
{
  TwilightPrincess game;
  ASSERT_TRUE(game.Load());
  game.memory.Write16(MODEL_DATA + 0x2C, 41);
  EXPECT_FALSE(game.Frame());
  game.memory.Write16(MODEL_DATA + 0x2C, 35);
  game.memory.Write32(MODEL, 0x803A0000);
  EXPECT_FALSE(game.Frame());
  game.memory.Write32(MODEL, 0x803CED20);
  // The other pointer to Link's actor.
  game.memory.Write32(PLAYER, 0);
  EXPECT_TRUE(game.Frame());
  game.memory.Write32(OTHER_PLAYER, 0);
  EXPECT_FALSE(game.Frame());
  game.memory.Write32(PLAYER, ACTOR);
  EXPECT_TRUE(game.Frame());
}

// ---- The Wind Waker (Data/Sys/VRPatches/GZLE01/first-person.lua) ----

namespace
{
constexpr u32 WW_PLAYER = 0x803CA74C;
constexpr u32 WW_ACTOR = 0x80AB86BC;
constexpr u32 WW_MODEL = 0x815FEF90;
constexpr u32 WW_MODEL_DATA = 0x8153AAEC;
constexpr u32 WW_NODES = 0x815FF13C;
constexpr u32 WW_CAMERA = 0x803EDA58;

struct WindWaker
{
  WindWaker()
  {
    memory.Write32(WW_PLAYER, WW_ACTOR);
    memory.Write32(WW_ACTOR + 0x32C, WW_MODEL);
    memory.Write32(WW_MODEL, 0x8039EA50);
    memory.Write32(WW_MODEL + 4, WW_MODEL_DATA);
    memory.Write32(WW_MODEL + 0x14, WW_ACTOR);
    memory.Write16(WW_MODEL_DATA + 0x28, 42);
    // Link faces 40 degrees from +Z; his head joint's +Y faces forward, its +X up.
    memory.SetMtx(WW_MODEL + 0x24, Make(RotY(40), {100, 145, -200}));
    memory.Write32(WW_MODEL + 0x8C, WW_NODES);
    const Mat33 head = VR::CameraAnchorMath::Multiply33(RotY(40), {0, 1, 0, 0, 0, 1, 1, 0, 0});
    memory.SetMtx(WW_NODES + 15 * 0x30, Make(head, HEAD_POSITION));
    memory.SetMtx(WW_CAMERA, VIEW);
    env.memory = &memory;
    env.input = &input;
    env.game_id = "GZLE01";
    env.field = 100;
    env.units_per_meter = 100;
  }

  ::testing::AssertionResult Load()
  {
    std::string source;
    File::ReadFileToString(File::GetSysDirectory() + VRPATCHES_DIR "/GZLE01/first-person.lua",
                           source);
    if (!script.Load(source, env) || !script.Call(Hook::OnLoad, env))
      return ::testing::AssertionFailure() << script.Error();
    return ::testing::AssertionSuccess();
  }

  std::optional<VR::CameraAnchorMath::Anchor> Frame()
  {
    EXPECT_TRUE(script.Call(Hook::OnFrame, env)) << script.Error();
    return script.TakeCameraAnchor();
  }

  Memory memory;
  VRInputFrame input;
  ScriptEnvironment env;
  PatchScript script{"first-person"};
};
}  // namespace

// In Link's head, level and turned with his body, his head's draws hidden around the head joint.
TEST(WindWakerFirstPersonPatch, LevelInTheHeadWithTheHeadHidden)
{
  WindWaker game;
  ASSERT_TRUE(game.Load());
  EXPECT_EQ(game.script.GetManifest().games, std::vector<std::string>{"GZLE01"});
  const auto anchor = game.Frame();
  ASSERT_TRUE(anchor);
  ExpectNear(anchor->position, VR::CameraAnchorMath::TransformPoint(VIEW, HEAD_POSITION));
  ExpectNear(VR::CameraAnchorMath::Multiply33(Transpose(VR::CameraAnchorMath::Basis(VIEW)),
                                              anchor->rotation),
             RotY(220));
  ExpectNear(anchor->hide_center, VR::CameraAnchorMath::TransformPoint(VIEW, HEAD_POSITION));
  EXPECT_EQ(anchor->hide_radius, 40.0f);
  EXPECT_EQ(anchor->hide_below, 5.0f);
}

// No anchor unless the model is Link's: his actor's, pointing back at it, with 42 joints.
TEST(WindWakerFirstPersonPatch, OnlyLinksModel)
{
  WindWaker game;
  ASSERT_TRUE(game.Load());
  game.memory.Write32(WW_MODEL + 0x14, 0x80AB0000);
  EXPECT_FALSE(game.Frame());
  game.memory.Write32(WW_MODEL + 0x14, WW_ACTOR);
  game.memory.Write16(WW_MODEL_DATA + 0x28, 35);
  EXPECT_FALSE(game.Frame());
  game.memory.Write16(WW_MODEL_DATA + 0x28, 42);
  EXPECT_TRUE(game.Frame());
}
