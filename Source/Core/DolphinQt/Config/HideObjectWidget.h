// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QWidget>

#include <optional>
#include <string>
#include <vector>

#include "VideoCommon/HideObjectEngine.h"

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTimer;

class HideObjectWidget : public QWidget
{
  Q_OBJECT
public:
  explicit HideObjectWidget(std::string game_id, std::optional<u16> revision = std::nullopt);
  ~HideObjectWidget() override;

private:
  void CreateWidgets();
  void ConnectWidgets();
  void UpdateList();
  void LoadCodes();
  void SaveCodes();

  void OnItemChanged(QListWidgetItem* item);
  void OnSelectionChanged();
  void OnAddClicked();
  void OnEditClicked();
  void OnRemoveClicked();
  void OnRefreshClicked();
  void OnReloadClicked();
  void OnPickClicked();
  void CheckPickResult();

  std::string m_game_id;
  std::optional<u16> m_revision;

  QListWidget* m_code_list;
  QPushButton* m_code_add;
  QPushButton* m_code_edit;
  QPushButton* m_code_remove;
  QPushButton* m_code_refresh;
  QPushButton* m_code_reload;
  QPushButton* m_code_pick = nullptr;
  QLabel* m_pick_label = nullptr;
  QTimer* m_pick_timer = nullptr;
  u64 m_pick_serial = 0;

  std::vector<HideObjectEngine::HideObject> m_codes;
};
