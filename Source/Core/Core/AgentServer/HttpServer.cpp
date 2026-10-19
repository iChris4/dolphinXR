// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/AgentServer/HttpServer.h"

#include <array>
#include <chrono>
#include <exception>

#include <SFML/Network/SocketSelector.hpp>
#include <SFML/Network/TcpListener.hpp>
#include <SFML/Network/TcpSocket.hpp>
#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"

namespace AgentServer
{
namespace
{
constexpr size_t MAX_CLIENTS = 8;
// A keep-alive connection the client has not used for this long is closed.
constexpr auto CLIENT_IDLE_TIMEOUT = std::chrono::minutes(15);

std::string ToLowerCopy(std::string_view text)
{
  std::string result(text);
  Common::ToLower(&result);
  return result;
}
}  // namespace

std::optional<std::string> HttpRequest::GetHeader(std::string_view name) const
{
  for (const auto& [key, value] : headers)
  {
    if (Common::CaseInsensitiveEquals(key, name))
      return value;
  }
  return std::nullopt;
}

HttpParseStatus ParseHttpRequest(std::string_view buffer, HttpRequest* out, size_t* consumed)
{
  const size_t header_end = buffer.find("\r\n\r\n");
  if (header_end == std::string_view::npos)
  {
    return buffer.size() > MAX_HTTP_HEADER_BYTES ? HttpParseStatus::TooLarge :
                                                   HttpParseStatus::NeedHeaders;
  }
  if (header_end > MAX_HTTP_HEADER_BYTES)
    return HttpParseStatus::TooLarge;

  *out = HttpRequest{};
  const std::string_view head = buffer.substr(0, header_end);
  size_t line_end = head.find("\r\n");
  const std::string_view request_line = head.substr(0, line_end);

  const size_t first_space = request_line.find(' ');
  const size_t last_space = request_line.rfind(' ');
  if (first_space == std::string_view::npos || last_space == first_space)
    return HttpParseStatus::BadRequest;
  out->method = std::string(request_line.substr(0, first_space));
  std::string_view target = request_line.substr(first_space + 1, last_space - first_space - 1);
  out->version = std::string(request_line.substr(last_space + 1));
  if (out->method.empty() || target.empty() || !out->version.starts_with("HTTP/1."))
    return HttpParseStatus::BadRequest;
  const size_t query = target.find_first_of("?#");
  if (query != std::string_view::npos)
    target = target.substr(0, query);
  out->path = std::string(target);

  while (line_end != std::string_view::npos)
  {
    const size_t start = line_end + 2;
    line_end = head.find("\r\n", start);
    const std::string_view line = head.substr(
        start, line_end == std::string_view::npos ? std::string_view::npos : line_end - start);
    if (line.empty())
      continue;
    // Obsolete line folding is not supported.
    if (line.front() == ' ' || line.front() == '\t')
      return HttpParseStatus::BadRequest;
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0)
      return HttpParseStatus::BadRequest;
    out->headers.emplace_back(ToLowerCopy(StripWhitespace(line.substr(0, colon))),
                              std::string(StripWhitespace(line.substr(colon + 1))));
  }

  if (out->GetHeader("transfer-encoding"))
    return HttpParseStatus::BadRequest;

  size_t content_length = 0;
  if (const auto length_header = out->GetHeader("content-length"))
  {
    u64 parsed;
    if (!TryParse(*length_header, &parsed, 10))
      return HttpParseStatus::BadRequest;
    if (parsed > MAX_HTTP_BODY_BYTES)
      return HttpParseStatus::TooLarge;
    content_length = static_cast<size_t>(parsed);
  }

  const std::string connection = ToLowerCopy(out->GetHeader("connection").value_or(""));
  if (out->version == "HTTP/1.0")
    out->keep_alive = connection.find("keep-alive") != std::string::npos;
  else
    out->keep_alive = connection.find("close") == std::string::npos;
  out->expect_continue =
      Common::CaseInsensitiveEquals(out->GetHeader("expect").value_or(""), "100-continue");

  const size_t body_start = header_end + 4;
  if (buffer.size() - body_start < content_length)
    return HttpParseStatus::NeedBody;

  out->body = std::string(buffer.substr(body_start, content_length));
  *consumed = body_start + content_length;
  return HttpParseStatus::Ok;
}

std::string_view GetHttpStatusText(int status)
{
  switch (status)
  {
  case 100:
    return "Continue";
  case 200:
    return "OK";
  case 202:
    return "Accepted";
  case 400:
    return "Bad Request";
  case 401:
    return "Unauthorized";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 413:
    return "Content Too Large";
  case 500:
    return "Internal Server Error";
  default:
    return "Unknown";
  }
}

std::string SerializeHttpResponse(const HttpResponse& response, bool keep_alive)
{
  std::string out =
      fmt::format("HTTP/1.1 {} {}\r\n", response.status, GetHttpStatusText(response.status));
  if (!response.content_type.empty())
    out += fmt::format("Content-Type: {}\r\n", response.content_type);
  out += fmt::format("Content-Length: {}\r\n", response.body.size());
  out += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
  out += "Cache-Control: no-store\r\n";
  for (const auto& [name, value] : response.headers)
    out += fmt::format("{}: {}\r\n", name, value);
  out += "\r\n";
  out += response.body;
  return out;
}

