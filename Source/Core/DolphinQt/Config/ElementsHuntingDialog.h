// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDialog>

#include <string>
#include <vector>

#include "Common/CommonTypes.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTimer;

class ElementsHuntingDialog : public QDialog
{
  Q_OBJECT
public:
  explicit ElementsHuntingDialog(std::string game_id, QWidget* parent = nullptr);
  ~ElementsHuntingDialog() override;

signals:
  void OverridesChanged();

private:
  void closeEvent(QCloseEvent* event) override;
  void CreateWidgets();
  void ConnectWidgets();
  void UpdateDisplay();
  void RequestRefresh();
  void RefreshPendingTextureSummary();
  void SaveCurrentOverride();
  void SaveCurrentHideObjectCode();
  void OnSeedSelectionChanged();
  void OnCurrentMatchSelectionChanged();
  void OnCurrentMatchItemChanged(QListWidgetItem* item);
  void ShowTextureBrowser();

  std::string m_game_id;
  QCheckBox* m_enable_check = nullptr;
  QComboBox* m_option_combo = nullptr;
  QComboBox* m_match_filter_mode_combo = nullptr;
  QComboBox* m_texture_mode_combo = nullptr;
  QListWidget* m_seed_list = nullptr;
  QLabel* m_seed_label = nullptr;
  QLabel* m_match_label = nullptr;
  QLabel* m_pending_texture_label = nullptr;
  QCheckBox* m_auto_refresh_check = nullptr;
  QCheckBox* m_projection_check = nullptr;
  QCheckBox* m_layer_check = nullptr;
  QCheckBox* m_viewport_check = nullptr;
  QCheckBox* m_scissor_check = nullptr;
  QCheckBox* m_render_state_check = nullptr;
  QPushButton* m_refresh_button = nullptr;
  QPushButton* m_prev_match_button = nullptr;
  QPushButton* m_next_match_button = nullptr;
  QPushButton* m_view_textures_button = nullptr;
  QPushButton* m_pick_button = nullptr;
  QLabel* m_pick_label = nullptr;
  // The last pick result shown (VR::DrawPick).
  u64 m_pick_serial = 0;
  QListWidget* m_current_match_list = nullptr;
  QPushButton* m_save_button = nullptr;
  QPushButton* m_save_hide_objects_button = nullptr;
  QTimer* m_update_timer = nullptr;
  bool m_refresh_pending = true;
  std::vector<u64> m_pending_texture_hashes;
};
