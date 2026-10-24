// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"

namespace VR
{
// Dolphin's alert messages during an OpenXR session. The desktop and Android dialogs are not
// visible in the headset, and they block the thread that raised them until someone answers: a
// warning raised on the video thread left the game black until the app was killed (Metroid
// Prime 3's first boot on the Quest, a dual core FIFO desync while the shader cache was rebuilt:
// an "Unknown Opcode" warning, or an ASSERT_MSG "Ignore and continue?").
// While a session runs, the platform handlers hand every alert but the questions to this module:
// they are logged and shown in the VR menu, which opens, and the emulation goes on as if OK had
// been pressed. A warning that asks "Ignore and continue?" (PanicYesNo, ASSERT_MSG) is answered
// yes, as Dolphin does when alerts are disabled. Questions (AskYesNo) still use the dialogs.
struct HeadsetAlert
{
  u64 id = 0;
  std::string caption;
  std::string text;
  Common::MsgType style = Common::MsgType::Information;
  bool answered_yes = false;
};

// Whether an alert of this style raised during a session is shown in the headset.
bool IsHeadsetAlert(Common::MsgType style);

// Any thread. True when the alert was taken: a session runs and IsHeadsetAlert(style). The
// caller then returns true, as if the user had pressed OK or Yes.
bool ShowAlertInHeadset(const char* caption, const char* text, bool yes_no, Common::MsgType style);

// Queues an alert for the VR menu (the most recent ones are kept). Any thread.
void QueueHeadsetAlert(std::string caption, std::string text, Common::MsgType style,
                       bool answered_yes = false);
std::vector<HeadsetAlert> GetHeadsetAlerts();
void DismissHeadsetAlert(u64 id);
void DismissAllHeadsetAlerts();
}  // namespace VR
