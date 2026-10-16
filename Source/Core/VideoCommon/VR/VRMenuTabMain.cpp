// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/Config/Layer.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Common/Timer.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/UISettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/Host.h"
#include "Core/System.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"

#include "VideoCommon/VR/OpenXRManager.h"
#include "VideoCommon/VR/VRMenuActions.h"
#include "VideoCommon/VR/VRMenuTabs.h"
#include "VideoCommon/VR/VRMenuWidgets.h"
#include "VideoCommon/VR/VRVideoRecorder.h"

namespace VR
{
namespace
{
// A destructive button needs a second press within this time.
constexpr u64 CONFIRM_TIMEOUT_US = 4'000'000;

struct Disc
{
  std::string path;
  u8 number = 0;
  std::string file_name;
};

// Game folder entries on Android are content:// URIs; show their last path segment decoded.
std::string GetDisplayFileName(const std::string& path)
{
  if (!path.starts_with("content://"))
  {
    std::string name, extension;
    SplitPath(path, nullptr, &name, &extension);
    return name + extension;
  }

  const auto hex_value = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  std::string decoded;
  for (size_t i = 0; i < path.size(); ++i)
  {
    const int high = path[i] == '%' && i + 2 < path.size() ? hex_value(path[i + 1]) : -1;
    const int low = high >= 0 ? hex_value(path[i + 2]) : -1;
    if (low >= 0)
    {
      decoded += static_cast<char>(high * 16 + low);
      i += 2;
    }
    else
    {
      decoded += path[i];
    }
  }
  const size_t separator = decoded.find_last_of("/:");
  return separator == std::string::npos ? decoded : decoded.substr(separator + 1);
}

// Host thread: the discs in the game folders that belong to the running game (multi-disc
// games), by disc number. Opens every disc image in those folders.
std::vector<Disc> FindDiscsOfRunningGame()
{
  const std::string game_id = SConfig::GetInstance().GetGameID();
  const std::vector<std::string> folders = Config::GetIsoPaths();
  const std::vector<std::string_view> folder_views(folders.begin(), folders.end());
  constexpr auto extensions = std::to_array<std::string_view>(
      {".gcm", ".iso", ".tgc", ".bin", ".ciso", ".gcz", ".wbfs", ".wia", ".rvz", ".nfs"});

  std::vector<Disc> discs;
  for (const std::string& path : Common::DoFileSearch(
           folder_views, extensions, Config::Get(Config::MAIN_RECURSIVE_ISO_PATHS)))
  {
    const std::unique_ptr<DiscIO::VolumeDisc> disc = DiscIO::CreateDisc(path);
    if (!disc || disc->GetGameID() != game_id)
      continue;
    discs.push_back({path, disc->GetDiscNumber().value_or(0), GetDisplayFileName(path)});
  }
  std::ranges::sort(discs, [](const Disc& a, const Disc& b) {
    return std::tie(a.number, a.file_name) < std::tie(b.number, b.file_name);
  });
  return discs;
}

class MainTab final : public VRMenuTab
{
public:
  const char* GetName() const override { return "Main"; }

  void OnOpen(VRMenuContext& context) override
  {
    const SConfig& sconfig = SConfig::GetInstance();
    m_title = sconfig.GetTitleDescription();
    m_confirm_exit_until_us = 0;
  }

  void Draw(VRMenuContext& context) override
  {
    Core::System& system = Core::System::GetInstance();
    const bool paused = Core::GetState(system) == Core::State::Paused;

    // The running game.
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.25f);
    CenteredText(m_title.empty() ? context.game_id : m_title, false);
    ImGui::PopFont();
    CenteredText(fmt::format("{}    {}", context.game_id, paused ? "Paused" : "Running"), true);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const bool busy = context.actions.GetPendingJobs() > 0;
    ImGui::BeginDisabled(busy);

