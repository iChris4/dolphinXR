// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <QWidget>

#include "Common/CommonTypes.h"
#include "Core/VRPatch/VRPatch.h"

class QFormLayout;
class QGroupBox;
class QLabel;
class QTableWidget;
class QTimer;

// The VR Patches pane of a game's properties: the game's VR patches (Core/VRPatch), on or off, with
// their status and settings. Changes are saved in the user's GameSettingsVR/<GameID>.ini and reach
// a running game at once.
class VRPatchesWidget final : public QWidget
{
  Q_OBJECT

public:
  VRPatchesWidget(std::string game_id, std::optional<u16> revision, QWidget* parent = nullptr);

private:
  void CreateWidgets();
  // Lists the patches again; `rebuild_settings` also rebuilds the selected patch's settings form.
  void Refresh(bool rebuild_settings);
  void UpdateStatus();
  void BuildSettings();
  void OnItemChanged(int row, int column);
  void SetSetting(const std::string& id, const std::string& name, double value);
  void ResetSettings();
  void OpenFolder();
  void Import();
  std::optional<VRPatch::PatchInfo> SelectedPatch() const;

  const std::string m_game_id;
  const std::optional<u16> m_revision;
  std::vector<VRPatch::PatchInfo> m_patches;
  std::string m_settings_for;
  bool m_updating = false;

  QTableWidget* m_table = nullptr;
  QLabel* m_details = nullptr;
  QGroupBox* m_settings_group = nullptr;
  QFormLayout* m_settings_form = nullptr;
  QWidget* m_settings_widget = nullptr;
  QTimer* m_timer = nullptr;
  std::map<std::string, QTimer*> m_save_timers;
  std::map<std::string, double> m_pending_values;
};
