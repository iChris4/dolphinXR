// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/AutoUpdate.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <picojson.h>

#include "Common/HttpRequest.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Common/Version.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <sys/stat.h>
#endif

#if defined(_WIN32) || defined(__APPLE__)
#define OS_SUPPORTS_UPDATER
#include "Common/CommonFuncs.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#endif

// Refer to docs/autoupdate_overview.md for a detailed overview of the autoupdate process

namespace
{
std::atomic_bool s_check_in_progress = false;
bool s_update_triggered = false;

#ifdef __APPLE__
const char UPDATER_CONTENT_PATH[] = "/Contents/MacOS/Dolphin Updater";
#endif

#ifdef OS_SUPPORTS_UPDATER

const char UPDATER_LOG_FILE[] = "Updater.log";

std::string UpdaterPath(bool relocated = false)
{
#ifdef __APPLE__
  if (relocated)
    return File::GetExeDirectory() + DIR_SEP + ".Dolphin Updater.2.app";
  else
    return File::GetBundleDirectory() + DIR_SEP + "Contents/Helpers/Dolphin Updater.app";
#else
  return File::GetExeDirectory() + DIR_SEP + "Updater.exe";
#endif
}

std::string MakeUpdaterCommandLine(const std::map<std::string, std::string>& flags)
{
#ifdef __APPLE__
  std::string cmdline = "\"" + UpdaterPath(true) + UPDATER_CONTENT_PATH + "\"";
#else
  std::string cmdline = UpdaterPath();
#endif

  cmdline += " ";

  for (const auto& pair : flags)
  {
    std::string value = "--" + pair.first + "=" + pair.second;
    value = ReplaceAll(value, "\"", "\\\"");  // Escape double quotes.
    value = "\"" + value + "\" ";
    cmdline += value;
  }
  return cmdline;
}

void CleanupFromPreviousUpdate()
{
#ifdef __APPLE__
  // Remove the relocated updater file.
  File::DeleteDirRecursively(UpdaterPath(true));

  // Remove the old (non-embedded) updater app bundle.
  // While the update process will delete the files within the old bundle after updating to a
  // version with an embedded updater, it won't delete the folder structure of the bundle, so
  // we should clean those leftovers up.
  File::DeleteDirRecursively(File::GetExeDirectory() + DIR_SEP + "Dolphin Updater.app");
#endif

  // Updater.log was moved from GetExeDirectory() to GetUserPath(D_LOGS_IDX) in 5.0-14529.
  File::Delete(File::GetExeDirectory() + DIR_SEP + "Updater.log",
               File::IfAbsentBehavior::NoConsoleWarning);
}
#endif

// DolphinXR publishes its own releases with Tools/dolphinxr-update.py (docs/DolphinXR-Updates.md):
// upstream's server would replace a VR build that carries an upstream hash with upstream binaries.
// The server is a static site:
//   update/v1/<track>/<platform>/latest.json  the releases of a track, newest first
//   update/v1/<platform>/manifests/<hash>     the signed manifest of each release
//   update/v1/content/                        the content store, gzipped files by hash
constexpr char DOLPHINXR_UPDATE_SERVER_URL[] = "https://ichris4.github.io/dolphinxr-updates";
constexpr char DOLPHINXR_RELEASES_URL[] = "https://github.com/iChris4/dolphinXR/releases";

struct Release
{
  std::string name;
  std::string hash;
  std::string notes_html;
};

std::vector<Release> ParseReleases(const picojson::value& json)
{
  std::vector<Release> releases;
  if (!json.is<picojson::object>() || !json.get("releases").is<picojson::array>())
    return releases;

  for (const picojson::value& entry : json.get("releases").get<picojson::array>())
  {
    if (!entry.is<picojson::object>() || !entry.get("name").is<std::string>() ||
        !entry.get("hash").is<std::string>())
    {
      continue;
    }
    Release& release = releases.emplace_back();
    release.name = entry.get("name").get<std::string>();
    release.hash = entry.get("hash").get<std::string>();
    if (entry.get("notes_html").is<std::string>())
      release.notes_html = entry.get("notes_html").get<std::string>();
  }
  return releases;
}

// This ignores i18n because the release notes are only written in English anyway.
std::string GenerateChangelog(std::span<const Release> releases)
{
  std::string changelog;
  for (const Release& release : releases)
  {
    if (!changelog.empty())
      changelog += "<hr>";
    changelog += "<b>DolphinVR Redux " + Common::GetEscapedHtml(release.name) + "</b>";
    changelog += release.notes_html;
  }
  return changelog;
}
}  // namespace

