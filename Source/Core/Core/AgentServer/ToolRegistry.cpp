// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/AgentServer/ToolRegistry.h"

#include <cmath>
#include <utility>

#include <fmt/format.h>
#include <mbedtls/base64.h>

#include "Common/Assert.h"
#include "Common/StringUtil.h"

namespace AgentServer
{
namespace
{
const char* JsonTypeName(const picojson::value& value)
{
  if (value.is<bool>())
    return "boolean";
  if (value.is<double>())
    return "number";
  if (value.is<std::string>())
    return "string";
  if (value.is<picojson::array>())
    return "array";
  if (value.is<picojson::object>())
    return "object";
  return "null";
}

[[noreturn]] void ThrowType(const std::string& key, const char* expected,
                            const picojson::value& value)
{
  throw ToolError(
      fmt::format("Argument \"{}\" must be {} (got {}).", key, expected, JsonTypeName(value)));
}

[[noreturn]] void ThrowMissing(const std::string& key)
{
  throw ToolError(fmt::format("Missing required argument \"{}\".", key));
}
}  // namespace

std::optional<u64> ParseHexString(std::string_view text)
{
  text = StripWhitespace(text);
  if (text.starts_with("0x") || text.starts_with("0X"))
    text.remove_prefix(2);
  if (text.empty() || text.size() > 16)
    return std::nullopt;

  u64 value = 0;
  for (const char c : text)
  {
    u64 digit;
    if (c >= '0' && c <= '9')
      digit = c - '0';
    else if (c >= 'a' && c <= 'f')
      digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      digit = c - 'A' + 10;
    else
      return std::nullopt;
    value = (value << 4) | digit;
  }
  return value;
}

std::optional<u64> ParseHexValue(const picojson::value& value)
{
  if (value.is<std::string>())
    return ParseHexString(value.get<std::string>());
  if (value.is<double>())
  {
    const double number = value.get<double>();
    if (number < 0 || number != std::floor(number) || number > 18446744073709549568.0)
      return std::nullopt;
    return static_cast<u64>(number);
  }
  return std::nullopt;
}

std::string Base64Encode(std::span<const u8> data)
{
  size_t needed = 0;
  mbedtls_base64_encode(nullptr, 0, &needed, data.data(), data.size());
  std::string out(needed, '\0');
  size_t written = 0;
  if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out.data()), out.size(), &written,
                            data.data(), data.size()) != 0)
  {
    return {};
  }
  out.resize(written);
  return out;
}

std::optional<std::string> Base64Decode(std::string_view text)
{
  const auto* src = reinterpret_cast<const unsigned char*>(text.data());
  size_t needed = 0;
  const int probe = mbedtls_base64_decode(nullptr, 0, &needed, src, text.size());
  if (probe == MBEDTLS_ERR_BASE64_INVALID_CHARACTER)
    return std::nullopt;
  std::string out(needed, '\0');
  size_t written = 0;
  if (mbedtls_base64_decode(reinterpret_cast<unsigned char*>(out.data()), out.size(), &written, src,
                            text.size()) != 0)
  {
    return std::nullopt;
  }
  out.resize(written);
  return out;
}

ToolArgs::ToolArgs(picojson::object args) : m_args(std::move(args))
{
}

const picojson::value* ToolArgs::Find(const std::string& key) const
{
  const auto it = m_args.find(key);
  if (it == m_args.end() || it->second.is<picojson::null>())
    return nullptr;
  return &it->second;
}

bool ToolArgs::Has(const std::string& key) const
{
  return Find(key) != nullptr;
}

s64 ToolArgs::GetInt(const std::string& key, s64 default_value) const
{
  const picojson::value* value = Find(key);
  if (!value)
    return default_value;
  if (value->is<double>())
    return static_cast<s64>(std::llround(value->get<double>()));
  // Models sometimes quote numbers.
  if (value->is<std::string>())
  {
    s64 parsed;
    if (TryParse(value->get<std::string>(), &parsed))
      return parsed;
  }
  ThrowType(key, "an integer", *value);
}

s64 ToolArgs::RequireInt(const std::string& key) const
{
  if (!Has(key))
    ThrowMissing(key);
  return GetInt(key, 0);
}

double ToolArgs::GetNumber(const std::string& key, double default_value) const
{
  return GetOptionalNumber(key).value_or(default_value);
}

std::optional<double> ToolArgs::GetOptionalNumber(const std::string& key) const
{
  const picojson::value* value = Find(key);
  if (!value)
    return std::nullopt;
  if (value->is<double>())
    return value->get<double>();
  if (value->is<std::string>())
  {
    double parsed;
    if (TryParse(value->get<std::string>(), &parsed))
      return parsed;
  }
  ThrowType(key, "a number", *value);
}

