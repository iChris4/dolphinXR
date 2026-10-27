// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "Core/VRPatch/GeckoRegion.h"
#include "Core/VRPatch/MemoryAccess.h"
#include "Core/VRPatch/PatchIni.h"
#include "Core/VRPatch/PatchScript.h"
#include "Core/VRPatch/PatchTypes.h"
#include "Core/VRPatch/VRPatch.h"

using namespace VRPatch;

namespace
{
// MEM1 as a sparse byte map; unwritten bytes read as 0.
class FakeMemory final : public MemoryAccess
{
public:
  bool IsRAM(u32 address, u32 size) const override
  {
    const u64 end = static_cast<u64>(address) + size;
    return size != 0 && address >= 0x80000000 && end <= 0x81800000;
  }
  std::optional<u8> Read8(u32 address) const override { return Read<u8>(address); }
  std::optional<u16> Read16(u32 address) const override { return Read<u16>(address); }
  std::optional<u32> Read32(u32 address) const override { return Read<u32>(address); }
  std::optional<u64> Read64(u32 address) const override { return Read<u64>(address); }
  bool Write8(u32 address, u8 value) override { return Write<u8>(address, value); }
  bool Write16(u32 address, u16 value) override { return Write<u16>(address, value); }
  bool Write32(u32 address, u32 value) override { return Write<u32>(address, value); }
  bool Write64(u32 address, u64 value) override { return Write<u64>(address, value); }
  void InvalidateCode(u32 address, u32 size) override { invalidations.emplace_back(address, size); }

  void Set32(u32 address, u32 value) { Write32(address, value); }
  u32 Get32(u32 address) const { return Read32(address).value_or(0xDEADBEEF); }
  void SetF32(u32 address, float value) { Write32(address, std::bit_cast<u32>(value)); }
  float GetF32(u32 address) const { return std::bit_cast<float>(Get32(address)); }

  std::vector<std::pair<u32, u32>> invalidations;
  u32 writes = 0;

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
    ++writes;
    return true;
  }

  std::map<u32, u8> bytes;
};

// A GameCube controller on port 1 only.
class FakePad final : public PadResolver
{
public:
  std::vector<PadControl> Resolve(PadDevice device, u8 port, PadControlKind kind,
                                  std::string_view name, std::string* error) override
  {
    ++resolves;
    if (device == PadDevice::Wiimote && port == 0 && kind == PadControlKind::Pointer &&
        name == "IR")
    {
      return {{"IR", "X"}, {"IR", "Y"}};
    }
    if (device != PadDevice::GCPad || port != 0)
    {
      *error = "nothing plugged in";
      return {};
    }
    if (kind == PadControlKind::Button && (name == "A" || name == "B"))
      return {{"Buttons", std::string(name)}};
    if (kind == PadControlKind::Button && name == "L")
      return {{"Triggers", "L"}, {"Triggers", "L-Analog"}};
    if (kind == PadControlKind::Analog && name == "L-Analog")
      return {{"Triggers", "L-Analog"}};
    if (kind == PadControlKind::Stick && (name.empty() || name == "Main Stick"))
      return {{"Main Stick", "X"}, {"Main Stick", "Y"}};
    *error = "unknown control";
    return {};
  }
  int resolves = 0;
};

struct Harness
{
  Harness()
  {
    env.memory = &memory;
    env.input = &input;
    env.pad = &pad;
    env.game_id = "GTEST1";
    env.field = 42;
    env.scratch_base = 0x80002F00;
    env.scratch_size = 0x100;
    env.units_per_meter = 1.5;
  }

  // Loads the source (with a minimal manifest prepended when it has none).
  bool Load(PatchScript& script, const std::string& body)
  {
    const std::string source =
        body.find("patch =") == std::string::npos ? "patch = {title = 'Test'}\n" + body : body;
    return script.Load(source, env);
  }

  FakeMemory memory;
  VRInputFrame input;
  FakePad pad;
  ScriptEnvironment env;
};

// Runs `body` as on_frame and expects it to succeed.
void ExpectFrame(const std::string& body)
{
  Harness h;
  PatchScript script("test");
  ASSERT_TRUE(h.Load(script, "function on_frame()\n" + body + "\nend")) << script.Error();
  EXPECT_TRUE(script.Call(Hook::OnFrame, h.env)) << script.Error();
}
}  // namespace

TEST(VRPatch, SandboxHasNoFileOrProcessAccess)
{
  ExpectFrame(R"(
    assert(io == nil and os == nil and package == nil and debug == nil)
    assert(require == nil and load == nil and loadfile == nil and dofile == nil)
    assert(string and table and math and utf8 and coroutine)
  )");
}

TEST(VRPatch, RefusesBinaryChunks)
{
  Harness h;
  PatchScript script("test");
  EXPECT_FALSE(script.Load(std::string("\x1bLua\x54", 5), h.env));
}

