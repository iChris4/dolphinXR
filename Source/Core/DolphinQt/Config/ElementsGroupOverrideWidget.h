// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QPointer>
#include <QWidget>

#include <optional>
#include <string>
#include <vector>

#include "VideoCommon/ElementsGroupManager.h"

class ElementsHuntingDialog;
class QListWidget;
class QListWidgetItem;
class QPushButton;

class ElementsGroupOverrideWidget : public QWidget
{
  Q_OBJECT
public:
  explicit ElementsGroupOverrideWidget(std::string game_id,
                                       std::optional<u16> revision = std::nullopt);
  ~ElementsGroupOverrideWidget() override;

private:
  void CreateWidgets();
  void ConnectWidgets();
  void UpdateList();
  void LoadOverrides();
  void SaveOverrides();
  std::vector<std::string> CollectAvailableFlags() const;

  void OnItemChanged(QListWidgetItem* item);
  void OnSelectionChanged();
  void OnContextMenuRequested();
  void SortAlphabetically();
  void SortEnabledCodesFirst();
  void SortDisabledCodesFirst();
  void OnListReordered();
  void OnAddClicked();
  void OnEditClicked();
  void OnRemoveClicked();
  void OnRefreshClicked();
  void OnReloadClicked();
  void OnHuntClicked();

  std::string m_game_id;
  std::optional<u16> m_revision;
  QListWidget* m_code_list = nullptr;
  QPushButton* m_code_add = nullptr;
  QPushButton* m_code_edit = nullptr;
  QPushButton* m_code_remove = nullptr;
  QPushButton* m_code_refresh = nullptr;
  QPushButton* m_code_reload = nullptr;
  QPushButton* m_elements_hunting = nullptr;
  QPointer<ElementsHuntingDialog> m_hunting_dialog;
  std::vector<ElementsGroupManager::ElementGroupOverride> m_overrides;
};