bool ToolArgs::GetBool(const std::string& key, bool default_value) const
{
  const picojson::value* value = Find(key);
  if (!value)
    return default_value;
  if (value->is<bool>())
    return value->get<bool>();
  if (value->is<std::string>())
  {
    bool parsed;
    if (TryParse(value->get<std::string>(), &parsed))
      return parsed;
  }
  ThrowType(key, "a boolean", *value);
}

std::string ToolArgs::GetString(const std::string& key, std::string default_value) const
{
  const picojson::value* value = Find(key);
  if (!value)
    return default_value;
  if (value->is<std::string>())
    return value->get<std::string>();
  if (value->is<double>() || value->is<bool>())
    return value->to_str();
  ThrowType(key, "a string", *value);
}

std::string ToolArgs::RequireString(const std::string& key) const
{
  if (!Has(key))
    ThrowMissing(key);
  return GetString(key);
}

std::optional<u64> ToolArgs::GetHex(const std::string& key) const
{
  const picojson::value* value = Find(key);
  if (!value)
    return std::nullopt;
  const std::optional<u64> parsed = ParseHexValue(*value);
  if (!parsed)
    ThrowType(key, "a hex string such as \"0x80123456\"", *value);
  return parsed;
}

std::optional<u32> ToolArgs::GetAddress(const std::string& key) const
{
  const std::optional<u64> value = GetHex(key);
  if (!value)
    return std::nullopt;
  if (*value > 0xFFFFFFFFull)
    throw ToolError(fmt::format("Argument \"{}\" is not a 32-bit address.", key));
  return static_cast<u32>(*value);
}

u32 ToolArgs::RequireAddress(const std::string& key) const
{
  const std::optional<u32> value = GetAddress(key);
  if (!value)
    ThrowMissing(key);
  return *value;
}

std::vector<std::string> ToolArgs::GetStringArray(const std::string& key) const
{
  std::vector<std::string> result;
  const picojson::value* value = Find(key);
  if (!value)
    return result;
  // A lone string is accepted as a one-element array.
  if (value->is<std::string>())
  {
    result.push_back(value->get<std::string>());
    return result;
  }
  if (!value->is<picojson::array>())
    ThrowType(key, "an array of strings", *value);
  for (const picojson::value& item : value->get<picojson::array>())
  {
    if (!item.is<std::string>())
      ThrowType(key, "an array of strings", *value);
    result.push_back(item.get<std::string>());
  }
  return result;
}

std::vector<u64> ToolArgs::GetHexArray(const std::string& key) const
{
  std::vector<u64> result;
  const picojson::value* value = Find(key);
  if (!value)
    return result;
  if (!value->is<picojson::array>())
  {
    const std::optional<u64> single = ParseHexValue(*value);
    if (!single)
      ThrowType(key, "an array of hex strings", *value);
    result.push_back(*single);
    return result;
  }
  for (const picojson::value& item : value->get<picojson::array>())
  {
    const std::optional<u64> parsed = ParseHexValue(item);
    if (!parsed)
      ThrowType(key, "an array of hex strings", item);
    result.push_back(*parsed);
  }
  return result;
}

const picojson::array* ToolArgs::GetArray(const std::string& key) const
{
  const picojson::value* value = Find(key);
  if (!value)
    return nullptr;
  if (!value->is<picojson::array>())
    ThrowType(key, "an array", *value);
  return &value->get<picojson::array>();
}

const picojson::object* ToolArgs::GetObject(const std::string& key) const
{
  const picojson::value* value = Find(key);
  if (!value)
    return nullptr;
  if (!value->is<picojson::object>())
    ThrowType(key, "an object", *value);
  return &value->get<picojson::object>();
}

ToolResult ToolResult::Text(std::string text)
{
  ToolResult result;
  result.AddText(std::move(text));
  return result;
}

ToolResult ToolResult::Error(std::string text)
{
  ToolResult result = Text(std::move(text));
  result.is_error = true;
  return result;
}

ToolResult& ToolResult::AddText(std::string text)
{
  picojson::object block;
  block["type"] = JsonString("text");
  block["text"] = JsonString(std::move(text));
  content.emplace_back(std::move(block));
  return *this;
}

ToolResult& ToolResult::AddImagePng(std::span<const u8> png)
{
  picojson::object block;
  block["type"] = JsonString("image");
  block["data"] = JsonString(Base64Encode(png));
  block["mimeType"] = JsonString("image/png");
  content.emplace_back(std::move(block));
  return *this;
}

ToolResult& ToolResult::SetStructured(picojson::object object)
{
  structured = std::move(object);
  return *this;
}

picojson::object ToolResult::ToJson() const
{
  picojson::object result;
  result["content"] = picojson::value(content);
  if (is_error)
    result["isError"] = JsonBool(true);
  if (structured)
    result["structuredContent"] = picojson::value(*structured);
  return result;
}

