// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The shipped arm cannon patches against fake memory: Metroid Prime
// (Data/Sys/VRPatches/GM8E01/arm-cannon.lua: its hooks, its arming, and its cannon basis compared
// with PrimedGun's math), Metroid Prime 2: Echoes (G2ME01) and Metroid Prime 3: Corruption
// (RM3E01): their hook, their cave and the pose they feed the cave.

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <string>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Core/VRPatch/MemoryAccess.h"
#include "Core/VRPatch/PatchScript.h"

using namespace VRPatch;

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

  u32 Get32(u32 a) const { return Read32(a).value_or(0); }
  float GetF32(u32 a) const { return std::bit_cast<float>(Get32(a)); }
  void SetF32(u32 a, float v) { Write32(a, std::bit_cast<u32>(v)); }
  void SetMatrix(u32 a, const std::array<float, 12>& m)
  {
    for (size_t i = 0; i < 12; ++i)
      SetF32(a + static_cast<u32>(i) * 4, m[i]);
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

constexpr u32 SCRATCH = 0x80002DC0;
constexpr u32 ROTATION_HOOK = 0x80040FEC;
constexpr u32 MODEL_HOOK = 0x80041A8C;
constexpr u32 MODEL_ORIGINAL = 0x9421FFD0;  // stwu r1, -0x30(r1)
constexpr u32 PROJECTILE_FUNCTION = 0x801BB888;
constexpr std::array<u32, 6> PROJECTILE_CALLS = {0x800DF8DC, 0x800DF008, 0x800DF038,
                                                 0x800E1240, 0x8018C950, 0x8018C988};
constexpr u32 STATE_MANAGER = 0x8045A1A8;
constexpr u32 PLAYER = 0x80500000;
constexpr u32 GUN = 0x80600000;
constexpr u32 CAMERA = 0x80720000;
constexpr float PI = 3.14159265358979f;

u32 Branch(u32 from, u32 to, bool link)
{
  return 0x48000000u | ((to - from) & 0x03FFFFFCu) | (link ? 1u : 0u);
}

// ---- PrimedGun's math (NativeRuntime.cpp), as the reference ----

struct Quat
{
  float x, y, z, w;
};

Quat Mul(const Quat& a, const Quat& b)
{
  return {
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// ApplyTrackingWorldYaw then PoseToPrimeMatrix's rotation, rows of the 3x3.
std::array<float, 9> PrimedGunBasis(Quat q, float yaw_deg, float rx, float ry, float rz)
{
  const float half = yaw_deg * PI / 180 * 0.5f;
  q = Mul({0, std::sin(half), 0, std::cos(half)}, q);
  const auto axis = [](float deg, int index) {
    const float h = deg * PI / 180 * 0.5f;
    Quat r{0, 0, 0, std::cos(h)};
    (&r.x)[index] = std::sin(h);
    return r;
  };
  q = Mul(q, Mul(Mul(axis(ry, 1), axis(rx, 0)), axis(rz, 2)));
  const float r00 = 1 - 2 * (q.y * q.y + q.z * q.z), r01 = 2 * (q.x * q.y - q.w * q.z),
              r02 = 2 * (q.x * q.z + q.w * q.y), r10 = 2 * (q.x * q.y + q.w * q.z),
              r11 = 1 - 2 * (q.x * q.x + q.z * q.z), r12 = 2 * (q.y * q.z - q.w * q.x),
              r20 = 2 * (q.x * q.z - q.w * q.y), r21 = 2 * (q.y * q.z + q.w * q.x),
              r22 = 1 - 2 * (q.x * q.x + q.y * q.y);
  return {-r00, r02, -r01, r20, -r22, r21, r10, -r12, r11};
}

struct Game
{
  Game()
  {
    memory.Write32(ROTATION_HOOK, 0x807C0740);
    memory.Write32(MODEL_HOOK, MODEL_ORIGINAL);
    for (const u32 call : PROJECTILE_CALLS)
      memory.Write32(call, Branch(call, PROJECTILE_FUNCTION, true));
    // The player, ready to shoot in first person, and its cannon.
    memory.Write32(STATE_MANAGER + 0x84C, PLAYER);
    memory.Write32(PLAYER + 0x2F4, 0);
    memory.Write32(PLAYER + 0x2F8, 0);
    memory.SetF32(PLAYER + 0x494, 1.0f);
    memory.Write32(PLAYER + 0x498, 2);
    memory.Write32(PLAYER + 0x490, GUN);
    const std::array<float, 12> identity = {1, 0, 0, 5, 0, 1, 0, 6, 0, 0, 1, 7};
    memory.SetMatrix(GUN + 0x4A8, identity);
    memory.SetMatrix(GUN + 0x4D8, identity);
    // The camera manager -> camera uid 5 -> the camera in the object list.
    memory.Write32(STATE_MANAGER + 0x86C, 0x80700000);
    memory.Write32(0x80700000, 0x00050000);
    memory.Write32(STATE_MANAGER + 0x810, 0x80710000);
    memory.Write32(0x80710000 + 5 * 8 + 4, CAMERA);
    SetCameraYaw(30);

    env.memory = &memory;
    env.input = &input;
    env.game_id = "GM8E01";
    env.scratch_base = SCRATCH;
    env.scratch_size = 0x230;
    env.units_per_meter = 1.5;
  }

  void SetCameraYaw(float degrees)
  {
    const float c = std::cos(degrees * PI / 180), s = std::sin(degrees * PI / 180);
    memory.SetMatrix(CAMERA + 0x34, {c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0});
  }

  Memory memory;
  VRInputFrame input;
  ScriptEnvironment env;
};

std::string ReadPatch()
{
  std::string source;
  File::ReadFileToString(File::GetSysDirectory() + VRPATCHES_DIR "/GM8E01/arm-cannon.lua", source);
  return source;
}
}  // namespace

TEST(ArmCannonPatch, ManifestIsValid)
{
  const std::string source = ReadPatch();
  ASSERT_FALSE(source.empty()) << "Sys/VRPatches/GM8E01/arm-cannon.lua is missing";
  const auto read = ReadManifest(source, "arm-cannon");
  ASSERT_TRUE(std::holds_alternative<Manifest>(read)) << std::get<std::string>(read);
  const Manifest& manifest = std::get<Manifest>(read);
  EXPECT_EQ(manifest.games, std::vector<std::string>{"GM8E01"});
  EXPECT_EQ(manifest.revision, 0);
  EXPECT_GE(manifest.scratch_bytes, 0x230u);
  EXPECT_TRUE(manifest.reserve.empty());  // its caves are in its scratch memory
  ASSERT_EQ(manifest.conflicts.size(), 1u);
  EXPECT_EQ(manifest.conflicts[0].name, "Cannon");
}

TEST(ArmCannonPatch, HooksTracksAndRestores)
{
  Game game;
  PatchScript script("arm-cannon");
  ASSERT_TRUE(script.Load(ReadPatch(), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  for (const LogLine& line : script.TakeLog())
    EXPECT_NE(line.level, 1) << line.text;

  // Branches into the caves, which run the replaced instructions and come back.
  Memory& m = game.memory;
  EXPECT_EQ(m.Get32(ROTATION_HOOK), Branch(ROTATION_HOOK, SCRATCH + 0xA0, false));
  EXPECT_EQ(m.Get32(MODEL_HOOK), Branch(MODEL_HOOK, SCRATCH + 0x140, false));
  for (const u32 call : PROJECTILE_CALLS)
    EXPECT_EQ(m.Get32(call), Branch(call, SCRATCH + 0x190, true)) << std::hex << call;
  EXPECT_EQ(m.Get32(SCRATCH + 0xA0), 0x807C0740u);  // lwz r3, 0x740(r28)
  EXPECT_EQ(m.Get32(SCRATCH + 0xA0 + 35 * 4),
            Branch(SCRATCH + 0xA0 + 35 * 4, ROTATION_HOOK + 4, false));
  EXPECT_EQ(m.Get32(SCRATCH + 0x140 + 15 * 4), MODEL_ORIGINAL);
  EXPECT_EQ(m.Get32(SCRATCH + 0x140 + 16 * 4),
            Branch(SCRATCH + 0x140 + 16 * 4, MODEL_HOOK + 4, false));
  EXPECT_EQ(m.Get32(SCRATCH + 0x190), 0x9421FFF0u);  // stwu r1, -16(r1)
  // Not armed before a hand is tracked.
  EXPECT_EQ(m.Get32(SCRATCH + 0x38), 0u);

  // The right hand, pitched up and turned a little, seen from the game camera.
  HandInput& hand = game.input.hands[1];
  hand.connected = true;
  const Quat rotation =
      Mul(Quat{0, std::sin(0.2f), 0, std::cos(0.2f)}, Quat{std::sin(0.15f), 0, 0, std::cos(0.15f)});
  hand.view = {true, {0.2f, -0.3f, -0.4f}, {rotation.x, rotation.y, rotation.z, rotation.w}};
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  EXPECT_EQ(m.Get32(SCRATCH + 0x38), GUN);

  // PrimedGun's yaw delta for a camera yaw of 30 degrees: -(30 + 180) wrapped = 150.
  const std::array<float, 9> expected = PrimedGunBasis(rotation, 150, 0, 0, 0);
  for (size_t i = 0; i < 9; ++i)
    EXPECT_NEAR(m.GetF32(SCRATCH + static_cast<u32>(i) * 4), expected[i], 1e-4f) << i;
  // The position, camera-local in game units: (x, -z, y) * 1.5.
  EXPECT_NEAR(m.GetF32(0x8045BCE8), 0.3f, 1e-5f);
  EXPECT_NEAR(m.GetF32(0x8045BCEC), 0.6f, 1e-5f);
  EXPECT_NEAR(m.GetF32(0x8045BCF0), -0.45f, 1e-5f);
  // The model offset: the basis times (0, -0.08, 0) * 1.5.
  EXPECT_NEAR(m.GetF32(SCRATCH + 0x40), expected[1] * -0.12f, 1e-5f);
  EXPECT_NEAR(m.GetF32(SCRATCH + 0x44), expected[4] * -0.12f, 1e-5f);

  // Morph ball: the patch stops feeding the cannon.
  m.Write32(PLAYER + 0x2F8, 1);
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  EXPECT_EQ(m.Get32(SCRATCH + 0x38), 0u);
  m.Write32(PLAYER + 0x2F8, 0);

  // A setting turns the beam hooks off.
  const Manifest& manifest = script.GetManifest();
  ASSERT_TRUE(script.SetSettingValue(*manifest.FindSetting("beams"), 0));
  ASSERT_TRUE(script.CallSettingChanged("beams", 0, game.env)) << script.Error();
  for (const u32 call : PROJECTILE_CALLS)
    EXPECT_EQ(m.Get32(call), Branch(call, PROJECTILE_FUNCTION, true));

  // Disabled: the game's code is back.
  ASSERT_TRUE(script.Call(Hook::OnUnload, game.env)) << script.Error();
  EXPECT_EQ(m.Get32(ROTATION_HOOK), 0x807C0740u);
  EXPECT_EQ(m.Get32(MODEL_HOOK), MODEL_ORIGINAL);
  EXPECT_EQ(m.Get32(SCRATCH + 0x38), 0u);
}

TEST(ArmCannonPatch, RefusesOtherCode)
{
  Game game;
  game.memory.Write32(ROTATION_HOOK, 0x60000000);  // not Metroid Prime 1.00 code
  PatchScript script("arm-cannon");
  ASSERT_TRUE(script.Load(ReadPatch(), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(MODEL_HOOK), MODEL_ORIGINAL);
  bool warned = false;
  for (const LogLine& line : script.TakeLog())
    warned |= line.level == 1 && line.text.find("not installed") != std::string::npos;
  EXPECT_TRUE(warned);
}

TEST(ArmCannonPatch, ReinstallsOverAnotherScratchAddress)
{
  // A savestate made while the patch had its caves at another address.
  Game game;
  constexpr u32 OLD = 0x80002C00;
  game.memory.Write32(ROTATION_HOOK, Branch(ROTATION_HOOK, OLD + 0xA0, false));
  game.memory.Write32(MODEL_HOOK, Branch(MODEL_HOOK, OLD + 0x140, false));
  game.memory.Write32(OLD + 0x140 + 15 * 4, MODEL_ORIGINAL);
  PatchScript script("arm-cannon");
  ASSERT_TRUE(script.Load(ReadPatch(), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnStateLoaded, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(ROTATION_HOOK), Branch(ROTATION_HOOK, SCRATCH + 0xA0, false));
  EXPECT_EQ(game.memory.Get32(MODEL_HOOK), Branch(MODEL_HOOK, SCRATCH + 0x140, false));
  EXPECT_EQ(game.memory.Get32(SCRATCH + 0x140 + 15 * 4), MODEL_ORIGINAL);
  ASSERT_TRUE(script.Call(Hook::OnUnload, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(MODEL_HOOK), MODEL_ORIGINAL);
}

// ---- Metroid Prime 2: Echoes ----

namespace
{
constexpr u32 ECHOES_SCRATCH = 0x80002DF0;
constexpr u32 ECHOES_SCRATCH_SIZE = 0x200;
constexpr u32 ECHOES_CAVE = ECHOES_SCRATCH + 0x40;
constexpr u32 ECHOES_GUARD = ECHOES_SCRATCH + 0x30;
constexpr u32 ECHOES_HOOK = 0x801DDA84;
constexpr u32 ECHOES_HOOK_ORIGINAL = 0x7FE4FB78;  // mr r4, r31
constexpr u32 ECHOES_STATE_MANAGER = 0x803DB6E0;
constexpr u32 ECHOES_PLAYER = 0x80820F40;
constexpr u32 ECHOES_GUN = 0x80B689E0;

struct EchoesGame
{
  EchoesGame()
  {
    memory.Write32(ECHOES_HOOK, ECHOES_HOOK_ORIGINAL);
    // The player in first person, not morphed, and its gun.
    memory.Write32(ECHOES_STATE_MANAGER + 0x14FC, ECHOES_PLAYER);
    memory.Write32(ECHOES_PLAYER + 0x388, 0);
    memory.Write32(ECHOES_PLAYER + 0x38C, 0);
    memory.Write32(ECHOES_PLAYER + 0xEBC, ECHOES_GUN);

    env.memory = &memory;
    env.input = &input;
    env.game_id = "G2ME01";
    env.scratch_base = ECHOES_SCRATCH;
    env.scratch_size = ECHOES_SCRATCH_SIZE;
    env.units_per_meter = 1.25;
  }

  Memory memory;
  VRInputFrame input;
  ScriptEnvironment env;
};

std::string ReadEchoesPatch()
{
  std::string source;
  File::ReadFileToString(File::GetSysDirectory() + VRPATCHES_DIR "/G2ME01/arm-cannon.lua", source);
  return source;
}

using Vec3 = std::array<float, 3>;

Vec3 Rotate(const Quat& q, const Vec3& v)
{
  // v + 2w (u x v) + 2 u x (u x v)
  const Vec3 u = {q.x, q.y, q.z};
  const auto cross = [](const Vec3& a, const Vec3& b) {
    return Vec3{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
  };
  const Vec3 t = cross(u, v);
  const Vec3 tt = cross(u, t);
  return {v[0] + 2 * q.w * t[0] + 2 * tt[0], v[1] + 2 * q.w * t[1] + 2 * tt[1],
          v[2] + 2 * q.w * t[2] + 2 * tt[2]};
}

// An OpenXR view-space vector (+X right, +Y up, -Z forward) in the game camera's axes (+X right,
// +Y forward, +Z up).
Vec3 GameAxes(const Vec3& v)
{
  return {v[0], -v[2], v[1]};
}
}  // namespace

TEST(EchoesArmCannonPatch, ManifestIsValid)
{
  const std::string source = ReadEchoesPatch();
  ASSERT_FALSE(source.empty()) << "Sys/VRPatches/G2ME01/arm-cannon.lua is missing";
  const auto read = ReadManifest(source, "arm-cannon");
  ASSERT_TRUE(std::holds_alternative<Manifest>(read)) << std::get<std::string>(read);
  const Manifest& manifest = std::get<Manifest>(read);
  EXPECT_EQ(manifest.games, std::vector<std::string>{"G2ME01"});
  EXPECT_EQ(manifest.revision, 0);
  EXPECT_EQ(manifest.scratch_bytes, ECHOES_SCRATCH_SIZE);
  EXPECT_TRUE(manifest.reserve.empty());  // its cave is in its scratch memory
  EXPECT_TRUE(manifest.conflicts.empty());
}

TEST(EchoesArmCannonPatch, HooksTracksAndRestores)
{
  EchoesGame game;
  PatchScript script("arm-cannon");
  ASSERT_TRUE(script.Load(ReadEchoesPatch(), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  for (const LogLine& line : script.TakeLog())
    EXPECT_NE(line.level, 1) << line.text;

  // A branch to the cave, which ends with the replaced instruction and the way back, inside the
  // scratch memory (past it are the OS globals at 0x80003000).
  Memory& m = game.memory;
  EXPECT_EQ(m.Get32(ECHOES_HOOK), Branch(ECHOES_HOOK, ECHOES_CAVE, false));
  EXPECT_EQ(m.Get32(ECHOES_CAVE), 0x3D608000u);  // lis r11, 0x8000
  u32 back = 0;
  for (u32 a = ECHOES_CAVE; a < ECHOES_SCRATCH + ECHOES_SCRATCH_SIZE; a += 4)
  {
    if (m.Get32(a) == Branch(a, ECHOES_HOOK + 4, false))
    {
      back = a;
      break;
    }
  }
  ASSERT_NE(back, 0u) << "the cave's branch back is not inside the scratch memory";
  EXPECT_EQ(m.Get32(back - 4), ECHOES_HOOK_ORIGINAL);
  // Not armed before a hand is tracked.
  EXPECT_EQ(m.Get32(ECHOES_GUARD), 0u);

  // The right hand, pitched up and turned a little, seen from the game camera.
  HandInput& hand = game.input.hands[1];
  hand.connected = true;
  const Quat rotation =
      Mul(Quat{0, std::sin(0.2f), 0, std::cos(0.2f)}, Quat{std::sin(0.15f), 0, 0, std::cos(0.15f)});
  hand.view = {true, {0.2f, -0.3f, -0.4f}, {rotation.x, rotation.y, rotation.z, rotation.w}};
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  EXPECT_EQ(m.Get32(ECHOES_GUARD), ECHOES_GUN);

  // The basis: the controller's right, aim (-Z) and up axes as columns, in the camera's axes.
  const Vec3 right = GameAxes(Rotate(rotation, {1, 0, 0}));
  const Vec3 forward = GameAxes(Rotate(rotation, {0, 0, -1}));
  const Vec3 up = GameAxes(Rotate(rotation, {0, 1, 0}));
  for (u32 row = 0; row < 3; ++row)
  {
    EXPECT_NEAR(m.GetF32(ECHOES_SCRATCH + row * 12 + 0), right[row], 1e-4f) << row;
    EXPECT_NEAR(m.GetF32(ECHOES_SCRATCH + row * 12 + 4), forward[row], 1e-4f) << row;
    EXPECT_NEAR(m.GetF32(ECHOES_SCRATCH + row * 12 + 8), up[row], 1e-4f) << row;
  }
  // The offset from the eye: the hand in game units, minus where the game draws the cannon
  // (0.25, 0.30, -0.35), plus the model offset (8 cm back along the aim).
  const Vec3 position = GameAxes({0.2f, -0.3f, -0.4f});
  const Vec3 root = {0.25f, 0.30f, -0.35f};
  for (u32 i = 0; i < 3; ++i)
  {
    EXPECT_NEAR(m.GetF32(ECHOES_SCRATCH + 0x24 + i * 4),
                position[i] * 1.25f - root[i] - forward[i] * 0.08f * 1.25f, 1e-4f)
        << i;
  }

  // PrimedGun's smoothing: a small move keeps 0.24 of the previous offset...
  const float first_x = m.GetF32(ECHOES_SCRATCH + 0x24);
  hand.view.position = {0.3f, -0.3f, -0.4f};
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  const float target_x = 0.3f * 1.25f - root[0] - forward[0] * 0.08f * 1.25f;
  EXPECT_NEAR(m.GetF32(ECHOES_SCRATCH + 0x24), first_x * 0.24f + target_x * 0.76f, 1e-4f);
  // ...and a jump of more than 2 game units is taken as is.
  hand.view.position = {3.0f, -0.3f, -0.4f};
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  EXPECT_NEAR(m.GetF32(ECHOES_SCRATCH + 0x24), 3.0f * 1.25f - root[0] - forward[0] * 0.08f * 1.25f,
              1e-4f);

  // Morph ball: the patch stops feeding the cannon.
  m.Write32(ECHOES_PLAYER + 0x38C, 1);
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  EXPECT_EQ(m.Get32(ECHOES_GUARD), 0u);

  // Disabled: the game's code is back.
  ASSERT_TRUE(script.Call(Hook::OnUnload, game.env)) << script.Error();
  EXPECT_EQ(m.Get32(ECHOES_HOOK), ECHOES_HOOK_ORIGINAL);
}

TEST(EchoesArmCannonPatch, RefusesOtherCode)
{
  EchoesGame game;
  game.memory.Write32(ECHOES_HOOK, 0x60000000);  // not Echoes NTSC-U code
  PatchScript script("arm-cannon");
  ASSERT_TRUE(script.Load(ReadEchoesPatch(), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(ECHOES_HOOK), 0x60000000u);
  EXPECT_EQ(game.memory.Get32(ECHOES_CAVE), 0u);
  bool warned = false;
  for (const LogLine& line : script.TakeLog())
    warned |= line.level == 1 && line.text.find("not installed") != std::string::npos;
  EXPECT_TRUE(warned);
}

// ---- Metroid Prime 3: Corruption ----

namespace
{
constexpr u32 CORRUPTION_SCRATCH = 0x80002DF0;
constexpr u32 CORRUPTION_SCRATCH_SIZE = 0x200;
constexpr u32 CORRUPTION_CAVE = CORRUPTION_SCRATCH + 0x40;
constexpr u32 CORRUPTION_GUARD = CORRUPTION_SCRATCH + 0x30;
constexpr u32 CORRUPTION_CAMERA_XF = CORRUPTION_SCRATCH + 0x34;
constexpr u32 CORRUPTION_HOOK = 0x8018748C;
constexpr u32 CORRUPTION_HOOK_ORIGINAL = 0xC0010208;  // lfs f0, 0x208(r1)
constexpr u32 CORRUPTION_GAME = 0x805C4F70;
constexpr u32 CORRUPTION_STATE = 0x80A475E0;
constexpr u32 CORRUPTION_PLAYER = 0x80B82FE0;
constexpr u32 CORRUPTION_GUN = 0x80B888E0;
constexpr u32 CORRUPTION_CAMERA = 0x80AFBBA0;

struct CorruptionGame
{
  CorruptionGame()
  {
    memory.Write32(CORRUPTION_HOOK, CORRUPTION_HOOK_ORIGINAL);
    // The state manager -> the player, standing, and its gun.
    memory.Write32(CORRUPTION_GAME + 0x28, CORRUPTION_STATE);
    memory.Write32(CORRUPTION_STATE + 0x2184, CORRUPTION_PLAYER);
    memory.Write32(CORRUPTION_PLAYER + 0x358, 0);
    memory.Write32(CORRUPTION_PLAYER + 0x3AD8, CORRUPTION_GUN);
    // The camera: [[[game + 0x38] + 0x34] + 0x20].
    memory.Write32(CORRUPTION_GAME + 0x38, 0x80AFB3A0);
    memory.Write32(0x80AFB3A0 + 0x34, 0x80B9CA80);
    memory.Write32(0x80B9CA80 + 0x20, CORRUPTION_CAMERA);

    env.memory = &memory;
    env.input = &input;
    env.game_id = "RM3E01";
    env.is_wii = true;
    env.scratch_base = CORRUPTION_SCRATCH;
    env.scratch_size = CORRUPTION_SCRATCH_SIZE;
    env.units_per_meter = 1.5;
  }

  Memory memory;
  VRInputFrame input;
  ScriptEnvironment env;
};

std::string ReadCorruptionPatch()
{
  std::string source;
  File::ReadFileToString(File::GetSysDirectory() + VRPATCHES_DIR "/RM3E01/arm-cannon.lua", source);
  return source;
}
}  // namespace

TEST(CorruptionArmCannonPatch, ManifestIsValid)
{
  const std::string source = ReadCorruptionPatch();
  ASSERT_FALSE(source.empty()) << "Sys/VRPatches/RM3E01/arm-cannon.lua is missing";
  const auto read = ReadManifest(source, "arm-cannon");
  ASSERT_TRUE(std::holds_alternative<Manifest>(read)) << std::get<std::string>(read);
  const Manifest& manifest = std::get<Manifest>(read);
  EXPECT_EQ(manifest.games, std::vector<std::string>{"RM3E01"});
  EXPECT_EQ(manifest.revision, 0);
  EXPECT_EQ(manifest.scratch_bytes, CORRUPTION_SCRATCH_SIZE);
  EXPECT_TRUE(manifest.reserve.empty());  // its cave is in its scratch memory
  // The profile's Controller Anchor of the cannon (off by default) would move it twice.
  ASSERT_EQ(manifest.conflicts.size(), 1u);
  EXPECT_EQ(manifest.conflicts[0].name, "Element Draw 111");
}

TEST(CorruptionArmCannonPatch, HooksTracksAndRestores)
{
  CorruptionGame game;
  PatchScript script("arm-cannon");
  ASSERT_TRUE(script.Load(ReadCorruptionPatch(), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  for (const LogLine& line : script.TakeLog())
    EXPECT_NE(line.level, 1) << line.text;

  // A branch to the cave, which ends with the replaced instruction and the way back, inside the
  // scratch memory.
  Memory& m = game.memory;
  EXPECT_EQ(m.Get32(CORRUPTION_HOOK), Branch(CORRUPTION_HOOK, CORRUPTION_CAVE, false));
  EXPECT_EQ(m.Get32(CORRUPTION_CAVE), 0x3D608000u);  // lis r11, 0x8000
  u32 back = 0;
  for (u32 a = CORRUPTION_CAVE; a < CORRUPTION_SCRATCH + CORRUPTION_SCRATCH_SIZE; a += 4)
  {
    if (m.Get32(a) == Branch(a, CORRUPTION_HOOK + 4, false))
    {
      back = a;
      break;
    }
  }
  ASSERT_NE(back, 0u) << "the cave's branch back is not inside the scratch memory";
  EXPECT_EQ(m.Get32(back - 4), CORRUPTION_HOOK_ORIGINAL);
  // Not armed before a hand is tracked.
  EXPECT_EQ(m.Get32(CORRUPTION_GUARD), 0u);

  // The right hand, pitched up and turned a little, seen from the game camera.
  HandInput& hand = game.input.hands[1];
  hand.connected = true;
  const Quat rotation =
      Mul(Quat{0, std::sin(0.2f), 0, std::cos(0.2f)}, Quat{std::sin(0.15f), 0, 0, std::cos(0.15f)});
  hand.view = {true, {0.2f, -0.3f, -0.4f}, {rotation.x, rotation.y, rotation.z, rotation.w}};
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  // Armed for the cannon updater's r28 (gun + 0x4D8), with the camera's transform to read.
  EXPECT_EQ(m.Get32(CORRUPTION_GUARD), CORRUPTION_GUN + 0x4D8);
  EXPECT_EQ(m.Get32(CORRUPTION_CAMERA_XF), CORRUPTION_CAMERA + 0x3C);

  // The basis: the controller's right, aim (-Z) and up axes as columns, in the camera's axes.
  const Vec3 right = GameAxes(Rotate(rotation, {1, 0, 0}));
  const Vec3 forward = GameAxes(Rotate(rotation, {0, 0, -1}));
  const Vec3 up = GameAxes(Rotate(rotation, {0, 1, 0}));
  for (u32 row = 0; row < 3; ++row)
  {
    EXPECT_NEAR(m.GetF32(CORRUPTION_SCRATCH + row * 12 + 0), right[row], 1e-4f) << row;
    EXPECT_NEAR(m.GetF32(CORRUPTION_SCRATCH + row * 12 + 4), forward[row], 1e-4f) << row;
    EXPECT_NEAR(m.GetF32(CORRUPTION_SCRATCH + row * 12 + 8), up[row], 1e-4f) << row;
  }
  // The offset from the camera: the hand in game units plus the model offset (8 cm back along
  // the aim); the game draws the cannon at its transform.
  const Vec3 position = GameAxes({0.2f, -0.3f, -0.4f});
  for (u32 i = 0; i < 3; ++i)
  {
    EXPECT_NEAR(m.GetF32(CORRUPTION_SCRATCH + 0x24 + i * 4),
                position[i] * 1.5f - forward[i] * 0.08f * 1.5f, 1e-4f)
        << i;
  }

  // Morph ball: the patch stops feeding the cannon.
  m.Write32(CORRUPTION_PLAYER + 0x358, 1);
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  EXPECT_EQ(m.Get32(CORRUPTION_GUARD), 0u);

  // Disabled: the game's code is back.
  ASSERT_TRUE(script.Call(Hook::OnUnload, game.env)) << script.Error();
  EXPECT_EQ(m.Get32(CORRUPTION_HOOK), CORRUPTION_HOOK_ORIGINAL);
}

TEST(CorruptionArmCannonPatch, RefusesOtherCode)
{
  CorruptionGame game;
  game.memory.Write32(CORRUPTION_HOOK, 0x60000000);  // not Corruption NTSC-U code
  PatchScript script("arm-cannon");
  ASSERT_TRUE(script.Load(ReadCorruptionPatch(), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(CORRUPTION_HOOK), 0x60000000u);
  EXPECT_EQ(game.memory.Get32(CORRUPTION_CAVE), 0u);
  bool warned = false;
  for (const LogLine& line : script.TakeLog())
    warned |= line.level == 1 && line.text.find("not installed") != std::string::npos;
  EXPECT_TRUE(warned);
}
