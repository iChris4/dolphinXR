// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The shipped Metroid Prime state patches (Data/Sys/VRPatches/<id>/metroid-state.lua) against fake
// memory: the override flags they raise for the Sys profiles' entries.

#include <gtest/gtest.h>

#include <map>
#include <set>
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
  bool Write8(u32 a, u8 v) override { return Fail(); }
  bool Write16(u32 a, u16 v) override { return Fail(); }
  bool Write32(u32 a, u32 v) override { return Fail(); }
  bool Write64(u32 a, u64 v) override { return Fail(); }
  void InvalidateCode(u32, u32) override {}

  // The game's side: the patches only read.
  void Set32(u32 address, u32 value)
  {
    for (u32 i = 0; i < 4; ++i)
      bytes[address + i] = static_cast<u8>(value >> (8 * (3 - i)));
  }
  bool wrote = false;

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
  bool Fail()
  {
    wrote = true;
    return false;
  }
  std::map<u32, u8> bytes;
};

using Flags = std::set<std::string>;

std::string ReadStatePatch(const std::string& game_id)
{
  std::string source;
  File::ReadFileToString(
      File::GetSysDirectory() + VRPATCHES_DIR "/" + game_id + "/metroid-state.lua", source);
  return source;
}
}  // namespace

// ---- Metroid Prime 2: Echoes ----

namespace
{
constexpr u32 ECHOES_STATE_MANAGER = 0x803DB6E0;
constexpr u32 ECHOES_PLAYER = 0x80820F40;
constexpr u32 ECHOES_PLAYER_STATE = 0x80830000;

struct EchoesGame
{
  EchoesGame()
  {
    // In play, first person, the combat visor.
    memory.Set32(ECHOES_STATE_MANAGER + 0x14FC, ECHOES_PLAYER);
    memory.Set32(ECHOES_PLAYER + 0x1314, ECHOES_PLAYER_STATE);
    SetVisor(0);

    env.memory = &memory;
    env.input = &input;
    env.game_id = "G2ME01";
  }

  void SetVisor(u32 visor) { memory.Set32(ECHOES_PLAYER_STATE + 0x30, visor); }
  void SetMenu(u32 menu) { memory.Set32(ECHOES_STATE_MANAGER + 0x2470, menu); }
  void SetCamera(u32 camera) { memory.Set32(ECHOES_PLAYER + 0x388, camera); }
  void SetBall(u32 ball) { memory.Set32(ECHOES_PLAYER + 0x38C, ball); }

  Memory memory;
  VRInputFrame input;
  ScriptEnvironment env;
};
}  // namespace

TEST(EchoesStatePatch, ManifestIsValid)
{
  const std::string source = ReadStatePatch("G2ME01");
  ASSERT_FALSE(source.empty()) << "Sys/VRPatches/G2ME01/metroid-state.lua is missing";
  const auto read = ReadManifest(source, "metroid-state");
  ASSERT_TRUE(std::holds_alternative<Manifest>(read)) << std::get<std::string>(read);
  const Manifest& manifest = std::get<Manifest>(read);
  EXPECT_EQ(manifest.games, std::vector<std::string>{"G2ME01"});
  EXPECT_EQ(manifest.revision, 0);
  EXPECT_EQ(manifest.scratch_bytes, 0u);
  EXPECT_TRUE(manifest.reserve.empty());
  EXPECT_TRUE(manifest.conflicts.empty());
}

TEST(EchoesStatePatch, RaisesTheGameState)
{
  EchoesGame game;
  PatchScript script("metroid-state");
  ASSERT_TRUE(script.Load(ReadStatePatch("G2ME01"), game.env)) << script.Error();
  const auto frame = [&] {
    EXPECT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
    return script.GetFlags();
  };

  EXPECT_EQ(frame(), Flags{"mp_visor_combat"});
  game.SetVisor(1);
  EXPECT_EQ(frame(), Flags{"mp_visor_echo"});
  game.SetVisor(2);
  EXPECT_EQ(frame(), Flags{"mp_visor_scan"});
  game.SetVisor(3);
  EXPECT_EQ(frame(), Flags{"mp_visor_dark"});

  // The map, then the data network: no visor while paused.
  game.SetMenu(1);
  EXPECT_EQ(frame(), Flags{"mp_paused"});
  game.SetMenu(3);
  EXPECT_EQ(frame(), Flags{"mp_paused"});
  game.SetMenu(0);

  // Morph ball and morphing: the ball's camera is not a cutscene's.
  game.SetVisor(0);
  game.SetBall(1);
  game.SetCamera(2);
  EXPECT_EQ(frame(), (Flags{"mp_morphball", "mp_visor_combat"}));
  game.SetBall(3);
  game.SetCamera(4);
  EXPECT_EQ(frame(), (Flags{"mp_morphball", "mp_visor_combat"}));

  // Standing, another camera than the first person one.
  game.SetBall(0);
  EXPECT_EQ(frame(), (Flags{"mp_cinematic", "mp_visor_combat"}));
  game.SetCamera(0);
  EXPECT_EQ(frame(), Flags{"mp_visor_combat"});

  // No player (a load, the title screen): nothing.
  game.memory.Set32(ECHOES_STATE_MANAGER + 0x14FC, 0);
  EXPECT_EQ(frame(), Flags{});
  EXPECT_FALSE(game.memory.wrote);
  for (const LogLine& line : script.TakeLog())
    EXPECT_NE(line.level, 1) << line.text;
}
