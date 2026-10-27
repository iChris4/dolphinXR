// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The shipped Metroid Prime state patches (Data/Sys/VRPatches/<id>/metroid-state.lua) against fake
// memory: the override flags they raise for the Sys profiles' entries (Prime, Echoes, Corruption,
// the Trilogy).
// Also Echoes' view patches: no morph ball or cutscene bars, no scan box highlight, smooth scan
// lines; and the wide culling of the Trilogy and of the three games' own discs.

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

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
  // MEM1, and the Wii's MEM2.
  bool IsRAM(u32 address, u32 size) const override
  {
    const u64 end = static_cast<u64>(address) + size;
    return size != 0 && ((address >= 0x80000000 && end <= 0x81800000) ||
                         (address >= 0x90000000 && end <= 0x94000000));
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

  // The game's side, not counted as a patch write.
  void Set32(u32 address, u32 value)
  {
    for (u32 i = 0; i < 4; ++i)
      bytes[address + i] = static_cast<u8>(value >> (8 * (3 - i)));
  }
  u32 Get32(u32 address) const { return Read32(address).value_or(0); }
  // Whether a patch wrote.
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
  template <typename T>
  bool Write(u32 address, T value)
  {
    if (!IsRAM(address, sizeof(T)))
      return false;
    wrote = true;
    for (u32 i = 0; i < sizeof(T); ++i)
      bytes[address + i] = static_cast<u8>(static_cast<u64>(value) >> (8 * (sizeof(T) - 1 - i)));
    return true;
  }
  std::map<u32, u8> bytes;
};

using Flags = std::set<std::string>;

std::string ReadPatch(const std::string& game_id, const std::string& id)
{
  std::string source;
  File::ReadFileToString(File::GetSysDirectory() + VRPATCHES_DIR "/" + game_id + "/" + id + ".lua",
                         source);
  return source;
}

std::string ReadStatePatch(const std::string& game_id)
{
  return ReadPatch(game_id, "metroid-state");
}

