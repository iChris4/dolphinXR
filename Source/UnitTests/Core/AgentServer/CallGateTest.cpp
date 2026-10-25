// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

#include <SFML/Network/IpAddress.hpp>
#include <SFML/Network/TcpSocket.hpp>
#include <SFML/System/Time.hpp>
#include <picojson.h>

#include "Core/AgentServer/CallGate.h"
#include "Core/AgentServer/HttpServer.h"
#include "Core/AgentServer/McpServer.h"
#include "Core/AgentServer/ToolRegistry.h"

using namespace AgentServer;
using namespace std::chrono_literals;

namespace
{
template <typename F>
bool WaitFor(F&& predicate, std::chrono::milliseconds timeout = 5s)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate())
  {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

// A call that holds the gate on its own thread until released.
class HeldCall final
{
public:
  HeldCall(CallGate& gate, bool runs_emulation, std::string request_id = "1")
      : m_thread([this, &gate, runs_emulation, request_id] {
          std::optional<CallGate::Ticket> ticket = gate.Enter("hold", request_id, nullptr);
          if (runs_emulation)
            MarkCallRunsEmulation();
          m_entered = true;
          WaitFor([this] { return m_release.load() || IsCallCancelled(); }, 10s);
          m_saw_cancel = IsCallCancelled();
        })
  {
    WaitFor([this] { return m_entered.load(); });
  }
  ~HeldCall() { Release(); }
  void Release()
  {
    m_release = true;
    if (m_thread.joinable())
      m_thread.join();
  }
  bool SawCancel() const { return m_saw_cancel; }

private:
  std::atomic<bool> m_entered{false};
  std::atomic<bool> m_release{false};
  bool m_saw_cancel = false;
  std::thread m_thread;
};

HttpRequest ToolCall(const std::string& name, int id)
{
  HttpRequest request;
  request.method = "POST";
  request.path = "/mcp";
  request.version = "HTTP/1.1";
  request.body = R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
                 R"(,"method":"tools/call","params":{"name":")" + name + R"(","arguments":{}}})";
  return request;
}

// The text of a tools/call response, with "ERROR: " in front when isError is set.
std::string ResultText(const HttpResponse& response)
{
  picojson::value value;
  if (!picojson::parse(value, response.body).empty())
    return "unparsable: " + response.body;
  const picojson::object& result =
      value.get<picojson::object>().at("result").get<picojson::object>();
  std::string text;
  if (const auto error = result.find("isError");
      error != result.end() && error->second.evaluate_as_boolean())
  {
    text = "ERROR: ";
  }
  for (const picojson::value& block : result.at("content").get<picojson::array>())
    text += block.get<picojson::object>().at("text").get<std::string>();
  return text;
}

// One HTTP response body, read within `timeout` from a non-blocking socket.
std::optional<std::string> ReadBody(sf::TcpSocket& socket, std::chrono::milliseconds timeout)
{
  std::string data;
  std::array<char, 4096> chunk;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline)
  {
    std::size_t received = 0;
    const sf::Socket::Status status = socket.receive(chunk.data(), chunk.size(), received);
    if (status == sf::Socket::Status::Done)
      data.append(chunk.data(), received);
    else if (status != sf::Socket::Status::NotReady)
      return std::nullopt;
    const size_t header_end = data.find("\r\n\r\n");
    const size_t length_at = data.find("Content-Length: ");
    if (header_end != std::string::npos && length_at < header_end)
    {
      const size_t length = std::stoul(data.substr(length_at + 16));
      if (data.size() >= header_end + 4 + length)
        return data.substr(header_end + 4, length);
    }
    std::this_thread::sleep_for(1ms);
  }
  return std::nullopt;
}
}  // namespace

TEST(AgentCallGate, QuickCallsWaitTheirTurn)
{
  CallGate gate;
  auto held = std::make_optional<HeldCall>(gate, false);
  std::thread releaser([&] {
    std::this_thread::sleep_for(50ms);
    held->Release();
  });
  CallGate::RunningCall busy;
  const auto start = std::chrono::steady_clock::now();
  const std::optional<CallGate::Ticket> ticket = gate.Enter("next", "2", &busy);
  EXPECT_TRUE(ticket);
  EXPECT_GE(std::chrono::steady_clock::now() - start, 40ms);
  releaser.join();
}

TEST(AgentCallGate, RefusesCallsWhileOneRunsEmulation)
{
  CallGate gate;
  HeldCall held(gate, true);
  CallGate::RunningCall busy;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(gate.Enter("next", "2", &busy));
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
  EXPECT_EQ(busy.tool, "hold");
  EXPECT_TRUE(busy.runs_emulation);
  ASSERT_TRUE(gate.GetRunning());
  EXPECT_EQ(gate.GetRunning()->tool, "hold");
}

TEST(AgentCallGate, WaitingCallIsRefusedOnceTheRunningOneRunsEmulation)
{
  CallGate gate;
  std::atomic<bool> mark{false};
  std::atomic<bool> entered{false};
  std::thread holder([&] {
    std::optional<CallGate::Ticket> ticket = gate.Enter("hold", "1", nullptr);
    entered = true;
    WaitFor([&] { return mark.load(); });
    MarkCallRunsEmulation();
    std::this_thread::sleep_for(500ms);
  });
  WaitFor([&] { return entered.load(); });
  std::thread marker([&] {
    std::this_thread::sleep_for(50ms);
    mark = true;
  });
  CallGate::RunningCall busy;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(gate.Enter("next", "2", &busy));
  // Woken by the mark, not by the end of the call.
  EXPECT_LT(std::chrono::steady_clock::now() - start, 400ms);
  EXPECT_TRUE(busy.runs_emulation);
  marker.join();
  holder.join();
  EXPECT_FALSE(gate.GetRunning());
}