TEST(VRPatch, ReadsTheManifest)
{
  const auto read = ReadManifest(R"(
    local mem_u32 = mem.u32  -- the API tables exist while the manifest is read
    patch = {
      title = "Arm cannon", author = "Someone", version = "1.2", games = {"GM8E01", "GM8"},
      revision = 0, scratch = 0x34,
      reserve = {{0x80002300, 0x80002400}},
      gecko = {"04001234 00000001\n# comment\n04001238   00000002"},
      conflicts = {texture = {"Cannon"}, elements = "Canon test"},
      settings = {
        {name = "hand", type = "choice", options = {"Left", "Right"}, default = 1},
        {name = "pitch", label = "Pitch", unit = "deg", default = 200, min = -90, max = 90, step = 1},
        {name = "beams", type = "bool", default = true},
      },
    }
    function on_frame() end
  )",
                                 "cannon");
  ASSERT_TRUE(std::holds_alternative<Manifest>(read)) << std::get<std::string>(read);
  const Manifest& manifest = std::get<Manifest>(read);
  EXPECT_EQ(manifest.title, "Arm cannon");
  EXPECT_EQ(manifest.author, "Someone");
  EXPECT_EQ(manifest.games, (std::vector<std::string>{"GM8E01", "GM8"}));
  EXPECT_EQ(manifest.revision, 0);
  EXPECT_EQ(manifest.scratch_bytes, 0x40u);  // rounded up to 16
  ASSERT_EQ(manifest.reserve.size(), 1u);
  EXPECT_EQ(manifest.reserve[0].start, 0x80002300u);
  EXPECT_EQ(manifest.gecko_lines,
            (std::vector<std::string>{"04001234 00000001", "04001238 00000002"}));
  ASSERT_EQ(manifest.conflicts.size(), 2u);
  ASSERT_EQ(manifest.settings.size(), 3u);
  EXPECT_EQ(manifest.settings[0].type, SettingType::Choice);
  EXPECT_EQ(manifest.settings[0].max, 1.0);
  EXPECT_EQ(manifest.settings[0].default_value, 1.0);
  EXPECT_EQ(manifest.settings[1].label, "Pitch");
  EXPECT_EQ(manifest.settings[1].default_value, 90.0);  // clamped
  EXPECT_EQ(manifest.settings[2].type, SettingType::Bool);
  EXPECT_EQ(manifest.settings[2].default_value, 1.0);
  EXPECT_EQ(manifest.settings[2].label, "beams");
}

TEST(VRPatch, RejectsInvalidManifests)
{
  const auto error_of = [](const std::string& source) {
    const auto read = ReadManifest(source, "bad");
    return std::holds_alternative<std::string>(read) ? std::get<std::string>(read) : std::string();
  };
  EXPECT_NE(error_of("function on_frame() end"), "");
  EXPECT_NE(error_of("patch = {}").find("title"), std::string::npos);
  EXPECT_NE(error_of("patch = {title = 'x', settings = {{name = 'a', type = 'slider'}}}"), "");
  EXPECT_NE(error_of("patch = {title = 'x', settings = {{name = 'a', min = 2, max = 1}}}"), "");
  EXPECT_NE(error_of("patch = {title = 'x', settings = {{name = 'a'}, {name = 'a'}}}"), "");
  EXPECT_NE(error_of("patch = {title = 'x', settings = {{name = '1a'}}}"), "");
  EXPECT_NE(error_of("patch = {title = 'x', settings = {{name = 'c', type = 'choice'}}}"), "");
  EXPECT_NE(error_of("patch = {title = 'x', reserve = {{0x80002400, 0x80002300}}}"), "");
  EXPECT_NE(error_of("patch = {title = 'x', reserve = {{0x10, 0x20}}}"), "");
  EXPECT_NE(error_of("patch = {title = 'x', gecko = {'0400123 1'}}"), "");
  EXPECT_NE(error_of("patch = {title = 'x', games = {'GM8E01X'}}"), "");
  EXPECT_NE(error_of("patch = {title = 'x', scratch = 0x10000}"), "");
  EXPECT_NE(error_of("patch = {title = 'x'} error('top level fails')").find("top level fails"),
            std::string::npos);
  EXPECT_NE(error_of("patch = {title = 'x' "), "");  // syntax error
}

TEST(VRPatch, StopsRunawayCalls)
{
  Harness h;
  ScriptLimits limits;
  limits.max_milliseconds = 20;
  PatchScript script("loop", limits);
  ASSERT_TRUE(h.Load(script, "n = 0\nfunction on_frame() n = n + 1 while true do end end\n"
                             "function on_unload() mem.write_u32(0x80010000, n) end"))
      << script.Error();
  EXPECT_FALSE(script.Call(Hook::OnFrame, h.env));
  EXPECT_NE(script.Error().find("too long"), std::string::npos) << script.Error();
  // The state stays usable after an abort.
  EXPECT_FALSE(script.Call(Hook::OnFrame, h.env));
  EXPECT_TRUE(script.Call(Hook::OnUnload, h.env)) << script.Error();
  EXPECT_EQ(h.memory.Get32(0x80010000), 2u);
}

TEST(VRPatch, CapsMemory)
{
  Harness h;
  ScriptLimits limits;
  limits.max_memory_bytes = 2 * 1024 * 1024;
  PatchScript script("hog", limits);
  ASSERT_TRUE(h.Load(script, "function on_frame() local t = {} for i = 1, 1e7 do t[i] = i end end"))
      << script.Error();
  EXPECT_FALSE(script.Call(Hook::OnFrame, h.env));
  EXPECT_NE(script.Error().find("memory"), std::string::npos) << script.Error();
  EXPECT_LE(script.GetMemoryUsed(), limits.max_memory_bytes);
}

