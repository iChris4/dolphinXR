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
#include <string_view>
#include <utility>

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

// In the head, level, facing where Link's body faces: the camera's pitch and yaw are gone. (With
// "Link's facing": the VR controls' Fixed view does not turn with him.)
TEST(FirstPersonPatch, LevelInTheHeadWithTheBodysHeading)
{
  TwilightPrincess game;
  ASSERT_TRUE(game.Load());
  game.SetSetting("stick", 0);
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
  game.SetSetting("stick", 0);
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

// Up on the stick walks straight ahead in the view while Link moves freely, not during an event (a
// dialog) or in a menu.
TEST(FirstPersonPatch, StickWalksStraightAheadInFreePlay)
{
  TwilightPrincess game;
  ASSERT_TRUE(game.Load());
  auto anchor = game.Frame();
  ASSERT_TRUE(anchor);
  // The game camera's heading is 30 degrees, Link's 220.
  const std::optional<float> turn = VR::CameraAnchorMath::StickTurn(*anchor, nullptr);
  ASSERT_TRUE(turn);
  EXPECT_NEAR(*turn / DEG, -170.0f, 0.05f);
  for (const u32 busy : {0x8040B16Cu, 0x8040B16Du, 0x8040C075u})
  {
    SCOPED_TRACE(busy);
    game.memory.Write8(busy, 1);
    anchor = game.Frame();
    ASSERT_TRUE(anchor);
    EXPECT_FALSE(VR::CameraAnchorMath::StickTurn(*anchor, nullptr));
    game.memory.Write8(busy, 0);
  }
  anchor = game.Frame();
  ASSERT_TRUE(anchor);
  EXPECT_TRUE(VR::CameraAnchorMath::StickTurn(*anchor, nullptr));
}

// VR controls, the default: the view stays where it engaged while Link turns (Fixed), the stick's
// reference is where the headset looks, and its left and right turn the view at the turn speed.
TEST(FirstPersonPatch, VRControls)
{
  TwilightPrincess game;
  ASSERT_TRUE(game.Load());
  auto anchor = game.Frame();
  ASSERT_TRUE(anchor);
  const Mat33 engaged = game.WorldRig(*anchor);
  EXPECT_EQ(anchor->stick_turn_speed, 180.0f);
  EXPECT_TRUE(anchor->stick_head);
  EXPECT_FALSE(anchor->stick_hold);
  game.memory.SetMtx(MODEL + 0x24, Make(RotY(130), {100, 800, -200}));
  anchor = game.Frame();
  ASSERT_TRUE(anchor);
  ExpectNear(game.WorldRig(*anchor), engaged);
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

  void SetSetting(const std::string& name, double value)
  {
    ASSERT_TRUE(script.SetSettingValue(*script.GetManifest().FindSetting(name), value));
    ASSERT_TRUE(script.CallSettingChanged(name, value, env)) << script.Error();
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

// Up on the stick walks straight ahead in the view while Link moves freely; the menus and dialogs
// read it as the game does.
TEST(WindWakerFirstPersonPatch, StickWalksStraightAheadInFreePlay)
{
  constexpr u32 GAME_MODE = 0x803CA80A;
  WindWaker game;
  ASSERT_TRUE(game.Load());
  auto anchor = game.Frame();
  ASSERT_TRUE(anchor);
  // The game camera's heading is 30 degrees, Link's 220.
  const std::optional<float> turn = VR::CameraAnchorMath::StickTurn(*anchor, nullptr);
  ASSERT_TRUE(turn);
  EXPECT_NEAR(*turn / DEG, -170.0f, 0.05f);
  for (const u8 mode : {1, 7})
  {
    game.memory.Write8(GAME_MODE, mode);
    anchor = game.Frame();
    ASSERT_TRUE(anchor);
    EXPECT_FALSE(VR::CameraAnchorMath::StickTurn(*anchor, nullptr));
  }
  game.memory.Write8(GAME_MODE, 0);
  anchor = game.Frame();
  ASSERT_TRUE(anchor);
  EXPECT_TRUE(VR::CameraAnchorMath::StickTurn(*anchor, nullptr));
}

// VR controls, the default: a Fixed view that Link's turns do not move, where the headset looks
// for the stick, whose left and right turn the view at the turn speed.
TEST(WindWakerFirstPersonPatch, VRControls)
{
  WindWaker game;
  ASSERT_TRUE(game.Load());
  auto anchor = game.Frame();
  ASSERT_TRUE(anchor);
  EXPECT_EQ(anchor->stick_turn_speed, 180.0f);
  EXPECT_TRUE(anchor->stick_head);
  EXPECT_FALSE(anchor->stick_hold);
  game.SetSetting("turn_speed", 150);
  anchor = game.Frame();
  ASSERT_TRUE(anchor);
  EXPECT_EQ(anchor->stick_turn_speed, 150.0f);
  game.SetSetting("stick", 0);
  anchor = game.Frame();
  ASSERT_TRUE(anchor);
  EXPECT_EQ(anchor->stick_turn_speed, 0.0f);
  EXPECT_TRUE(anchor->stick_hold);
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

// ---- Wii Sports (Data/Sys/VRPatches/RSPE01/first-person.lua) ----

namespace
{
// [[804DFB90]] is the sport's scene; its g3d root (vtable 80361260) a little way in.
constexpr u32 WS_SCENE_HOLDER = 0x80600000;
constexpr u32 WS_SCENE = 0x80610000;
constexpr u32 WS_ROOT = WS_SCENE + 0xD4;
constexpr u32 WS_GROUP = 0x80620000;
constexpr u32 WS_ARRAY = 0x80630000;
constexpr u32 WS_BODY_MDL = 0x80640000;
constexpr u32 WS_NET_MDL = 0x80650000;
constexpr u32 WS_CROWD_MDL = 0x80660000;

struct WiiSports
{
  WiiSports()
  {
    memory.Write32(0x804DFB90, WS_SCENE_HOLDER);
    memory.Write32(WS_SCENE_HOLDER, WS_SCENE);
    memory.Write32(WS_ROOT, 0x80361260);
    memory.SetMtx(WS_ROOT + 0x6C, VIEW);
    memory.Write32(WS_ROOT + 0xB8, WS_GROUP);
    memory.Write32(WS_GROUP, 0x80361238);
    memory.Write32(WS_GROUP + 4, WS_ARRAY);
    WriteMdl0(WS_BODY_MDL, "ava_bodyA1");
    WriteMdl0(WS_NET_MDL, "tns_net_a");
    WriteMdl0(WS_CROWD_MDL, "ava_bodyA0");
    env.memory = &memory;
    env.input = &input;
    env.game_id = "RSPE01";
    env.field = 100;
    env.units_per_meter = 10;
  }

  void WriteMdl0(u32 mdl, std::string_view name)
  {
    memory.Write32(mdl, 0x4D444C30);
    memory.Write32(mdl + 0x3C, 0x100);
    for (size_t i = 0; i < name.size(); ++i)
      memory.Write8(mdl + 0x100 + static_cast<u32>(i), static_cast<u8>(name[i]));
  }

  // A model in the scene's group: its head joint (+Y where it looks, +X up) at `distance` from the
  // game camera, straight ahead of it, facing `facing_deg`; `mirror` flips it (a reflection).
  void AddModel(u32 model, u32 mdl, float distance, float facing_deg, float scale = 1,
                bool mirror = false)
  {
    memory.Write32(WS_ARRAY + count * 4, model);
    memory.Write32(WS_GROUP + 0x10, ++count);
    memory.Write32(model, 0x80361310);
    memory.Write32(model + 0xB8, mdl);
    memory.Write32(model + 0xBC, model + 0x160);
    const Mtx34 camera = *VR::CameraAnchorMath::Invert(VIEW);
    const std::array<float, 3> ahead = {-camera[2] * distance, -camera[6] * distance,
                                        -camera[10] * distance};
    const std::array<float, 3> head = {camera[3] + ahead[0], camera[7] + ahead[1],
                                       camera[11] + ahead[2]};
    memory.SetMtx(model + 0x0C, Make(RotY(facing_deg), {head[0], head[1] - 8.7f, head[2]}));
    Mat33 r = VR::CameraAnchorMath::Multiply33(RotY(facing_deg), {0, 1, 0, 0, 0, 1, 1, 0, 0});
    for (float& v : r)
      v *= scale;
    if (mirror)
    {
      for (int c = 0; c < 3; ++c)
        r[3 + c] = -r[3 + c];
    }
    memory.SetMtx(model + 0x190, Make(r, head));
    heads[model] = head;
  }

  ::testing::AssertionResult Load()
  {
    std::string source;
    File::ReadFileToString(File::GetSysDirectory() + VRPATCHES_DIR "/RSPE01/first-person.lua",
                           source);
    if (!script.Load(source, env) || !script.Call(Hook::OnLoad, env))
      return ::testing::AssertionFailure() << script.Error();
    return ::testing::AssertionSuccess();
  }

  std::optional<VR::CameraAnchorMath::Anchor> Frame()
  {
    ++env.field;
    EXPECT_TRUE(script.Call(Hook::OnFrame, env)) << script.Error();
    return script.TakeCameraAnchor();
  }

  Memory memory;
  VRInputFrame input;
  ScriptEnvironment env;
  PatchScript script{"first-person"};
  u32 count = 0;
  std::map<u32, std::array<float, 3>> heads;
};
}  // namespace

// In the head of the Mii nearest the game camera (yours: every sport keeps the camera behind you),
// level and turned with its body; the reflection, the other models and the farther Miis are not
// used.
TEST(WiiSportsFirstPersonPatch, InTheHeadOfTheNearestMii)
{
  WiiSports game;
  game.AddModel(0x80700000, WS_BODY_MDL, 120, 0);
  game.AddModel(0x80710000, WS_NET_MDL, 10, 0);
  game.AddModel(0x80720000, WS_BODY_MDL, 15, 0, 1, true);
  game.AddModel(0x80730000, WS_BODY_MDL, 30, 40);
  ASSERT_TRUE(game.Load());
  EXPECT_EQ(game.script.GetManifest().games, std::vector<std::string>{"RSPE01"});
  const auto anchor = game.Frame();
  ASSERT_TRUE(anchor);
  ExpectNear(anchor->position, VR::CameraAnchorMath::TransformPoint(VIEW, game.heads[0x80730000]));
  ExpectNear(VR::CameraAnchorMath::Multiply33(Transpose(VR::CameraAnchorMath::Basis(VIEW)),
                                              anchor->rotation),
             RotY(220));
  // Eyes 1.6 m above the feet of a full-size Mii, and its head hidden.
  EXPECT_NEAR(anchor->units_per_meter, 8.7f / 1.6f, 1e-4f);
  EXPECT_EQ(anchor->hide_radius, 5.0f);
  EXPECT_EQ(anchor->hide_below, 1.0f);
}

// Tennis's serve camera is nearer a spectator on the side line (ava_bodyA0) than your Mii: the
// players' bodies (A1, B1, Boxing's C0) come first, the others only when no player is in front.
TEST(WiiSportsFirstPersonPatch, PlayersBeforeSpectators)
{
  WiiSports game;
  game.AddModel(0x80700000, WS_CROWD_MDL, 20, 0);
  game.AddModel(0x80710000, WS_BODY_MDL, 30, 40);
  ASSERT_TRUE(game.Load());
  auto anchor = game.Frame();
  ASSERT_TRUE(anchor);
  ExpectNear(anchor->position, VR::CameraAnchorMath::TransformPoint(VIEW, game.heads[0x80710000]));

  // Your Mii behind the camera (moved, listed twice): the spectator.
  game.AddModel(0x80710000, WS_BODY_MDL, -30, 40);
  anchor = game.Frame();
  ASSERT_TRUE(anchor);
  ExpectNear(anchor->position, VR::CameraAnchorMath::TransformPoint(VIEW, game.heads[0x80700000]));
}

// Bowling draws its Miis at 0.7: the world's scale and the hidden region follow.
TEST(WiiSportsFirstPersonPatch, ScaledMii)
{
  WiiSports game;
  game.AddModel(0x80700000, WS_BODY_MDL, 30, 0, 0.7f);
  ASSERT_TRUE(game.Load());
  game.Frame();
  const auto anchor = game.Frame();
  ASSERT_TRUE(anchor);
  EXPECT_NEAR(anchor->units_per_meter, 8.7f * 0.7f / 1.6f, 1e-4f);
  EXPECT_NEAR(anchor->hide_radius, 3.5f, 1e-5f);
  EXPECT_NEAR(anchor->hide_below, 0.7f, 1e-5f);
}

// No scene, or a scene without Miis: no anchor.
TEST(WiiSportsFirstPersonPatch, NoMii)
{
  WiiSports game;
  ASSERT_TRUE(game.Load());
  EXPECT_FALSE(game.Frame());
  game.AddModel(0x80710000, WS_NET_MDL, 10, 0);
  game.env.field += 400;
  EXPECT_FALSE(game.Frame());
  game.memory.Write32(0x804DFB90, 0);
  EXPECT_FALSE(game.Frame());
}

// ---- Shadow the Hedgehog: Reloaded (Data/Sys/VRPatches/GUP/first-person.lua) ----

namespace
{
constexpr u32 SH_HOLDER = 0x807D71A0;
constexpr u32 SH_HOLDER_TARGET = 0x807D2DD8;
constexpr u32 SH_OTHER_HOLDER = 0x805EF324;
constexpr u32 SH_OTHER_TARGET = 0x81013098;
constexpr u32 SH_PLAYER = 0x80F44128;
constexpr u32 SH_MOTION = 0x80F40AB0;
constexpr u32 SH_SKELETON = 0x812DE240;
constexpr u32 SH_BONES = SH_SKELETON + 0x160;
constexpr u32 SH_CAMERAS = 0x8056EE14;
constexpr u32 SH_CAMERA_LIST = 0x807D1768;
constexpr u32 SH_IDLE_CAMERA = 0x807D7858;
constexpr u32 SH_CAMERA = 0x807D76A8;
constexpr u32 SH_CONTROLS_HELD = 0x8057D768;
constexpr u32 SH_PAUSE_MENU = 0x8057D724;
constexpr u32 SH_HOOK = 0x80015774;
constexpr u32 SH_HOOK_ORIGINAL = 0x48000C1D;  // bl glxCopyMatrix
// The other hooked calls: {address, original word, cave entry}.
constexpr std::array<std::array<u32, 3>, 5> SH_OTHER_HOOKS = {{{0x802A5590, 0x4BD70E01, 0},
                                                               {0x802A5804, 0x4BD70B8D, 0},
                                                               {0x800B60CC, 0x4BFA3125, 1},
                                                               {0x800B0EEC, 0x4835B25D, 2},
                                                               {0x8023AA48, 0x4BE6FD19, 3}}};
constexpr u32 SH_SCRATCH = 0x80002C20;
constexpr u32 SH_SCRATCH_SIZE = 0x3D0;
constexpr u32 SH_ROOT_POINTER = SH_SCRATCH + 0x18;
constexpr u32 SH_MOVE_COUNT = SH_SCRATCH + 0x1C;
constexpr u32 SH_TABLE = SH_SCRATCH + 0x78;
constexpr u32 SH_CAVE = SH_TABLE + 2 * 0x34;
constexpr u32 SH_STATUS = SH_PLAYER + 0xAC;
// The right glove: a 7-bone skeleton, one of its bones in the motion controller's records.
constexpr u32 SH_RIGHT_GLOVE = 0x812F24E0;
constexpr u32 SH_RIGHT_GLOVE_BONES = SH_RIGHT_GLOVE + 0x160;
constexpr u32 SH_LEFT_HAND = SH_BONES + 25 * 0x70;

// Shadow's feet, facing 40 degrees from -Z (he faces his bones' -Z), and his head joint at his
// neck, 5.8 units above.
const std::array<float, 3> SH_ROOT = {21.6f, -4730.0f, -2651.9f};
const std::array<float, 3> SH_HEAD = {21.8f, -4724.2f, -2652.1f};

struct Shadow
{
  Shadow()
  {
    memory.Write32(0x80270840, 0x387F003C);
    memory.Write32(0x80270848, 0x4819B901);
    memory.Write32(0x80015768, 0x80860004);
    memory.Write32(SH_HOOK, SH_HOOK_ORIGINAL);
    memory.Write32(0x80015778, 0x4800001C);
    memory.Write32(0x802A558C, 0x38650010);
    memory.Write32(0x802A5800, 0x38650010);
    memory.Write32(0x800B60C8, 0x80840000);
    memory.Write32(0x800B60D0, 0x80810008);
    memory.Write32(0x800B0EE8, 0x809E0000);
    memory.Write32(0x800B0EF4, 0x386100C0);
    memory.Write32(0x8023AA30, 0x38800005);
    memory.Write32(0x8023AA44, 0x80630230);
    memory.Write32(0x8023AA4C, 0x7C7F1B78);
    for (const auto& hook : SH_OTHER_HOOKS)
      memory.Write32(hook[0], hook[1]);
    memory.Write32(SH_STATUS, 0x40000000);  // on the ground
    // The motion controller's records: his head joint, a hand and the right glove's bone 3.
    memory.Write32(SH_MOTION + 0xD0 + 2 * 0x38, SH_BONES + 12 * 0x70);
    memory.Write32(SH_MOTION + 0xD0 + 3 * 0x38, SH_BONES + 29 * 0x70);
    memory.Write32(SH_MOTION + 0xD0 + 4 * 0x38, SH_RIGHT_GLOVE_BONES + 3 * 0x70);
    memory.Write32(SH_RIGHT_GLOVE, 7);
    memory.Write32(SH_RIGHT_GLOVE + 4, SH_RIGHT_GLOVE_BONES);
    memory.Write32(SH_HOLDER, SH_HOLDER_TARGET);
    memory.Write32(SH_HOLDER_TARGET, SH_PLAYER);
    memory.Write32(SH_OTHER_HOLDER, SH_OTHER_TARGET);
    memory.Write32(SH_OTHER_TARGET + 0x60, SH_PLAYER);
    memory.Write32(SH_PLAYER + 0x3C, 0x53686164);  // "Shadow"
    memory.Write32(SH_PLAYER + 0x40, 0x6F770000);
    memory.Write32(SH_PLAYER + 0x230, SH_MOTION);
    memory.Write32(SH_MOTION + 0x3E8, SH_SKELETON);
    memory.Write32(SH_SKELETON, 30);
    memory.Write32(SH_SKELETON + 4, SH_BONES);
    memory.SetMtx(SH_BONES, Make(RotY(40), SH_ROOT));
    // The head nods 20 degrees: the view stays level.
    memory.SetMtx(SH_BONES + 12 * 0x70,
                  Make(VR::CameraAnchorMath::Multiply33(RotY(40), RotX(-20)), SH_HEAD));
    // Two cameras, the first idle.
    memory.Write32(SH_CAMERAS + 8, 2);
    memory.Write32(SH_CAMERAS + 0xC, SH_CAMERA_LIST);
    memory.Write32(SH_CAMERA_LIST, SH_IDLE_CAMERA);
    memory.Write32(SH_CAMERA_LIST + 8, SH_CAMERA);
    memory.Write8(SH_CAMERA, 1);
    memory.SetMtx(SH_IDLE_CAMERA + 0x3C, Make(RotY(90), {0, 0, 0}));
    memory.SetMtx(SH_CAMERA + 0x3C, VIEW);
    env.memory = &memory;
    env.input = &input;
    env.game_id = "GUPR8P";
    env.field = 100;
    env.units_per_meter = 30;
    env.scratch_base = SH_SCRATCH;
    env.scratch_size = SH_SCRATCH_SIZE;
    env.camera_anchor_active = true;
  }

  // A range of bones the cave hides (0 his, 1 his right glove): the first one's and the end's
  // addresses, 0 when it is off.
  std::pair<u32, u32> Hidden(u32 range) const
  {
    return {memory.Read32(SH_SCRATCH + range * 8).value_or(1),
            memory.Read32(SH_SCRATCH + range * 8 + 4).value_or(1)};
  }
  std::pair<u32, u32> HiddenHead() const { return Hidden(0); }
  std::pair<u32, u32> HiddenItem() const { return Hidden(1); }

  // A matrix of the cave's table (12 floats after the key).
  Mtx34 TableMatrix(u32 entry) const
  {
    Mtx34 m{};
    for (u32 i = 0; i < 12; ++i)
    {
      m[i] = std::bit_cast<float>(memory.Read32(SH_TABLE + entry * 0x34 + 4 + i * 4).value_or(0));
    }
    return m;
  }

  ::testing::AssertionResult Load()
  {
    std::string source;
    File::ReadFileToString(File::GetSysDirectory() + VRPATCHES_DIR "/GUP/first-person.lua", source);
    if (!script.Load(source, env) || !script.Call(Hook::OnLoad, env))
      return ::testing::AssertionFailure() << script.Error();
    return ::testing::AssertionSuccess();
  }

  std::optional<VR::CameraAnchorMath::Anchor> Frame()
  {
    EXPECT_TRUE(script.Call(Hook::OnFrame, env)) << script.Error();
    ++env.field;
    return script.TakeCameraAnchor();
  }

  void SetSetting(const std::string& name, double value)
  {
    ASSERT_TRUE(script.SetSettingValue(*script.GetManifest().FindSetting(name), value));
    ASSERT_TRUE(script.CallSettingChanged(name, value, env)) << script.Error();
  }

  bool Warned(std::string_view text)
  {
    bool warned = false;
    for (const LogLine& line : script.TakeLog())
      warned |= line.level == 1 && line.text.find(text) != std::string::npos;
    return warned;
  }

  // `up` units above `from` and `forward` units ahead of Shadow, in the camera's view.
  static std::array<float, 3> Eyes(const std::array<float, 3>& from, float up, float forward)
  {
    const Mat33 facing = RotY(40);
    // His forward is the rig's -Z: (0, up, -forward) turned with him.
    const std::array<float, 3> offset = {facing[1] * up - facing[2] * forward,
                                         facing[4] * up - facing[5] * forward,
                                         facing[7] * up - facing[8] * forward};
    return VR::CameraAnchorMath::TransformPoint(
        VIEW, {from[0] + offset[0], from[1] + offset[1], from[2] + offset[2]});
  }

  Memory memory;
  VRInputFrame input;
  ScriptEnvironment env;
  PatchScript script{"first-person"};
};

void ExpectNearPosition(const std::array<float, 3>& actual, const std::array<float, 3>& expected)
{
  for (size_t i = 0; i < 3; ++i)
  {
    SCOPED_TRACE(i);
    EXPECT_NEAR(actual[i], expected[i], 0.01f);
  }
}
}  // namespace

// At his eyes above his feet, level and turned with him, true to scale, from the active camera.
TEST(ShadowFirstPersonPatch, LevelAtTheEyesAboveHisFeet)
{
  Shadow game;
  ASSERT_TRUE(game.Load());
  EXPECT_EQ(game.script.GetManifest().games, (std::vector<std::string>{"GUPE8P", "GUPR8P"}));
  const auto anchor = game.Frame();
  ASSERT_TRUE(anchor);
  ExpectNearPosition(anchor->position, Shadow::Eyes(SH_ROOT, 6.7f, 2.2f));
  ExpectNear(VR::CameraAnchorMath::Multiply33(Transpose(VR::CameraAnchorMath::Basis(VIEW)),
                                              anchor->rotation),
             RotY(40));
  EXPECT_EQ(anchor->units_per_meter, 10.0f);
  EXPECT_TRUE(anchor->stick_turn);
}

// Following his head: from the head joint, still level.
TEST(ShadowFirstPersonPatch, FollowsHisHead)
{
  Shadow game;
  ASSERT_TRUE(game.Load());
  game.SetSetting("follow", 1);
  const auto anchor = game.Frame();
  ASSERT_TRUE(anchor);
  ExpectNearPosition(anchor->position, Shadow::Eyes(SH_HEAD, 0.9f, 1.6f));
  ExpectNear(VR::CameraAnchorMath::Multiply33(Transpose(VR::CameraAnchorMath::Basis(VIEW)),
                                              anchor->rotation),
             RotY(40));
}

// The cave that collapses his bones is hooked into the five calls that hand them to RenderWare
// and the weapon's call for his hand's record; it hides bones 12 to 21 only while the view is in
// his head, and the game's calls come back when the patch stops.
TEST(ShadowFirstPersonPatch, HidesHisHeadWhileTheViewIsInIt)
{
  Shadow game;
  ASSERT_TRUE(game.Load());
  EXPECT_EQ(game.memory.Read32(SH_HOOK).value_or(0),
            0x48000001u | ((SH_CAVE - SH_HOOK) & 0x03FFFFFCu));
  for (const auto& hook : SH_OTHER_HOOKS)
  {
    const u32 entry = SH_CAVE + hook[2] * 16;
    EXPECT_EQ(game.memory.Read32(hook[0]).value_or(0),
              0x48000001u | ((entry - hook[0]) & 0x03FFFFFCu));
  }
  EXPECT_NE(game.memory.Read32(SH_CAVE).value_or(0), 0u);
  const std::pair<u32, u32> head = {SH_BONES + 12 * 0x70, SH_BONES + 22 * 0x70};
  const std::pair<u32, u32> off = {0, 0};
  EXPECT_TRUE(game.Frame());
  EXPECT_EQ(game.HiddenHead(), head);

  // Camera Anchor off, the setting off, or an event in the game's camera: the head shows.
  game.env.camera_anchor_active = false;
  game.Frame();
  EXPECT_EQ(game.HiddenHead(), off);
  game.env.camera_anchor_active = true;
  game.SetSetting("hide_head", 0);
  game.Frame();
  EXPECT_EQ(game.HiddenHead(), off);
  game.SetSetting("hide_head", 1);
  game.memory.Write8(SH_CONTROLS_HELD, 1);
  EXPECT_FALSE(game.Frame());
  EXPECT_EQ(game.HiddenHead(), off);
  game.memory.Write8(SH_CONTROLS_HELD, 0);
  game.Frame();
  EXPECT_EQ(game.HiddenHead(), head);

  // A savestate made without the patch: the hook comes back.
  game.memory.Write32(SH_HOOK, SH_HOOK_ORIGINAL);
  ASSERT_TRUE(game.script.Call(Hook::OnStateLoaded, game.env)) << game.script.Error();
  EXPECT_NE(game.memory.Read32(SH_HOOK).value_or(0), SH_HOOK_ORIGINAL);

  ASSERT_TRUE(game.script.Call(Hook::OnUnload, game.env)) << game.script.Error();
  EXPECT_EQ(game.memory.Read32(SH_HOOK).value_or(0), SH_HOOK_ORIGINAL);
  for (const auto& hook : SH_OTHER_HOOKS)
    EXPECT_EQ(game.memory.Read32(hook[0]).value_or(0), hook[1]);
  EXPECT_EQ(game.HiddenHead(), off);
}

// In the air his whole body is hidden, with his right glove (found from the motion controller's
// records); on the ground again, only his head. "Always" hides his body on the ground too, "Never"
// not in the air either.
TEST(ShadowFirstPersonPatch, HidesHisBodyInTheAir)
{
  Shadow game;
  ASSERT_TRUE(game.Load());
  const std::pair<u32, u32> head = {SH_BONES + 12 * 0x70, SH_BONES + 22 * 0x70};
  const std::pair<u32, u32> body = {SH_BONES, SH_BONES + 30 * 0x70};
  const std::pair<u32, u32> right_glove = {SH_RIGHT_GLOVE_BONES, SH_RIGHT_GLOVE_BONES + 7 * 0x70};
  const std::pair<u32, u32> off = {0, 0};
  game.Frame();
  EXPECT_EQ(game.HiddenHead(), head);
  EXPECT_EQ(game.HiddenItem(), off);

  game.memory.Write32(SH_STATUS, 0x00200000);  // jumping
  game.Frame();
  EXPECT_EQ(game.HiddenHead(), body);
  EXPECT_EQ(game.HiddenItem(), right_glove);

  // No glove found: his body only.
  game.memory.Write32(SH_MOTION + 0xD0 + 4 * 0x38, 0);
  game.Frame();
  EXPECT_EQ(game.HiddenHead(), body);
  EXPECT_EQ(game.HiddenItem(), off);

  game.memory.Write32(SH_STATUS, 0x40000000);
  game.Frame();
  EXPECT_EQ(game.HiddenHead(), head);
  game.SetSetting("hide_body", 2);
  game.Frame();
  EXPECT_EQ(game.HiddenHead(), body);
  game.SetSetting("hide_body", 1);
  game.memory.Write32(SH_STATUS, 0x00200000);
  game.Frame();
  EXPECT_EQ(game.HiddenHead(), head);
}

// With a controller, his left hand (a piece on bone 25) is drawn at the controller, and the weapon
// it holds asks for his hand's record (keyed by his motion controller) and gets the controller's
// pose: the cave's table holds both relative to his root. His left forearm is hidden with his head;
// in the air the hand and weapon stay shown. Without the controller, or with the setting off, the
// weapon stays in his hand.
TEST(ShadowFirstPersonPatch, GunInYourHand)
{
  Shadow game;
  ASSERT_TRUE(game.Load());
  PoseInput& pose = game.input.hands[1].view;
  pose.valid = true;
  pose.position = {0.2f, -0.3f, -0.5f};
  const std::pair<u32, u32> head_and_arm = {SH_BONES + 12 * 0x70, SH_BONES + 26 * 0x70};
  const std::pair<u32, u32> head = {SH_BONES + 12 * 0x70, SH_BONES + 22 * 0x70};
  const std::pair<u32, u32> off = {0, 0};
  game.Frame();
  ASSERT_EQ(game.memory.Read32(SH_MOVE_COUNT).value_or(0), 2u);
  EXPECT_EQ(game.memory.Read32(SH_ROOT_POINTER).value_or(0), SH_BONES);
  EXPECT_EQ(game.memory.Read32(SH_TABLE).value_or(0), SH_LEFT_HAND + 1);
  EXPECT_EQ(game.memory.Read32(SH_TABLE + 0x34).value_or(0), SH_MOTION);
  EXPECT_EQ(game.HiddenHead(), head_and_arm);

  // Drawn at the root times the table's matrix: the hand at the controller, 10 units per meter,
  // turned by the grip; the weapon's record the same.
  const Mtx34 root = Make(RotY(40), SH_ROOT);
  const Mtx34 to_world = *VR::CameraAnchorMath::Invert(VIEW);
  const auto controller = VR::CameraAnchorMath::TransformPoint(to_world, {2.0f, -3.0f, -5.0f});
  for (u32 entry = 0; entry < 2; ++entry)
  {
    SCOPED_TRACE(entry);
    const Mtx34 hand = VR::CameraAnchorMath::Multiply(root, game.TableMatrix(entry));
    ExpectNearPosition(VR::CameraAnchorMath::Translation(hand), controller);
    ExpectNear(VR::CameraAnchorMath::Basis(hand),
               VR::CameraAnchorMath::Multiply33(VR::CameraAnchorMath::Basis(to_world),
                                                {0.9934f, -0.0673f, -0.0984f, -0.0844f, -0.9817f,
                                                 -0.1775f, -0.0848f, 0.1850f, -0.9798f}));
  }

  // In the air: his body and the right glove are hidden; the hand and weapon stay moved.
  game.memory.Write32(SH_STATUS, 0x00200000);
  game.Frame();
  EXPECT_EQ(game.HiddenHead(), (std::pair<u32, u32>{SH_BONES, SH_BONES + 30 * 0x70}));
  EXPECT_EQ(game.HiddenItem(),
            (std::pair<u32, u32>{SH_RIGHT_GLOVE_BONES, SH_RIGHT_GLOVE_BONES + 7 * 0x70}));
  EXPECT_EQ(game.memory.Read32(SH_MOVE_COUNT).value_or(0), 2u);
  game.memory.Write32(SH_STATUS, 0x40000000);

  pose.valid = false;
  game.Frame();
  EXPECT_EQ(game.memory.Read32(SH_MOVE_COUNT).value_or(1), 0u);
  EXPECT_EQ(game.HiddenHead(), head);
  pose.valid = true;
  game.SetSetting("gun", 0);
  game.Frame();
  EXPECT_EQ(game.memory.Read32(SH_MOVE_COUNT).value_or(1), 0u);
  EXPECT_EQ(game.HiddenHead(), head);
  game.SetSetting("gun", 1);
  game.Frame();
  EXPECT_EQ(game.memory.Read32(SH_MOVE_COUNT).value_or(0), 2u);

  // Camera Anchor off: nothing moved or hidden.
  game.env.camera_anchor_active = false;
  game.Frame();
  EXPECT_EQ(game.memory.Read32(SH_MOVE_COUNT).value_or(1), 0u);
  EXPECT_EQ(game.HiddenHead(), off);
}

// VR controls, the default: a Fixed view that his turns do not move, where the headset looks for
// the stick, whose left and right turn the view at the turn speed.
TEST(ShadowFirstPersonPatch, VRControls)
{
  Shadow game;
  ASSERT_TRUE(game.Load());
  auto anchor = game.Frame();
  ASSERT_TRUE(anchor);
  const Mat33 engaged = VR::CameraAnchorMath::Multiply33(
      Transpose(VR::CameraAnchorMath::Basis(VIEW)), anchor->rotation);
  ExpectNear(engaged, RotY(40));
  EXPECT_EQ(anchor->stick_turn_speed, 180.0f);
  EXPECT_TRUE(anchor->stick_head);
  EXPECT_FALSE(anchor->stick_hold);
  game.memory.SetMtx(SH_BONES, Make(RotY(130), SH_ROOT));
  anchor = game.Frame();
  ASSERT_TRUE(anchor);
  ExpectNear(VR::CameraAnchorMath::Multiply33(Transpose(VR::CameraAnchorMath::Basis(VIEW)),
                                              anchor->rotation),
             engaged);
}

// While the game holds the controls: the pause menu keeps the view without turning the stick;
// events go back to the game's camera unless the setting keeps first person.
TEST(ShadowFirstPersonPatch, PauseMenuAndEvents)
{
  Shadow game;
  ASSERT_TRUE(game.Load());
  game.memory.Write8(SH_CONTROLS_HELD, 1);
  game.memory.Write8(SH_PAUSE_MENU, 1);
  auto anchor = game.Frame();
  ASSERT_TRUE(anchor);
  EXPECT_FALSE(anchor->stick_turn);

  game.memory.Write8(SH_PAUSE_MENU, 0);
  EXPECT_FALSE(game.Frame());
  game.SetSetting("events", 1);
  anchor = game.Frame();
  ASSERT_TRUE(anchor);
  EXPECT_FALSE(anchor->stick_turn);

  game.memory.Write8(SH_CONTROLS_HELD, 0);
  anchor = game.Frame();
  ASSERT_TRUE(anchor);
  EXPECT_TRUE(anchor->stick_turn);
}

// Only Shadow's 30-bone skeleton, through the second path when the first is gone, and only with
// an active camera.
TEST(ShadowFirstPersonPatch, OnlyShadowsSkeleton)
{
  Shadow game;
  ASSERT_TRUE(game.Load());
  game.memory.Write32(SH_SKELETON, 29);
  EXPECT_FALSE(game.Frame());
  game.memory.Write32(SH_SKELETON, 30);
  game.memory.Write32(SH_PLAYER + 0x40, 0x6F770001);
  EXPECT_FALSE(game.Frame());
  game.memory.Write32(SH_PLAYER + 0x40, 0x6F770000);

  game.memory.Write32(SH_HOLDER, 0);
  EXPECT_TRUE(game.Frame());
  game.memory.Write32(SH_OTHER_TARGET + 0x60, 0);
  EXPECT_FALSE(game.Frame());
  game.memory.Write32(SH_HOLDER, SH_HOLDER_TARGET);

  game.memory.Write8(SH_CAMERA, 0);
  EXPECT_FALSE(game.Frame());
  game.memory.Write8(SH_CAMERA, 1);
  game.memory.Write32(SH_CAMERAS + 8, 0);
  EXPECT_FALSE(game.Frame());
  EXPECT_FALSE(game.Warned(""));
}

TEST(ShadowFirstPersonPatch, RefusesOtherCode)
{
  Shadow game;
  game.memory.Write32(0x80270840, 0x60000000);
  ASSERT_TRUE(game.Load());
  EXPECT_FALSE(game.Frame());
  EXPECT_TRUE(game.Warned("not installed"));
  EXPECT_EQ(game.memory.Read32(SH_HOOK).value_or(0), SH_HOOK_ORIGINAL);
}

// ---- Shadow the Hedgehog: wide culling (Data/Sys/VRPatches/GUP/wide-culling.lua) ----

namespace
{
constexpr u32 SC_HOOK = 0x804852B4;
constexpr u32 SC_HOOK_ORIGINAL = 0x4BFFF015;  // bl 804842C8, the frustum builder
constexpr u32 SC_SCRATCH = 0x80002A50;
constexpr u32 SC_SCRATCH_SIZE = 0x1D0;
constexpr u32 SC_ARMED = SC_SCRATCH;
constexpr u32 SC_CAMERA_AT = SC_SCRATCH + 0x10;
constexpr u32 SC_PLANES = SC_SCRATCH + 0x20;
constexpr u32 SC_CORNERS = SC_SCRATCH + 0x98;

// Shadow's level with the wide-culling patch: the camera manager and view of the first-person
// fixture, and the code around the hook.
struct ShadowCulling
{
  ShadowCulling()
  {
    constexpr std::array<std::array<u32, 2>, 8> code = {{{0x804852A4, 0x2C000001},
                                                         {0x804852AC, 0x4BFFF801},
                                                         {0x804852B0, 0x7FE3FB78},
                                                         {0x804852B8, 0x48000010},
                                                         {0x804852D4, 0x4BFFE1C9},
                                                         {0x804842C8, 0x9421FF70},
                                                         {0x8048431C, 0x80A30004},
                                                         {0x80484578, 0x907E0094}}};
    for (const auto& word : code)
      game.memory.Write32(word[0], word[1]);
    game.memory.Write32(SC_HOOK, SC_HOOK_ORIGINAL);
    game.env.scratch_base = SC_SCRATCH;
    game.env.scratch_size = SC_SCRATCH_SIZE;
  }

  ::testing::AssertionResult Load()
  {
    std::string source;
    File::ReadFileToString(File::GetSysDirectory() + VRPATCHES_DIR "/GUP/wide-culling.lua", source);
    if (!script.Load(source, game.env) || !script.Call(Hook::OnLoad, game.env))
      return ::testing::AssertionFailure() << script.Error();
    return ::testing::AssertionSuccess();
  }

  void Frame()
  {
    EXPECT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
    ++game.env.field;
  }

  void SetSetting(const std::string& name, double value)
  {
    ASSERT_TRUE(script.SetSettingValue(*script.GetManifest().FindSetting(name), value));
  }

  float F32(u32 address) const
  {
    return std::bit_cast<float>(game.memory.Read32(address).value_or(0));
  }

  std::array<float, 3> Point(u32 address) const
  {
    return {F32(address), F32(address + 4), F32(address + 8)};
  }

  // A plane of the patch's frustum: its normal and distance.
  std::array<float, 3> Normal(u32 index) const { return Point(SC_PLANES + index * 20); }
  float Distance(u32 index) const { return F32(SC_PLANES + index * 20 + 12); }

  Shadow game;
  PatchScript script{"wide-culling"};
};

std::array<float, 3> Along(const std::array<float, 3>& from, const std::array<float, 3>& axis,
                           float k)
{
  return {from[0] + axis[0] * k, from[1] + axis[1] * k, from[2] + axis[2] * k};
}

float Dot3(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
}  // namespace

// The builder's call goes to the patch's routine in the scratch memory, and comes back when the
// patch stops.
TEST(ShadowWideCullingPatch, HooksTheFrustumAndRestores)
{
  ShadowCulling cull;
  ASSERT_TRUE(cull.Load());
  EXPECT_EQ(cull.script.GetManifest().games, (std::vector<std::string>{"GUPE8P", "GUPR8P"}));
  const u32 word = cull.game.memory.Read32(SC_HOOK).value_or(0);
  EXPECT_EQ(word & 0xFC000003, 0x48000001u);
  const u32 routine = SC_HOOK + ((word & 0x03FFFFFC) | 0xFC000000);
  EXPECT_GE(routine, SC_SCRATCH);
  EXPECT_LT(routine, SC_SCRATCH + SC_SCRATCH_SIZE);
  ASSERT_TRUE(cull.script.Call(Hook::OnUnload, cull.game.env)) << cull.script.Error();
  EXPECT_EQ(cull.game.memory.Read32(SC_HOOK).value_or(0), SC_HOOK_ORIGINAL);
  EXPECT_EQ(cull.game.memory.Read32(SC_ARMED).value_or(1), 0u);
}

// The frustum is at the headset's eyes (the "view" pose at the anchor's scale, turned to the world
// by the game camera's view), its apex 30 units behind them: the far plane looks where the eyes
// look, the sides open 65 degrees. Not following the head, the game camera's.
TEST(ShadowWideCullingPatch, FrustumAtTheEyes)
{
  ShadowCulling cull;
  ASSERT_TRUE(cull.Load());
  const Mtx34 to_world = *VR::CameraAnchorMath::Invert(VIEW);
  const auto column = [&](u32 c) {
    return std::array<float, 3>{to_world[c], to_world[4 + c], to_world[8 + c]};
  };
  // The head turned 90 degrees to the right, 0.2 m right and 0.5 m ahead of the camera, at 10
  // units per meter.
  const float s = std::sqrt(0.5f);
  cull.game.input.head_view = {true, {0.2f, 0.0f, -0.5f}, {0.0f, -s, 0.0f, s}};
  cull.game.input.view_units_per_meter = 10.0f;
  cull.Frame();
  EXPECT_EQ(cull.game.memory.Read32(SC_ARMED).value_or(0), 1u);
  ExpectNear(cull.Point(SC_CAMERA_AT), column(3));
  // Turned right: the eyes look along the camera's right (+X), their right is its back (+Z).
  const std::array<float, 3> forward = column(0);
  const std::array<float, 3> back = Along({0, 0, 0}, forward, -1);
  const std::array<float, 3> right = column(2);
  const std::array<float, 3> apex =
      Along(VR::CameraAnchorMath::TransformPoint(to_world, {2.0f, 0.0f, -5.0f}), back, 30);
  ExpectNear(cull.Normal(0), forward);
  EXPECT_NEAR(cull.Distance(0), Dot3(forward, apex) + 30000, 0.5f);
  ExpectNear(cull.Normal(1), back);
  EXPECT_NEAR(cull.Distance(1), Dot3(back, apex), 0.05f);
  const float c = std::cos(65 * DEG), sn = std::sin(65 * DEG);
  const std::array<float, 3> right_normal = {
      c * right[0] + sn * back[0], c * right[1] + sn * back[1], c * right[2] + sn * back[2]};
  ExpectNear(cull.Normal(2), right_normal);
  EXPECT_NEAR(cull.Distance(2), Dot3(right_normal, apex), 0.05f);
  // closestX/Y/Z: 1 where the normal's component is not negative (those far from 0 checked).
  const u32 closest = cull.game.memory.Read32(SC_PLANES + 2 * 20 + 16).value_or(0);
  for (u32 i = 0; i < 3; ++i)
  {
    if (std::abs(right_normal[i]) > 1e-3f)
      EXPECT_EQ((closest >> (24 - 8 * i)) & 0xFF, right_normal[i] >= 0 ? 1u : 0u) << i;
  }
  // The corners: four at the apex, four on the far plane.
  ExpectNear(cull.Point(SC_CORNERS), apex);
  EXPECT_NEAR(Dot3(forward, cull.Point(SC_CORNERS + 4 * 12)), cull.Distance(0), 0.5f);

  // Not following the head: the game camera.
  cull.SetSetting("follow_head", 0);
  cull.Frame();
  ExpectNear(cull.Normal(1), column(2));
  EXPECT_NEAR(cull.Distance(1), Dot3(column(2), Along(column(3), column(2), 30)), 0.05f);
  // No culling: nothing is ever outside a plane.
  cull.SetSetting("mode", 1);
  cull.Frame();
  for (u32 i = 0; i < 6; ++i)
    EXPECT_GT(cull.Distance(i), 1e29f) << i;

  // A stage loading (no active camera), or no camera in the list: the game's own frustum.
  cull.game.memory.Write8(SH_CAMERA, 0);
  cull.Frame();
  EXPECT_EQ(cull.game.memory.Read32(SC_ARMED).value_or(1), 0u);
  cull.game.memory.Write8(SH_CAMERA, 1);
  cull.Frame();
  EXPECT_EQ(cull.game.memory.Read32(SC_ARMED).value_or(0), 1u);
  cull.game.memory.Write32(SH_CAMERAS + 8, 0);
  cull.Frame();
  EXPECT_EQ(cull.game.memory.Read32(SC_ARMED).value_or(1), 0u);
}

TEST(ShadowWideCullingPatch, RefusesOtherCode)
{
  ShadowCulling cull;
  cull.game.memory.Write32(0x804842C8, 0x60000000);
  ASSERT_TRUE(cull.Load());
  cull.Frame();
  EXPECT_EQ(cull.game.memory.Read32(SC_HOOK).value_or(0), SC_HOOK_ORIGINAL);
  bool warned = false;
  for (const LogLine& line : cull.script.TakeLog())
    warned |= line.level == 1 && line.text.find("not installed") != std::string::npos;
  EXPECT_TRUE(warned);
}
