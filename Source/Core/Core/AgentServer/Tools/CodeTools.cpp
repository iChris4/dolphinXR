// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Action Replay and Gecko codes of the running game (User/GameSettings/<GameID>.ini).

#include <algorithm>
#include <variant>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/StringUtil.h"
#include "Core/ARDecrypt.h"
#include "Core/ActionReplay.h"
#include "Core/AgentServer/AgentServer.h"
#include "Core/AgentServer/ToolHelpers.h"
#include "Core/AgentServer/Tools/Tools.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/GeckoCode.h"
#include "Core/GeckoCodeConfig.h"

namespace AgentServer
{
namespace
{
std::string LocalIniPath()
{
  return File::GetUserPath(D_GAMESETTINGS_IDX) + SConfig::GetInstance().GetGameID() + ".ini";
}

std::vector<ActionReplay::ARCode> LoadARCodes()
{
  const SConfig& config = SConfig::GetInstance();
  return ActionReplay::LoadCodes(config.LoadDefaultGameIni(), config.LoadLocalGameIni());
}

std::vector<Gecko::GeckoCode> LoadGeckoCodes()
{
  const SConfig& config = SConfig::GetInstance();
  return Gecko::LoadCodes(config.LoadDefaultGameIni(), config.LoadLocalGameIni());
}

// Writes the codes the way the Qt cheat widgets do, then makes them the active set.
void SaveAndApply(const std::vector<ActionReplay::ARCode>& codes)
{
  const std::string path = LocalIniPath();
  Common::IniFile ini;
  ini.Load(path);
  ActionReplay::SaveCodes(&ini, codes);
  ini.Save(path);
  const SConfig& config = SConfig::GetInstance();
  ActionReplay::ApplyCodes(codes, config.GetGameID(), config.GetRevision());
}

void SaveAndApply(const std::vector<Gecko::GeckoCode>& codes)
{
  const std::string path = LocalIniPath();
  Common::IniFile ini;
  ini.Load(path);
  Gecko::SaveCodes(ini, codes);
  ini.Save(path);
  const SConfig& config = SConfig::GetInstance();
  Gecko::SetActiveCodes(codes, config.GetGameID(), config.GetRevision());
}

std::string CheatsNote()
{
  return Config::AreCheatsEnabled() ?
             "" :
             " Cheats are disabled in Dolphin's settings, so codes do not run: ask the user to "
             "enable cheats (Config > General > Enable Cheats), or call set_cheats_enabled for "
             "this session.";
}

std::string FormatARCode(const ActionReplay::ARCode& code, bool lines)
{
  std::string text =
      fmt::format("{} \"{}\" ({}, {} line(s))", code.enabled ? "on " : "off", code.name,
                  code.user_defined ? "user" : "default", code.ops.size());
  if (lines)
  {
    for (const ActionReplay::AREntry& op : code.ops)
      text += "\n    " + ActionReplay::SerializeLine(op);
  }
  return text;
}

std::string FormatGeckoCode(const Gecko::GeckoCode& code, bool lines)
{
  std::string text =
      fmt::format("{} \"{}\" ({}, {} line(s){})", code.enabled ? "on " : "off", code.name,
                  code.user_defined ? "user" : "default", code.codes.size(),
                  code.creator.empty() ? "" : ", by " + code.creator);
  if (lines)
  {
    for (const Gecko::GeckoCode::Code& line : code.codes)
      text += fmt::format("\n    {:08X} {:08X}", line.address, line.data);
  }
  return text;
}

std::string RequireType(const ToolArgs& args)
{
  const std::string type = args.GetString("type", "ar");
  if (type != "ar" && type != "gecko")
    throw ToolError("type is ar (Action Replay) or gecko.");
  return type;
}

ToolResult ListCodes(const ToolArgs& args)
{
  RequireGame();
  const std::string type = args.GetString("type", "all");
  const bool lines = args.GetBool("lines", false);
  std::string text;
  if (type == "all" || type == "ar")
  {
    const auto codes = LoadARCodes();
    text += fmt::format("Action Replay codes ({}):\n", codes.size());
    for (const auto& code : codes)
      text += "  " + FormatARCode(code, lines) + "\n";
  }
  if (type == "all" || type == "gecko")
  {
    const auto codes = LoadGeckoCodes();
    text += fmt::format("Gecko codes ({}):\n", codes.size());
    for (const auto& code : codes)
      text += "  " + FormatGeckoCode(code, lines) + "\n";
  }
  text += fmt::format("Cheats are {} in Dolphin's settings.",
                      Config::AreCheatsEnabled() ? "enabled" : "disabled");
  return ToolResult::Text(std::move(text));
}

ToolResult AddCode(const ToolArgs& args)
{
  RequireGame();
  const std::string type = RequireType(args);
  const std::string name = args.RequireString("name");
  const std::vector<std::string> lines = args.GetStringArray("lines");
  const bool enabled = args.GetBool("enabled", true);
  const bool replace = args.GetBool("replace", false);
  if (lines.empty())
    throw ToolError("lines is the code's lines, e.g. [\"04123456 38600001\"].");

  if (type == "ar")
    return ToolResult::Text(AddActionReplayCode(name, lines, enabled, replace) + CheatsNote());

  Gecko::GeckoCode code;
  code.name = name;
  code.enabled = enabled;
  code.user_defined = true;
  code.creator = "DolphinXR agent";
  for (const std::string& note : args.GetStringArray("notes"))
    code.notes.push_back(note);
  for (const std::string& line : lines)
  {
    const auto parsed = Gecko::DeserializeLine(line);
    if (!parsed)
      throw ToolError(fmt::format("\"{}\" is not a Gecko code line.", line));
    code.codes.push_back(*parsed);
  }

  std::vector<Gecko::GeckoCode> codes = LoadGeckoCodes();
  const auto existing =
      std::find_if(codes.begin(), codes.end(), [&](const auto& c) { return c.name == name; });
  if (existing != codes.end())
  {
    if (!replace)
      throw ToolError(fmt::format("A Gecko code named \"{}\" exists; pass replace=true.", name));
    *existing = std::move(code);
  }
  else
  {
    codes.push_back(std::move(code));
  }
  SaveAndApply(codes);
  return ToolResult::Text(fmt::format("Gecko code \"{}\" saved to {} and {}.{}", name,
                                      LocalIniPath(), enabled ? "active" : "disabled",
                                      CheatsNote()));
}

ToolResult SetCodeEnabled(const ToolArgs& args)
{
  RequireGame();
  const std::string type = RequireType(args);
  const std::string name = args.RequireString("name");
  const bool enabled = args.GetBool("enabled", true);
  const auto toggle = [&](auto codes) {
    const auto it =
        std::find_if(codes.begin(), codes.end(), [&](const auto& c) { return c.name == name; });
    if (it == codes.end())
      throw ToolError(fmt::format("There is no {} code named \"{}\".", type, name));
    it->enabled = enabled;
    SaveAndApply(codes);
  };
  if (type == "ar")
    toggle(LoadARCodes());
  else
    toggle(LoadGeckoCodes());
  return ToolResult::Text(
      fmt::format("\"{}\" {} and saved.{}", name, enabled ? "enabled" : "disabled", CheatsNote()));
}

ToolResult RemoveCode(const ToolArgs& args)
{
  RequireGame();
  const std::string type = RequireType(args);
  const std::string name = args.RequireString("name");
  const auto remove = [&](auto codes) {
    const auto it =
        std::find_if(codes.begin(), codes.end(), [&](const auto& c) { return c.name == name; });
    if (it == codes.end())
      throw ToolError(fmt::format("There is no {} code named \"{}\".", type, name));
    if (!it->user_defined)
      throw ToolError("Codes shipped with Dolphin cannot be removed; disable it instead.");
    codes.erase(it);
    SaveAndApply(codes);
  };
  if (type == "ar")
    remove(LoadARCodes());
  else
    remove(LoadGeckoCodes());
  return ToolResult::Text(fmt::format("Removed \"{}\".", name));
}

ToolResult SetCheatsEnabled(const ToolArgs& args)
{
  RequireGame();
  const bool enabled = args.GetBool("enabled", true);
  Config::SetCurrent(Config::MAIN_ENABLE_CHEATS, enabled);
  const SConfig& config = SConfig::GetInstance();
  ActionReplay::ApplyCodes(LoadARCodes(), config.GetGameID(), config.GetRevision());
  Gecko::SetActiveCodes(LoadGeckoCodes(), config.GetGameID(), config.GetRevision());
  return ToolResult::Text(fmt::format("Cheats {} for this session (not saved to the user's "
                                      "settings).",
                                      enabled ? "enabled" : "disabled"));
}
}  // namespace

std::string AddActionReplayCode(const std::string& name, const std::vector<std::string>& lines,
                                bool enabled, bool replace)
{
  ActionReplay::ARCode code;
  code.name = name;
  code.enabled = enabled;
  code.default_enabled = false;
  code.user_defined = true;
  std::vector<std::string> encrypted;
  for (const std::string& line : lines)
  {
    const auto parsed = ActionReplay::DeserializeLine(line);
    if (const auto* entry = std::get_if<ActionReplay::AREntry>(&parsed))
      code.ops.push_back(*entry);
    else if (const auto* encrypted_line = std::get_if<ActionReplay::EncryptedLine>(&parsed))
      encrypted.push_back(*encrypted_line);
    else
      throw ToolError(fmt::format("\"{}\" is not an Action Replay line.", line));
  }
  if (!encrypted.empty())
  {
    if (!code.ops.empty())
      throw ToolError("Do not mix encrypted and decrypted Action Replay lines.");
    if (!ActionReplay::DecryptARCode(encrypted, &code.ops))
      throw ToolError("The encrypted Action Replay code could not be decrypted.");
  }

  std::vector<ActionReplay::ARCode> codes = LoadARCodes();
  const auto existing =
      std::find_if(codes.begin(), codes.end(), [&](const auto& c) { return c.name == name; });
  if (existing != codes.end())
  {
    if (!replace)
      throw ToolError(
          fmt::format("An Action Replay code named \"{}\" exists; pass replace=true.", name));
    *existing = std::move(code);
  }
  else
  {
    codes.push_back(std::move(code));
  }
  SaveAndApply(codes);
  return fmt::format("Action Replay code \"{}\" saved to {} and {}.", name, LocalIniPath(),
                     enabled ? "active" : "disabled");
}

void RegisterCodeTools(ToolRegistry& registry, Server&)
{
  registry.Add({.name = "list_codes",
                .title = "List cheat codes",
                .description = "The game's Action Replay and Gecko codes (default and user), "
                               "on or off, optionally with their lines.",
                .input_schema = Schema()
                                    .String("type", "Default all.", {"all", "ar", "gecko"})
                                    .Boolean("lines", "Include the code lines.")
                                    .Build(),
                .read_only = true,
                .idempotent = true,
                .handler = ListCodes});

  registry.Add(
      {.name = "add_code",
       .title = "Add a cheat code",
       .description =
           "Saves an Action Replay or Gecko code to User/GameSettings/<GameID>.ini and applies it "
           "at once (the same file Dolphin's cheat dialogs use). Codes found on the web or built "
           "with save_patch_as_ar_code. Ask the user before adding codes.",
       .input_schema =
           Schema()
               .String("type", "ar (default) or gecko.", {"ar", "gecko"})
               .String("name", "Code name.")
               .StringArray("lines", "Code lines, \"XXXXXXXX YYYYYYYY\" (AR may be encrypted).")
               .Boolean("enabled", "Default true.")
               .Boolean("replace", "Replace a code of the same name.")
               .StringArray("notes", "Gecko notes, e.g. the source.")
               .Required({"name", "lines"})
               .Build(),
       .destructive = true,
       .handler = AddCode});

  registry.Add({.name = "set_code_enabled",
                .title = "Turn a cheat code on or off",
                .description = "Enables or disables an Action Replay or Gecko code and saves it.",
                .input_schema = Schema()
                                    .String("type", "ar (default) or gecko.", {"ar", "gecko"})
                                    .String("name", "Code name.")
                                    .Boolean("enabled", "Default true.")
                                    .Required({"name"})
                                    .Build(),
                .idempotent = true,
                .handler = SetCodeEnabled});

  registry.Add({.name = "remove_code",
                .title = "Remove a cheat code",
                .description = "Removes a user Action Replay or Gecko code.",
                .input_schema = Schema()
                                    .String("type", "ar (default) or gecko.", {"ar", "gecko"})
                                    .String("name", "Code name.")
                                    .Required({"name"})
                                    .Build(),
                .destructive = true,
                .handler = RemoveCode});

  registry.Add({.name = "set_cheats_enabled",
                .title = "Enable cheats for this session",
                .description = "Turns Dolphin's Enable Cheats on (or off) for this session only, "
                               "so codes run without changing the user's settings.",
                .input_schema = Schema().Boolean("enabled", "Default true.").Build(),
                .idempotent = true,
                .handler = SetCheatsEnabled});
}
}  // namespace AgentServer
