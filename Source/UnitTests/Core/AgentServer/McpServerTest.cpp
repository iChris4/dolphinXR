// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include <picojson.h>

#include "Core/AgentServer/McpServer.h"
#include "Core/AgentServer/ToolRegistry.h"

using namespace AgentServer;

namespace
{
ToolRegistry MakeRegistry()
{
  ToolRegistry registry;
  registry.Add({.name = "echo",
                .description = "Echoes a message.",
                .input_schema = Schema().String("message", "Text.").Required({"message"}).Build(),
                .read_only = true,
                .handler = [](const ToolArgs& args) {
                  return ToolResult::Text(args.RequireString("message"));
                }});
  registry.Add({.name = "fail",
                .description = "Fails.",
                .input_schema = Schema().Build(),
                .handler = [](const ToolArgs&) -> ToolResult { throw ToolError("nope"); }});
  registry.Add(
      {.name = "crash",
       .description = "Throws.",
       .input_schema = Schema().Build(),
       .handler = [](const ToolArgs&) -> ToolResult { throw std::runtime_error("boom"); }});
  return registry;
}

HttpRequest Post(std::string body)
{
  HttpRequest request;
  request.method = "POST";
  request.path = "/mcp";
  request.version = "HTTP/1.1";
  request.headers.emplace_back("host", "127.0.0.1:7311");
  request.headers.emplace_back("content-type", "application/json");
  request.body = std::move(body);
  return request;
}

HttpRequest ModernPost(const std::string& method, const std::string& params_json,
                       const std::string& name = {})
{
  HttpRequest request =
      Post(R"({"jsonrpc":"2.0","id":7,"method":")" + method + R"(","params":{"_meta":{)" +
           R"("io.modelcontextprotocol/protocolVersion":"2026-07-28"})" +
           (params_json.empty() ? "" : "," + params_json) + "}}");
  request.headers.emplace_back("mcp-protocol-version", "2026-07-28");
  request.headers.emplace_back("mcp-method", method);
  if (!name.empty())
    request.headers.emplace_back("mcp-name", name);
  return request;
}

picojson::object ParseBody(const HttpResponse& response)
{
  picojson::value value;
  const std::string error = picojson::parse(value, response.body);
  EXPECT_TRUE(error.empty()) << error << " in " << response.body;
  return value.is<picojson::object>() ? value.get<picojson::object>() : picojson::object{};
}

const picojson::object& Result(const picojson::object& message)
{
  static const picojson::object empty;
  const auto it = message.find("result");
  return it != message.end() && it->second.is<picojson::object>() ?
             it->second.get<picojson::object>() :
             empty;
}

double ErrorCode(const picojson::object& message)
{
  const auto it = message.find("error");
  if (it == message.end() || !it->second.is<picojson::object>())
    return 0;
  return it->second.get<picojson::object>().at("code").get<double>();
}

struct Fixture
{
  ToolRegistry registry = MakeRegistry();
  McpServer server{registry, {.server_version = "test", .instructions = "Hello agent."}};
};
}  // namespace

TEST(AgentMcpServer, LegacyInitialize)
{
  Fixture f;
  const HttpResponse response = f.server.Handle(Post(
      R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"t","version":"0"}}})"));
  ASSERT_EQ(response.status, 200);
  EXPECT_EQ(response.content_type, "application/json");
  const picojson::object message = ParseBody(response);
  EXPECT_EQ(message.at("id").get<double>(), 1);
  const picojson::object& result = Result(message);
  EXPECT_EQ(result.at("protocolVersion").get<std::string>(), "2025-06-18");
  EXPECT_EQ(result.at("serverInfo").get<picojson::object>().at("name").get<std::string>(),
            "dolphinxr");
  EXPECT_EQ(result.at("instructions").get<std::string>(), "Hello agent.");
  EXPECT_TRUE(result.at("capabilities").get<picojson::object>().contains("tools"));
}

TEST(AgentMcpServer, LegacyInitializeFallsBackToKnownVersion)
{
  Fixture f;
  const picojson::object message = ParseBody(f.server.Handle(Post(
      R"({"jsonrpc":"2.0","id":"a","method":"initialize","params":{"protocolVersion":"2099-01-01"}})")));
  EXPECT_EQ(message.at("id").get<std::string>(), "a");
  EXPECT_EQ(Result(message).at("protocolVersion").get<std::string>(),
            std::string(MCP_DEFAULT_LEGACY_VERSION));
}