    if (BigButton(paused ? "Resume Emulation" : "Pause Emulation"))
    {
      const bool pause = !paused;
      context.actions.Run(pause ? "Pausing..." : "Resuming...", [pause](Core::System& sys) {
        Core::SetState(sys, pause ? Core::State::Paused : Core::State::Running);
        const bool now_paused = Core::GetState(sys) == Core::State::Paused;
        if (pause != now_paused)
          return std::string(pause ? "Emulation could not be paused" : "Could not resume");
        return std::string(now_paused ? "Emulation paused" : "Emulation resumed");
      });
    }

    if (BigButton("Take Screenshot"))
    {
      context.actions.Run("Taking a screenshot...", [](Core::System&) {
        Core::SaveScreenShot();
        return fmt::format("Screenshot saved to ScreenShots/{}",
                           SConfig::GetInstance().GetGameID());
      });
    }

    DrawRecordButton(context);

    if (BigButton("Change Disc"))
    {
      m_show_discs = !m_show_discs;
      if (m_show_discs)
        SearchDiscs(context);
    }
    if (m_show_discs)
      DrawDiscs(context);

    ImGui::EndDisabled();

    // Exiting loses unsaved progress: ask for a second press.
    const u64 now_us = Common::Timer::NowUs();
    const bool confirming = now_us < m_confirm_exit_until_us;
    if (confirming)
    {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.2f, 0.15f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.2f, 1.0f));
    }
    const bool exit_pressed =
        BigButton(confirming ? "Press Again to Exit###exit" : "Exit Emulation###exit");
    if (confirming)
      ImGui::PopStyleColor(2);
    if (exit_pressed && !confirming)
    {
      m_confirm_exit_until_us = now_us + CONFIRM_TIMEOUT_US;
    }
    else if (exit_pressed)
    {
      m_confirm_exit_until_us = 0;
      context.actions.Run("Exiting...", [](Core::System&) {
        RequestStopWithoutDesktopConfirmation();
        return std::string();
      });
    }
  }

