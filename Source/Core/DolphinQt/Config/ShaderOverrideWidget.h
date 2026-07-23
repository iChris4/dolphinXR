// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QPointer>
#include <QWidget>

#include <optional>
#include <string>
#include <vector>

#include "VideoCommon/ShaderHunter.h"

class QListWidget;
class QListWidgetItem;
class QPushButton;
class ShaderHunterWidget;

class ShaderOverrideWidget : public QWidget
{
  Q_OBJECT
public:
  explicit ShaderOverrideWidget(std::string game_id, std::optional<u16> revision = std::nullopt);
  ~ShaderOverrideWidget() override;

private:
  void CreateWidgets();
  void ConnectWidgets();
  void UpdateList();
  void LoadOverrides();
  void SaveOverrides();

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
  std::vector<std::string> CollectAvailableFlags() const;

  std::string m_game_id;
  std::optional<u16> m_revision;

  QListWidget* m_code_list;
  QPushButton* m_code_add;
  QPushButton* m_code_edit;
  QPushButton* m_code_remove;
  QPushButton* m_code_refresh;
  QPushButton* m_code_reload;
  QPushButton* m_shader_hunter;
  QPointer<ShaderHunterWidget> m_shader_hunter_widget;

  std::vector<ShaderHunter::ShaderOverride> m_overrides;
};
