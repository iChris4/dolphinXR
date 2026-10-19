// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/AgentServer/McpServer.h"

#include <algorithm>
#include <exception>
#include <optional>
#include <utility>

#include <fmt/format.h>
#include <picojson.h>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/AgentServer/ToolRegistry.h"

namespace AgentServer
{
namespace
{
constexpr int RPC_PARSE_ERROR = -32700;
constexpr int RPC_INVALID_REQUEST = -32600;
constexpr int RPC_METHOD_NOT_FOUND = -32601;
constexpr int RPC_INVALID_PARAMS = -32602;
constexpr int RPC_HEADER_MISMATCH = -32020;
constexpr int RPC_UNSUPPORTED_PROTOCOL_VERSION = -32022;

constexpr std::string_view META_PROTOCOL_VERSION = "io.modelcontextprotocol/protocolVersion";
constexpr std::string_view META_SERVER_INFO = "io.modelcontextprotocol/serverInfo";

HttpResponse PlainResponse(int status, std::string body)
{
  return {.status = status, .content_type = "text/plain; charset=utf-8", .body = std::move(body)};
}

HttpResponse JsonResponse(int status, const picojson::object& message)
{
  return {.status = status,
          .content_type = "application/json",
          .body = picojson::value(message).serialize()};
}

picojson::object RpcResult(const picojson::value& id, picojson::object result)
{
  picojson::object message;
  message["jsonrpc"] = JsonString("2.0");
  message["id"] = id;
  message["result"] = picojson::value(std::move(result));
  return message;
}

picojson::object RpcError(const picojson::value& id, int code, std::string text,
                          std::optional<picojson::value> data = std::nullopt)
{
  picojson::object error;
  error["code"] = JsonNumber(code);
  error["message"] = JsonString(std::move(text));
  if (data)
    error["data"] = std::move(*data);
  picojson::object message;
  message["jsonrpc"] = JsonString("2.0");
  message["id"] = id;
  message["error"] = picojson::value(std::move(error));
  return message;
}

picojson::array SupportedVersions()
{
  picojson::array versions;
  versions.push_back(JsonString(std::string(MCP_MODERN_VERSION)));
  for (const std::string_view version : MCP_LEGACY_VERSIONS)
    versions.push_back(JsonString(std::string(version)));
  return versions;
}

bool IsSupportedVersion(std::string_view version)
{
  return version == MCP_MODERN_VERSION ||
         std::ranges::find(MCP_LEGACY_VERSIONS, version) != MCP_LEGACY_VERSIONS.end();
}

HttpResponse UnsupportedVersion(const picojson::value& id, std::string_view requested)
{
  picojson::object data;
  data["supported"] = picojson::value(SupportedVersions());
  data["requested"] = JsonString(std::string(requested));
  return JsonResponse(400, RpcError(id, RPC_UNSUPPORTED_PROTOCOL_VERSION,
                                    "Unsupported protocol version", picojson::value(data)));
}

const std::string* GetString(const picojson::object& object, std::string_view key)
{
  const auto it = object.find(std::string(key));
  if (it == object.end() || !it->second.is<std::string>())
    return nullptr;
  return &it->second.get<std::string>();
}

const picojson::object* GetObject(const picojson::object& object, std::string_view key)
{
  const auto it = object.find(std::string(key));
  if (it == object.end() || !it->second.is<picojson::object>())
    return nullptr;
  return &it->second.get<picojson::object>();
}

// Mcp-Name and Mcp-Param-* values outside the plain header charset are sent as
// "=?base64?<value>?=".
std::optional<std::string> DecodeHeaderValue(const std::string& value)
{
  constexpr std::string_view prefix = "=?base64?";
  constexpr std::string_view suffix = "?=";
  if (value.size() >= prefix.size() + suffix.size() && value.starts_with(prefix) &&
      value.ends_with(suffix))
  {
    return Base64Decode(std::string_view(value).substr(prefix.size(), value.size() - prefix.size() -
                                                                          suffix.size()));
  }
  return value;
}

// Splits "name[:port]" or "[v6][:port]" and checks the port is numeric.
std::optional<std::string> HostName(std::string_view host)
{
  std::string_view name;
  std::string_view rest;
  if (host.starts_with('['))
  {
    const size_t close = host.find(']');
    if (close == std::string_view::npos)
      return std::nullopt;
    name = host.substr(1, close - 1);
    rest = host.substr(close + 1);
  }
  else
  {
    const size_t colon = host.find(':');
    name = host.substr(0, colon);
    rest = colon == std::string_view::npos ? std::string_view{} : host.substr(colon);
  }
  if (!rest.empty())
  {
    if (rest.front() != ':' || rest.size() == 1)
      return std::nullopt;
    for (const char c : rest.substr(1))
    {
      if (c < '0' || c > '9')
        return std::nullopt;
    }
  }
  return std::string(name);
}
}  // namespace

bool IsLoopbackHost(std::string_view host_header)
{
  const std::optional<std::string> name = HostName(StripWhitespace(host_header));
  if (!name)
    return false;
  return *name == "127.0.0.1" || *name == "::1" ||
         Common::CaseInsensitiveEquals(*name, "localhost");
}

bool IsLoopbackOrigin(std::string_view origin_header)
{
  origin_header = StripWhitespace(origin_header);
  constexpr std::string_view scheme = "http://";
  if (origin_header.size() <= scheme.size() ||
      !Common::CaseInsensitiveEquals(origin_header.substr(0, scheme.size()), scheme))
  {
    return false;
  }
  return IsLoopbackHost(origin_header.substr(scheme.size()));
}

McpServer::McpServer(const ToolRegistry& registry, McpServerOptions options)
    : m_registry(registry), m_options(std::move(options))
{
}

HttpResponse McpServer::Handle(const HttpRequest& request) const
{
  if (request.path != MCP_ENDPOINT_PATH)
    return PlainResponse(404, fmt::format("The MCP endpoint is {}", MCP_ENDPOINT_PATH));

  if (request.method != "POST")
  {
    HttpResponse response = PlainResponse(405, "Only POST is supported");
    response.headers.emplace_back("Allow", "POST");
    return response;
  }

  const picojson::value null_id;
  if (const auto host = request.GetHeader("host"); host && !IsLoopbackHost(*host))
    return JsonResponse(403, RpcError(null_id, RPC_INVALID_REQUEST, "Host not allowed"));
  if (const auto origin = request.GetHeader("origin"); origin && !IsLoopbackOrigin(*origin))
    return JsonResponse(403, RpcError(null_id, RPC_INVALID_REQUEST, "Origin not allowed"));
  if (!m_options.token.empty())
  {
    const auto authorization = request.GetHeader("authorization");
    if (!authorization || *authorization != "Bearer " + m_options.token)
    {
      HttpResponse response = PlainResponse(401, "Missing or wrong Authorization bearer token");
      response.headers.emplace_back("WWW-Authenticate", "Bearer");
      return response;
    }
  }

  picojson::value root;
  const std::string parse_error = picojson::parse(root, request.body);
  if (!parse_error.empty())
    return JsonResponse(400, RpcError(null_id, RPC_PARSE_ERROR, "Parse error: " + parse_error));
  if (!root.is<picojson::object>())
  {
    return JsonResponse(400, RpcError(null_id, RPC_INVALID_REQUEST,
                                      "Expected one JSON-RPC message (batches are not supported)"));
  }

  const picojson::object& message = root.get<picojson::object>();
  const auto id_it = message.find("id");
  const bool is_notification = id_it == message.end();
  const picojson::value id = is_notification ? null_id : id_it->second;
  const std::string* method = GetString(message, "method");
  if (!method)
  {
    // A JSON-RPC response from the client (legacy sampling/elicitation answers): nothing to do.
    if (message.contains("result") || message.contains("error"))
      return {.status = 202};
    return JsonResponse(400, RpcError(id, RPC_INVALID_REQUEST, "Missing method"));
  }

  static const picojson::object empty_object;
  const picojson::object* params_ptr = GetObject(message, "params");
  const picojson::object& params = params_ptr ? *params_ptr : empty_object;
  const picojson::object* meta = GetObject(params, "_meta");
  const std::string* modern_version = meta ? GetString(*meta, META_PROTOCOL_VERSION) : nullptr;
  const bool modern = modern_version && *method != "initialize";

  if (modern)
  {
    if (*modern_version != MCP_MODERN_VERSION)
      return UnsupportedVersion(id, *modern_version);

    const auto mismatch = [&](std::string_view header) {
      return JsonResponse(
          400, RpcError(id, RPC_HEADER_MISMATCH, fmt::format("Header mismatch: {}", header)));
    };
    if (request.GetHeader("mcp-protocol-version") != *modern_version)
      return mismatch("MCP-Protocol-Version");
    if (request.GetHeader("mcp-method") != *method)
      return mismatch("Mcp-Method");
    if (*method == "tools/call" || *method == "resources/read" || *method == "prompts/get")
    {
      const std::string* body_name =
          GetString(params, *method == "resources/read" ? "uri" : "name");
      const auto header_name = request.GetHeader("mcp-name");
      const std::optional<std::string> decoded =
          header_name ? DecodeHeaderValue(*header_name) : std::nullopt;
      if (!body_name || !decoded || *decoded != *body_name)
        return mismatch("Mcp-Name");
    }
  }
  else if (const auto header_version = request.GetHeader("mcp-protocol-version");
           header_version && !IsSupportedVersion(*header_version))
  {
    return UnsupportedVersion(id, *header_version);
  }

  if (is_notification)
    return {.status = 202};

  const auto server_info = [this] {
    picojson::object info;
    info["name"] = JsonString(m_options.server_name);
    info["version"] = JsonString(m_options.server_version);
    return info;
  };
  const auto complete = [modern](picojson::object result) {
    if (modern)
      result["resultType"] = JsonString("complete");
    return result;
  };

  if (*method == "initialize")
  {
    const std::string* requested = GetString(params, "protocolVersion");
    std::string chosen(MCP_DEFAULT_LEGACY_VERSION);
    if (requested &&
        std::ranges::find(MCP_LEGACY_VERSIONS, *requested) != MCP_LEGACY_VERSIONS.end())
      chosen = *requested;

    picojson::object tools_capability;
    tools_capability["listChanged"] = JsonBool(false);
    picojson::object capabilities;
    capabilities["tools"] = picojson::value(tools_capability);

    picojson::object result;
    result["protocolVersion"] = JsonString(chosen);
    result["capabilities"] = picojson::value(capabilities);
    result["serverInfo"] = picojson::value(server_info());
    result["instructions"] = JsonString(m_options.instructions);
    return JsonResponse(200, RpcResult(id, std::move(result)));
  }

  if (*method == "server/discover")
  {
    picojson::object capabilities;
    capabilities["tools"] = picojson::value(picojson::object{});
    picojson::object result_meta;
    result_meta[std::string(META_SERVER_INFO)] = picojson::value(server_info());

    picojson::object result;
    result["supportedVersions"] = picojson::value(SupportedVersions());
    result["capabilities"] = picojson::value(capabilities);
    result["_meta"] = picojson::value(result_meta);
    result["instructions"] = JsonString(m_options.instructions);
    picojson::object complete_result = std::move(result);
    complete_result["resultType"] = JsonString("complete");
    return JsonResponse(200, RpcResult(id, std::move(complete_result)));
  }

  if (*method == "ping")
    return JsonResponse(200, RpcResult(id, complete({})));

  if (*method == "tools/list")
  {
    picojson::object result;
    result["tools"] = picojson::value(m_registry.ToJson());
    return JsonResponse(200, RpcResult(id, complete(std::move(result))));
  }

  if (*method == "tools/call")
  {
    const std::string* name = GetString(params, "name");
    if (!name)
      return JsonResponse(200, RpcError(id, RPC_INVALID_PARAMS, "Missing tool name"));
    const ToolDef* tool = m_registry.Find(*name);
    if (!tool)
      return JsonResponse(200, RpcError(id, RPC_INVALID_PARAMS, "Unknown tool: " + *name));

    const picojson::object* arguments_ptr = GetObject(params, "arguments");
    picojson::object arguments = arguments_ptr ? *arguments_ptr : picojson::object{};

    ToolResult tool_result;
    std::string missing;
    if (const auto required = tool->input_schema.find("required");
        required != tool->input_schema.end() && required->second.is<picojson::array>())
    {
      for (const picojson::value& key : required->second.get<picojson::array>())
      {
        const auto it = arguments.find(key.to_str());
        if (it == arguments.end() || it->second.is<picojson::null>())
          missing += (missing.empty() ? "" : ", ") + key.to_str();
      }
    }

    // Arguments the schema does not list are ignored; say so, since it is usually a typo.
    std::string unknown;
    std::string known;
    if (const auto properties = tool->input_schema.find("properties");
        properties != tool->input_schema.end() && properties->second.is<picojson::object>())
    {
      const picojson::object& listed = properties->second.get<picojson::object>();
      for (const auto& [key, value] : arguments)
      {
        if (!listed.contains(key))
          unknown += (unknown.empty() ? "" : ", ") + key;
      }
      for (const auto& [key, value] : listed)
        known += (known.empty() ? "" : ", ") + key;
    }

    if (!missing.empty())
    {
      tool_result = ToolResult::Error(fmt::format("Missing required argument(s): {}", missing));
    }
    else
    {
      try
      {
        tool_result = tool->handler(ToolArgs(std::move(arguments)));
      }
      catch (const ToolError& e)
      {
        tool_result = ToolResult::Error(e.what());
      }
      catch (const std::exception& e)
      {
        ERROR_LOG_FMT(CORE, "AgentServer: tool {} failed: {}", *name, e.what());
        tool_result = ToolResult::Error(fmt::format("Internal error: {}", e.what()));
      }
    }
    if (!unknown.empty())
    {
      tool_result.AddText(fmt::format("Note: ignored unknown argument(s): {}. {} takes: {}.",
                                      unknown, *name, known.empty() ? "no arguments" : known));
    }
    return JsonResponse(200, RpcResult(id, complete(tool_result.ToJson())));
  }

  return JsonResponse(modern ? 404 : 200,
                      RpcError(id, RPC_METHOD_NOT_FOUND, "Method not found: " + *method));
}
}  // namespace AgentServer