TEST(VRPatch, ReadsAndWritesMemory)
{
  Harness h;
  h.memory.Set32(0x80001000, 0x80002000);  // a pointer
  h.memory.Set32(0x80002010, 0x80003000);  // another, at +0x10
  h.memory.Set32(0x80003008, 0x12345678);
  h.memory.SetF32(0x80004000, 1.5f);
  PatchScript script("mem");
  ASSERT_TRUE(h.Load(script, R"(
    function on_frame()
      assert(mem.u32(0x80003008) == 0x12345678)
      assert(mem.u16(0x8000300A) == 0x5678 and mem.u8(0x80003008) == 0x12)
      assert(mem.s8(0x80003008) == 0x12 and mem.s32(0x80003008) == 0x12345678)
      assert(mem.f32(0x80004000) == 1.5)
      assert(mem.u32(0x10) == nil, "not RAM")
      assert(mem.ptr(0x80001000) == 0x80002000)
      assert(mem.ptr(0x80001000, 0x10) == 0x80003000)
      assert(mem.chain(0x80001000, 0x10, 0x8) == 0x80003008)
      assert(mem.u32(mem.chain(0x80001000, 0x10, 0x8)) == 0x12345678)
      assert(mem.ptr(0x80003008, 0) == nil, "0x12345678 is not RAM")
      assert(mem.write_u32(0x80005000, -1) and mem.u32(0x80005000) == 0xFFFFFFFF)
      assert(mem.write_u16(0x80005004, 0xABCD) and mem.u16(0x80005004) == 0xABCD)
      assert(mem.write_f32(0x80005008, 0.25) and mem.f32(0x80005008) == 0.25)
      assert(not mem.write_f32(0x8000500C, 0 / 0), "NaN refused")
      assert(not mem.write_f32(0x8000500C, math.huge), "infinity refused")
      assert(mem.write_f64(0x80005010, 0.1) and mem.f64(0x80005010) == 0.1)
      assert(not mem.write_u32(0xCC000000, 1), "MMIO refused")
      local m = mat34(1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30)
      assert(mem.write_mat34(0x80006000, m))
      local back = mem.mat34(0x80006000)
      assert(back:translation() == vec3(10, 20, 30) and back[1] == 1)
      assert(mem.write_vec3(0x80006100, vec3(1, 2, 3)) and mem.vec3(0x80006100) == vec3(1, 2, 3))
      assert(mem.valid(0x80006100, 12) and not mem.valid(0x81800000))
      assert(mem.scratch == 0x80002F00 and mem.scratch_size == 0x100)
      assert(mem.hex(0x8045A1A8) == "8045A1A8")
    end
  )")) << script.Error();
  EXPECT_TRUE(script.Call(Hook::OnFrame, h.env)) << script.Error();
  EXPECT_EQ(h.memory.Get32(0x80005000), 0xFFFFFFFFu);
  EXPECT_EQ(h.memory.GetF32(0x80006000 + 3 * 4), 10.0f);
}

TEST(VRPatch, WritesCode)
{
  Harness h;
  PatchScript script("code");
  ASSERT_TRUE(h.Load(script, R"(
    first, second = nil, nil
    function on_frame()
      first = mem.ensure_code(0x80040000, {0x60000000, 0x4E800020})
      second = mem.ensure_code(0x80040000, {0x60000000, 0x4E800020})
      assert(first == true and second == false)
      local words = mem.read_words(0x80040000, 2)
      assert(words[1] == 0x60000000 and words[2] == 0x4E800020)
      assert(mem.write_code(0x80040010, 0x38600001))
      assert(mem.ensure_code(0x80040002, {0}) == nil, "unaligned")
      assert(mem.ensure_patches({{0x80040020, 0x60000000}, {0x80040024, 0x60000000}}) == true)
    end
  )")) << script.Error();
  EXPECT_TRUE(script.Call(Hook::OnFrame, h.env)) << script.Error();
  EXPECT_EQ(h.memory.Get32(0x80040004), 0x4E800020u);
  // Only writes that changed something invalidate the JIT.
  ASSERT_EQ(h.memory.invalidations.size(), 4u);
  EXPECT_EQ(h.memory.invalidations[0], std::make_pair(0x80040000u, 8u));
  EXPECT_EQ(h.memory.invalidations[1], std::make_pair(0x80040010u, 4u));
}

TEST(VRPatch, GuardsLowMemory)
{
  // Low memory (OS globals, exception vectors, Gecko code handler) takes writes only in the
  // patch's scratch memory (80002F00-80003000 here) and its reserve ranges.
  const auto error_of = [](const std::string& body) {
    Harness h;
    PatchScript script("low");
    EXPECT_TRUE(h.Load(script, "patch = {title = 'Low', reserve = {{0x80001800, 0x80001810}}}\n"
                               "function on_frame()\n" +
                                   body + "\nend"))
        << script.Error();
    return script.Call(Hook::OnFrame, h.env) ? std::string() : script.Error();
  };
  EXPECT_EQ(error_of("assert(mem.write_u32(mem.scratch + 0xFC, 1))"), "");
  EXPECT_EQ(error_of("assert(mem.write_mat34(mem.scratch + 0xC0, mat34.identity()))"), "");
  EXPECT_EQ(error_of("assert(mem.ensure_code(0x80001800, {0x60000000, 0x60000000}))"), "");
  EXPECT_EQ(error_of("assert(mem.write_code(0x80004000, 0x60000000))"), "");

  // A cave that does not fit its scratch: the error names the range, the scratch and the line.
  const std::string over = error_of("mem.ensure_code(mem.scratch + 0xF8, {0, 0, 0})");
  EXPECT_NE(over.find("mem.ensure_code: 80002FF8-80003004 is in low memory"), std::string::npos)
      << over;
  EXPECT_NE(over.find("80002F00-80003000"), std::string::npos) << over;
  EXPECT_NE(over.find("low.lua:3:"), std::string::npos) << over;

  EXPECT_NE(error_of("mem.write_u32(0x800030E4, 0)"), "");            // OS globals
  EXPECT_NE(error_of("mem.write_code(0x80000300, 0x4C000064)"), "");  // an exception vector
  EXPECT_NE(error_of("mem.write_f32s(0x8000180C, 1, 2)"), "");        // past the reserve range
  EXPECT_NE(error_of("mem.write_vec3(mem.scratch + 0xF8, vec3(1, 2, 3))"), "");
  EXPECT_NE(error_of("mem.ensure_patches({{0x80002E00, 0x60000000}})"), "");
  EXPECT_NE(error_of("mem.write_u8(0xC0003000, 0)"), "");  // the uncached mirror

  // The manager gives the scratch block only to the load; later calls get none, and the scratch
  // stays writable.
  Harness h;
  PatchScript script("calls");
  ASSERT_TRUE(h.Load(script, "function on_frame() assert(mem.write_u32(mem.scratch, 7)) end"))
      << script.Error();
  ScriptEnvironment call_env = h.env;
  call_env.scratch_base = 0;
  call_env.scratch_size = 0;
  EXPECT_TRUE(script.Call(Hook::OnFrame, call_env)) << script.Error();
  EXPECT_EQ(h.memory.Get32(0x80002F00), 7u);
}

TEST(VRPatch, NoMemoryMeansNil)
{
  Harness h;
  h.env.memory = nullptr;
  PatchScript script("nomem");
  ASSERT_TRUE(h.Load(script, R"(
    function on_unload()
      assert(mem.u32(0x80010000) == nil and mem.write_u32(0x80010000, 1) == false)
      assert(mem.ensure_code(0x80010000, 0) == nil and mem.mat34(0x80010000) == nil)
    end
  )")) << script.Error();
  EXPECT_TRUE(script.Call(Hook::OnUnload, h.env)) << script.Error();
}

TEST(VRPatch, Assembles)
{
  ExpectFrame(R"(
    local words = assert(asm.assemble("nop\nblr", 0x80001000))
    assert(#words == 2 and words[1] == 0x60000000 and words[2] == 0x4E800020)
    -- The hook PrimedGun writes at 80040FEC (b 0x80002400) and its cave's way back.
    assert(asm.branch(0x80040FEC, 0x80002400) == 0x4BFC1414)
    assert(asm.branch(0x800025CC, 0x80040FF0) == 0x4803EA24)
    assert(asm.branch(0x800DF8DC, 0x80002300, true) == 0x4BF22A25)
    -- An absolute target goes in backquotes; a bare number is a displacement.
    local b = assert(asm.assemble("b `0x80040FF0`\nb 8", 0x800025CC))
    assert(b[1] == 0x4803EA24 and b[2] == 0x48000008)
    local none, err = asm.assemble("not_an_instruction r3", 0x80001000)
    assert(none == nil and type(err) == "string")
    assert(asm.disassemble(0x60000000, 0x80001000):find("nop"))
  )");
}

TEST(VRPatch, MathHelpers)
{
  ExpectFrame(R"(
    local function near(a, b, eps) return math.abs(a - b) < (eps or 1e-6) end
    local function near3(a, b) return near(a.x, b.x) and near(a.y, b.y) and near(a.z, b.z) end

    -- +90 degrees about +Y turns -Z (forward) into -X (left).
    local q = quat.axis_y(90)
    assert(near3(q * vec3(0, 0, -1), vec3(-1, 0, 0)))
    assert(near3(q:conjugate() * (q * vec3(1, 2, 3)), vec3(1, 2, 3)))
    local yaw, pitch, roll = q:to_yaw_pitch_roll()
    assert(near(yaw, 90, 1e-4) and near(pitch, 0, 1e-4) and near(roll, 0, 1e-4))

    -- Quaternion -> matrix -> quaternion.
    local r = quat.from_euler(20, -35, 10)
    local m = mat33.from_quat(r)
    assert(m:is_orthonormal(1e-6) and near(m:determinant(), 1))
    assert(r:angle_to(m:to_quat()) < 1e-3)
    assert(near3(m * vec3(0.3, -2, 5), r * vec3(0.3, -2, 5)))
    assert(near3((m * m:transpose()) * vec3(1, 2, 3), vec3(1, 2, 3)))

    -- Affine inverse.
    local t = mat34.from_quat(r, vec3(4, 5, 6))
    local p = vec3(-1, 7, 2)
    assert(near3(t:inverse() * (t * p), p))
    assert(near3((t * t:inverse()):translation(), vec3(0, 0, 0)))
    assert(t:looks_like_transform() and not mat34(0,0,0,0,0,0,0,0,0,0,0,0):looks_like_transform())

    -- Poses.
    local a = pose(vec3(1, 0, 0), quat.axis_y(90))
    local b = pose(vec3(0, 0, -2), quat.identity())
    local ab = a * b
    assert(near3(ab.pos, vec3(-1, 0, 0)))
    assert(near3((a:inverse() * ab).pos, b.pos))
    assert(near3(a:matrix() * vec3(0, 0, -2), ab.pos))
    assert(near3(pose():forward(), vec3(0, 0, -1)))

    -- Smoothing keeps 24 percent of the previous value and snaps on jumps.
    local s = smoother(0.24, 2.0)
    assert(s:update(vec3(0, 0, 0)) == vec3(0, 0, 0))
    assert(near3(s:update(vec3(1, 0, 0)), vec3(0.76, 0, 0)))
    assert(near3(s:update(vec3(10, 0, 0)), vec3(10, 0, 0)))
    local n = smoother(0.5)
    n:update(0)
    assert(near(n:update(1), 0.5))

    assert(wrap180(190) == -170 and wrap180(-180) == -180 and wrap180(45) == 45)
    assert(clamp(5, 0, 1) == 1 and lerp(0, 10, 0.25) == 2.5)
  )");
}

TEST(VRPatch, ReadsVRInput)
{
  Harness h;
  h.input.runtime_active = true;
  h.input.head = {true, {0.0f, 1.6f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
  h.input.head_view = {true, {0.0f, 0.1f, 0.05f}, {0.0f, 0.0f, 0.0f, 1.0f}};
  HandInput& right = h.input.hands[1];
  right.connected = true;
  right.primary = true;
  right.trigger = 0.75f;
  // 90 degrees about +Y.
  const float s = std::sqrt(0.5f);
  right.aim = {true, {0.2f, 1.2f, -0.4f}, {0.0f, s, 0.0f, s}};
  right.linear_velocity_valid = true;
  right.linear_velocity = {1.0f, 0.0f, 0.0f};
  PatchScript script("vr");
  ASSERT_TRUE(h.Load(script, R"(
    function on_frame()
      local function near(a, b) return math.abs(a - b) < 1e-5 end
      assert(vr.active() and vr.connected("right") and not vr.connected(0))
      assert(vr.button("right", "primary") and not vr.button(1, "secondary"))
      assert(near(vr.axis("right", "trigger"), 0.75))
      local hand = assert(vr.hand("right"))
      assert(near(hand.pos.y, 1.2) and near(hand.rot.w, math.sqrt(0.5)))
      assert(vr.hand("left") == nil and vr.hand("right", "grip") == nil)
      local rel = assert(vr.hand_in_head_space("right"))
      assert(near(rel.pos.y, -0.4) and near(rel.pos.z, -0.4))
      assert(near(vr.head().pos.y, 1.6) and near(vr.head("view").pos.y, 0.1))
      local ok, _, _, z = vr.raw_pose("head", "view")
      assert(ok and near(z, 0.05))
      local linear, angular = vr.velocity("right")
      assert(linear.x == 1 and angular == nil)
      assert(vr.units_per_meter() == 1.5 and game.id() == "GTEST1" and game.field() == 42)
      local ok = pcall(vr.button, "middle", "primary")
      assert(not ok)
    end
  )")) << script.Error();
  EXPECT_TRUE(script.Call(Hook::OnFrame, h.env)) << script.Error();
}

TEST(VRPatch, SettingsAndHooks)
{
  Harness h;
  PatchScript script("settings");
  ASSERT_TRUE(h.Load(script, R"(
    patch = {title = "S", settings = {
      {name = "pitch", default = 5, min = -10, max = 10},
      {name = "on", type = "bool", default = false},
    }}
    changes = 0
    function on_setting_changed(name, value)
      changes = changes + 1
      mem.write_u32(0x80010000, changes)
      if name == "on" then assert(value == true) end
    end
    function on_frame()
      mem.write_f32(0x80010004, settings.pitch)
      mem.write_u32(0x80010008, settings.on and 1 or 0)
    end
  )")) << script.Error();
  ASSERT_TRUE(script.HasSettingChangedHook());
  EXPECT_TRUE(script.Call(Hook::OnFrame, h.env)) << script.Error();
  EXPECT_EQ(h.memory.GetF32(0x80010004), 5.0f);
  EXPECT_EQ(h.memory.Get32(0x80010008), 0u);

  const Manifest& manifest = script.GetManifest();
  ASSERT_TRUE(script.SetSettingValue(*manifest.FindSetting("pitch"), 50));  // clamped to 10
  ASSERT_TRUE(script.SetSettingValue(*manifest.FindSetting("on"), 1));
  EXPECT_TRUE(script.CallSettingChanged("on", 1, h.env)) << script.Error();
  EXPECT_TRUE(script.Call(Hook::OnFrame, h.env)) << script.Error();
  EXPECT_EQ(h.memory.GetF32(0x80010004), 10.0f);
  EXPECT_EQ(h.memory.Get32(0x80010008), 1u);
  EXPECT_EQ(h.memory.Get32(0x80010000), 1u);
}

TEST(VRPatch, LogsAreRateLimited)
{
  Harness h;
  PatchScript script("log");
  ASSERT_TRUE(h.Load(script, R"(
    function on_load() log("hello", 1, true, nil) warn("careful") print("p") end
    function on_frame() for i = 1, 500 do log(i) end end
  )")) << script.Error();
  EXPECT_TRUE(script.Call(Hook::OnLoad, h.env)) << script.Error();
  std::vector<LogLine> log = script.TakeLog();
  ASSERT_EQ(log.size(), 3u);
  EXPECT_EQ(log[0].text, "hello 1 true nil");
  EXPECT_EQ(log[0].field, 42u);
  EXPECT_EQ(log[1].level, 1);
  EXPECT_TRUE(script.Call(Hook::OnFrame, h.env)) << script.Error();
  EXPECT_LE(script.TakeLog().size(), 60u);
}

TEST(VRPatch, HoldsControllerInput)
{
  Harness h;
  PatchScript script("pad");
  ASSERT_TRUE(h.Load(script, R"(
    function on_frame()
      assert(pad.set_button(1, "A"))
      assert(pad.set_button(1, "L", true))
      assert(pad.set_stick(1, "Main Stick", 0.5, -2))
      local ok, message = pad.set_button(1, "Q")
      assert(not ok and message:find("Q"))
      assert(not pad.set_button(2, "A"))
    end
    function on_unload()
      assert(pad.set_button(1, "A", false))
      assert(pad.release(1, "Main Stick"))
    end
  )")) << script.Error();
  EXPECT_TRUE(script.Call(Hook::OnFrame, h.env)) << script.Error();
  const PadHolds& holds = script.GetPadHolds();
  const auto value = [&](const char* group, const char* control) {
    const auto it = holds.find({PadDevice::GCPad, 0, group, control});
    return it == holds.end() ? -100.0 : it->second;
  };
  EXPECT_EQ(value("Buttons", "A"), 1.0);
  EXPECT_EQ(value("Triggers", "L"), 1.0);
  EXPECT_EQ(value("Triggers", "L-Analog"), 1.0);
  EXPECT_EQ(value("Main Stick", "X"), 0.5);
  EXPECT_EQ(value("Main Stick", "Y"), -1.0);  // clamped
  const u64 generation = script.GetPadHoldsGeneration();

  // Names resolve once; unknown names are logged once.
  const int resolves = h.pad.resolves;
  EXPECT_TRUE(script.Call(Hook::OnFrame, h.env)) << script.Error();
  EXPECT_EQ(h.pad.resolves, resolves);
  EXPECT_EQ(script.GetPadHoldsGeneration(), generation);  // nothing changed
  std::vector<LogLine> log = script.TakeLog();
  EXPECT_EQ(log.size(), 2u);  // "Q" and port 2

  EXPECT_TRUE(script.Call(Hook::OnUnload, h.env)) << script.Error();
  EXPECT_EQ(value("Buttons", "A"), -100.0);
  EXPECT_EQ(value("Main Stick", "X"), -100.0);
  EXPECT_EQ(value("Triggers", "L"), 1.0);
}

TEST(VRPatch, HoldsTheWiiPointer)
{
  Harness h;
  PatchScript script("pointer");
  ASSERT_TRUE(h.Load(script, R"(
    patch = {title = "P", settings = {{name = "hide", type = "bool", default = false}}}
    function on_frame()
      if settings.hide then
        assert(wiimote.set_pointer(1))
      else
        assert(wiimote.set_pointer(1, 0.25, 2))
      end
    end
    function on_unload() assert(wiimote.release(1, "IR")) end
  )")) << script.Error();
  const auto value = [&](const char* control) {
    const auto it = script.GetPadHolds().find({PadDevice::Wiimote, 0, "IR", control});
    return it == script.GetPadHolds().end() ? -100.0 : it->second;
  };
  EXPECT_TRUE(script.Call(Hook::OnFrame, h.env)) << script.Error();
  EXPECT_EQ(value("X"), 0.25);
  EXPECT_EQ(value("Y"), 1.0);  // clamped

  // Off the screen: NaN, and holding it again changes nothing.
  ASSERT_TRUE(script.SetSettingValue(*script.GetManifest().FindSetting("hide"), 1));
  EXPECT_TRUE(script.Call(Hook::OnFrame, h.env)) << script.Error();
  EXPECT_TRUE(std::isnan(value("X")));
  EXPECT_TRUE(std::isnan(value("Y")));
  const u64 generation = script.GetPadHoldsGeneration();
  EXPECT_TRUE(script.Call(Hook::OnFrame, h.env)) << script.Error();
  EXPECT_EQ(script.GetPadHoldsGeneration(), generation);

  EXPECT_TRUE(script.Call(Hook::OnUnload, h.env)) << script.Error();
  EXPECT_TRUE(script.GetPadHolds().empty());
}

TEST(VRPatch, RaisesOverrideFlags)
{
  Harness h;
  PatchScript script("flags");
  ASSERT_TRUE(h.Load(script, R"(
    patch = {title = "F", settings = {{name = "visor", default = 2, min = 0, max = 3}}}
    function on_frame()
      vr.set_flag("mp_visor_scan", settings.visor == 2)
      vr.set_flag("mp_paused")
    end
    function on_unload() vr.clear_flags() end
    function on_load() vr.set_flag("no spaces") end
  )")) << script.Error();
  using Flags = std::set<std::string>;
  EXPECT_TRUE(script.GetFlags().empty());

  EXPECT_TRUE(script.Call(Hook::OnFrame, h.env)) << script.Error();
  EXPECT_EQ(script.GetFlags(), (Flags{"mp_paused", "mp_visor_scan"}));
  const u64 generation = script.GetFlagsGeneration();

  // Setting the same flags again changes nothing.
  EXPECT_TRUE(script.Call(Hook::OnFrame, h.env)) << script.Error();
  EXPECT_EQ(script.GetFlagsGeneration(), generation);

  // False lowers a flag.
  ASSERT_TRUE(script.SetSettingValue(*script.GetManifest().FindSetting("visor"), 0));
  EXPECT_TRUE(script.Call(Hook::OnFrame, h.env)) << script.Error();
  EXPECT_EQ(script.GetFlags(), (Flags{"mp_paused"}));
  EXPECT_NE(script.GetFlagsGeneration(), generation);

  EXPECT_TRUE(script.Call(Hook::OnUnload, h.env)) << script.Error();
  EXPECT_TRUE(script.GetFlags().empty());

  EXPECT_FALSE(script.Call(Hook::OnLoad, h.env));
  EXPECT_NE(script.Error().find("flag name"), std::string::npos) << script.Error();
}

// A patch names where the game keeps its camera pitch's sine; nil stops it.
TEST(VRPatch, NamesTheCameraPitchSource)
{
  Harness h;
  PatchScript script("pitch");
  ASSERT_TRUE(h.Load(script, R"(
    patch = {title = "P", settings = {{name = "on", type = "bool", default = true}}}
    function on_frame()
      vr.set_camera_pitch_source(settings.on and 0x80B8B8D0 or nil)
    end
    function on_load() vr.set_camera_pitch_source(0x80B8B8D1) end
  )")) << script.Error();
  EXPECT_FALSE(script.GetCameraPitchSource());
  EXPECT_TRUE(script.Call(Hook::OnFrame, h.env)) << script.Error();
  EXPECT_EQ(script.GetCameraPitchSource(), 0x80B8B8D0u);
  ASSERT_TRUE(script.SetSettingValue(*script.GetManifest().FindSetting("on"), 0));
  EXPECT_TRUE(script.Call(Hook::OnFrame, h.env)) << script.Error();
  EXPECT_FALSE(script.GetCameraPitchSource());
  // Unaligned.
  EXPECT_FALSE(script.Call(Hook::OnLoad, h.env));
}

TEST(VRPatch, KeepsThePointerOnScreen)
{
  Harness h;
  PatchScript script("keep");
  ASSERT_TRUE(h.Load(script, R"(
    patch = {title = "K", settings = {{name = "on", type = "bool", default = true}}}
    function on_frame() vr.keep_pointer_on_screen(settings.on) end
  )")) << script.Error();
  EXPECT_FALSE(script.KeepsPointerOnScreen());
  EXPECT_TRUE(script.Call(Hook::OnFrame, h.env)) << script.Error();
  EXPECT_TRUE(script.KeepsPointerOnScreen());
  ASSERT_TRUE(script.SetSettingValue(*script.GetManifest().FindSetting("on"), 0));
  EXPECT_TRUE(script.Call(Hook::OnFrame, h.env)) << script.Error();
  EXPECT_FALSE(script.KeepsPointerOnScreen());
}

TEST(VRPatch, ValidatesIds)
{
  EXPECT_TRUE(IsValidPatchId("arm-cannon"));
  EXPECT_TRUE(IsValidPatchId("motion_v2.1"));
  EXPECT_FALSE(IsValidPatchId(""));
  EXPECT_FALSE(IsValidPatchId(".hidden"));
  EXPECT_FALSE(IsValidPatchId("../escape"));
  EXPECT_FALSE(IsValidPatchId("a b"));
}

TEST(VRPatch, IniSection)
{
  PatchValues values;
  MergePatchValues(&values, "[Graphics.VR]\nUnitsPerMeter = 1.5\n[VRPatch]\narm-cannon = True\n"
                            "arm-cannon/hand = 1\narm-cannon/beams = False\nother = False\n"
                            "[ShaderOverride]\n$x\nhandling=skip\n");
  EXPECT_TRUE(values.IsEnabled("arm-cannon"));
  EXPECT_FALSE(values.IsEnabled("other"));
  EXPECT_FALSE(values.IsEnabled("missing"));
  EXPECT_EQ(values.GetSetting("arm-cannon", "hand"), 1.0);
  EXPECT_EQ(values.GetSetting("arm-cannon", "beams"), 0.0);
  EXPECT_FALSE(values.GetSetting("arm-cannon", "missing"));

  // A later (user) file wins key by key.
  MergePatchValues(&values, "[vrpatch]\nother = True\narm-cannon/hand = 0\n");
  EXPECT_TRUE(values.IsEnabled("other"));
  EXPECT_TRUE(values.IsEnabled("arm-cannon"));
  EXPECT_EQ(values.GetSetting("arm-cannon", "hand"), 0.0);

  // Rewriting keeps the other sections byte for byte, CRLF included.
  const std::string original = "[Graphics.VR]\r\nUnitsPerMeter = 1.5\r\n[VRPatch]\r\nold = True\r\n"
                               "[ShaderOverride]\r\n$x\r\nhandling=skip\r\n";
  PatchValues saved;
  saved.enabled["arm-cannon"] = true;
  saved.settings["arm-cannon"]["pitch"] = -12.5;
  const std::string rewritten = ReplacePatchSection(original, saved);
  EXPECT_EQ(rewritten, "[Graphics.VR]\r\nUnitsPerMeter = 1.5\r\n[ShaderOverride]\r\n$x\r\n"
                       "handling=skip\r\n[VRPatch]\narm-cannon = True\narm-cannon/pitch = -12.5\n");
  PatchValues reread;
  MergePatchValues(&reread, rewritten);
  EXPECT_FALSE(reread.IsEnabled("old"));
  EXPECT_EQ(reread.GetSetting("arm-cannon", "pitch"), -12.5);

  // Empty values remove the section.
  EXPECT_EQ(ReplacePatchSection("[A]\nx = 1\n[VRPatch]\na = True\n", {}), "[A]\nx = 1\n");
  EXPECT_EQ(ReplacePatchSection("", saved),
            "[VRPatch]\narm-cannon = True\narm-cannon/pitch = -12.5\n");
}

TEST(VRPatch, ScratchLayout)
{
  // The first block sits at the top, 16-aligned.
  EXPECT_EQ(AllocateScratch({}, 0x40), SCRATCH_TOP - 0x48);
  EXPECT_EQ((SCRATCH_TOP - 0x48) % 16, 0u);
  const std::vector<AddressRange> used = {{0x80002FB0, 0x80002FF8}};
  const std::optional<u32> second = AllocateScratch(used, 0x100);
  ASSERT_TRUE(second);
  EXPECT_LE(*second + 0x100, 0x80002FB0u);
  EXPECT_FALSE(AllocateScratch({}, MAX_SCRATCH_BYTES + 1));
  EXPECT_FALSE(AllocateScratch({}, 0));
  // A reserve range in the way pushes the block below it.
  const std::vector<AddressRange> reserved = {{0x80002800, 0x80002E40}};
  EXPECT_EQ(AllocateScratch(reserved, 0x100), SCRATCH_TOP - 0x108);
  EXPECT_FALSE(AllocateScratch(reserved, 0x200));

  const std::vector<AddressRange> blocks = {{0x80002F00, 0x80002F40}, {0x80002E00, 0x80002E80}};
  EXPECT_EQ(GeckoCodeListEnd(blocks), 0x80002E00u);
  EXPECT_EQ(GeckoCodeListEnd({}), GECKO_REGION_END);
  EXPECT_TRUE(InGeckoRegion({0x80002300, 0x80002400}));
  EXPECT_FALSE(InGeckoRegion({0x80003000, 0x80003100}));
  const std::vector<AddressRange> a = {{0x80001000, 0x80001010}, {0x80002000, 0x80002010}};
  const std::vector<AddressRange> b = {{0x8000200C, 0x80002020}};
  EXPECT_EQ(FindOverlap(a, b)->start, 0x80002000u);
  EXPECT_FALSE(FindOverlap(b, std::vector<AddressRange>{{0x80002020, 0x80002030}}));
}

TEST(VRPatch, IntegerSettingsAndCleanErrors)
{
  Harness h;
  PatchScript script("ints");
  ASSERT_TRUE(h.Load(script, R"(
    patch = {title = "I", settings = {
      {name = "hand", type = "choice", options = {"Left", "Right"}, default = 1},
      {name = "count", default = 3, min = 0, max = 10, step = 1},
      {name = "scale", default = 1.5, min = 0, max = 5, step = 0.05},
    }}
    function on_frame()
      assert(math.type(settings.hand) == "integer" and settings.hand == 1)
      assert(math.type(settings.count) == "integer")
      assert(math.type(settings.scale) == "float")
      local t = nil
      return t.x
    end
  )")) << script.Error();
  EXPECT_FALSE(script.Call(Hook::OnFrame, h.env));
  // The message and the patch's own frames, without the engine's C entry points.
  EXPECT_NE(script.Error().find("ints.lua:"), std::string::npos) << script.Error();
  EXPECT_NE(script.Error().find("in function 'on_frame'"), std::string::npos) << script.Error();
  EXPECT_EQ(script.Error().find("[C]: in ?"), std::string::npos) << script.Error();

  // A syntax error has no traceback at all.
  PatchScript broken("broken");
  EXPECT_FALSE(broken.Load("patch = {title = 'x'", h.env));
  EXPECT_EQ(broken.Error().find("stack traceback"), std::string::npos) << broken.Error();

  // A runaway call names the line it was stopped at.
  ScriptLimits limits;
  limits.max_milliseconds = 10;
  PatchScript loop("loop", limits);
  ASSERT_TRUE(h.Load(loop, "function on_frame()\n  while true do end\nend")) << loop.Error();
  EXPECT_FALSE(loop.Call(Hook::OnFrame, h.env));
  EXPECT_EQ(loop.Error().rfind("loop.lua:3: the patch ran for too long", 0), 0u) << loop.Error();
}

TEST(VRPatch, IniRewriteIsByteExact)
{
  const std::string original = "[A]\r\nx = 1\r\n\r\n[B]\r\ny = 2\r\n\r\n";
  PatchValues saved;
  saved.enabled["p"] = true;
  const std::string with_section = ReplacePatchSection(original, saved);
  EXPECT_EQ(with_section, original + "[VRPatch]\np = True\n");
  EXPECT_EQ(ReplacePatchSection(with_section, {}), original);
  // A file without a final newline.
  EXPECT_EQ(ReplacePatchSection("[A]\nx = 1", saved), "[A]\nx = 1\n[VRPatch]\np = True\n");
}

// The agent's session values over the files', key by key.
TEST(VRPatch, SessionValuesOverlayTheFiles)
{
  PatchValues values;
  MergePatchValues(&values, "[VRPatch]\narm-cannon = True\narm-cannon/hand = 1\nother = True\n");
  PatchValues session;
  session.enabled["other"] = false;
  session.settings["arm-cannon"]["pitch"] = 20;
  OverlayPatchValues(&values, session);
  EXPECT_TRUE(values.IsEnabled("arm-cannon"));
  EXPECT_FALSE(values.IsEnabled("other"));
  EXPECT_EQ(values.GetSetting("arm-cannon", "hand"), 1.0);
  EXPECT_EQ(values.GetSetting("arm-cannon", "pitch"), 20.0);
}

// The agent tools change patches for the session: over the saved values, dropped on discard, and
// dropped by a change from a UI (the user's choice wins). A live source stands in for a file.
TEST(VRPatch, SessionValuesStayInTheSession)
{
  OnGameLoaded("ZZTEST", 0, false);
  ASSERT_EQ(SetSessionSource("probe", R"(patch = {title = "P", settings = {
      {name = "gain", default = 1, min = 0, max = 10}}})"),
            "");
  const auto gain = [] {
    const std::optional<PatchInfo> info = FindPatch("ZZTEST", 0, "probe");
    EXPECT_TRUE(info);
    const auto value = info->values.find("gain");
    const auto saved = info->saved_values.find("gain");
    return std::make_pair(value == info->values.end() ? -1.0 : value->second,
                          saved == info->saved_values.end() ? -1.0 : saved->second);
  };

  EXPECT_TRUE(SetSessionSetting("probe", "gain", 40));  // clamped to 10
  EXPECT_FALSE(SetSessionSetting("probe", "missing", 1));
  EXPECT_EQ(gain(), std::make_pair(10.0, 1.0));
  EXPECT_TRUE(FindPatch("ZZTEST", 0, "probe")->session_settings.contains("gain"));

  ClearSessionValues("probe");
  EXPECT_EQ(gain(), std::make_pair(1.0, 1.0));
  EXPECT_TRUE(FindPatch("ZZTEST", 0, "probe")->session_settings.empty());

  // A slider dragged in a UI (not saved) drops the session value.
  EXPECT_TRUE(SetSessionSetting("probe", "gain", 4));
  EXPECT_TRUE(SetPatchSetting("ZZTEST", "probe", "gain", 2, false));
  EXPECT_TRUE(FindPatch("ZZTEST", 0, "probe")->session_settings.empty());

  EXPECT_TRUE(SetSessionEnabled("probe", false));
  EXPECT_EQ(FindPatch("ZZTEST", 0, "probe")->session_enabled, false);
  ClearSessionValues({});
  EXPECT_FALSE(FindPatch("ZZTEST", 0, "probe")->session_enabled);

  ClearSessionSources();
  OnGameUnloaded(nullptr);
  EXPECT_FALSE(SetSessionEnabled("probe", true));
}