HttpServer::HttpServer() = default;

HttpServer::~HttpServer()
{
  Stop();
}

bool HttpServer::Start(u16 port, Handler handler, std::string* error)
{
  Stop();

  auto listener = std::make_unique<sf::TcpListener>();
  if (listener->listen(port, sf::IpAddress::LocalHost) != sf::Socket::Status::Done)
  {
    if (error)
      *error = fmt::format("Cannot listen on 127.0.0.1:{} (port in use?)", port);
    return false;
  }

  m_listener = std::move(listener);
  m_handler = std::move(handler);
  m_port = port;
  m_running.Set();
  m_thread = std::thread(&HttpServer::ThreadFunc, this);
  return true;
}

void HttpServer::Stop()
{
  if (!m_thread.joinable())
    return;
  m_running.Clear();
  m_thread.join();
  m_listener.reset();
  m_handler = nullptr;
}

void HttpServer::ThreadFunc()
{
  Common::SetCurrentThreadName("Agent Server");

  struct Client
  {
    std::unique_ptr<sf::TcpSocket> socket;
    std::string buffer;
    bool sent_continue = false;
    std::chrono::steady_clock::time_point last_activity;
  };
  std::vector<Client> clients;
  sf::SocketSelector selector;
  selector.add(*m_listener);
  std::array<char, 64 * 1024> chunk;

  const auto send_all = [](sf::TcpSocket& socket, std::string_view data) {
    return socket.send(data.data(), data.size()) == sf::Socket::Status::Done;
  };

  // Serves every complete request in the client's buffer. Returns false to close the connection.
  const auto serve = [&](Client& client) {
    while (true)
    {
      HttpRequest request;
      size_t consumed = 0;
      switch (ParseHttpRequest(client.buffer, &request, &consumed))
      {
      case HttpParseStatus::NeedHeaders:
        return true;
      case HttpParseStatus::NeedBody:
        if (request.expect_continue && !client.sent_continue)
        {
          client.sent_continue = true;
          return send_all(*client.socket, "HTTP/1.1 100 Continue\r\n\r\n");
        }
        return true;
      case HttpParseStatus::BadRequest:
        send_all(*client.socket, SerializeHttpResponse({.status = 400}, false));
        return false;
      case HttpParseStatus::TooLarge:
        send_all(*client.socket, SerializeHttpResponse({.status = 413}, false));
        return false;
      case HttpParseStatus::Ok:
        break;
      }

      client.buffer.erase(0, consumed);
      client.sent_continue = false;

      HttpResponse response;
      try
      {
        response = m_handler(request);
      }
      catch (const std::exception& e)
      {
        ERROR_LOG_FMT(CORE, "AgentServer: request handler failed: {}", e.what());
        response = {.status = 500, .content_type = "text/plain", .body = e.what()};
      }
      catch (...)
      {
        ERROR_LOG_FMT(CORE, "AgentServer: request handler failed");
        response = {.status = 500, .content_type = "text/plain", .body = "Internal error"};
      }

      const bool keep_alive = request.keep_alive && m_running.IsSet();
      if (!send_all(*client.socket, SerializeHttpResponse(response, keep_alive)) || !keep_alive)
        return false;
    }
  };

  const auto close_client = [&](size_t index) {
    selector.remove(*clients[index].socket);
    clients[index].socket->disconnect();
    clients.erase(clients.begin() + index);
  };

  while (m_running.IsSet())
  {
    const auto now = std::chrono::steady_clock::now();
    if (!selector.wait(sf::milliseconds(100)))
    {
      for (size_t i = clients.size(); i-- > 0;)
      {
        if (now - clients[i].last_activity > CLIENT_IDLE_TIMEOUT)
          close_client(i);
      }
      continue;
    }

    if (selector.isReady(*m_listener))
    {
      auto socket = std::make_unique<sf::TcpSocket>();
      if (m_listener->accept(*socket) == sf::Socket::Status::Done)
      {
        if (clients.size() >= MAX_CLIENTS)
          close_client(0);
        selector.add(*socket);
        clients.push_back({std::move(socket), {}, false, now});
      }
    }

    for (size_t i = 0; i < clients.size();)
    {
      Client& client = clients[i];
      bool keep = true;
      if (selector.isReady(*client.socket))
      {
        size_t received = 0;
        const sf::Socket::Status status =
            client.socket->receive(chunk.data(), chunk.size(), received);
        if (status == sf::Socket::Status::Done)
        {
          client.buffer.append(chunk.data(), received);
          client.last_activity = std::chrono::steady_clock::now();
          keep = serve(client);
        }
        else if (status != sf::Socket::Status::NotReady && status != sf::Socket::Status::Partial)
        {
          keep = false;
        }
      }

      if (keep)
        ++i;
      else
        close_client(i);
    }
  }

  while (!clients.empty())
    close_client(clients.size() - 1);
  selector.clear();
  m_listener->close();
}
}  // namespace AgentServer