private:
  struct DiscSearch
  {
    std::mutex mutex;
    bool done = false;
    std::vector<Disc> discs;
  };

  // Records the left eye with the hardware encoder into Movies/DolphinXR (Android, Vulkan).
  static void DrawRecordButton(VRMenuContext& context)
  {
    if (!VideoRecorder::IsSupported())
      return;
    VideoRecorder& recorder = VideoRecorder::Get();
    if (std::optional<std::string> message = recorder.TakeFinishedMessage())
      context.actions.SetStatus(std::move(*message));

    switch (recorder.GetState())
    {
    case VideoRecorder::State::Idle:
    {
      const IOpenXRSwapchain* swapchain = g_openxr ? g_openxr->GetSwapchain() : nullptr;
      const bool can_record =
          !context.flat_screen && swapchain && swapchain->SupportsVideoCapture();
      ImGui::BeginDisabled(!can_record);
      if (BigButton("Record"))
      {
        context.actions.Run("Starting the recording...",
                            [game_id = context.game_id](Core::System&) {
                              return VideoRecorder::Get().Start(game_id);
                            });
      }
      ImGui::EndDisabled();
      if (!can_record && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Recording needs the Vulkan backend in stereo VR.");
      break;
    }
    case VideoRecorder::State::Recording:
    {
      const int seconds = static_cast<int>(recorder.GetElapsedSeconds());
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.2f, 0.15f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.2f, 1.0f));
      const std::string label =
          fmt::format("Stop Recording  {:02}:{:02}###record", seconds / 60, seconds % 60);
      if (BigButton(label.c_str()))
      {
        recorder.RequestStop();
        context.actions.SetStatus("Finishing the recording...");
      }
      ImGui::PopStyleColor(2);
      break;
    }
    case VideoRecorder::State::Stopping:
      ImGui::BeginDisabled();
      BigButton("Finishing the recording...###record");
      ImGui::EndDisabled();
      break;
    }
  }

  static float BigButtonWidth()
  {
    return std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 22.0f);
  }

  // Offset that centers a big button (and the disc list under it) in the page.
  static float CenteringOffset()
  {
    return std::max(0.0f, (ImGui::GetContentRegionAvail().x - BigButtonWidth()) * 0.5f);
  }

  static bool BigButton(const char* label)
  {
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + CenteringOffset());
    return ImGui::Button(label, ImVec2(BigButtonWidth(), ImGui::GetFrameHeight() * 1.6f));
  }

  static void CenteredText(const std::string& text, bool disabled)
  {
    const float available = ImGui::GetContentRegionAvail().x;
    const float width = ImGui::CalcTextSize(text.c_str()).x;
    if (width >= available)
    {
      ImGui::TextWrapped("%s", text.c_str());
      return;
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available - width) * 0.5f);
    if (disabled)
      ImGui::TextDisabled("%s", text.c_str());
    else
      ImGui::TextUnformatted(text.c_str());
  }

  // Host thread. The VR menu already asked for confirmation; the desktop's "Confirm on Stop"
  // dialog would open on the monitor, out of sight in the headset.
  static void RequestStopWithoutDesktopConfirmation()
  {
    const auto layer = Config::GetLayer(Config::LayerType::CurrentRun);
    const std::optional<bool> previous =
        layer->Get<bool>(Config::MAIN_CONFIRM_ON_STOP.GetLocation());
    Config::SetCurrent(Config::MAIN_CONFIRM_ON_STOP, false);
    Host_Message(HostMessageID::WMUserStop);
    if (previous)
      Config::SetCurrent(Config::MAIN_CONFIRM_ON_STOP, *previous);
    else
      Config::DeleteKey(Config::LayerType::CurrentRun, Config::MAIN_CONFIRM_ON_STOP);
  }

  void SearchDiscs(VRMenuContext& context)
  {
    m_disc_search = std::make_shared<DiscSearch>();
    context.actions.Run("Looking for the game's discs...", [search = m_disc_search](Core::System&) {
      std::vector<Disc> discs = FindDiscsOfRunningGame();
      std::lock_guard lock(search->mutex);
      search->discs = std::move(discs);
      search->done = true;
      return std::string();
    });
  }

  void DrawDiscs(VRMenuContext& context)
  {
    std::vector<Disc> discs;
    bool done = false;
    if (m_disc_search)
    {
      std::lock_guard lock(m_disc_search->mutex);
      done = m_disc_search->done;
      discs = m_disc_search->discs;
    }

    // Line the list up under the (centered) Change Disc button.
    const float indent = CenteringOffset() + ImGui::GetStyle().IndentSpacing;
    ImGui::Indent(indent);
    if (!done)
    {
      ImGui::TextDisabled("Searching the game folders...");
    }
    else if (discs.empty())
    {
      ImGui::TextWrapped("No disc of this game was found in the game folders.");
    }
    else
    {
      for (size_t i = 0; i < discs.size(); ++i)
      {
        const Disc& disc = discs[i];
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Button("Insert"))
        {
          context.actions.Run(
              fmt::format("Changing to disc {}...", disc.number + 1),
              [path = disc.path, number = disc.number](Core::System& system) {
                system.GetDVDInterface().ChangeDisc(Core::CPUThreadGuard{system}, path);
                return fmt::format("Inserted disc {}", number + 1);
              });
          m_show_discs = false;
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Disc %d   %s", disc.number + 1, disc.file_name.c_str());
        ImGui::PopID();
      }
    }
    ImGui::Unindent(indent);
    ImGui::Spacing();
  }

  std::string m_title;
  u64 m_confirm_exit_until_us = 0;
  bool m_show_discs = false;
  std::shared_ptr<DiscSearch> m_disc_search;
};
}  // namespace

std::unique_ptr<VRMenuTab> CreateMainTab()
{
  return std::make_unique<MainTab>();
}
}  // namespace VR