TEST(AgentCallGate, QueueWaitIsBounded)
{
  CallGate gate;
  HeldCall held(gate, false);
  CallGate::RunningCall busy;
  EXPECT_FALSE(gate.Enter("next", "2", &busy, 30ms));
  EXPECT_EQ(busy.tool, "hold");
  EXPECT_FALSE(busy.runs_emulation);
}

TEST(AgentCallGate, CancelReachesOnlyTheRunningCall)
{
  CallGate gate;
  EXPECT_FALSE(gate.Cancel());
  {
    HeldCall held(gate, true, "7");
    const std::string other_id = "8";
    EXPECT_FALSE(gate.Cancel(&other_id));
    const std::string id = "7";
    EXPECT_TRUE(gate.Cancel(&id));
    // The flag belongs to the call's thread.
    EXPECT_FALSE(IsCallCancelled());
    held.Release();
    EXPECT_TRUE(held.SawCancel());
  }
  EXPECT_FALSE(gate.GetRunning());
}

TEST(AgentCallGate, McpServerGatesToolCalls)
{
  CallGate gate;
  std::atomic<bool> holding{false};
  ToolRegistry registry;
  registry.Add({.name = "hold",
                .description = "Runs emulation until cancelled.",
                .input_schema = Schema().Build(),
                .handler = [&](const ToolArgs&) {
                  MarkCallRunsEmulation();
                  holding = true;
                  WaitFor([] { return IsCallCancelled(); }, 10s);
                  return ToolResult::Text("held.");
                }});
  registry.Add({.name = "echo",
                .description = "Echoes.",
                .input_schema = Schema().Build(),
                .handler = [](const ToolArgs&) { return ToolResult::Text("echo"); }});
  registry.Add({.name = "status",
                .description = "Status.",
                .input_schema = Schema().Build(),
                .anytime = true,
                .handler = [](const ToolArgs&) { return ToolResult::Text("status"); }});
  const McpServer server{registry, {.server_version = "test", .gate = &gate}};

  std::string hold_text;
  std::thread caller([&] { hold_text = ResultText(server.Handle(ToolCall("hold", 5))); });
  ASSERT_TRUE(WaitFor([&] { return holding.load(); }));

  const std::string refused = ResultText(server.Handle(ToolCall("echo", 6)));
  EXPECT_TRUE(refused.starts_with("ERROR: Not run: hold is running")) << refused;
  EXPECT_NE(refused.find("status"), std::string::npos) << refused;
  EXPECT_EQ(ResultText(server.Handle(ToolCall("status", 7))), "status");

  // The client gives up on the call.
  HttpRequest cancel = ToolCall("", 0);
  cancel.body = R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":5}})";
  EXPECT_EQ(server.Handle(cancel).status, 202);
  caller.join();
  EXPECT_TRUE(hold_text.starts_with("held.")) << hold_text;
  EXPECT_NE(hold_text.find("cancelled after"), std::string::npos) << hold_text;

  EXPECT_EQ(ResultText(server.Handle(ToolCall("echo", 8))), "echo");
}

TEST(AgentHttpServer, SlowRequestDoesNotHoldUpOtherConnections)
{
  std::atomic<bool> slow_started{false};
  std::atomic<bool> release{false};
  HttpServer http;
  // Releases the slow request before the server stops, however the test ends.
  struct StopGuard
  {
    std::atomic<bool>& release;
    HttpServer& http;
    ~StopGuard()
    {
      release = true;
      http.Stop();
    }
  } guard{release, http};

  const HttpServer::Handler handler = [&](const HttpRequest& request) {
    if (request.path == "/slow")
    {
      slow_started = true;
      WaitFor([&] { return release.load(); }, 10s);
      return HttpResponse{.status = 200, .content_type = "text/plain", .body = "slow"};
    }
    return HttpResponse{.status = 200, .content_type = "text/plain", .body = "fast"};
  };
  u16 port = 0;
  for (u16 candidate = 47311; candidate < 47331 && !port; ++candidate)
  {
    if (http.Start(candidate, handler, nullptr))
      port = candidate;
  }
  ASSERT_NE(port, 0) << "no free port";

  const auto send = [port](sf::TcpSocket& socket, const std::string& path) {
    if (socket.connect(sf::IpAddress::LocalHost, port, sf::seconds(2)) != sf::Socket::Status::Done)
      return false;
    socket.setBlocking(false);
    const std::string request =
        "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n";
    std::size_t sent = 0;
    return WaitFor([&] {
             std::size_t now_sent = 0;
             const sf::Socket::Status status =
                 socket.send(request.data() + sent, request.size() - sent, now_sent);
             sent += now_sent;
             return sent == request.size() ||
                    (status != sf::Socket::Status::Done && status != sf::Socket::Status::Partial &&
                     status != sf::Socket::Status::NotReady);
           }) &&
           sent == request.size();
  };

  sf::TcpSocket slow;
  ASSERT_TRUE(send(slow, "/slow"));
  ASSERT_TRUE(WaitFor([&] { return slow_started.load(); }));

  sf::TcpSocket fast;
  ASSERT_TRUE(send(fast, "/fast"));
  EXPECT_EQ(ReadBody(fast, 3s), "fast");
  EXPECT_EQ(ReadBody(slow, 100ms), std::nullopt);

  release = true;
  EXPECT_EQ(ReadBody(slow, 3s), "slow");
}