bool AutoUpdateChecker::SystemSupportsAutoUpdates()
{
#if defined(AUTOUPDATE) && defined(OS_SUPPORTS_UPDATER)
  return true;
#else
  return false;
#endif
}

static std::string GetPlatformID()
{
#if defined(_WIN32)
#if defined(_M_ARM_64)
  return "win-arm64";
#else
  return "win";
#endif
#elif defined(__APPLE__)
#if defined(MACOS_UNIVERSAL_BUILD)
  return "macos-universal";
#else
  return "macos";
#endif
#else
  return "unknown";
#endif
}

static std::string GetUpdateServerUrl()
{
  auto server_url = std::getenv("DOLPHIN_UPDATE_SERVER_URL");
  if (server_url)
    return server_url;
  return DOLPHINXR_UPDATE_SERVER_URL;
}

static u32 GetOwnProcessId()
{
#ifdef _WIN32
  return GetCurrentProcessId();
#else
  return getpid();
#endif
}

void AutoUpdateChecker::CheckForUpdate(std::string_view update_track,
                                       std::string_view hash_override, const CheckType check_type)
{
  bool expected_check_in_progress = false;
  if (!s_check_in_progress.compare_exchange_strong(expected_check_in_progress, true))
    return;

  Common::ScopeGuard guard([]() { s_check_in_progress.store(false); });

  if (s_update_triggered)
  {
    if (check_type == CheckType::Manual)
      SuccessAlertFmtT("A Dolphin update is already scheduled for the next time it closes.");

    return;
  }

  // Don't bother checking if updates are not supported or not enabled.
  if (!SystemSupportsAutoUpdates() || update_track.empty())
    return;

#ifdef OS_SUPPORTS_UPDATER
  CleanupFromPreviousUpdate();
#endif

  const bool is_manual_check = check_type == CheckType::Manual;

  // A build with uncommitted changes carries the hash of a release but not its files.
  if (hash_override.empty() && Common::GetScmDescStr().ends_with("-dirty"))
  {
    if (is_manual_check)
      SuccessAlertFmtT("This build has uncommitted changes, so it cannot update itself.");
    INFO_LOG_FMT(COMMON, "Auto-update: this build has uncommitted changes.");
    return;
  }

  const std::string server_url = GetUpdateServerUrl();
  const std::string platform = GetPlatformID();
  std::string_view version_hash = hash_override.empty() ? Common::GetScmRevGitStr() : hash_override;
  std::string url =
      fmt::format("{}/update/v1/{}/{}/latest.json", server_url, update_track, platform);

  Common::HttpRequest req{std::chrono::seconds{10}};
  auto resp = req.Get(url);
  if (!resp)
  {
    if (is_manual_check)
      CriticalAlertFmtT("Unable to contact update server.");
    return;
  }
  const std::string contents(reinterpret_cast<char*>(resp->data()), resp->size());
  INFO_LOG_FMT(COMMON, "Auto-update JSON response: {}", contents);

  picojson::value json;
  const std::string err = picojson::parse(json, contents);
  if (!err.empty())
  {
    CriticalAlertFmtT("Invalid JSON received from auto-update service : {0}", err);
    return;
  }
  // Only a release of the track can update: the updater needs the manifest of the installed files,
  // and an unlisted build could be newer than the track.
  const std::vector<Release> releases = ParseReleases(json);
  const auto current = std::ranges::find(releases, version_hash, &Release::hash);
  if (current == releases.end())
  {
    if (is_manual_check)
    {
      SuccessAlertFmtT("This build is not a release of this update track, so it cannot update "
                       "itself. DolphinVR Redux releases can be downloaded from {0}",
                       DOLPHINXR_RELEASES_URL);
    }
    INFO_LOG_FMT(COMMON, "Auto-update: this build is not a release of the track.");
    return;
  }

  if (current == releases.begin())
  {
    if (is_manual_check)
      SuccessAlertFmtT("You are running the latest version available on this update track.");
    INFO_LOG_FMT(COMMON, "Auto-update status: we are up to date.");
    return;
  }

  const Release& latest = releases.front();
  NewVersionInformation nvi;
  nvi.this_manifest_url =
      fmt::format("{}/update/v1/{}/manifests/{}", server_url, platform, version_hash);
  nvi.next_manifest_url =
      fmt::format("{}/update/v1/{}/manifests/{}", server_url, platform, latest.hash);
  nvi.content_store_url = fmt::format("{}/update/v1/content/", server_url);
  nvi.current_shortrev = current->name;
  nvi.new_shortrev = latest.name;
  nvi.new_hash = latest.hash;
  nvi.changelog_html = GenerateChangelog(std::span(releases.begin(), current));

  if (std::getenv("DOLPHIN_UPDATE_TEST_DONE"))
  {
    // We are at end of updater test flow, send a message to server, which will kill us.
    req.Get(fmt::format("{}/update-test-done/{}", server_url, GetOwnProcessId()));
  }
  else
  {
    OnUpdateAvailable(nvi);
  }
}

