// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Core/AgentServer/HttpServer.h"

namespace AgentServer
{
class CallGate;
class ToolRegistry;

// The protocol revision served statelessly ("modern": every request carries its version in
// params._meta) and the handshake-based ("legacy") revisions served after an initialize request.
constexpr std::string_view MCP_MODERN_VERSION = "2026-07-28";
constexpr std::array<std::string_view, 3> MCP_LEGACY_VERSIONS = {"2025-11-25", "2025-06-18",
                                                                 "2025-03-26"};
constexpr std::string_view MCP_DEFAULT_LEGACY_VERSION = "2025-06-18";
constexpr std::string_view MCP_ENDPOINT_PATH = "/mcp";

struct McpServerOptions
{
  std::string server_name = "dolphinxr";
  std::string server_version;
  std::string instructions;
  // When not empty, requests must carry "Authorization: Bearer <token>".
  std::string token;
  // Lets tool calls in one at a time, except the `anytime` tools. None: calls are not gated.
  CallGate* gate = nullptr;
};

// MCP over Streamable HTTP: one POST endpoint, one JSON-RPC message per request, JSON responses
// (no SSE, no sessions). Handle() depends only on the request, the tool registry and the gate,
// which keeps the protocol logic unit-testable; tool handlers do the real work. It is called from
// several threads at once.
class McpServer final
{
public:
  McpServer(const ToolRegistry& registry, McpServerOptions options);

  HttpResponse Handle(const HttpRequest& request) const;

private:
  const ToolRegistry& m_registry;
  McpServerOptions m_options;
};

// Host and Origin checks against DNS rebinding: only loopback names are accepted.
bool IsLoopbackHost(std::string_view host_header);
bool IsLoopbackOrigin(std::string_view origin_header);
}  // namespace AgentServer