Schema& Schema::Add(const std::string& name, picojson::object property)
{
  m_properties[name] = picojson::value(std::move(property));
  return *this;
}

Schema& Schema::Integer(const std::string& name, std::string description,
                        std::optional<s64> minimum, std::optional<s64> maximum)
{
  picojson::object property;
  property["type"] = JsonString("integer");
  property["description"] = JsonString(std::move(description));
  if (minimum)
    property["minimum"] = JsonNumber(static_cast<double>(*minimum));
  if (maximum)
    property["maximum"] = JsonNumber(static_cast<double>(*maximum));
  return Add(name, std::move(property));
}

Schema& Schema::Number(const std::string& name, std::string description)
{
  picojson::object property;
  property["type"] = JsonString("number");
  property["description"] = JsonString(std::move(description));
  return Add(name, std::move(property));
}

Schema& Schema::Boolean(const std::string& name, std::string description)
{
  picojson::object property;
  property["type"] = JsonString("boolean");
  property["description"] = JsonString(std::move(description));
  return Add(name, std::move(property));
}

Schema& Schema::String(const std::string& name, std::string description,
                       std::initializer_list<std::string_view> enum_values)
{
  picojson::object property;
  property["type"] = JsonString("string");
  property["description"] = JsonString(std::move(description));
  if (enum_values.size() != 0)
  {
    picojson::array values;
    for (const std::string_view value : enum_values)
      values.push_back(JsonString(std::string(value)));
    property["enum"] = picojson::value(std::move(values));
  }
  return Add(name, std::move(property));
}

Schema& Schema::Hex(const std::string& name, std::string description)
{
  picojson::object property;
  property["type"] = JsonString("string");
  property["description"] = JsonString(std::move(description));
  return Add(name, std::move(property));
}

Schema& Schema::StringArray(const std::string& name, std::string description,
                            std::initializer_list<std::string_view> enum_values)
{
  picojson::object items;
  items["type"] = JsonString("string");
  if (enum_values.size() != 0)
  {
    picojson::array values;
    for (const std::string_view value : enum_values)
      values.push_back(JsonString(std::string(value)));
    items["enum"] = picojson::value(std::move(values));
  }
  picojson::object property;
  property["type"] = JsonString("array");
  property["description"] = JsonString(std::move(description));
  property["items"] = picojson::value(std::move(items));
  return Add(name, std::move(property));
}

Schema& Schema::HexArray(const std::string& name, std::string description)
{
  return StringArray(name, std::move(description));
}

Schema& Schema::ArrayOf(const std::string& name, std::string description,
                        picojson::object item_schema)
{
  picojson::object property;
  property["type"] = JsonString("array");
  property["description"] = JsonString(std::move(description));
  property["items"] = picojson::value(std::move(item_schema));
  return Add(name, std::move(property));
}

Schema& Schema::Object(const std::string& name, std::string description, picojson::object schema)
{
  schema["description"] = JsonString(std::move(description));
  return Add(name, std::move(schema));
}

Schema& Schema::Required(std::initializer_list<std::string_view> names)
{
  for (const std::string_view name : names)
    m_required.emplace_back(name);
  return *this;
}

picojson::object Schema::Build() const
{
  picojson::object schema;
  schema["type"] = JsonString("object");
  schema["properties"] = picojson::value(m_properties);
  if (!m_required.empty())
  {
    picojson::array required;
    for (const std::string& name : m_required)
      required.push_back(JsonString(name));
    schema["required"] = picojson::value(std::move(required));
  }
  return schema;
}

void ToolRegistry::Add(ToolDef def)
{
  ASSERT_MSG(CORE, Find(def.name) == nullptr, "Agent tool {} registered twice", def.name);
  m_tools.push_back(std::move(def));
}

const ToolDef* ToolRegistry::Find(std::string_view name) const
{
  for (const ToolDef& def : m_tools)
  {
    if (def.name == name)
      return &def;
  }
  return nullptr;
}

picojson::array ToolRegistry::ToJson() const
{
  picojson::array tools;
  tools.reserve(m_tools.size());
  for (const ToolDef& def : m_tools)
  {
    picojson::object tool;
    tool["name"] = JsonString(def.name);
    if (!def.title.empty())
      tool["title"] = JsonString(def.title);
    tool["description"] = JsonString(def.description);
    tool["inputSchema"] = picojson::value(def.input_schema);
    picojson::object annotations;
    annotations["readOnlyHint"] = JsonBool(def.read_only);
    annotations["destructiveHint"] = JsonBool(def.destructive);
    annotations["idempotentHint"] = JsonBool(def.idempotent);
    annotations["openWorldHint"] = JsonBool(false);
    tool["annotations"] = picojson::value(std::move(annotations));
    tools.emplace_back(std::move(tool));
  }
  return tools;
}
}  // namespace AgentServer
