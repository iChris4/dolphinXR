// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/VRAlerts.h"

#include <mutex>
#include <string_view>

#include "Common/Logging/Log.h"
#include "VideoCommon/VR/OpenXRManager.h"

namespace VR
{
namespace
{
constexpr size_t MAX_ALERTS = 4;

std::mutex s_mutex;
std::vector<HeadsetAlert> s_alerts;
u64 s_next_id = 1;
}  // namespace

bool IsHeadsetAlert(Common::MsgType style)
{
  return style != Common::MsgType::Question;
}

bool ShowAlertInHeadset(const char* caption, const char* text, bool yes_no, Common::MsgType style)
{
  if (!IsHeadsetAlert(style) || !g_openxr || !g_openxr->IsSessionRunning())
    return false;
  QueueHeadsetAlert(caption ? caption : "", text ? text : "", style, yes_no);
  g_openxr->RequestMenuOpen(true);
  return true;
}

void QueueHeadsetAlert(std::string caption, std::string text, Common::MsgType style,
                       bool answered_yes)
{
  const std::string_view answer = answered_yes ? " (answered yes)" : "";
  if (style == Common::MsgType::Warning)
    WARN_LOG_FMT(OPENXR, "Alert shown in the VR menu{}: {}: {}", answer, caption, text);
  else
    ERROR_LOG_FMT(OPENXR, "Alert shown in the VR menu{}: {}: {}", answer, caption, text);

  std::lock_guard lock(s_mutex);
  if (s_alerts.size() == MAX_ALERTS)
    s_alerts.erase(s_alerts.begin());
  s_alerts.push_back({s_next_id++, std::move(caption), std::move(text), style, answered_yes});
}

std::vector<HeadsetAlert> GetHeadsetAlerts()
{
  std::lock_guard lock(s_mutex);
  return s_alerts;
}

void DismissHeadsetAlert(u64 id)
{
  std::lock_guard lock(s_mutex);
  std::erase_if(s_alerts, [id](const HeadsetAlert& alert) { return alert.id == id; });
}

void DismissAllHeadsetAlerts()
{
  std::lock_guard lock(s_mutex);
  s_alerts.clear();
}
}  // namespace VR
