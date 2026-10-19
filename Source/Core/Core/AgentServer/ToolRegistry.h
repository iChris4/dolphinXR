// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"

namespace AgentServer
{
// Thrown by a tool handler for a failure the agent should see as a tool result with isError set
// (bad argument, no game running, timeout...), as opposed to a JSON-RPC protocol error.
class ToolError final : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

// The "arguments" object of a tools/call request, with typed accessors. Accessors throw ToolError
// when a value has the wrong type; Require* also throw when the key is missing.
class ToolArgs final
{
public:
  ToolArgs() = default;
  explicit ToolArgs(picojson::object args);

  // Present and not null.
  bool Has(const std::string& key) const;
  const picojson::object& Raw() const { return m_args; }

  s64 GetInt(const std::string& key, s64 default_value) const;
  s64 RequireInt(const std::string& key) const;
  double GetNumber(const std::string& key, double default_value) const;
  std::optional<double> GetOptionalNumber(const std::string& key) const;
  bool GetBool(const std::string& key, bool default_value) const;
  std::string GetString(const std::string& key, std::string default_value = {}) const;
  std::string RequireString(const std::string& key) const;
  // An emulated address or a 64-bit hash, given as a hex string ("0x80123456" or "80123456") or a
  // JSON number.
  std::optional<u64> GetHex(const std::string& key) const;
  std::optional<u32> GetAddress(const std::string& key) const;
  u32 RequireAddress(const std::string& key) const;
  std::vector<std::string> GetStringArray(const std::string& key) const;
  // Hex strings or numbers.
  std::vector<u64> GetHexArray(const std::string& key) const;
  const picojson::array* GetArray(const std::string& key) const;
  const picojson::object* GetObject(const std::string& key) const;

private:
  const picojson::value* Find(const std::string& key) const;

  picojson::object m_args;
};

// Parses a hex string ("0x1A2B", "1a2b") or a non-negative integral JSON number.
std::optional<u64> ParseHexValue(const picojson::value& value);
std::optional<u64> ParseHexString(std::string_view text);

std::string Base64Encode(std::span<const u8> data);
std::optional<std::string> Base64Decode(std::string_view text);

// The result of one tool call: MCP content blocks (text, images), the error flag, and optional
// structured content for clients that read it.
struct ToolResult
{
  static ToolResult Text(std::string text);
  static ToolResult Error(std::string text);

  ToolResult& AddText(std::string text);
  ToolResult& AddImagePng(std::span<const u8> png);
  ToolResult& SetStructured(picojson::object object);

  picojson::object ToJson() const;

  picojson::array content;
  bool is_error = false;
  std::optional<picojson::object> structured;
};

using ToolHandler = std::function<ToolResult(const ToolArgs&)>;

struct ToolDef
{
  std::string name;
  std::string title;
  std::string description;
  // JSON Schema of the arguments object.
  picojson::object input_schema;
  bool read_only = false;
  bool destructive = false;
  bool idempotent = false;
  ToolHandler handler;
};

// Builds the JSON Schema of a tool's arguments object.
class Schema final
{
public:
  Schema& Integer(const std::string& name, std::string description,
                  std::optional<s64> minimum = std::nullopt,
                  std::optional<s64> maximum = std::nullopt);
  Schema& Number(const std::string& name, std::string description);
  Schema& Boolean(const std::string& name, std::string description);
  Schema& String(const std::string& name, std::string description,
                 std::initializer_list<std::string_view> enum_values = {});
  // A hex string such as "0x80123456" (a JSON number is accepted too).
  Schema& Hex(const std::string& name, std::string description);
  Schema& StringArray(const std::string& name, std::string description,
                      std::initializer_list<std::string_view> enum_values = {});
  Schema& HexArray(const std::string& name, std::string description);
  Schema& ArrayOf(const std::string& name, std::string description, picojson::object item_schema);
  Schema& Object(const std::string& name, std::string description, picojson::object schema);
  Schema& Required(std::initializer_list<std::string_view> names);

  picojson::object Build() const;

private:
  Schema& Add(const std::string& name, picojson::object property);

  picojson::object m_properties;
  std::vector<std::string> m_required;
};

class ToolRegistry final
{
public:
  void Add(ToolDef def);
  const ToolDef* Find(std::string_view name) const;
  const std::vector<ToolDef>& GetTools() const { return m_tools; }
  // The "tools" array of a tools/list result.
  picojson::array ToJson() const;

private:
  std::vector<ToolDef> m_tools;
};

// JSON helpers: picojson has no int constructor without PICOJSON_USE_INT64.
inline picojson::value JsonNumber(double value)
{
  return picojson::value(value);
}
inline picojson::value JsonString(std::string value)
{
  return picojson::value(std::move(value));
}
inline picojson::value JsonBool(bool value)
{
  return picojson::value(value);
}
}  // namespace AgentServer
