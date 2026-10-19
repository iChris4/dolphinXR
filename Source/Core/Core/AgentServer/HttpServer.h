// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Flag.h"

namespace sf
{
class TcpListener;
}

namespace AgentServer
{
struct HttpRequest
{
  // Header names are stored lowercase; lookups are case-insensitive.
  std::optional<std::string> GetHeader(std::string_view name) const;

  std::string method;
  // Without the query string.
  std::string path;
  std::string version;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  bool keep_alive = true;
  bool expect_continue = false;
};

struct HttpResponse
{
  int status = 200;
  std::string content_type;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
};

enum class HttpParseStatus
{
  // The request line and headers are not complete yet.
  NeedHeaders,
  // The headers are parsed into the output request; the body is not complete yet.
  NeedBody,
  Ok,
  BadRequest,
  TooLarge,
};

constexpr size_t MAX_HTTP_HEADER_BYTES = 16 * 1024;
constexpr size_t MAX_HTTP_BODY_BYTES = 4 * 1024 * 1024;

// Parses one HTTP/1.x request from the start of `buffer`. On Ok, `consumed` is the size of the
// request (more pipelined requests may follow). Only Content-Length bodies are supported.
HttpParseStatus ParseHttpRequest(std::string_view buffer, HttpRequest* out, size_t* consumed);
std::string SerializeHttpResponse(const HttpResponse& response, bool keep_alive);
std::string_view GetHttpStatusText(int status);

// A minimal HTTP/1.1 server on 127.0.0.1. Its thread accepts connections and serves the requests
// of all of them one at a time, calling the handler on this thread: a slow handler delays the
// other connections, which is what the agent server wants (tool calls are serialized).
class HttpServer final
{
public:
  using Handler = std::function<HttpResponse(const HttpRequest&)>;

  HttpServer();
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  // Returns false (with the reason in *error) when the port cannot be bound.
  bool Start(u16 port, Handler handler, std::string* error);
  void Stop();
  bool IsRunning() const { return m_thread.joinable(); }
  u16 GetPort() const { return m_port; }

private:
  void ThreadFunc();

  std::unique_ptr<sf::TcpListener> m_listener;
  std::thread m_thread;
  Common::Flag m_running;
  Handler m_handler;
  u16 m_port = 0;
};
}  // namespace AgentServer
