// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The shipped Metroid Prime state patches (Data/Sys/VRPatches/<id>/metroid-state.lua) against fake
// memory: the override flags they raise for the Sys profiles' entries. Also Echoes' view patches:
// no morph ball or cutscene bars, no scan box highlight, smooth scan lines.

#include <gtest/gtest.h>

#include <array>
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
