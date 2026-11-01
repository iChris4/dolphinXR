// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The shipped Shadow the Hedgehog frame rate patch (Data/Sys/VRPatches/GUP/headset-frame-rate.lua)
// against fake memory: the instruction it writes into the game's system update and the
// ForcedVBIFrequency it asks for, from the headset's refresh rate (vr.refresh_rate) or a fixed
// rate. Also the VBI rates ForcedVBIFrequency accepts.

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Core/Config/GraphicsSettings.h"
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
  void Set32(u32 a, u32 v) { Write32(a, v); }

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

using Values = std::vector<std::string>;

constexpr u32 RATE_LOAD = 0x801E1F54;
constexpr u32 RATE_LOAD_ORIGINAL = 0x80010018;  // lwz r0, 0x18(r1)
constexpr u32 NTSC_FALLBACK = 0x801E1F94;
constexpr u32 NTSC_FALLBACK_ORIGINAL = 0x3800003C;  // li r0, 60

constexpr u32 LiR0(u32 value)
{
  return 0x38000000 | value;
}

// Shadow the Hedgehog: Reloaded 1.2 (GUPR8P), its system update around the frame rate.
struct Shadow
{
  Shadow()
  {
    Config::Init();
    // The user's profile asks for 120 Hz (with a 120 FPS code).
    Config::SetCurrent(Config::GFX_VR_FORCED_VBI_FREQUENCY, 120);
    memory.Set32(0x801E1F50, 0x482A3E95);  // bl RwEngineGetVideoModeInfo
    memory.Set32(RATE_LOAD, RATE_LOAD_ORIGINAL);
    memory.Set32(0x801E1F74, 0x901F0050);  // stw r0, 0x50(r31)
    memory.Set32(NTSC_FALLBACK, NTSC_FALLBACK_ORIGINAL);
    memory.Set32(0x801E1F98, 0x901F0050);
    env.memory = &memory;
    env.input = &input;
    env.game_id = "GUPR8P";
    env.field = 1;
  }
  ~Shadow() { Config::ClearCurrentRunLayer(); }

  ::testing::AssertionResult Load()
  {
    std::string source;
    File::ReadFileToString(File::GetSysDirectory() + VRPATCHES_DIR "/GUP/headset-frame-rate.lua",
                           source);
    if (!script.Load(source, env))
      return ::testing::AssertionFailure() << script.Error();
    return ::testing::AssertionSuccess();
  }

  // One field; the ForcedVBIFrequency values the patch asked for in it.
  Values Frame(u64 field = 1)
  {
    env.field = field;
    EXPECT_TRUE(script.Call(Hook::OnFrame, env)) << script.Error();
    return Requests();
  }

  Values Requests()
  {
    Values values;
    for (const SettingRequest& request : script.TakeSettingRequests())
    {
      EXPECT_EQ(request.key, "ForcedVBIFrequency");
      values.push_back(request.value);
    }
    return values;
  }

  bool Warned(std::string_view text)
  {
    bool warned = false;
    for (const LogLine& line : script.TakeLog())
      warned |= line.level == 1 && line.text.find(text) != std::string::npos;
    return warned;
  }

  void SetRate(double index)
  {
    ASSERT_TRUE(script.SetSettingValue(*script.GetManifest().FindSetting("rate"), index));
  }

  Memory memory;
  VRInputFrame input;
  ScriptEnvironment env;
  PatchScript script{"headset-frame-rate"};
};
}  // namespace

TEST(FrameRatePatch, ManifestIsValid)
{
  Shadow game;
  ASSERT_TRUE(game.Load());
  const Manifest& manifest = game.script.GetManifest();
  EXPECT_EQ(manifest.games, (std::vector<std::string>{"GUPE8P", "GUPR8P"}));
  EXPECT_EQ(manifest.scratch_bytes, 0u);
  const SettingDef* rate = manifest.FindSetting("rate");
  ASSERT_NE(rate, nullptr);
  EXPECT_EQ(rate->options.size(), 7u);
  EXPECT_EQ(rate->default_value, 0.0);
}

TEST(FrameRatePatch, FollowsTheHeadsetAndRestores)
{
  Shadow game;
  ASSERT_TRUE(game.Load());

  // No VR session (or its first frames): the game's own 60 fps and VBI.
  EXPECT_EQ(game.Frame(), Values{});
  EXPECT_EQ(game.Frame(30), Values{});
  EXPECT_EQ(game.memory.Get32(RATE_LOAD), RATE_LOAD_ORIGINAL);

  // The runtime reports 89.96 Hz: 90 fps and a 90 Hz VBI, asked once.
  game.env.display_refresh_rate = 89.96;
  EXPECT_EQ(game.Frame(), Values{"90"});
  EXPECT_EQ(game.memory.Get32(RATE_LOAD), LiR0(90));
  EXPECT_EQ(game.memory.Get32(NTSC_FALLBACK), LiR0(90));
  EXPECT_EQ(game.Frame(), Values{});
  EXPECT_EQ(game.Frame(60), Values{});

  // A savestate made without the patch: its code comes back at once.
  game.memory.Set32(RATE_LOAD, RATE_LOAD_ORIGINAL);
  game.memory.Set32(NTSC_FALLBACK, NTSC_FALLBACK_ORIGINAL);
  ASSERT_TRUE(game.script.Call(Hook::OnStateLoaded, game.env)) << game.script.Error();
  EXPECT_EQ(game.Requests(), Values{});
  EXPECT_EQ(game.memory.Get32(RATE_LOAD), LiR0(90));
  EXPECT_EQ(game.memory.Get32(NTSC_FALLBACK), LiR0(90));
  EXPECT_FALSE(game.Warned(""));

  // Stopped: the game's code and the profile's VBI are back.
  ASSERT_TRUE(game.script.Call(Hook::OnUnload, game.env)) << game.script.Error();
  EXPECT_EQ(game.Requests(), Values{"120"});
  EXPECT_EQ(game.memory.Get32(RATE_LOAD), RATE_LOAD_ORIGINAL);
  EXPECT_EQ(game.memory.Get32(NTSC_FALLBACK), NTSC_FALLBACK_ORIGINAL);
}