TEST(AgentMcpServer, NotificationIsAccepted)
{
  Fixture f;
  const HttpResponse response =
      f.server.Handle(Post(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
  EXPECT_EQ(response.status, 202);
  EXPECT_TRUE(response.body.empty());
}

TEST(AgentMcpServer, ListsTools)
{
  Fixture f;
  const picojson::object message =
      ParseBody(f.server.Handle(Post(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})")));
  const picojson::array& tools = Result(message).at("tools").get<picojson::array>();
  ASSERT_EQ(tools.size(), 3u);
  const picojson::object& echo = tools[0].get<picojson::object>();
  EXPECT_EQ(echo.at("name").get<std::string>(), "echo");
  EXPECT_EQ(echo.at("inputSchema").get<picojson::object>().at("type").get<std::string>(), "object");
  EXPECT_TRUE(echo.at("annotations").get<picojson::object>().at("readOnlyHint").get<bool>());
}

TEST(AgentMcpServer, CallsTools)
{
  Fixture f;
  picojson::object message = ParseBody(f.server.Handle(Post(
      R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"echo","arguments":{"message":"hi"}}})")));
  const picojson::object& result = Result(message);
  EXPECT_FALSE(result.contains("isError"));
  const picojson::object& block =
      result.at("content").get<picojson::array>()[0].get<picojson::object>();
  EXPECT_EQ(block.at("type").get<std::string>(), "text");
  EXPECT_EQ(block.at("text").get<std::string>(), "hi");

  message = ParseBody(f.server.Handle(Post(
      R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"echo","arguments":{}}})")));
  EXPECT_TRUE(Result(message).at("isError").get<bool>());

  message = ParseBody(f.server.Handle(
      Post(R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"fail"}})")));
  EXPECT_TRUE(Result(message).at("isError").get<bool>());

  message = ParseBody(f.server.Handle(
      Post(R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"crash"}})")));
  EXPECT_TRUE(Result(message).at("isError").get<bool>());

  message = ParseBody(f.server.Handle(
      Post(R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"nope"}})")));
  EXPECT_EQ(ErrorCode(message), -32602);
}

TEST(AgentMcpServer, ReportsUnknownArguments)
{
  Fixture f;
  const picojson::object message = ParseBody(f.server.Handle(Post(
      R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"echo","arguments":{"message":"hi","size":8}}})")));
  const picojson::array& content = Result(message).at("content").get<picojson::array>();
  ASSERT_EQ(content.size(), 2u);
  EXPECT_EQ(content[0].get<picojson::object>().at("text").get<std::string>(), "hi");
  const std::string note = content[1].get<picojson::object>().at("text").get<std::string>();
  EXPECT_NE(note.find("ignored unknown argument(s): size"), std::string::npos) << note;
  EXPECT_NE(note.find("echo takes: message"), std::string::npos) << note;
}

TEST(AgentMcpServer, LegacyErrors)
{
  Fixture f;
  HttpResponse response = f.server.Handle(Post("{not json"));
  EXPECT_EQ(response.status, 400);
  EXPECT_EQ(ErrorCode(ParseBody(response)), -32700);

  response = f.server.Handle(Post(R"({"jsonrpc":"2.0","id":1,"method":"resources/list"})"));
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(ErrorCode(ParseBody(response)), -32601);

  HttpRequest request = Post(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
  request.headers.emplace_back("mcp-protocol-version", "1999-01-01");
  response = f.server.Handle(request);
  EXPECT_EQ(response.status, 400);
  EXPECT_EQ(ErrorCode(ParseBody(response)), -32022);
}

TEST(AgentMcpServer, ModernDiscover)
{
  Fixture f;
  const HttpResponse response = f.server.Handle(ModernPost("server/discover", ""));
  ASSERT_EQ(response.status, 200);
  const picojson::object message = ParseBody(response);
  const picojson::object& result = Result(message);
  EXPECT_EQ(result.at("resultType").get<std::string>(), "complete");
  const picojson::array& versions = result.at("supportedVersions").get<picojson::array>();
  EXPECT_EQ(versions[0].get<std::string>(), std::string(MCP_MODERN_VERSION));
  EXPECT_EQ(result.at("instructions").get<std::string>(), "Hello agent.");
  EXPECT_EQ(result.at("_meta")
                .get<picojson::object>()
                .at("io.modelcontextprotocol/serverInfo")
                .get<picojson::object>()
                .at("name")
                .get<std::string>(),
            "dolphinxr");
}

TEST(AgentMcpServer, ModernToolCall)
{
  Fixture f;
  HttpResponse response = f.server.Handle(
      ModernPost("tools/call", R"("name":"echo","arguments":{"message":"m"})", "echo"));
  ASSERT_EQ(response.status, 200);
  const picojson::object message = ParseBody(response);
  EXPECT_EQ(Result(message).at("resultType").get<std::string>(), "complete");

  // Mcp-Name may be base64-encoded.
  response = f.server.Handle(ModernPost(
      "tools/call", R"("name":"echo","arguments":{"message":"m"})", "=?base64?ZWNobw==?="));
  EXPECT_EQ(response.status, 200);
}

TEST(AgentMcpServer, ModernValidation)
{
  Fixture f;
  // Mcp-Name does not match params.name.
  HttpResponse response = f.server.Handle(
      ModernPost("tools/call", R"("name":"echo","arguments":{"message":"m"})", "fail"));
  EXPECT_EQ(response.status, 400);
  EXPECT_EQ(ErrorCode(ParseBody(response)), -32020);

  // Mcp-Method does not match the method.
  HttpRequest request = ModernPost("tools/list", "");
  for (auto& [name, value] : request.headers)
  {
    if (name == "mcp-method")
      value = "tools/call";
  }
  response = f.server.Handle(request);
  EXPECT_EQ(response.status, 400);
  EXPECT_EQ(ErrorCode(ParseBody(response)), -32020);

  // Unknown method.
  response = f.server.Handle(ModernPost("resources/list", ""));
  EXPECT_EQ(response.status, 404);
  EXPECT_EQ(ErrorCode(ParseBody(response)), -32601);

  // Unsupported version.
  request = Post(
      R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2099-01-01"}}})");
  request.headers.emplace_back("mcp-protocol-version", "2099-01-01");
  request.headers.emplace_back("mcp-method", "tools/list");
  response = f.server.Handle(request);
  EXPECT_EQ(response.status, 400);
  const picojson::object message = ParseBody(response);
  EXPECT_EQ(ErrorCode(message), -32022);
  EXPECT_TRUE(message.at("error")
                  .get<picojson::object>()
                  .at("data")
                  .get<picojson::object>()
                  .at("supported")
                  .is<picojson::array>());
}

TEST(AgentMcpServer, HttpLevelChecks)
{
  Fixture f;
  HttpRequest request = Post("{}");
  request.method = "GET";
  HttpResponse response = f.server.Handle(request);
  EXPECT_EQ(response.status, 405);
  ASSERT_FALSE(response.headers.empty());
  EXPECT_EQ(response.headers[0].first, "Allow");

  request = Post("{}");
  request.path = "/other";
  EXPECT_EQ(f.server.Handle(request).status, 404);

  request = Post(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
  request.headers.emplace_back("origin", "http://evil.example");
  EXPECT_EQ(f.server.Handle(request).status, 403);

  request = Post(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
  request.headers.emplace_back("origin", "http://localhost:7311");
  EXPECT_EQ(f.server.Handle(request).status, 200);

  request = Post(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
  request.headers[0].second = "attacker.example:7311";
  EXPECT_EQ(f.server.Handle(request).status, 403);
}

TEST(AgentMcpServer, BearerToken)
{
  const ToolRegistry registry = MakeRegistry();
  const McpServer server(registry, {.token = "secret"});
  HttpRequest request = Post(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
  EXPECT_EQ(server.Handle(request).status, 401);
  request.headers.emplace_back("authorization", "Bearer wrong");
  EXPECT_EQ(server.Handle(request).status, 401);
  request.headers.back().second = "Bearer secret";
  EXPECT_EQ(server.Handle(request).status, 200);
}

TEST(AgentMcpServer, LoopbackNames)
{
  EXPECT_TRUE(IsLoopbackHost("127.0.0.1"));
  EXPECT_TRUE(IsLoopbackHost("127.0.0.1:7311"));
  EXPECT_TRUE(IsLoopbackHost("LOCALHOST:80"));
  EXPECT_TRUE(IsLoopbackHost("[::1]:7311"));
  EXPECT_FALSE(IsLoopbackHost("127.0.0.1.evil.com"));
  EXPECT_FALSE(IsLoopbackHost("localhost:abc"));
  EXPECT_FALSE(IsLoopbackHost("192.168.1.2:7311"));
  EXPECT_TRUE(IsLoopbackOrigin("http://127.0.0.1:7311"));
  EXPECT_FALSE(IsLoopbackOrigin("https://localhost"));
  EXPECT_FALSE(IsLoopbackOrigin("null"));
}
