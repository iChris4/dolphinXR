// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The shipped Metroid Prime state patches (Data/Sys/VRPatches/<id>/metroid-state.lua) against fake
// memory: the override flags they raise for the Sys profiles' entries (Prime, Echoes, Corruption).
// Also Echoes' view patches: no morph ball or cutscene bars, no scan box highlight, smooth scan
// lines.

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <map>
#include <set>
#include <string>
#include <string_view>

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

void SetTexture(Memory& memory, const TextureWords& words)
{
  for (u32 i = 0; i < words.size(); ++i)
    memory.Set32(LINES_TEXTURE + i * 4, words[i]);
}

TextureWords GetTexture(const Memory& memory)
{
  TextureWords words;
  for (u32 i = 0; i < words.size(); ++i)
    words[i] = memory.Get32(LINES_TEXTURE + i * 4);
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
  // look up and down read 0.
  game.input.hands[1].thumbstick_x = 0.6f;
  frame();
  EXPECT_EQ(m.Get32(TURN_SCRATCH), CORRUPTION_PLAYER + 0x35C4);
  EXPECT_EQ(m.Get32(TURN_SCRATCH + 4), 1u);
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

  // While playing, - follows the right grip and + the right B (and the thumbstick no longer
  // presses them); on the map the user's bindings are back.
  game.input.hands[1].squeeze_button = true;
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
