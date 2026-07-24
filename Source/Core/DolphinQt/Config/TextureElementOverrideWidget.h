// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <QWidget>

#include "VideoCommon/TextureElementManager.h"

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QShowEvent;

// Per-game Properties pane for Texture Element Overrides: reclassify VR draws based purely on the
// bound texture hash, across all shaders/elements. Modeled on ShaderOverrideWidget.
class TextureElementOverrideWidget : public QWidget
{
  Q_OBJECT
public:
  explicit TextureElementOverrideWidget(std::string game_id,
                                        std::optional<u16> revision = std::nullopt);
  ~TextureElementOverrideWidget() override;

protected:
  void showEvent(QShowEvent* event) override;

private:
  void CreateWidgets();
  void ConnectWidgets();
  void UpdateList();
  void LoadOverrides();
  void SaveOverrides();
  void ReloadRuntime();
  std::vector<std::string> CollectAvailableFlags() const;
  void UpdateDumpWarning();
  bool HasTextureDumps() const;

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

  std::string m_game_id;
  std::optional<u16> m_revision;

  QWidget* m_dump_warning;
  QLabel* m_dump_warning_text;
  QListWidget* m_code_list;
  QPushButton* m_code_add;
  QPushButton* m_code_edit;
  QPushButton* m_code_remove;
  QPushButton* m_code_refresh;
  QPushButton* m_code_reload;

  std::vector<TextureElementManager::TextureElementOverride> m_overrides;
};
