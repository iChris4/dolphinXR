// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include <picojson.h>

#include "Core/AgentServer/AgentServer.h"
#include "Core/AgentServer/PatchTemplates.h"
#include "Core/AgentServer/ToolRegistry.h"
#include "Core/State.h"

using namespace AgentServer;

namespace
{
ToolArgs ArgsFromJson(const std::string& json)
{
  picojson::value value;
  EXPECT_TRUE(picojson::parse(value, json).empty());
  return ToolArgs(value.get<picojson::object>());
}
}  // namespace

TEST(AgentTools, SchemasAreWellFormed)
{
  const ToolRegistry& registry = Server::GetInstance().GetRegistry();
  ASSERT_FALSE(registry.GetTools().empty());

  std::set<std::string> names;
  for (const ToolDef& tool : registry.GetTools())
  {
    SCOPED_TRACE(tool.name);
    EXPECT_TRUE(names.insert(tool.name).second);
    for (const char c : tool.name)
      EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_');
    EXPECT_FALSE(tool.description.empty());
    EXPECT_TRUE(tool.handler != nullptr);

    ASSERT_EQ(tool.input_schema.at("type").get<std::string>(), "object");
    const picojson::object& properties = tool.input_schema.at("properties").get<picojson::object>();
    for (const auto& [key, property] : properties)
    {
      SCOPED_TRACE(key);
      ASSERT_TRUE(property.is<picojson::object>());
      EXPECT_TRUE(property.get<picojson::object>().contains("type"));
      EXPECT_TRUE(property.get<picojson::object>().contains("description"));
    }
    if (const auto required = tool.input_schema.find("required");
        required != tool.input_schema.end())
    {
      for (const picojson::value& key : required->second.get<picojson::array>())
        EXPECT_TRUE(properties.contains(key.get<std::string>())) << key.get<std::string>();
    }
  }
}

TEST(AgentTools, ArgumentAccessors)
{
  const ToolArgs args = ArgsFromJson(
      R"({"a":"0x80001234","b":"80001234","c":2147483648,"n":"12","f":1.5,"t":true,"s":"x","bad":"zz","list":["1a","0x2B"]})");
  EXPECT_EQ(args.GetAddress("a"), 0x80001234u);
  EXPECT_EQ(args.GetAddress("b"), 0x80001234u);
  EXPECT_EQ(args.GetAddress("c"), 0x80000000u);
  EXPECT_EQ(args.GetAddress("missing"), std::nullopt);
  EXPECT_EQ(args.GetInt("n", 0), 12);
  EXPECT_EQ(args.GetInt("missing", 5), 5);
  EXPECT_DOUBLE_EQ(args.GetNumber("f", 0), 1.5);
  EXPECT_TRUE(args.GetBool("t", false));
  EXPECT_EQ(args.RequireString("s"), "x");
  EXPECT_EQ(args.GetHexArray("list"), (std::vector<u64>{0x1a, 0x2b}));
  EXPECT_THROW(args.GetAddress("bad"), ToolError);
  EXPECT_THROW(args.RequireString("missing"), ToolError);
  EXPECT_THROW(args.GetBool("s", false), ToolError);
}

TEST(AgentTools, Base64RoundTrip)
{
  const std::string text = "DolphinXR \x01\xff";
  const std::string encoded = Base64Encode({reinterpret_cast<const u8*>(text.data()), text.size()});
  EXPECT_EQ(Base64Decode(encoded), text);
  EXPECT_EQ(Base64Decode("ZWNobw=="), "echo");
  EXPECT_EQ(Base64Decode("!!!"), std::nullopt);
}

TEST(AgentPatchTemplates, BuildsActionReplayLines)
{
  // The Dolphin VR culling codes for I-Ninja and Metroid Prime.
  const std::vector<u32> early = GetPatchTemplateWords(PatchTemplate::ReturnEarly);
  const std::vector<u32> i_ninja = {0x60000001, BLR_INSTRUCTION};
  EXPECT_EQ(BuildARCodeLines(0x80052FE8, i_ninja),
            (std::vector<std::string>{"04052FE8 60000001", "04052FEC 4E800020"}));
  EXPECT_EQ(BuildARCodeText(0x80345C68, GetPatchTemplateWords(PatchTemplate::ReturnTrue)),
            "04345C68 38600001\n04345C6C 4E800020");
  EXPECT_EQ(early, (std::vector<u32>{0x60000000, BLR_INSTRUCTION}));

  EXPECT_EQ(ParsePatchTemplate("li_r3_0"), PatchTemplate::ReturnFalse);
  EXPECT_EQ(ParsePatchTemplate("RETURN_TRUE"), PatchTemplate::ReturnTrue);
  EXPECT_EQ(ParsePatchTemplate("nop"), std::nullopt);
  EXPECT_EQ(GetPatchTemplateName(PatchTemplate::ReturnEarly), "nop_blr");
}

TEST(State, SaveOptionsDefaultsMatchExpectedCompressionBehavior)
{
  State::SaveOptions default_options;
  EXPECT_TRUE(default_options.compress);
  EXPECT_FALSE(default_options.wait_for_completion);
}
