// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "VideoCommon/VR/VRAlerts.h"

TEST(VRAlerts, OnlyTakesAlertsDuringASession)
{
  // No OpenXR session: the platform dialog shows the alert.
  EXPECT_FALSE(VR::ShowAlertInHeadset("Warning", "text", false, Common::MsgType::Warning));
  EXPECT_FALSE(VR::ShowAlertInHeadset("Question", "text?", true, Common::MsgType::Question));
}

TEST(VRAlerts, AnswersWarningsButNotQuestions)
{
  // PanicYesNo and ASSERT_MSG are warnings asking "Ignore and continue?": the headset answers yes.
  EXPECT_TRUE(VR::IsHeadsetAlert(Common::MsgType::Warning));
  EXPECT_TRUE(VR::IsHeadsetAlert(Common::MsgType::Information));
  EXPECT_TRUE(VR::IsHeadsetAlert(Common::MsgType::Critical));
  EXPECT_FALSE(VR::IsHeadsetAlert(Common::MsgType::Question));

  VR::DismissAllHeadsetAlerts();
  VR::QueueHeadsetAlert("Warning", "Ignore and continue?", Common::MsgType::Warning, true);
  const std::vector<VR::HeadsetAlert> alerts = VR::GetHeadsetAlerts();
  ASSERT_EQ(alerts.size(), 1u);
  EXPECT_TRUE(alerts[0].answered_yes);
  VR::DismissAllHeadsetAlerts();
}

TEST(VRAlerts, KeepsTheMostRecentAndDismisses)
{
  VR::DismissAllHeadsetAlerts();
  for (int i = 0; i < 6; ++i)
    VR::QueueHeadsetAlert("Warning", "alert " + std::to_string(i), Common::MsgType::Warning);

  std::vector<VR::HeadsetAlert> alerts = VR::GetHeadsetAlerts();
  ASSERT_EQ(alerts.size(), 4u);
  EXPECT_EQ(alerts.front().text, "alert 2");
  EXPECT_EQ(alerts.back().text, "alert 5");
  EXPECT_LT(alerts.front().id, alerts.back().id);

  VR::DismissHeadsetAlert(alerts[1].id);
  alerts = VR::GetHeadsetAlerts();
  ASSERT_EQ(alerts.size(), 3u);
  EXPECT_EQ(alerts[1].text, "alert 4");

  VR::DismissAllHeadsetAlerts();
  EXPECT_TRUE(VR::GetHeadsetAlerts().empty());
}