bool Warned(PatchScript& script, std::string_view text)
{
  bool warned = false;
  for (const LogLine& line : script.TakeLog())
    warned |= line.level == 1 && line.text.find(text) != std::string::npos;
  return warned;
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

// ---- Echoes: no bars in morph ball ----

namespace
{
constexpr u32 BARS_SITE = 0x8003AF60;
constexpr u32 BARS_ORIGINAL = 0xC3FB11E8;     // lfs f31, 0x11E8(r27)
constexpr u32 BARS_FULL_HEIGHT = 0xC3E28ED4;  // lfs f31, -0x712C(r2)
constexpr u32 ECHOES_ONE = 0x804223C0 - 0x712C;
}  // namespace

TEST(EchoesNoMorphBallBarsPatch, LoadsFullHeightAndRestores)
{
  EchoesGame game;
  game.memory.Set32(BARS_SITE, BARS_ORIGINAL);
  game.memory.Set32(ECHOES_ONE, 0x3F800000);
  PatchScript script("no-morph-ball-bars");
  ASSERT_TRUE(script.Load(ReadPatch("G2ME01", "no-morph-ball-bars"), game.env)) << script.Error();
  EXPECT_EQ(script.GetManifest().games, std::vector<std::string>{"G2ME01"});
  EXPECT_EQ(script.GetManifest().scratch_bytes, 0u);

  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(BARS_SITE), BARS_FULL_HEIGHT);
  // A savestate made without the patch.
  game.memory.Set32(BARS_SITE, BARS_ORIGINAL);
  ASSERT_TRUE(script.Call(Hook::OnStateLoaded, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(BARS_SITE), BARS_FULL_HEIGHT);
  EXPECT_FALSE(Warned(script, ""));

  ASSERT_TRUE(script.Call(Hook::OnUnload, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(BARS_SITE), BARS_ORIGINAL);
}

TEST(EchoesNoMorphBallBarsPatch, RefusesOtherCode)
{
  EchoesGame game;
  game.memory.Set32(BARS_SITE, 0x60000000);  // not Echoes NTSC-U code
  game.memory.Set32(ECHOES_ONE, 0x3F800000);
  PatchScript script("no-morph-ball-bars");
  ASSERT_TRUE(script.Load(ReadPatch("G2ME01", "no-morph-ball-bars"), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(BARS_SITE), 0x60000000u);
  EXPECT_TRUE(Warned(script, "not installed"));
  ASSERT_TRUE(script.Call(Hook::OnUnload, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(BARS_SITE), 0x60000000u);
}

// ---- Echoes: smooth scan lines ----

namespace
{
constexpr u32 LINES_TEXTURE = 0x806FE5A0;
using TextureWords = std::array<u32, 8>;
constexpr TextureWords LINES = {0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0};

void SetTexture(Memory& memory, const TextureWords& words, u32 address = LINES_TEXTURE)
{
  for (u32 i = 0; i < words.size(); ++i)
    memory.Set32(address + i * 4, words[i]);
}

TextureWords GetTexture(const Memory& memory, u32 address = LINES_TEXTURE)
{
  TextureWords words;
  for (u32 i = 0; i < words.size(); ++i)
    words[i] = memory.Get32(address + i * 4);
  return words;
}

TextureWords Filled(u32 word)
{
  TextureWords words;
  words.fill(word);
  return words;
}
}  // namespace

TEST(EchoesSmoothScanLinesPatch, FillsAndRestores)
{
  EchoesGame game;
  SetTexture(game.memory, LINES);
  PatchScript script("smooth-scan-lines");
  ASSERT_TRUE(script.Load(ReadPatch("G2ME01", "smooth-scan-lines"), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  // The lines' average.
  EXPECT_EQ(GetTexture(game.memory), Filled(0x88888888));

  const Manifest& manifest = script.GetManifest();
  ASSERT_TRUE(script.SetSettingValue(*manifest.FindSetting("level"), 3));
  ASSERT_TRUE(script.CallSettingChanged("level", 3, game.env)) << script.Error();
  EXPECT_EQ(GetTexture(game.memory), Filled(0x33333333));

  // A savestate load brings the lines back.
  SetTexture(game.memory, LINES);
  ASSERT_TRUE(script.Call(Hook::OnStateLoaded, game.env)) << script.Error();
  EXPECT_EQ(GetTexture(game.memory), Filled(0x33333333));
  EXPECT_FALSE(Warned(script, ""));

  ASSERT_TRUE(script.Call(Hook::OnUnload, game.env)) << script.Error();
  EXPECT_EQ(GetTexture(game.memory), LINES);
}

TEST(EchoesSmoothScanLinesPatch, LeavesOtherDataAlone)
{
  EchoesGame game;
  const TextureWords other = {1, 2, 3, 4, 5, 6, 7, 8};
  SetTexture(game.memory, other);
  PatchScript script("smooth-scan-lines");
  ASSERT_TRUE(script.Load(ReadPatch("G2ME01", "smooth-scan-lines"), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  EXPECT_EQ(GetTexture(game.memory), other);
  EXPECT_TRUE(Warned(script, "not applied"));
  ASSERT_TRUE(script.Call(Hook::OnUnload, game.env)) << script.Error();
  EXPECT_EQ(GetTexture(game.memory), other);
}

// ---- Echoes: no scan box highlight ----

namespace
{
constexpr u32 BOX_SITE = 0x80266348;
constexpr u32 BOX_ORIGINAL = 0x48057651;  // bl 0x802BD998: begins the box's strip
constexpr u32 BOX_SKIP = 0x48000080;      // b 0x802663C8
constexpr u32 BOX_END_CALL = 0x802663C4;
constexpr u32 BOX_END_ORIGINAL = 0x480575D1;
}  // namespace

TEST(EchoesNoScanBoxHighlightPatch, SkipsTheBoxStripAndRestores)
{
  EchoesGame game;
  game.memory.Set32(BOX_SITE, BOX_ORIGINAL);
  game.memory.Set32(BOX_END_CALL, BOX_END_ORIGINAL);
  PatchScript script("no-scan-box-highlight");
  ASSERT_TRUE(script.Load(ReadPatch("G2ME01", "no-scan-box-highlight"), game.env))
      << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(BOX_SITE), BOX_SKIP);
  game.memory.Set32(BOX_SITE, BOX_ORIGINAL);
  ASSERT_TRUE(script.Call(Hook::OnStateLoaded, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(BOX_SITE), BOX_SKIP);
  EXPECT_FALSE(Warned(script, ""));

  ASSERT_TRUE(script.Call(Hook::OnUnload, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(BOX_SITE), BOX_ORIGINAL);
}

TEST(EchoesNoScanBoxHighlightPatch, RefusesOtherCode)
{
  EchoesGame game;
  game.memory.Set32(BOX_SITE, BOX_ORIGINAL);
  game.memory.Set32(BOX_END_CALL, 0x60000000);  // another layout
  PatchScript script("no-scan-box-highlight");
  ASSERT_TRUE(script.Load(ReadPatch("G2ME01", "no-scan-box-highlight"), game.env))
      << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(BOX_SITE), BOX_ORIGINAL);
  EXPECT_TRUE(Warned(script, "not installed"));
}

// ---- Echoes: no cutscene bars ----

namespace
{
constexpr u32 CUTSCENE_SITE = 0x8003AFA4;
constexpr u32 CUTSCENE_ORIGINAL = 0xFFE00890;  // fmr f31, f1
constexpr u32 CUTSCENE_COMPARE = 0x8003AF9C;
constexpr u32 CUTSCENE_COMPARE_ORIGINAL = 0xFC01F840;  // fcmpo cr0, f1, f31
}  // namespace

TEST(EchoesNoCutsceneBarsPatch, DropsTheFilterHeightAndRestores)
{
  EchoesGame game;
  game.memory.Set32(CUTSCENE_SITE, CUTSCENE_ORIGINAL);
  game.memory.Set32(CUTSCENE_COMPARE, CUTSCENE_COMPARE_ORIGINAL);
  PatchScript script("no-cutscene-bars");
  ASSERT_TRUE(script.Load(ReadPatch("G2ME01", "no-cutscene-bars"), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(CUTSCENE_SITE), 0x60000000u);
  game.memory.Set32(CUTSCENE_SITE, CUTSCENE_ORIGINAL);
  ASSERT_TRUE(script.Call(Hook::OnStateLoaded, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(CUTSCENE_SITE), 0x60000000u);
  EXPECT_FALSE(Warned(script, ""));

  ASSERT_TRUE(script.Call(Hook::OnUnload, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(CUTSCENE_SITE), CUTSCENE_ORIGINAL);
}

TEST(EchoesNoCutsceneBarsPatch, RefusesOtherCode)
{
  EchoesGame game;
  game.memory.Set32(CUTSCENE_SITE, CUTSCENE_ORIGINAL);
  game.memory.Set32(CUTSCENE_COMPARE, 0x60000000);  // another layout
  PatchScript script("no-cutscene-bars");
  ASSERT_TRUE(script.Load(ReadPatch("G2ME01", "no-cutscene-bars"), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(CUTSCENE_SITE), CUTSCENE_ORIGINAL);
  EXPECT_TRUE(Warned(script, "not installed"));
}

// ---- Metroid Prime ----

namespace
{
constexpr u32 PRIME_PLAYER = 0x80500000;
constexpr u32 PRIME_STATE_REF = 0x80510000;
constexpr u32 PRIME_PLAYER_STATE = 0x80520000;

struct PrimeGame
{
  // The state manager and the player's camera and ball fields of the disc revision.
  PrimeGame(u16 revision, u32 state_manager, u32 camera, u32 ball)
      : state_manager(state_manager), camera(camera), ball(ball)
  {
    // In play, first person, the combat visor.
    memory.Set32(state_manager + 0x84C, PRIME_PLAYER);
    memory.Set32(state_manager + 0x8B8, PRIME_STATE_REF);
    memory.Set32(PRIME_STATE_REF, PRIME_PLAYER_STATE);
    SetVisor(0);

    env.memory = &memory;
    env.input = &input;
    env.game_id = "GM8E01";
    env.revision = revision;
  }

  void SetVisor(u32 visor) { memory.Set32(PRIME_PLAYER_STATE + 0x14, visor); }
  void SetTransition(u32 transition) { memory.Set32(state_manager + 0xF90, transition); }
  void SetCamera(u32 state) { memory.Set32(PRIME_PLAYER + camera, state); }
  void SetBall(u32 state) { memory.Set32(PRIME_PLAYER + ball, state); }

  u32 state_manager, camera, ball;
  Memory memory;
  VRInputFrame input;
  ScriptEnvironment env;
};

void ExpectPrimeFlags(PrimeGame& game)
{
  PatchScript script("metroid-state");
  ASSERT_TRUE(script.Load(ReadStatePatch("GM8E01"), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  const auto frame = [&] {
    EXPECT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
    return script.GetFlags();
  };

  EXPECT_EQ(frame(), Flags{"mp_visor_combat"});
  game.SetVisor(1);
  EXPECT_EQ(frame(), Flags{"mp_visor_xray"});
  game.SetVisor(2);
  EXPECT_EQ(frame(), Flags{"mp_visor_scan"});
  game.SetVisor(3);
  EXPECT_EQ(frame(), Flags{"mp_visor_thermal"});

  // The map, then the pause screens: no visor while paused.
  game.SetTransition(1);
  EXPECT_EQ(frame(), Flags{"mp_paused"});
  game.SetTransition(2);
  EXPECT_EQ(frame(), Flags{"mp_paused"});
  game.SetTransition(0);

  game.SetVisor(0);
  game.SetBall(1);
  game.SetCamera(1);
  EXPECT_EQ(frame(), (Flags{"mp_morphball", "mp_visor_combat"}));
  game.SetBall(0);
  game.SetCamera(4);  // spawned: a cutscene
  EXPECT_EQ(frame(), (Flags{"mp_cinematic", "mp_visor_combat"}));
  game.SetCamera(0);

  game.memory.Set32(game.state_manager + 0x84C, 0);
  EXPECT_EQ(frame(), Flags{});
  EXPECT_FALSE(game.memory.wrote);
  EXPECT_FALSE(Warned(script, ""));
}
}  // namespace

TEST(PrimeStatePatch, ManifestIsValid)
{
  const std::string source = ReadStatePatch("GM8E01");
  ASSERT_FALSE(source.empty()) << "Sys/VRPatches/GM8E01/metroid-state.lua is missing";
  const auto read = ReadManifest(source, "metroid-state");
  ASSERT_TRUE(std::holds_alternative<Manifest>(read)) << std::get<std::string>(read);
  const Manifest& manifest = std::get<Manifest>(read);
  EXPECT_EQ(manifest.games, std::vector<std::string>{"GM8E01"});
  EXPECT_FALSE(manifest.revision);  // every revision, from its own table
  EXPECT_EQ(manifest.scratch_bytes, 0u);
  EXPECT_TRUE(manifest.conflicts.empty());
}

TEST(PrimeStatePatch, RaisesTheGameStateOn100)
{
  PrimeGame game(0, 0x8045A1A8, 0x2F4, 0x2F8);
  ExpectPrimeFlags(game);
}

TEST(PrimeStatePatch, RaisesTheGameStateOn102)
{
  PrimeGame game(2, 0x8045B208, 0x304, 0x308);
  ExpectPrimeFlags(game);
}

TEST(PrimeStatePatch, UnknownRevisionRaisesNothing)
{
  PrimeGame game(7, 0x8045B208, 0x304, 0x308);
  PatchScript script("metroid-state");
  ASSERT_TRUE(script.Load(ReadStatePatch("GM8E01"), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  EXPECT_EQ(script.GetFlags(), Flags{});
  EXPECT_TRUE(Warned(script, "unknown disc revision"));
}

// ---- Metroid Prime 3: Corruption ----

namespace
{
constexpr u32 CORRUPTION_STATE_MANAGER_VAR = 0x805C4F98;
constexpr u32 CORRUPTION_STATE_MANAGER = 0x80A14740;
constexpr u32 CORRUPTION_PLAYER = 0x80B87CC0;
constexpr u32 CORRUPTION_POWERUPS = 0x80C00000;

struct CorruptionGame
{
  CorruptionGame()
  {
    // In play, first person, the combat visor.
    memory.Set32(CORRUPTION_STATE_MANAGER_VAR, CORRUPTION_STATE_MANAGER);
    memory.Set32(CORRUPTION_STATE_MANAGER + 0x2184, CORRUPTION_PLAYER);
    memory.Set32(CORRUPTION_PLAYER + 0x35A0, CORRUPTION_POWERUPS);
    SetVisor(0);
    SetCamera(1);

    env.memory = &memory;
    env.input = &input;
    env.game_id = "RM3E01";
  }

  void SetVisor(u32 visor) { memory.Set32(CORRUPTION_POWERUPS + 0x34, visor); }
  void SetMenu(u32 menu) { memory.Set32(CORRUPTION_STATE_MANAGER_VAR + 0x32C, menu); }
  // The pointer dialog byte at 0x805C70C7.
  void SetDialog(u8 dialog) { memory.Set32(0x805C70C4, dialog); }
  void SetBall(u32 ball) { memory.Set32(CORRUPTION_PLAYER + 0x358, ball); }
  void SetCamera(u32 camera) { memory.Set32(CORRUPTION_PLAYER + 0x3C2C, camera); }

  Memory memory;
  VRInputFrame input;
  ScriptEnvironment env;
};
}  // namespace

TEST(CorruptionStatePatch, ManifestIsValid)
{
  const std::string source = ReadStatePatch("RM3E01");
  ASSERT_FALSE(source.empty()) << "Sys/VRPatches/RM3E01/metroid-state.lua is missing";
  const auto read = ReadManifest(source, "metroid-state");
  ASSERT_TRUE(std::holds_alternative<Manifest>(read)) << std::get<std::string>(read);
  const Manifest& manifest = std::get<Manifest>(read);
  EXPECT_EQ(manifest.games, std::vector<std::string>{"RM3E01"});
  EXPECT_EQ(manifest.revision, 0);
  EXPECT_EQ(manifest.scratch_bytes, 0u);
  EXPECT_TRUE(manifest.conflicts.empty());
}

TEST(CorruptionStatePatch, RaisesTheGameState)
{
  CorruptionGame game;
  PatchScript script("metroid-state");
  ASSERT_TRUE(script.Load(ReadStatePatch("RM3E01"), game.env)) << script.Error();
  const auto frame = [&] {
    EXPECT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
    return script.GetFlags();
  };

  EXPECT_EQ(frame(), Flags{"mp_visor_combat"});
  game.SetVisor(1);
  EXPECT_EQ(frame(), Flags{"mp_visor_scan"});
  game.SetVisor(2);
  EXPECT_EQ(frame(), Flags{"mp_visor_command"});
  game.SetVisor(3);
  EXPECT_EQ(frame(), Flags{"mp_visor_xray"});
  game.SetVisor(0);

  // The map and the solar system map, a pointer dialog: no visor while paused. The objective
  // screen and the ship's landing cutscene use other menu states and are not pauses.
  game.SetMenu(1);
  EXPECT_EQ(frame(), Flags{"mp_paused"});
  game.SetMenu(3);
  EXPECT_EQ(frame(), Flags{"mp_paused"});
  game.SetMenu(10);
  EXPECT_EQ(frame(), Flags{"mp_visor_combat"});
  game.SetMenu(0);
  game.SetDialog(1);
  EXPECT_EQ(frame(), Flags{"mp_paused"});
  game.SetDialog(0);

  game.SetBall(1);
  EXPECT_EQ(frame(), (Flags{"mp_morphball", "mp_visor_combat"}));
  game.SetBall(0);
  game.SetCamera(4);  // a cutscene or a conversation
  EXPECT_EQ(frame(), (Flags{"mp_cinematic", "mp_visor_combat"}));
  game.SetCamera(1);

  // The title screen: no player, the pointer drives the menus.
  game.memory.Set32(CORRUPTION_STATE_MANAGER + 0x2184, 0);
  EXPECT_EQ(frame(), Flags{});
  game.SetDialog(1);
  EXPECT_EQ(frame(), Flags{"mp_paused"});
  EXPECT_FALSE(game.memory.wrote);
  EXPECT_FALSE(Warned(script, ""));
}

// ---- Corruption: turn with the right thumbstick ----

namespace
{
constexpr u32 TURN_SCRATCH = 0x80002D70;
constexpr u32 TURN_CAVE = TURN_SCRATCH + 0x20;
constexpr u32 TURN_CONTROL = 0x80061BA8;           // zz_80061ba8: a control's value
constexpr u32 TURN_CONTROL_ORIGINAL = 0x9421FFC0;  // stwu sp, -0x40(sp)

float GetF32(const Memory& memory, u32 address)
{
  return std::bit_cast<float>(memory.Get32(address));
}

// The Wii Remote's buttons and the Nunchuk's stick by their names.
class WiimoteButtons final : public PadResolver
{
public:
  std::vector<PadControl> Resolve(PadDevice device, u8, PadControlKind kind, std::string_view name,
                                  std::string* error) override
  {
    if (device == PadDevice::Wiimote && kind == PadControlKind::Button &&
        (name == "-" || name == "+" || name == "1" || name == "2"))
    {
      return {{"Buttons", std::string(name)}};
    }
    if (device == PadDevice::Attachment && kind == PadControlKind::Stick && name.empty())
      return {{"Stick", "X"}, {"Stick", "Y"}};
    *error = "unknown";
    return {};
  }
};

struct TurnGame : CorruptionGame
{
  TurnGame()
  {
    memory.Set32(TURN_CONTROL, TURN_CONTROL_ORIGINAL);
    input.hands[1].connected = true;
    env.pad = &pad;
    env.is_wii = true;
    env.scratch_base = TURN_SCRATCH;
    env.scratch_size = 0x80;
  }

  WiimoteButtons pad;
};

std::string ReadTurnPatch()
{
  return ReadPatch("RM3E01", "stick-turn");
}

double Held(PatchScript& script, const std::string& button)
{
  const PadHolds& holds = script.GetPadHolds();
  const auto it = holds.find(PadHoldKey{PadDevice::Wiimote, 0, "Buttons", button});
  return it == holds.end() ? -1.0 : it->second;
}
}  // namespace

TEST(CorruptionStickTurnPatch, ManifestIsValid)
{
  const std::string source = ReadTurnPatch();
  ASSERT_FALSE(source.empty()) << "Sys/VRPatches/RM3E01/stick-turn.lua is missing";
  const auto read = ReadManifest(source, "stick-turn");
  ASSERT_TRUE(std::holds_alternative<Manifest>(read)) << std::get<std::string>(read);
  const Manifest& manifest = std::get<Manifest>(read);
  EXPECT_EQ(manifest.games, std::vector<std::string>{"RM3E01"});
  EXPECT_EQ(manifest.revision, 0);
  EXPECT_EQ(manifest.scratch_bytes, 0x80u);
  EXPECT_TRUE(manifest.reserve.empty());
}

TEST(CorruptionStickTurnPatch, TurnsWithTheThumbstickAndRestores)
{
  TurnGame game;
  PatchScript script("stick-turn");
  ASSERT_TRUE(script.Load(ReadTurnPatch(), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  Memory& m = game.memory;
  const auto frame = [&] { ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error(); };

  // The control getter branches to the cave, which ends with its first instruction and the way
  // back.
  EXPECT_EQ(m.Get32(TURN_CONTROL), 0x48000000u | ((TURN_CAVE - TURN_CONTROL) & 0x03FFFFFC));
  EXPECT_EQ(m.Get32(TURN_CAVE), 0x3D608000u);  // lis r11, 0x8000
  u32 back = 0;
  for (u32 a = TURN_CAVE; a < TURN_SCRATCH + 0x80; a += 4)
  {
    if (m.Get32(a) == (0x48000000u | ((TURN_CONTROL + 4 - a) & 0x03FFFFFC)))
      back = a;
  }
  ASSERT_NE(back, 0u) << "the cave's branch back is not inside the scratch memory";
  EXPECT_EQ(m.Get32(back - 4), TURN_CONTROL_ORIGINAL);

  // Right past the dead zone (0.2): turn right at half speed, for the player's control mapper;
  // look up and down stay the game's (the pointer's edges tilt the camera).
  game.input.hands[1].thumbstick_x = 0.6f;
  frame();
  EXPECT_EQ(m.Get32(TURN_SCRATCH), CORRUPTION_PLAYER + 0x35C4);
  EXPECT_EQ(m.Get32(TURN_SCRATCH + 4), 0u);
  EXPECT_NEAR(GetF32(m, TURN_SCRATCH + 8), 0.0f, 1e-6f);
  EXPECT_NEAR(GetF32(m, TURN_SCRATCH + 0xC), 0.5f, 1e-5f);
  EXPECT_EQ(script.GetFlags(), Flags{"rm3_hide_crosshair"});
  // Full left; inside the dead zone nothing.
  game.input.hands[1].thumbstick_x = -1.0f;
  frame();
  EXPECT_NEAR(GetF32(m, TURN_SCRATCH + 8), 1.0f, 1e-6f);
  EXPECT_NEAR(GetF32(m, TURN_SCRATCH + 0xC), 0.0f, 1e-6f);
  game.input.hands[1].thumbstick_x = 0.15f;
  frame();
  EXPECT_NEAR(GetF32(m, TURN_SCRATCH + 8), 0.0f, 1e-6f);
  EXPECT_NEAR(GetF32(m, TURN_SCRATCH + 0xC), 0.0f, 1e-6f);

  // While playing, - follows the right thumbstick click and + the right B (and the thumbstick no
  // longer presses them); on the map the user's bindings are back.
  game.input.hands[1].thumbstick_button = true;
  frame();
  EXPECT_EQ(Held(script, "-"), 1.0);
  EXPECT_EQ(Held(script, "+"), 0.0);
  game.SetMenu(1);
  frame();
  EXPECT_EQ(Held(script, "-"), -1.0);
  EXPECT_EQ(Held(script, "+"), -1.0);
  game.SetMenu(0);

  // Turning on a diagonal does not press the thumbstick's up and down (1 and 2: the map and the
  // pause screen); pushed up, they are the user's again.
  game.input.hands[1].thumbstick_x = 0.8f;
  game.input.hands[1].thumbstick_y = 0.6f;
  frame();
  EXPECT_EQ(Held(script, "1"), 0.0);
  EXPECT_EQ(Held(script, "2"), 0.0);
  game.input.hands[1].thumbstick_x = 0.0f;
  game.input.hands[1].thumbstick_y = 0.9f;
  frame();
  EXPECT_EQ(Held(script, "1"), -1.0);
  EXPECT_EQ(Held(script, "2"), -1.0);
  game.input.hands[1].thumbstick_y = 0.0f;

  // No player (a load): disarmed.
  m.Set32(CORRUPTION_STATE_MANAGER + 0x2184, 0);
  frame();
  EXPECT_EQ(m.Get32(TURN_SCRATCH), 0u);
  EXPECT_TRUE(script.GetFlags().empty());
  for (const LogLine& line : script.TakeLog())
    EXPECT_NE(line.level, 1) << line.text;

  // Disabled: the game's code is back.
  ASSERT_TRUE(script.Call(Hook::OnUnload, game.env)) << script.Error();
  EXPECT_EQ(m.Get32(TURN_CONTROL), TURN_CONTROL_ORIGINAL);
}

// In first person the VR view removes the game camera's pitch, named by the camera matrix's
// forward up component, and the pointer's edges tilt the camera with the pointer kept on the
// screen; on a menu the camera goes back to level.
TEST(CorruptionStickTurnPatch, RemovesTheCameraPitchFromTheView)
{
  TurnGame game;
  Memory& m = game.memory;
  constexpr u32 CAMERA = 0x80AFBBA0;
  m.Set32(0x805C4FA8, 0x80AFB3A0);
  m.Set32(0x80AFB3A0 + 0x34, 0x80B9CA80);
  m.Set32(0x80B9CA80 + 0x20, CAMERA);
  PatchScript script("stick-turn");
  ASSERT_TRUE(script.Load(ReadTurnPatch(), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  const auto frame = [&] { ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error(); };

  frame();
  EXPECT_EQ(script.GetCameraPitchSource(), CAMERA + 0x3C + 0x24);
  EXPECT_EQ(m.Get32(TURN_SCRATCH + 4), 0u);
  EXPECT_TRUE(script.KeepsPointerOnScreen());

  // The map: back to level, and the pointer as usual.
  m.Set32(CORRUPTION_PLAYER + 0x77C, std::bit_cast<u32>(0.5f));
  game.SetMenu(1);
  frame();
  EXPECT_EQ(GetF32(m, CORRUPTION_PLAYER + 0x77C), 0.0f);
  EXPECT_FALSE(script.KeepsPointerOnScreen());
  game.SetMenu(0);

  // The morph ball's third-person camera is shown as it is.
  game.SetBall(1);
  frame();
  EXPECT_FALSE(script.GetCameraPitchSource());
  EXPECT_FALSE(script.KeepsPointerOnScreen());
  game.SetBall(0);

  // Edges that do nothing: look up and down read 0; the pitch is still removed.
  ASSERT_TRUE(script.SetSettingValue(*script.GetManifest().FindSetting("edge_tilt"), 0));
  frame();
  EXPECT_EQ(m.Get32(TURN_SCRATCH + 4), 1u);
  EXPECT_EQ(script.GetCameraPitchSource(), CAMERA + 0x3C + 0x24);
  EXPECT_FALSE(script.KeepsPointerOnScreen());
}

TEST(CorruptionStickTurnPatch, RefusesOtherCode)
{
  TurnGame game;
  game.memory.Set32(TURN_CONTROL, 0x60000000);  // not Corruption NTSC-U code
  PatchScript script("stick-turn");
  ASSERT_TRUE(script.Load(ReadTurnPatch(), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(TURN_CONTROL), 0x60000000u);
  EXPECT_EQ(game.memory.Get32(TURN_CAVE), 0u);
  EXPECT_TRUE(Warned(script, "not installed"));
}

// ---- Corruption: change visors next to the head ----

namespace
{
std::string ReadVisorPatch()
{
  return ReadPatch("RM3E01", "visor-gesture");
}

u32 Visor(const Memory& memory)
{
  return memory.Get32(CORRUPTION_POWERUPS + 0x34);
}
}  // namespace

TEST(CorruptionVisorGesturePatch, ManifestIsValid)
{
  const std::string source = ReadVisorPatch();
  ASSERT_FALSE(source.empty()) << "Sys/VRPatches/RM3E01/visor-gesture.lua is missing";
  const auto read = ReadManifest(source, "visor-gesture");
  ASSERT_TRUE(std::holds_alternative<Manifest>(read)) << std::get<std::string>(read);
  const Manifest& manifest = std::get<Manifest>(read);
  EXPECT_EQ(manifest.games, std::vector<std::string>{"RM3E01"});
  EXPECT_EQ(manifest.revision, 0);
  EXPECT_EQ(manifest.scratch_bytes, 0u);
}

TEST(CorruptionVisorGesturePatch, PicksVisorsNextToTheHead)
{
  TurnGame game;
  Memory& m = game.memory;
  // Every visor owned but the X-ray visor.
  for (u32 id = 0x0B; id <= 0x0D; ++id)
    m.Set32(CORRUPTION_POWERUPS + id * 12 + 0x58, 1);
  game.input.head = {true, {0.0f, 1.6f, 0.0f}};
  HandInput& hand = game.input.hands[0];
  hand.connected = true;
  hand.aim = {true, {0.6f, 1.2f, -0.4f}};  // away from the head
  PatchScript script("visor-gesture");
  ASSERT_TRUE(script.Load(ReadVisorPatch(), game.env)) << script.Error();
  const auto frame = [&] { ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error(); };

  // Away from the head the thumbstick walks as usual.
  hand.thumbstick_x = -1.0f;
  frame();
  EXPECT_EQ(Visor(m), 0u);
  EXPECT_TRUE(script.GetPadHolds().empty());

  // Next to the head: left picks the scan visor, the Nunchuk stick held at the centre.
  hand.aim = {true, {0.12f, 1.55f, -0.05f}};
  frame();
  EXPECT_EQ(Visor(m), 1u);
  const PadHolds& holds = script.GetPadHolds();
  EXPECT_EQ(holds.size(), 2u);
  EXPECT_EQ(holds.count(PadHoldKey{PadDevice::Attachment, u8{0}, "Stick", "X"}), 1u);
  // Held there: no repeat. Back to the centre, then up: the command visor.
  game.SetVisor(0);
  frame();
  EXPECT_EQ(Visor(m), 0u);
  hand.thumbstick_x = 0.0f;
  frame();
  hand.thumbstick_y = 0.9f;
  frame();
  EXPECT_EQ(Visor(m), 2u);
  // Right: the X-ray visor is not owned.
  hand.thumbstick_y = 0.0f;
  frame();
  hand.thumbstick_x = 0.9f;
  frame();
  EXPECT_EQ(Visor(m), 2u);
  // Down: back to the combat visor.
  hand.thumbstick_x = 0.0f;
  frame();
  hand.thumbstick_y = -0.9f;
  frame();
  EXPECT_EQ(Visor(m), 0u);

  // On the map nothing changes and the stick is the user's again.
  hand.thumbstick_y = 0.0f;
  frame();
  game.SetMenu(1);
  hand.thumbstick_x = -1.0f;
  frame();
  EXPECT_EQ(Visor(m), 0u);
  EXPECT_TRUE(script.GetPadHolds().empty());
  for (const LogLine& line : script.TakeLog())
    EXPECT_NE(line.level, 1) << line.text;
}

// ---- Metroid Prime Trilogy ----

namespace
{
constexpr u32 TRILOGY_PLAYER = 0x80B00000;
constexpr u32 TRILOGY_POWERUPS = 0x80B10000;
constexpr u32 TRILOGY_BRANCH = 0x48001000;  // b +0x1000, as the arm cannon patch installs

// Each game of the disc in memory: the instruction its state patch recognizes it by (its arm
// cannon hook and the next one), and its state.
struct TrilogyGame
{
  TrilogyGame()
  {
    env.memory = &memory;
    env.input = &input;
    env.game_id = "R3ME01";
  }

  // Loads one game's program: Prime, Echoes or Corruption, in play, first person, combat visor.
  void RunPrime()
  {
    memory.Set32(0x8018BC74, 0x387904B0);
    memory.Set32(0x8018BC78, 0x38810380);
    memory.Set32(0x804BF420 + 0x84C, TRILOGY_PLAYER);
    memory.Set32(0x804BFCD4, TRILOGY_POWERUPS);
    visor = TRILOGY_POWERUPS + 0x18;
    next_visor = TRILOGY_POWERUPS + 0x1C;
    menu = 0x804BF420 + 0x117C;
    camera = TRILOGY_PLAYER + 0x2F0;
    ball = TRILOGY_PLAYER + 0x2F4;
  }
  void RunEchoes()
  {
    memory.Set32(0x8018ACB0, 0xC00102A0);
    memory.Set32(0x8018ACB4, 0x387C03B0);
    memory.Set32(0x804E72E8 + 0x14F4, TRILOGY_PLAYER);
    memory.Set32(TRILOGY_PLAYER + 0x12EC, TRILOGY_POWERUPS);
    visor = TRILOGY_POWERUPS + 0x34;
    next_visor = 0;
    menu = 0x804E72E8 + 0x2C0C;
    camera = TRILOGY_PLAYER + 0x374;
    ball = TRILOGY_PLAYER + 0x378;
  }
  void RunCorruption()
  {
    memory.Set32(0x80183C28, 0xC0010208);
    memory.Set32(0x80183C2C, 0x7F83E378);
    memory.Set32(0x805C6C68, 0x80A00000);
    memory.Set32(0x80A00000 + 0x2184, TRILOGY_PLAYER);
    memory.Set32(TRILOGY_PLAYER + 0x35A8, TRILOGY_POWERUPS);
    visor = TRILOGY_POWERUPS + 0x34;
    next_visor = 0;
    menu = 0x805C6C68 + 0x32C;
    camera = TRILOGY_PLAYER + 0x3C3C;
    ball = TRILOGY_PLAYER + 0x358;
    memory.Set32(camera, 1);
  }

  void Set(u32 address, u32 value) { memory.Set32(address, value); }
  // A visor in use: Prime also has the visor it switches to.
  void SetVisor(u32 value)
  {
    memory.Set32(visor, value);
    if (next_visor != 0)
      memory.Set32(next_visor, value);
  }

  u32 visor = 0, next_visor = 0, menu = 0, camera = 0, ball = 0;
  Memory memory;
  VRInputFrame input;
  ScriptEnvironment env;
};

}  // namespace

TEST(TrilogyStatePatch, ManifestIsValid)
{
  const std::string source = ReadStatePatch("R3ME01");
  ASSERT_FALSE(source.empty()) << "Sys/VRPatches/R3ME01/metroid-state.lua is missing";
  const auto read = ReadManifest(source, "metroid-state");
  ASSERT_TRUE(std::holds_alternative<Manifest>(read)) << std::get<std::string>(read);
  const Manifest& manifest = std::get<Manifest>(read);
  EXPECT_EQ(manifest.games, std::vector<std::string>{"R3ME01"});
  EXPECT_EQ(manifest.revision, 0);
  EXPECT_EQ(manifest.scratch_bytes, 0u);
  EXPECT_TRUE(manifest.conflicts.empty());
}

TEST(TrilogyStatePatch, RaisesEachGamesState)
{
  TrilogyGame game;
  PatchScript script("metroid-state");
  ASSERT_TRUE(script.Load(ReadStatePatch("R3ME01"), game.env)) << script.Error();
  const auto frame = [&] {
    EXPECT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
    return script.GetFlags();
  };

  // The Trilogy's menus: none of the three programs.
  EXPECT_EQ(frame(), Flags{"mp_game_menu"});

  game.RunPrime();
  EXPECT_EQ(frame(), (Flags{"mp_game_prime", "mp_visor_combat", "mp1_visor_combat"}));
  game.SetVisor(1);
  EXPECT_EQ(frame(), (Flags{"mp_game_prime", "mp_visor_xray", "mp1_visor_xray"}));
  // Leaving the X-ray visor: its view fades out while the next visor is already combat.
  game.Set(game.next_visor, 0);
  EXPECT_EQ(frame(), (Flags{"mp_game_prime", "mp_visor_combat", "mp1_visor_combat", "mp_visor_xray",
                            "mp1_visor_xray"}));
  game.SetVisor(3);
  EXPECT_EQ(frame(), (Flags{"mp_game_prime", "mp_visor_thermal", "mp1_visor_thermal"}));
  game.SetVisor(2);
  // The arm cannon patch's branch at the hook: still Prime.
  game.Set(0x8018BC74, TRILOGY_BRANCH);
  EXPECT_EQ(frame(), (Flags{"mp_game_prime", "mp_visor_scan", "mp1_visor_scan"}));
  // The map, the pause screen and a save station's two dialogs.
  for (const u32 menu : {1u, 2u, 4u, 5u})
  {
    game.Set(game.menu, menu);
    EXPECT_EQ(frame(), (Flags{"mp_game_prime", "mp_paused", "mp1_paused"})) << menu;
  }
  game.Set(game.menu, 3);
  EXPECT_EQ(frame(), (Flags{"mp_game_prime", "mp_visor_scan", "mp1_visor_scan"}));
  game.Set(game.menu, 0);
  game.SetVisor(0);
  game.Set(game.ball, 1);
  game.Set(game.camera, 1);
  EXPECT_EQ(frame(), (Flags{"mp_game_prime", "mp_morphball", "mp1_morphball", "mp_visor_combat",
                            "mp1_visor_combat"}));
  game.Set(game.ball, 0);
  game.Set(game.camera, 4);  // a cutscene or an elevator
  EXPECT_EQ(frame(), (Flags{"mp_game_prime", "mp_cinematic", "mp1_cinematic", "mp_visor_combat",
                            "mp1_visor_combat"}));

  // Echoes replaces Prime in memory: the same reads.
  game.Set(0x8018BC78, 0);
  game.RunEchoes();
  game.Set(game.visor, 3);
  EXPECT_EQ(frame(), (Flags{"mp_game_echoes", "mp_visor_dark", "mp2_visor_dark"}));
  game.Set(game.visor, 1);
  EXPECT_EQ(frame(), (Flags{"mp_game_echoes", "mp_visor_echo", "mp2_visor_echo"}));
  game.Set(game.menu, 3);
  EXPECT_EQ(frame(), (Flags{"mp_game_echoes", "mp_paused", "mp2_paused"}));
  game.Set(game.menu, 0);
  game.Set(game.camera, 5);
  EXPECT_EQ(frame(), (Flags{"mp_game_echoes", "mp_cinematic", "mp2_cinematic", "mp_visor_echo",
                            "mp2_visor_echo"}));

  // Then Corruption.
  game.Set(0x8018ACB4, 0);
  game.RunCorruption();
  game.Set(game.visor, 2);
  EXPECT_EQ(frame(), (Flags{"mp_game_corruption", "mp_visor_command", "mp3_visor_command"}));
  game.Set(game.menu, 3);  // the galaxy map
  EXPECT_EQ(frame(), (Flags{"mp_game_corruption", "mp_paused", "mp3_paused"}));
  game.Set(game.menu, 0);
  game.Set(0x805C8D74, 1);  // a pointer dialog (the byte at 0x805C8D77)
  EXPECT_EQ(frame(), (Flags{"mp_game_corruption", "mp_paused", "mp3_paused"}));
  game.Set(0x805C8D74, 0);
  game.Set(game.camera, 4);  // a conversation
  EXPECT_EQ(frame(), (Flags{"mp_game_corruption", "mp_cinematic", "mp3_cinematic",
                            "mp_visor_command", "mp3_visor_command"}));

  // Back to the menus.
  game.Set(0x80183C2C, 0);
  EXPECT_EQ(frame(), Flags{"mp_game_menu"});
  EXPECT_FALSE(game.memory.wrote);
  // The profile's GazeRamCopies applies to the three games.
  EXPECT_TRUE(script.TakeSettingRequests().empty());
  EXPECT_FALSE(Warned(script, ""));
}

TEST(TrilogySmoothScanLinesPatch, FillsOnlyWhileEchoesRuns)
{
  constexpr u32 TRILOGY_LINES_TEXTURE = 0x902C96C0;
  TrilogyGame game;
  SetTexture(game.memory, LINES, TRILOGY_LINES_TEXTURE);
  PatchScript script("smooth-scan-lines");
  ASSERT_TRUE(script.Load(ReadPatch("R3ME01", "smooth-scan-lines"), game.env)) << script.Error();

  // The Trilogy's menus, then Prime: not Echoes' memory.
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  game.RunPrime();
  ASSERT_TRUE(script.Call(Hook::OnStateLoaded, game.env)) << script.Error();
  EXPECT_EQ(GetTexture(game.memory, TRILOGY_LINES_TEXTURE), LINES);

  // Echoes before its texture is loaded: blank memory stays blank.
  game.Set(0x8018BC78, 0);
  game.RunEchoes();
  SetTexture(game.memory, Filled(0), TRILOGY_LINES_TEXTURE);
  ASSERT_TRUE(script.Call(Hook::OnStateLoaded, game.env)) << script.Error();
  EXPECT_EQ(GetTexture(game.memory, TRILOGY_LINES_TEXTURE), Filled(0));

  // Then the texture: the lines' average, and the lines back when the patch stops.
  SetTexture(game.memory, LINES, TRILOGY_LINES_TEXTURE);
  ASSERT_TRUE(script.Call(Hook::OnStateLoaded, game.env)) << script.Error();
  EXPECT_EQ(GetTexture(game.memory, TRILOGY_LINES_TEXTURE), Filled(0x88888888));
  EXPECT_FALSE(Warned(script, ""));
  ASSERT_TRUE(script.Call(Hook::OnUnload, game.env)) << script.Error();
  EXPECT_EQ(GetTexture(game.memory, TRILOGY_LINES_TEXTURE), LINES);
}

namespace
{
// Echoes' scan visor: the fan around the pointer, between its begin and end calls.
constexpr u32 SPOTLIGHT_SITE = 0x803AEEC8;
constexpr u32 SPOTLIGHT_ORIGINAL = 0x4BF59489;  // bl 0x80308350: begins the fan
constexpr u32 SPOTLIGHT_SKIP = 0x48000134;      // b 0x803AEFFC
constexpr u32 SPOTLIGHT_END_CALL = 0x803AEFF8;
constexpr u32 SPOTLIGHT_END_ORIGINAL = 0x4BF593C1;  // bl 0x803083B8
}  // namespace

TEST(TrilogyNoScanSpotlightPatch, SkipsTheFanOnlyInEchoes)
{
  TrilogyGame game;
  PatchScript script("no-scan-spotlight");
  ASSERT_TRUE(script.Load(ReadPatch("R3ME01", "no-scan-spotlight"), game.env)) << script.Error();

  // Prime's program has its own code there: left alone, no warning.
  game.RunPrime();
  game.Set(SPOTLIGHT_SITE, 0x981F0008);
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(SPOTLIGHT_SITE), 0x981F0008u);
  EXPECT_FALSE(Warned(script, ""));

  // Echoes: the fan is skipped, again after its program comes back unpatched, and restored when
  // the patch stops.
  game.Set(0x8018BC78, 0);
  game.RunEchoes();
  game.Set(SPOTLIGHT_SITE, SPOTLIGHT_ORIGINAL);
  game.Set(SPOTLIGHT_END_CALL, SPOTLIGHT_END_ORIGINAL);
  ASSERT_TRUE(script.Call(Hook::OnStateLoaded, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(SPOTLIGHT_SITE), SPOTLIGHT_SKIP);
  game.Set(SPOTLIGHT_SITE, SPOTLIGHT_ORIGINAL);
  ASSERT_TRUE(script.Call(Hook::OnStateLoaded, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(SPOTLIGHT_SITE), SPOTLIGHT_SKIP);
  EXPECT_FALSE(Warned(script, ""));
  ASSERT_TRUE(script.Call(Hook::OnUnload, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(SPOTLIGHT_SITE), SPOTLIGHT_ORIGINAL);
}

TEST(TrilogyNoScanSpotlightPatch, RefusesOtherCode)
{
  TrilogyGame game;
  game.RunEchoes();
  game.Set(SPOTLIGHT_SITE, SPOTLIGHT_ORIGINAL);
  game.Set(SPOTLIGHT_END_CALL, 0x60000000);  // another layout
  PatchScript script("no-scan-spotlight");
  ASSERT_TRUE(script.Load(ReadPatch("R3ME01", "no-scan-spotlight"), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(SPOTLIGHT_SITE), SPOTLIGHT_ORIGINAL);
  EXPECT_TRUE(Warned(script, "not installed"));
}

namespace
{
// The calls of the frustum's constructor that the wide culling patch hooks, per game of the disc.
struct CullSite
{
  u32 hook, original, after;
};
constexpr std::array<CullSite, 3> PRIME_CULL_SITES{{{0x8022C680, 0x480D0EE5, 0x807D0850},
                                                    {0x8022C9D0, 0x480D0B95, 0x806D9C74},
                                                    {0x8022DD38, 0x480CF82D, 0x806D9C74}}};
constexpr std::array<CullSite, 3> ECHOES_CULL_SITES{{{0x8024567C, 0x480C1EB5, 0x387B2C14},
                                                     {0x80245C44, 0x480C18ED, 0x387C2C14},
                                                     {0x80247934, 0x480BFBFD, 0x387D2C14}}};
constexpr std::array<CullSite, 1> CORRUPTION_CULL_SITES{{{0x800BC7A4, 0x482B0CD1, 0x80DF0014}}};
constexpr u32 PRIME_FRUSTUM = 0x802FD564;
constexpr u32 ECHOES_FRUSTUM = 0x80307530;
constexpr u32 CORRUPTION_FRUSTUM = 0x8036D474;
constexpr u32 CULL_SCRATCH = 0x80002970;
constexpr u32 CULL_SCRATCH_SIZE = 0x280;
constexpr u32 CULL_MODE = CULL_SCRATCH;
constexpr u32 CULL_FOV = CULL_SCRATCH + 4;
constexpr u32 CULL_HEAD = CULL_SCRATCH + 0x14;

// Where a b / bl word at `from` goes; 0 when it is not one.
u32 BranchTarget(u32 from, u32 word)
{
  if ((word & 0xFC000002) != 0x48000000)
    return 0;
  u32 offset = word & 0x03FFFFFC;
  if (offset & 0x02000000)
    offset -= 0x04000000;
  return from + offset;
}

template <size_t N>
void LoadCullSites(Memory& memory, const std::array<CullSite, N>& sites)
{
  for (const CullSite& site : sites)
  {
    memory.Set32(site.hook, site.original);
    memory.Set32(site.hook + 4, site.after);
  }
}

// Every site calls one routine in the scratch memory, which calls the constructor; or, when the
// patch stopped, the original call.
template <size_t N>
void ExpectCullHooks(const Memory& memory, const std::array<CullSite, N>& sites, u32 constructor)
{
  const u32 routine = BranchTarget(sites[0].hook, memory.Get32(sites[0].hook));
  ASSERT_GE(routine, CULL_SCRATCH);
  ASSERT_LT(routine, CULL_SCRATCH + CULL_SCRATCH_SIZE);
  for (const CullSite& site : sites)
  {
    const u32 word = memory.Get32(site.hook);
    EXPECT_EQ(word & 1, 1u) << "a call, so that the routine returns after it";
    EXPECT_EQ(BranchTarget(site.hook, word), routine);
  }
  bool calls_constructor = false;
  for (u32 at = routine; at < CULL_SCRATCH + CULL_SCRATCH_SIZE; at += 4)
    calls_constructor |= BranchTarget(at, memory.Get32(at)) == constructor;
  EXPECT_TRUE(calls_constructor);
}

template <size_t N>
void ExpectCullOriginals(const Memory& memory, const std::array<CullSite, N>& sites)
{
  for (const CullSite& site : sites)
    EXPECT_EQ(memory.Get32(site.hook), site.original);
}

std::string ReadCullPatch(const std::string& game_id = "R3ME01")
{
  return ReadPatch(game_id, "wide-culling");
}
}  // namespace

TEST(TrilogyWideCullingPatch, ManifestIsValid)
{
  const std::string source = ReadCullPatch();
  ASSERT_FALSE(source.empty()) << "Sys/VRPatches/R3ME01/wide-culling.lua is missing";
  const auto read = ReadManifest(source, "wide-culling");
  ASSERT_TRUE(std::holds_alternative<Manifest>(read)) << std::get<std::string>(read);
  const Manifest& manifest = std::get<Manifest>(read);
  EXPECT_EQ(manifest.games, std::vector<std::string>{"R3ME01"});
  EXPECT_EQ(manifest.revision, 0);
  EXPECT_EQ(manifest.scratch_bytes, CULL_SCRATCH_SIZE);
  EXPECT_TRUE(manifest.reserve.empty());
  EXPECT_NE(manifest.FindSetting("mode"), nullptr);
  EXPECT_NE(manifest.FindSetting("fov"), nullptr);
  EXPECT_NE(manifest.FindSetting("follow_head"), nullptr);
}

TEST(TrilogyWideCullingPatch, HooksEachGameAndRestores)
{
  TrilogyGame game;
  game.env.scratch_base = CULL_SCRATCH;
  game.env.scratch_size = CULL_SCRATCH_SIZE;
  PatchScript script("wide-culling");
  ASSERT_TRUE(script.Load(ReadCullPatch(), game.env)) << script.Error();

  // The Trilogy's menus: no game's code, nothing written there.
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(PRIME_CULL_SITES[0].hook), 0u);

  // Prime: its three frustums of the world view, wide by default (115 degrees).
  LoadCullSites(game.memory, PRIME_CULL_SITES);
  ASSERT_TRUE(script.Call(Hook::OnStateLoaded, game.env)) << script.Error();
  ExpectCullHooks(game.memory, PRIME_CULL_SITES, PRIME_FRUSTUM);
  EXPECT_EQ(game.memory.Get32(CULL_MODE), 1u);
  EXPECT_NEAR(GetF32(game.memory, CULL_FOV), 115.0f * 3.14159265f / 180.0f, 1e-5f);

  // Echoes replaces Prime's program, then Corruption replaces Echoes'.
  for (const CullSite& site : PRIME_CULL_SITES)
    game.Set(site.hook + 4, 0);
  LoadCullSites(game.memory, ECHOES_CULL_SITES);
  game.env.field = 60;
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  ExpectCullHooks(game.memory, ECHOES_CULL_SITES, ECHOES_FRUSTUM);
  for (const CullSite& site : ECHOES_CULL_SITES)
    game.Set(site.hook + 4, 0);
  LoadCullSites(game.memory, CORRUPTION_CULL_SITES);
  game.env.field = 120;
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  ExpectCullHooks(game.memory, CORRUPTION_CULL_SITES, CORRUPTION_FRUSTUM);

  // Stopped: the game's own calls, and the routine off for a savestate that kept the hooks.
  ASSERT_TRUE(script.Call(Hook::OnUnload, game.env)) << script.Error();
  ExpectCullOriginals(game.memory, CORRUPTION_CULL_SITES);
  EXPECT_EQ(game.memory.Get32(CULL_MODE), 0u);
  EXPECT_FALSE(Warned(script, ""));
}

TEST(TrilogyWideCullingPatch, TurnsWithTheHeadOrDropsThePlanes)
{
  TrilogyGame game;
  game.env.scratch_base = CULL_SCRATCH;
  game.env.scratch_size = CULL_SCRATCH_SIZE;
  LoadCullSites(game.memory, CORRUPTION_CULL_SITES);
  PatchScript script("wide-culling");
  ASSERT_TRUE(script.Load(ReadCullPatch(), game.env)) << script.Error();
  const auto head = [&](std::array<float, 9> expected) {
    for (u32 i = 0; i < 9; ++i)
      EXPECT_NEAR(GetF32(game.memory, CULL_HEAD + i * 4), expected[i], 1e-5f) << "element " << i;
  };

  // The head turned 90 degrees right: the frustum looks along the camera's +X (row-major, the
  // columns are the head's right, forward and up axes in the camera's).
  const float s = std::sqrt(0.5f);
  game.input.head_view = {true, {0.0f, 0.0f, 0.0f}, {0.0f, -s, 0.0f, s}};
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(CULL_MODE), 1u);
  head({0, 1, 0, -1, 0, 0, 0, 0, 1});

  // Without a head pose (flat screen), or not following it: the game camera's direction.
  game.input.head_view.valid = false;
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  head({1, 0, 0, 0, 1, 0, 0, 0, 1});
  game.input.head_view.valid = true;
  const Manifest& manifest = script.GetManifest();
  ASSERT_TRUE(script.SetSettingValue(*manifest.FindSetting("follow_head"), 0));
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  head({1, 0, 0, 0, 1, 0, 0, 0, 1});

  // No culling: the routine leaves the frustum without planes.
  ASSERT_TRUE(script.SetSettingValue(*manifest.FindSetting("mode"), 1));
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(CULL_MODE), 2u);
  ExpectCullHooks(game.memory, CORRUPTION_CULL_SITES, CORRUPTION_FRUSTUM);
}

// ---- The games' own discs: the same patch at their addresses ----

namespace
{
constexpr std::array<CullSite, 3> PRIME_100_CULL_SITES{{{0x80047488, 0x482FDDCD, 0x807C0850},
                                                        {0x800471EC, 0x482FE069, 0x806DA068},
                                                        {0x80045E94, 0x482FF3C1, 0x806DA068}}};
constexpr std::array<CullSite, 3> PRIME_102_CULL_SITES{{{0x80047774, 0x482FE579, 0x807C0850},
                                                        {0x800474D8, 0x482FE815, 0x806DA088},
                                                        {0x80046180, 0x482FFB6D, 0x806DA088}}};
constexpr std::array<CullSite, 3> ECHOES_GC_CULL_SITES{{{0x8003B3B0, 0x482C7169, 0x387A2478},
                                                        {0x8003B178, 0x482C73A1, 0x387C2478},
                                                        {0x8003910C, 0x482C940D, 0x387D2478}}};
constexpr std::array<CullSite, 1> CORRUPTION_WII_CULL_SITES{{{0x800BE128, 0x482B1585, 0x80DF0014}}};
constexpr u32 PRIME_100_FRUSTUM = 0x80345254;
constexpr u32 PRIME_102_FRUSTUM = 0x80345CEC;
constexpr u32 ECHOES_GC_FRUSTUM = 0x80302518;
constexpr u32 CORRUPTION_WII_FRUSTUM = 0x8036F6AC;

// A test that the no-culling Action Replay codes replace with "li r3, 1; blr", with its first two
// words.
struct CullTest
{
  u32 address, first, second;
};
constexpr std::array<CullTest, 5> PRIME_100_CULL_TESTS{{{0x80344FCC, 0x80030000, 0x38630004},
                                                        {0x8034502C, 0x80030000, 0x38630004},
                                                        {0x80345098, 0x9421FFC0, 0x7C0802A6},
                                                        {0x80345188, 0x9421FFF0, 0x7C0802A6},
                                                        {0x803451D0, 0x9421FFE0, 0x7C0802A6}}};
constexpr std::array<CullTest, 5> PRIME_102_CULL_TESTS{{{0x80345A64, 0x80030000, 0x38630004},
                                                        {0x80345AC4, 0x80030000, 0x38630004},
                                                        {0x80345B30, 0x9421FFC0, 0x7C0802A6},
                                                        {0x80345C20, 0x9421FFF0, 0x7C0802A6},
                                                        {0x80345C68, 0x9421FFE0, 0x7C0802A6}}};
constexpr std::array<CullTest, 5> ECHOES_GC_CULL_TESTS{{{0x80302298, 0x80030000, 0x38630004},
                                                        {0x803022F8, 0x80030000, 0x38630004},
                                                        {0x80302364, 0x9421FFC0, 0x7C0802A6},
                                                        {0x80302450, 0x9421FFF0, 0x7C0802A6},
                                                        {0x80302498, 0x9421FFE0, 0x7C0802A6}}};
constexpr std::array<CullTest, 1> CORRUPTION_WII_CULL_TESTS{{{0x80316A1C, 0xC003000C, 0xC0230000}}};

struct CullGame
{
  explicit CullGame(std::string_view game_id)
  {
    env.memory = &memory;
    env.input = &input;
    env.game_id = game_id;
    env.scratch_base = CULL_SCRATCH;
    env.scratch_size = CULL_SCRATCH_SIZE;
  }

  Memory memory;
  VRInputFrame input;
  ScriptEnvironment env;
};

// The tests as a no-culling code leaves them, or as the game has them.
template <size_t N>
void LoadCullTests(Memory& memory, const std::array<CullTest, N>& tests, bool replaced)
{
  for (const CullTest& test : tests)
  {
    memory.Set32(test.address, replaced ? 0x38600001 : test.first);
    memory.Set32(test.address + 4, replaced ? 0x4E800020 : test.second);
  }
}

template <size_t N>
void ExpectCullTests(const Memory& memory, const std::array<CullTest, N>& tests)
{
  for (const CullTest& test : tests)
  {
    EXPECT_EQ(memory.Get32(test.address), test.first);
    EXPECT_EQ(memory.Get32(test.address + 4), test.second);
  }
}

bool Logged(PatchScript& script, std::string_view text)
{
  bool logged = false;
  for (const LogLine& line : script.TakeLog())
    logged |= line.text.find(text) != std::string::npos;
  return logged;
}

// One disc: its sites hooked when the game's code is there, the tests a savestate brought back
// with a no-culling code's change put back, a warning when the code is still on, and the game's
// calls back when the patch stops.
template <size_t S, size_t T>
void ExpectStandaloneCulling(std::string_view game_id, const std::array<CullSite, S>& sites,
                             u32 constructor, const std::array<CullTest, T>& tests)
{
  CullGame game(game_id);
  PatchScript script("wide-culling");
  ASSERT_TRUE(script.Load(ReadCullPatch(std::string(game_id)), game.env)) << script.Error();

  // Before the game's code is in memory: nothing written.
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(sites[0].hook), 0u);

  // A savestate made with the no-culling code on.
  LoadCullSites(game.memory, sites);
  LoadCullTests(game.memory, tests, true);
  ASSERT_TRUE(script.Call(Hook::OnStateLoaded, game.env)) << script.Error();
  ExpectCullHooks(game.memory, sites, constructor);
  ExpectCullTests(game.memory, tests);
  EXPECT_EQ(game.memory.Get32(CULL_MODE), 1u);
  EXPECT_TRUE(Logged(script, "put back " + std::to_string(T) + " culling test"));

  // The code still on writes its change again: a warning, once.
  LoadCullTests(game.memory, tests, true);
  game.env.field = 60;
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  EXPECT_TRUE(Warned(script, "no-culling Action Replay code is on"));
  game.env.field = 120;
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  EXPECT_FALSE(Warned(script, "no-culling"));

  ASSERT_TRUE(script.Call(Hook::OnUnload, game.env)) << script.Error();
  ExpectCullOriginals(game.memory, sites);
  EXPECT_EQ(game.memory.Get32(CULL_MODE), 0u);
}
}  // namespace

TEST(StandaloneWideCullingPatch, ManifestsAreValid)
{
  for (const std::string game_id : {"GM8E01", "G2ME01", "RM3E01"})
  {
    SCOPED_TRACE(game_id);
    const std::string source = ReadCullPatch(game_id);
    ASSERT_FALSE(source.empty()) << "Sys/VRPatches/" << game_id << "/wide-culling.lua is missing";
    const auto read = ReadManifest(source, "wide-culling");
    ASSERT_TRUE(std::holds_alternative<Manifest>(read)) << std::get<std::string>(read);
    const Manifest& manifest = std::get<Manifest>(read);
    EXPECT_EQ(manifest.games, std::vector<std::string>{game_id});
    // Metroid Prime: 1.00 and 1.02, recognized by their code.
    if (game_id == "GM8E01")
      EXPECT_FALSE(manifest.revision);
    else
      EXPECT_EQ(manifest.revision, 0);
    EXPECT_EQ(manifest.scratch_bytes, CULL_SCRATCH_SIZE);
    EXPECT_NE(manifest.FindSetting("mode"), nullptr);
    EXPECT_NE(manifest.FindSetting("fov"), nullptr);
    EXPECT_NE(manifest.FindSetting("follow_head"), nullptr);
  }
}

TEST(StandaloneWideCullingPatch, Prime100)
{
  ExpectStandaloneCulling("GM8E01", PRIME_100_CULL_SITES, PRIME_100_FRUSTUM, PRIME_100_CULL_TESTS);
}

TEST(StandaloneWideCullingPatch, Prime102)
{
  ExpectStandaloneCulling("GM8E01", PRIME_102_CULL_SITES, PRIME_102_FRUSTUM, PRIME_102_CULL_TESTS);
}

TEST(StandaloneWideCullingPatch, EchoesGameCube)
{
  ExpectStandaloneCulling("G2ME01", ECHOES_GC_CULL_SITES, ECHOES_GC_FRUSTUM, ECHOES_GC_CULL_TESTS);
}

TEST(StandaloneWideCullingPatch, CorruptionWii)
{
  ExpectStandaloneCulling("RM3E01", CORRUPTION_WII_CULL_SITES, CORRUPTION_WII_FRUSTUM,
                          CORRUPTION_WII_CULL_TESTS);
}

TEST(StandaloneWideCullingPatch, LeavesGameTestsAlone)
{
  // The tests as the game has them: nothing to put back, no warning.
  CullGame game("GM8E01");
  LoadCullSites(game.memory, PRIME_102_CULL_SITES);
  LoadCullTests(game.memory, PRIME_102_CULL_TESTS, false);
  PatchScript script("wide-culling");
  ASSERT_TRUE(script.Load(ReadCullPatch("GM8E01"), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  game.env.field = 60;
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  ExpectCullHooks(game.memory, PRIME_102_CULL_SITES, PRIME_102_FRUSTUM);
  ExpectCullTests(game.memory, PRIME_102_CULL_TESTS);
  const std::vector<LogLine> log = script.TakeLog();
  for (const LogLine& line : log)
  {
    EXPECT_EQ(line.text.find("put back"), std::string::npos) << line.text;
    EXPECT_EQ(line.level, 0) << line.text;
  }
}

// ---- The Trilogy: turn with the right thumbstick ----

namespace
{
constexpr u32 TRILOGY_TURN_SCRATCH = 0x800028B0;
constexpr u32 TRILOGY_TURN_CAVE = TRILOGY_TURN_SCRATCH + 0x20;

// Per game: the control mapper's function and the word that tells its code apart, the player's
// mapper, the register of the control number, the turn left and right and look up and down
// controls, and the camera's transform.
struct TurnGameInfo
{
  u32 function, check_offset, check_word, player_mapper, control_register;
  u32 left, right, up, down, camera_xf;
};
constexpr TurnGameInfo PRIME_TURN{0x80075F68, 0x14, 0xC3E28944, 0xA40, 4,
                                  3,          4,    9,          0xA,   0x80C00000 + 0x2C};
constexpr TurnGameInfo ECHOES_TURN{0x8006FE1C, 0x18, 0x48248589, 0x13BC, 5,
                                   3,          4,    9,          0xA,    0x80C00000 + 0x20};
constexpr TurnGameInfo CORRUPTION_TURN{0x800618B0, 0x14, 0xC3E288C4, 0x35D8, 4,
                                       2,          3,    0x37,       0x38,   0x80C00000 + 0x3C};

// The Trilogy's games with their control mappers and cameras (the camera object at 0x80C00000).
struct TrilogyTurnGame : TrilogyGame
{
  TrilogyTurnGame()
  {
    for (const TurnGameInfo& g : {PRIME_TURN, ECHOES_TURN, CORRUPTION_TURN})
    {
      memory.Set32(g.function, 0x9421FFC0);  // stwu sp, -0x40(sp)
      memory.Set32(g.function + g.check_offset, g.check_word);
    }
    // Prime and Echoes: camera manager -> object id 0x403 -> the object list.
    memory.Set32(0x804BF420 + 0x868, 0x80C10000);
    memory.Set32(0x80C10000, 0x04030000);
    memory.Set32(0x804BF420 + 0x810, 0x80C20000);
    memory.Set32(0x80C20000 + 3 * 8 + 4, 0x80C00000);
    memory.Set32(0x804E72E8 + 0x1514, 0x80C30000);
    memory.Set32(0x80C30000 + 0x10, 0x04030000);
    memory.Set32(0x804E72E8 + 0x810, 0x80C20000);
    // Corruption: [[[0x805C6C78] + 0x34] + 0x20].
    memory.Set32(0x805C6C78, 0x80C40000);
    memory.Set32(0x80C40000 + 0x34, 0x80C50000);
    memory.Set32(0x80C50000 + 0x20, 0x80C00000);
    input.hands[1].connected = true;
    env.pad = &pad;
    env.is_wii = true;
    env.scratch_base = TRILOGY_TURN_SCRATCH;
    env.scratch_size = 0x80;
  }

  // The previous game's program is gone (each arm cannon hook's next instruction).
  void Leave()
  {
    for (const u32 next : {0x8018BC78u, 0x8018ACB4u, 0x80183C2Cu})
      memory.Set32(next, 0);
  }

  WiimoteButtons pad;
};

std::string ReadTrilogyTurnPatch()
{
  return ReadPatch("R3ME01", "stick-turn");
}

// Whether a cave word compares the control register with a control number (cmplwi).
bool CaveCompares(const Memory& memory, u32 control_register, u32 control)
{
  const u32 word = 0x28000000u | (control_register << 16) | control;
  for (u32 a = TRILOGY_TURN_CAVE; a < TRILOGY_TURN_SCRATCH + 0x80; a += 4)
  {
    if (memory.Get32(a) == word)
      return true;
  }
  return false;
}

// One game of the disc: its mapper branches to the cave, which answers its controls for the
// player's mapper and otherwise runs the game's function; the thumbstick feeds the turn.
void ExpectTurns(TrilogyTurnGame& game, PatchScript& script, const TurnGameInfo& g)
{
  Memory& m = game.memory;
  game.input.hands[1].thumbstick_x = 0.6f;
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  EXPECT_EQ(m.Get32(g.function), 0x48000000u | ((TRILOGY_TURN_CAVE - g.function) & 0x03FFFFFC));
  for (const u32 control : {g.left, g.right, g.up, g.down})
    EXPECT_TRUE(CaveCompares(m, g.control_register, control)) << std::hex << control;
  u32 back = 0;
  for (u32 a = TRILOGY_TURN_CAVE; a < TRILOGY_TURN_SCRATCH + 0x80; a += 4)
  {
    if (m.Get32(a) == (0x48000000u | ((g.function + 4 - a) & 0x03FFFFFC)))
      back = a;
  }
  ASSERT_NE(back, 0u) << "the cave's branch back is not inside the scratch memory";
  EXPECT_EQ(m.Get32(back - 4), 0x9421FFC0u);
  // Right past the dead zone (0.2): turn right at half speed, for the player's mapper.
  EXPECT_EQ(m.Get32(TRILOGY_TURN_SCRATCH), TRILOGY_PLAYER + g.player_mapper);
  EXPECT_NEAR(GetF32(m, TRILOGY_TURN_SCRATCH + 8), 0.0f, 1e-6f);
  EXPECT_NEAR(GetF32(m, TRILOGY_TURN_SCRATCH + 0xC), 0.5f, 1e-5f);
  // While playing in first person the VR view removes the camera's pitch and the pointer stays
  // on the screen.
  EXPECT_EQ(script.GetCameraPitchSource(), g.camera_xf + 0x24);
  EXPECT_TRUE(script.KeepsPointerOnScreen());
}
}  // namespace

TEST(TrilogyStickTurnPatch, ManifestIsValid)
{
  const std::string source = ReadTrilogyTurnPatch();
  ASSERT_FALSE(source.empty()) << "Sys/VRPatches/R3ME01/stick-turn.lua is missing";
  const auto read = ReadManifest(source, "stick-turn");
  ASSERT_TRUE(std::holds_alternative<Manifest>(read)) << std::get<std::string>(read);
  const Manifest& manifest = std::get<Manifest>(read);
  EXPECT_EQ(manifest.games, std::vector<std::string>{"R3ME01"});
  EXPECT_EQ(manifest.revision, 0);
  EXPECT_EQ(manifest.scratch_bytes, 0x80u);
  EXPECT_TRUE(manifest.reserve.empty());
  for (const char* name :
       {"speed", "deadzone", "edge_tilt", "visor_button", "plus_button", "test_stick"})
  {
    EXPECT_NE(manifest.FindSetting(name), nullptr) << name;
  }
}

TEST(TrilogyStickTurnPatch, TurnsInEachGameAndRestores)
{
  TrilogyTurnGame game;
  Memory& m = game.memory;
  PatchScript script("stick-turn");
  ASSERT_TRUE(script.Load(ReadTrilogyTurnPatch(), game.env)) << script.Error();

  // The disc's menu: nothing written in the games' code.
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  EXPECT_EQ(m.Get32(PRIME_TURN.function), 0x9421FFC0u);
  EXPECT_EQ(m.Get32(TRILOGY_TURN_SCRATCH), 0u);

  game.RunPrime();
  ExpectTurns(game, script, PRIME_TURN);
  // Full left; inside the dead zone nothing.
  game.input.hands[1].thumbstick_x = -1.0f;
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  EXPECT_NEAR(GetF32(m, TRILOGY_TURN_SCRATCH + 8), 1.0f, 1e-6f);
  EXPECT_NEAR(GetF32(m, TRILOGY_TURN_SCRATCH + 0xC), 0.0f, 1e-6f);
  game.input.hands[1].thumbstick_x = 0.15f;
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  EXPECT_NEAR(GetF32(m, TRILOGY_TURN_SCRATCH + 0xC), 0.0f, 1e-6f);

  // While playing, - follows the right thumbstick click and + the right B; on the map the user's
  // bindings are back.
  game.input.hands[1].thumbstick_button = true;
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  EXPECT_EQ(Held(script, "-"), 1.0);
  EXPECT_EQ(Held(script, "+"), 0.0);
  game.Set(game.menu, 1);
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  EXPECT_EQ(Held(script, "-"), -1.0);
  EXPECT_EQ(Held(script, "+"), -1.0);
  // Prime's map: its camera is shown as drawn, the pointer as usual.
  EXPECT_FALSE(script.GetCameraPitchSource());
  EXPECT_FALSE(script.KeepsPointerOnScreen());
  game.Set(game.menu, 0);
  game.input.hands[1].thumbstick_button = false;

  // Echoes replaces Prime's program (its mapper takes the control in r5), then Corruption.
  game.Leave();
  game.RunEchoes();
  game.env.field = 60;
  ExpectTurns(game, script, ECHOES_TURN);
  game.Leave();
  game.RunCorruption();
  game.env.field = 120;
  ExpectTurns(game, script, CORRUPTION_TURN);

  // No player (a load): disarmed.
  m.Set32(0x80A00000 + 0x2184, 0);
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  EXPECT_EQ(m.Get32(TRILOGY_TURN_SCRATCH), 0u);
  EXPECT_FALSE(script.GetCameraPitchSource());
  for (const LogLine& line : script.TakeLog())
    EXPECT_NE(line.level, 1) << line.text;

  // Disabled: the game's code is back.
  m.Set32(0x80A00000 + 0x2184, TRILOGY_PLAYER);
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnUnload, game.env)) << script.Error();
  EXPECT_EQ(m.Get32(CORRUPTION_TURN.function), 0x9421FFC0u);
  EXPECT_EQ(m.Get32(TRILOGY_TURN_SCRATCH), 0u);
}

// Corruption's camera goes back to level on its map; the morph ball's camera is shown as it is;
// with the edges off, look up and down read 0 and the pitch is still removed.
TEST(TrilogyStickTurnPatch, LevelsTheView)
{
  TrilogyTurnGame game;
  Memory& m = game.memory;
  game.RunCorruption();
  PatchScript script("stick-turn");
  ASSERT_TRUE(script.Load(ReadTrilogyTurnPatch(), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  const auto frame = [&] { ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error(); };

  frame();
  EXPECT_EQ(m.Get32(TRILOGY_TURN_SCRATCH + 4), 0u);
  EXPECT_EQ(script.GetCameraPitchSource(), CORRUPTION_TURN.camera_xf + 0x24);

  m.Set32(TRILOGY_PLAYER + 0x784, std::bit_cast<u32>(0.5f));
  game.Set(game.menu, 1);
  frame();
  EXPECT_EQ(GetF32(m, TRILOGY_PLAYER + 0x784), 0.0f);
  EXPECT_EQ(script.GetCameraPitchSource(), CORRUPTION_TURN.camera_xf + 0x24);
  EXPECT_FALSE(script.KeepsPointerOnScreen());
  game.Set(game.menu, 0);

  game.Set(game.ball, 1);
  frame();
  EXPECT_FALSE(script.GetCameraPitchSource());
  EXPECT_FALSE(script.KeepsPointerOnScreen());
  game.Set(game.ball, 0);

  ASSERT_TRUE(script.SetSettingValue(*script.GetManifest().FindSetting("edge_tilt"), 0));
  frame();
  EXPECT_EQ(m.Get32(TRILOGY_TURN_SCRATCH + 4), 1u);
  EXPECT_EQ(script.GetCameraPitchSource(), CORRUPTION_TURN.camera_xf + 0x24);
  // Echoes stops turning two seconds after the pointer leaves the screen: kept on it anyway.
  EXPECT_TRUE(script.KeepsPointerOnScreen());
}

TEST(TrilogyStickTurnPatch, RefusesOtherCode)
{
  TrilogyTurnGame game;
  game.RunPrime();
  game.Set(PRIME_TURN.function + PRIME_TURN.check_offset, 0x60000000);  // another build
  PatchScript script("stick-turn");
  ASSERT_TRUE(script.Load(ReadTrilogyTurnPatch(), game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnLoad, game.env)) << script.Error();
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  EXPECT_EQ(game.memory.Get32(PRIME_TURN.function), 0x9421FFC0u);
  EXPECT_EQ(game.memory.Get32(TRILOGY_TURN_CAVE), 0u);
  EXPECT_EQ(game.memory.Get32(TRILOGY_TURN_SCRATCH), 0u);
  EXPECT_TRUE(Warned(script, "not installed"));
}

// ---- The Trilogy: change visors next to the head ----

namespace
{
std::string ReadTrilogyVisorPatch()
{
  return ReadPatch("R3ME01", "visor-gesture");
}

// The left hand next to the head pushes its thumbstick one way, then back to the centre.
void Push(TrilogyTurnGame& game, PatchScript& script, float x, float y)
{
  HandInput& hand = game.input.hands[0];
  hand.thumbstick_x = x;
  hand.thumbstick_y = y;
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
  hand.thumbstick_x = 0.0f;
  hand.thumbstick_y = 0.0f;
  ASSERT_TRUE(script.Call(Hook::OnFrame, game.env)) << script.Error();
}
}  // namespace

TEST(TrilogyVisorGesturePatch, ManifestIsValid)
{
  const std::string source = ReadTrilogyVisorPatch();
  ASSERT_FALSE(source.empty()) << "Sys/VRPatches/R3ME01/visor-gesture.lua is missing";
  const auto read = ReadManifest(source, "visor-gesture");
  ASSERT_TRUE(std::holds_alternative<Manifest>(read)) << std::get<std::string>(read);
  const Manifest& manifest = std::get<Manifest>(read);
  EXPECT_EQ(manifest.games, std::vector<std::string>{"R3ME01"});
  EXPECT_EQ(manifest.revision, 0);
  EXPECT_EQ(manifest.scratch_bytes, 0u);
}

// The directions follow each game's visor menu: up scan, left and right its other two visors,
// down combat; a visor whose powerup is not owned is skipped. Prime and Echoes lower the arm
// cannon for the scan visor.
TEST(TrilogyVisorGesturePatch, PicksEachGamesVisors)
{
  TrilogyTurnGame game;
  Memory& m = game.memory;
  game.input.head = {true, {0.0f, 1.6f, 0.0f}};
  HandInput& hand = game.input.hands[0];
  hand.connected = true;
  hand.aim = {true, {0.6f, 1.2f, -0.4f}};  // away from the head
  PatchScript script("visor-gesture");
  ASSERT_TRUE(script.Load(ReadTrilogyVisorPatch(), game.env)) << script.Error();

  // Prime: 0 combat, 1 X-ray, 2 scan, 3 thermal; the X-ray visor (item 0x0D) is not owned.
  game.RunPrime();
  for (const u32 item : {0x11u, 0x05u, 0x09u})
    m.Set32(TRILOGY_POWERUPS + item * 8 + 0x30, 1);
  const u32 prime_visor = TRILOGY_POWERUPS + 0x1C;
  // Away from the head the thumbstick walks as usual.
  Push(game, script, 0.0f, 1.0f);
  EXPECT_EQ(m.Get32(prime_visor), 0u);
  EXPECT_TRUE(script.GetPadHolds().empty());
  hand.aim = {true, {-0.12f, 1.55f, -0.05f}};
  Push(game, script, 0.0f, 1.0f);
  EXPECT_EQ(m.Get32(prime_visor), 2u);
  EXPECT_EQ(m.Get32(TRILOGY_PLAYER + 0x488), 3u);  // the cannon's holster
  EXPECT_EQ(GetF32(m, TRILOGY_PLAYER + 0x48C), 0.2f);
  Push(game, script, -1.0f, 0.0f);
  EXPECT_EQ(m.Get32(prime_visor), 3u);
  Push(game, script, 1.0f, 0.0f);
  EXPECT_EQ(m.Get32(prime_visor), 3u);
  Push(game, script, 0.0f, -1.0f);
  EXPECT_EQ(m.Get32(prime_visor), 0u);

  // Echoes: 0 combat, 1 echo, 2 scan, 3 dark, at [player + 0x12EC] + 0x34.
  game.Leave();
  game.RunEchoes();
  for (const u32 item : {0x08u, 0x09u, 0x0Au, 0x0Bu})
    m.Set32(TRILOGY_POWERUPS + item * 12 + 0x5C, 1);
  m.Set32(TRILOGY_PLAYER + 0xEA8, 0x80B20000);  // the gun
  const u32 echoes_visor = TRILOGY_POWERUPS + 0x34;
  Push(game, script, 0.0f, 1.0f);
  EXPECT_EQ(m.Get32(echoes_visor), 2u);
  EXPECT_EQ(m.Get32(0x80B20000 + 0x3A4), 3u);
  EXPECT_EQ(GetF32(m, 0x80B20000 + 0x3A4 - 0x20), 0.2f);
  Push(game, script, -1.0f, 0.0f);
  EXPECT_EQ(m.Get32(echoes_visor), 3u);
  Push(game, script, 1.0f, 0.0f);
  EXPECT_EQ(m.Get32(echoes_visor), 1u);

  // Corruption: 0 combat, 1 scan, 2 command, 3 X-ray, at [player + 0x35A8] + 0x34.
  game.Leave();
  game.RunCorruption();
  for (const u32 item : {0x0Bu, 0x0Cu, 0x0Du, 0x0Eu})
    m.Set32(TRILOGY_POWERUPS + item * 12 + 0x58, 1);
  m.Set32(TRILOGY_POWERUPS + 0x34, 0);
  Push(game, script, 0.0f, 1.0f);
  EXPECT_EQ(m.Get32(TRILOGY_POWERUPS + 0x34), 1u);
  Push(game, script, -1.0f, 0.0f);
  EXPECT_EQ(m.Get32(TRILOGY_POWERUPS + 0x34), 3u);
  Push(game, script, 1.0f, 0.0f);
  EXPECT_EQ(m.Get32(TRILOGY_POWERUPS + 0x34), 2u);
  Push(game, script, 0.0f, -1.0f);
  EXPECT_EQ(m.Get32(TRILOGY_POWERUPS + 0x34), 0u);

  // On the map nothing changes and the stick is the user's again.
  game.Set(game.menu, 1);
  Push(game, script, 0.0f, 1.0f);
  EXPECT_EQ(m.Get32(TRILOGY_POWERUPS + 0x34), 0u);
  EXPECT_TRUE(script.GetPadHolds().empty());
  for (const LogLine& line : script.TakeLog())
    EXPECT_NE(line.level, 1) << line.text;
}
