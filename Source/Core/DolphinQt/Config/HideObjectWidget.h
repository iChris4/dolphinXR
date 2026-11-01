// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QWidget>

#include <optional>
#include <string>
#include <vector>

#include "VideoCommon/HideObjectEngine.h"

class QListWidget;
class QListWidgetItem;
class QPushButton;

class HideObjectWidget : public QWidget
{
  Q_OBJECT
public:
  explicit HideObjectWidget(std::string game_id, std::optional<u16> revision = std::nullopt);
  ~HideObjectWidget() override;

protected:
  void showEvent(QShowEvent* event) override;

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

  std::string m_game_id;
  std::optional<u16> m_revision;

  QListWidget* m_code_list;
  QPushButton* m_code_add;
  QPushButton* m_code_edit;
  QPushButton* m_code_remove;
  QPushButton* m_code_refresh;
  QPushButton* m_code_reload;

  std::vector<HideObjectEngine::HideObject> m_codes;
};
