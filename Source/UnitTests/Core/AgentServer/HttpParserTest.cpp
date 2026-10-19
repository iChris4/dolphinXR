// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <string>

#include "Core/AgentServer/HttpServer.h"

using namespace AgentServer;

TEST(AgentHttpParser, ParsesCompleteRequest)
{
  const std::string raw = "POST /mcp?x=1 HTTP/1.1\r\n"
                          "Host: 127.0.0.1:7311\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: 2\r\n"
                          "\r\n"
                          "{}";
  HttpRequest request;
  size_t consumed = 0;
  ASSERT_EQ(ParseHttpRequest(raw, &request, &consumed), HttpParseStatus::Ok);
  EXPECT_EQ(consumed, raw.size());
  EXPECT_EQ(request.method, "POST");
  EXPECT_EQ(request.path, "/mcp");
  EXPECT_EQ(request.version, "HTTP/1.1");
  EXPECT_EQ(request.body, "{}");
  EXPECT_TRUE(request.keep_alive);
  EXPECT_FALSE(request.expect_continue);
  EXPECT_EQ(request.GetHeader("CONTENT-TYPE"), "application/json");
  EXPECT_EQ(request.GetHeader("host"), "127.0.0.1:7311");
  EXPECT_FALSE(request.GetHeader("origin").has_value());
}

TEST(AgentHttpParser, WaitsForHeadersAndBody)
{
  HttpRequest request;
  size_t consumed = 0;
  EXPECT_EQ(ParseHttpRequest("POST /mcp HTTP/1.1\r\nHost: x\r\n", &request, &consumed),
            HttpParseStatus::NeedHeaders);

  const std::string partial = "POST /mcp HTTP/1.1\r\n"
                              "Expect: 100-continue\r\n"
                              "Content-Length: 10\r\n"
                              "\r\n"
                              "01234";
  ASSERT_EQ(ParseHttpRequest(partial, &request, &consumed), HttpParseStatus::NeedBody);
  EXPECT_TRUE(request.expect_continue);

  ASSERT_EQ(ParseHttpRequest(partial + "56789", &request, &consumed), HttpParseStatus::Ok);
  EXPECT_EQ(request.body, "0123456789");
}

TEST(AgentHttpParser, ParsesPipelinedRequestsOneAtATime)
{
  const std::string first = "POST /a HTTP/1.1\r\nContent-Length: 1\r\n\r\nA";
  const std::string second = "POST /b HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
  HttpRequest request;
  size_t consumed = 0;
  ASSERT_EQ(ParseHttpRequest(first + second, &request, &consumed), HttpParseStatus::Ok);
  EXPECT_EQ(request.path, "/a");
  EXPECT_EQ(consumed, first.size());
  ASSERT_EQ(ParseHttpRequest(second, &request, &consumed), HttpParseStatus::Ok);
  EXPECT_EQ(request.path, "/b");
  EXPECT_TRUE(request.body.empty());
}

TEST(AgentHttpParser, RejectsMalformedRequests)
{
  HttpRequest request;
  size_t consumed = 0;
  EXPECT_EQ(ParseHttpRequest("GARBAGE\r\n\r\n", &request, &consumed), HttpParseStatus::BadRequest);
  EXPECT_EQ(ParseHttpRequest("POST /mcp SPDY/3\r\n\r\n", &request, &consumed),
            HttpParseStatus::BadRequest);
  EXPECT_EQ(ParseHttpRequest("POST /mcp HTTP/1.1\r\nNoColon\r\n\r\n", &request, &consumed),
            HttpParseStatus::BadRequest);
  EXPECT_EQ(
      ParseHttpRequest("POST /mcp HTTP/1.1\r\nContent-Length: abc\r\n\r\n", &request, &consumed),
      HttpParseStatus::BadRequest);
  EXPECT_EQ(ParseHttpRequest("POST /mcp HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", &request,
                             &consumed),
            HttpParseStatus::BadRequest);
}

TEST(AgentHttpParser, RejectsOversizedRequests)
{
  HttpRequest request;
  size_t consumed = 0;
  EXPECT_EQ(ParseHttpRequest("POST /mcp HTTP/1.1\r\nContent-Length: 999999999\r\n\r\n", &request,
                             &consumed),
            HttpParseStatus::TooLarge);
  const std::string huge_header =
      "POST /mcp HTTP/1.1\r\nX: " + std::string(MAX_HTTP_HEADER_BYTES + 10, 'a');
  EXPECT_EQ(ParseHttpRequest(huge_header, &request, &consumed), HttpParseStatus::TooLarge);
}

TEST(AgentHttpParser, ConnectionPersistence)
{
  HttpRequest request;
  size_t consumed = 0;
  ASSERT_EQ(ParseHttpRequest("POST / HTTP/1.1\r\nConnection: close\r\n\r\n", &request, &consumed),
            HttpParseStatus::Ok);
  EXPECT_FALSE(request.keep_alive);
  ASSERT_EQ(ParseHttpRequest("POST / HTTP/1.0\r\n\r\n", &request, &consumed), HttpParseStatus::Ok);
  EXPECT_FALSE(request.keep_alive);
  ASSERT_EQ(
      ParseHttpRequest("POST / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", &request, &consumed),
      HttpParseStatus::Ok);
  EXPECT_TRUE(request.keep_alive);
}

TEST(AgentHttpParser, SerializesResponses)
{
  HttpResponse response{.status = 202};
  EXPECT_EQ(SerializeHttpResponse(response, true),
            "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\nConnection: keep-alive\r\n"
            "Cache-Control: no-store\r\n\r\n");

  response = {.status = 405, .content_type = "text/plain", .body = "no"};
  response.headers.emplace_back("Allow", "POST");
  EXPECT_EQ(SerializeHttpResponse(response, false),
            "HTTP/1.1 405 Method Not Allowed\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n"
            "Connection: close\r\nCache-Control: no-store\r\nAllow: POST\r\n\r\nno");
}