void AutoUpdateChecker::TriggerUpdate(const AutoUpdateChecker::NewVersionInformation& info,
                                      const AutoUpdateChecker::RestartMode restart_mode)
{
  // Check to make sure we don't already have an update triggered
  if (s_update_triggered)
  {
    WARN_LOG_FMT(COMMON, "Auto-update: received a redundant trigger request, ignoring");
    return;
  }

#ifdef OS_SUPPORTS_UPDATER
  std::map<std::string, std::string> updater_flags;
  updater_flags["this-manifest-url"] = info.this_manifest_url;
  updater_flags["next-manifest-url"] = info.next_manifest_url;
  updater_flags["content-store-url"] = info.content_store_url;
  updater_flags["parent-pid"] = std::to_string(GetOwnProcessId());
  updater_flags["install-base-path"] = File::GetExeDirectory();
  updater_flags["log-file"] = File::GetUserPath(D_LOGS_IDX) + UPDATER_LOG_FILE;

  if (restart_mode == RestartMode::RESTART_AFTER_UPDATE)
    updater_flags["binary-to-restart"] = File::GetExePath();

#ifdef __APPLE__
  // Copy the updater so it can update itself if needed.
  const std::string reloc_updater_path = UpdaterPath(true);
  if (!File::Copy(UpdaterPath(), reloc_updater_path))
  {
    CriticalAlertFmtT("Unable to create updater copy.");
    return;
  }
  if (chmod((reloc_updater_path + UPDATER_CONTENT_PATH).c_str(), 0700) != 0)
  {
    CriticalAlertFmtT("Unable to set permissions on updater copy.");
    return;
  }
#endif

  // Run the updater!
  std::string command_line = MakeUpdaterCommandLine(updater_flags);
  INFO_LOG_FMT(COMMON, "Updater command line: {}", command_line);

#ifdef _WIN32
  STARTUPINFO sinfo{.cb = sizeof(sinfo)};
  sinfo.dwFlags = STARTF_FORCEOFFFEEDBACK;  // No hourglass cursor after starting the process.
  PROCESS_INFORMATION pinfo;
  if (CreateProcessW(UTF8ToWString(UpdaterPath()).c_str(), UTF8ToWString(command_line).data(),
                     nullptr, nullptr, FALSE, 0, nullptr, nullptr, &sinfo, &pinfo))
  {
    CloseHandle(pinfo.hThread);
    CloseHandle(pinfo.hProcess);
    s_update_triggered = true;
  }
  else
  {
    const std::string error = Common::GetLastErrorString();
    CriticalAlertFmtT("Could not start updater process: {0}", error);
  }
#else
  if (popen(command_line.c_str(), "r") == nullptr)
  {
    const std::string error = Common::LastStrerrorString();
    CriticalAlertFmtT("Could not start updater process: {0}", error);
  }
  else
  {
    s_update_triggered = true;
  }
#endif

#endif
}