TEST(FrameRatePatch, FixedRateNeedsAVRSession)
{
  Shadow game;
  ASSERT_TRUE(game.Load());
  game.SetRate(5);  // 120 fps

  // Without a session ForcedVBIFrequency does not apply: 120 fps would play in slow motion.
  EXPECT_EQ(game.Frame(30), Values{});
  EXPECT_EQ(game.memory.Get32(RATE_LOAD), RATE_LOAD_ORIGINAL);

  game.env.display_refresh_rate = 90.0;
  EXPECT_EQ(game.Frame(), Values{"120"});
  EXPECT_EQ(game.memory.Get32(RATE_LOAD), LiR0(120));

  // Back to the headset's rate.
  game.SetRate(0);
  EXPECT_EQ(game.Frame(), Values{"90"});
  EXPECT_EQ(game.memory.Get32(RATE_LOAD), LiR0(90));
  EXPECT_EQ(game.memory.Get32(NTSC_FALLBACK), LiR0(90));
}

TEST(FrameRatePatch, WarnsAboutAFrameRateCode)
{
  Shadow game;
  ASSERT_TRUE(game.Load());
  game.env.display_refresh_rate = 90.0;
  EXPECT_EQ(game.Frame(), Values{"90"});
  EXPECT_FALSE(game.Warned(""));

  // The 120 FPS Action Replay code writes the same instruction.
  game.memory.Set32(RATE_LOAD, LiR0(120));
  EXPECT_EQ(game.Frame(30), Values{});
  EXPECT_TRUE(game.Warned("Action Replay"));
  EXPECT_EQ(game.memory.Get32(RATE_LOAD), LiR0(90));
}

TEST(FrameRatePatch, RefusesOtherCode)
{
  Shadow game;
  game.memory.Set32(0x801E1F74, 0x60000000);  // not this game's system update
  ASSERT_TRUE(game.Load());
  game.env.display_refresh_rate = 90.0;
  EXPECT_EQ(game.Frame(), Values{});
  EXPECT_TRUE(game.Warned("not installed"));
  EXPECT_EQ(game.memory.Get32(RATE_LOAD), RATE_LOAD_ORIGINAL);
  EXPECT_EQ(game.memory.Get32(NTSC_FALLBACK), NTSC_FALLBACK_ORIGINAL);
  ASSERT_TRUE(game.script.Call(Hook::OnUnload, game.env)) << game.script.Error();
  EXPECT_EQ(game.Requests(), Values{});
}

TEST(FrameRatePatch, ReadsTheHeadsetRefreshRate)
{
  Memory memory;
  VRInputFrame input;
  ScriptEnvironment env;
  env.memory = &memory;
  env.input = &input;
  PatchScript script("refresh-rate");
  ASSERT_TRUE(script.Load(R"(
    patch = {title = "Refresh rate"}
    function on_frame()
      mem.write_u32(0x80010000, vr.refresh_rate() == nil and 1 or 0)
      mem.write_f32(0x80010004, vr.refresh_rate() or -1)
    end
  )",
                          env))
      << script.Error();
  EXPECT_TRUE(script.Call(Hook::OnFrame, env)) << script.Error();
  EXPECT_EQ(memory.Get32(0x80010000), 1u);
  env.display_refresh_rate = 72.0;
  EXPECT_TRUE(script.Call(Hook::OnFrame, env)) << script.Error();
  EXPECT_EQ(memory.Get32(0x80010000), 0u);
  EXPECT_EQ(memory.Get32(0x80010004), 0x42900000u);  // 72.0f
}

TEST(FrameRatePatch, ForcedVBIFrequencyTakesAnyRate)
{
  EXPECT_EQ(Config::NormalizeVRForcedVBIFrequency(-1), -1);
  EXPECT_EQ(Config::NormalizeVRForcedVBIFrequency(0), 0);
  EXPECT_EQ(Config::NormalizeVRForcedVBIFrequency(72), 72);
  EXPECT_EQ(Config::NormalizeVRForcedVBIFrequency(80), 80);
  EXPECT_EQ(Config::NormalizeVRForcedVBIFrequency(144), 144);
  EXPECT_EQ(Config::NormalizeVRForcedVBIFrequency(29), 0);
  EXPECT_EQ(Config::NormalizeVRForcedVBIFrequency(241), 0);
  EXPECT_EQ(Config::NormalizeVRForcedVBIFrequency(-2), 0);
}
