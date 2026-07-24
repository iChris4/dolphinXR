// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDialog>

#include <optional>
#include <vector>

#include "VideoCommon/HideObjectEngine.h"

class QComboBox;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSlider;

class HideObjectAddEditDialog : public QDialog
{
  Q_OBJECT
public:
  // Pass nullptr for new code, or pointer to existing code for editing.
  // parent_codes is the full list (for name uniqueness check).
  explicit HideObjectAddEditDialog(QWidget* parent,
                                   const HideObjectEngine::HideObject* existing_code,
                                   const std::vector<HideObjectEngine::HideObject>& all_codes);

  HideObjectEngine::HideObject GetResult() const { return m_result; }

private:
  void CreateWidgets();
  void ConnectWidgets();

  void OnTypeChanged();
  void OnUpClicked();
  void OnDownClicked();
  void OnValueTextChanged();
  void OnValueSliderChanged(int value);
  void OnEntrySelectionChanged();
  void OnEntryCheckStateChanged(QListWidgetItem* item);
  void OnAddEntryClicked();
  void OnRemoveEntryClicked();
  void OnAccept();

  void UpdateEntryList();
  void UpdateCurrentEntryListItem();
  void UpdateValueDisplay();
  void UpdateValueSliderFromText();
  bool ParseValueFromUI();
  void StoreCurrentEntry();
  void ApplyTemporarily();

  bool m_is_edit = false;
  std::string m_original_name;  // For edit mode: original name to exclude from uniqueness check

  HideObjectEngine::HideObject m_result;
  HideObjectEngine::HideObjectEntry m_current_entry;
  std::vector<bool> m_entry_enabled;
  size_t m_current_entry_index = 0;
  std::optional<size_t> m_existing_code_index;

  const std::vector<HideObjectEngine::HideObject>& m_all_codes;

  QLineEdit* m_name_edit;
  QListWidget* m_entry_list;
  QPushButton* m_entry_add;
  QPushButton* m_entry_remove;
  QComboBox* m_type_combo;
  QLineEdit* m_value_edit;
  QSlider* m_value_slider;
  QPushButton* m_up_button;
  QPushButton* m_down_button;
};
