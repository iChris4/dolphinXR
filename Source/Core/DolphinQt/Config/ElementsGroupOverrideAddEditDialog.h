// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDialog>

#include <vector>

#include "VideoCommon/ElementsGroupManager.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QListWidget;
class QScrollArea;
class QSpinBox;
class QVBoxLayout;
class QWidget;

class ElementsGroupOverrideAddEditDialog : public QDialog
{
  Q_OBJECT
public:
  explicit ElementsGroupOverrideAddEditDialog(
      QWidget* parent,
      const ElementsGroupManager::ElementGroupOverride* edit_override = nullptr,
      const std::vector<std::string>& available_flags = {});

  ElementsGroupManager::ElementGroupOverride GetResult() const;

private:
  std::vector<u64> CollectTextureHashValues() const;
  void SetTextureHashValues(const std::vector<u64>& hashes);
  void AddTextureHashField(const QString& text = {});
  void EnsureTextureHashFieldRows();
  void SanitizeTextureHashField(QLineEdit* edit);
  void ShowTextureBrowser();
  void RefreshRuntimeElementSummary();
  void RefreshHandlingUi();
  void RefreshMatchKindUi();
  void RefreshClearEFBUi();
  std::vector<MetroidElementLayer> CollectProfileLayers() const;
  void SetProfileLayers(const std::vector<MetroidElementLayer>& layers);
  void CaptureCurrentSeed();
  void AddCurrentHuntMatch();
  void RemoveSelectedMatchFilter();
  void OnAccept();

  std::vector<std::string> m_available_flags;
  bool m_updating_texture_hash_fields = false;

  QLineEdit* m_name_edit = nullptr;
  QPlainTextEdit* m_comments_edit = nullptr;
  QLineEdit* m_credits_edit = nullptr;
  QComboBox* m_match_kind_combo = nullptr;
  QLabel* m_profile_label = nullptr;
  QComboBox* m_profile_combo = nullptr;
  QLabel* m_profile_layers_label = nullptr;
  QListWidget* m_profile_layers_list = nullptr;
  QComboBox* m_handling_combo = nullptr;
  QCheckBox* m_preserve_stereo_efb_check = nullptr;
  QLabel* m_screen_pane_depth_label = nullptr;
  QComboBox* m_screen_pane_depth_combo = nullptr;
  QDoubleSpinBox* m_element_depth_spin = nullptr;
  QLabel* m_element_depth_label = nullptr;
  QDoubleSpinBox* m_units_per_meter_spin = nullptr;
  QLabel* m_units_per_meter_label = nullptr;
  QDoubleSpinBox* m_passthrough_opacity_spin = nullptr;
  QLabel* m_passthrough_opacity_label = nullptr;
  QLabel* m_anchor_right_label = nullptr;
  QDoubleSpinBox* m_anchor_right_spin = nullptr;
  QLabel* m_anchor_up_label = nullptr;
  QDoubleSpinBox* m_anchor_up_spin = nullptr;
  QLabel* m_anchor_forward_label = nullptr;
  QDoubleSpinBox* m_anchor_forward_spin = nullptr;
  QLabel* m_anchor_rotation_label = nullptr;
  QComboBox* m_anchor_rotation_combo = nullptr;
  QLabel* m_anchor_yaw_label = nullptr;
  QDoubleSpinBox* m_anchor_yaw_spin = nullptr;
  QLabel* m_anchor_upm_label = nullptr;
  QDoubleSpinBox* m_anchor_upm_spin = nullptr;
  QCheckBox* m_anchor_hide_check = nullptr;
  QLabel* m_anchor_hand_label = nullptr;
  QComboBox* m_anchor_hand_combo = nullptr;
  QCheckBox* m_anchor_follow_rotation_check = nullptr;
  QLabel* m_anchor_ctrl_yaw_label = nullptr;
  QDoubleSpinBox* m_anchor_ctrl_yaw_spin = nullptr;
  QLabel* m_anchor_ctrl_pitch_label = nullptr;
  QDoubleSpinBox* m_anchor_ctrl_pitch_spin = nullptr;
  QLabel* m_anchor_ctrl_roll_label = nullptr;
  QDoubleSpinBox* m_anchor_ctrl_roll_spin = nullptr;
  QLineEdit* m_flag_edit = nullptr;
  QLabel* m_flag_label = nullptr;
  QComboBox* m_condition_combo = nullptr;
  QLabel* m_condition_label = nullptr;
  QComboBox* m_condition_mode_combo = nullptr;
  QLabel* m_condition_mode_label = nullptr;
  QPushButton* m_capture_seed_button = nullptr;
  QLabel* m_runtime_element_summary_label = nullptr;
  QCheckBox* m_runtime_use_projection_check = nullptr;
  QCheckBox* m_runtime_use_projection_type_check = nullptr;
  QCheckBox* m_runtime_use_layer_check = nullptr;
  QCheckBox* m_runtime_use_viewport_check = nullptr;
  QCheckBox* m_runtime_use_scissor_check = nullptr;
  QCheckBox* m_runtime_use_render_state_check = nullptr;
  QCheckBox* m_clear_efb_check = nullptr;
  QLabel* m_clear_efb_min_label = nullptr;
  QSpinBox* m_clear_efb_min_spin = nullptr;
  QLabel* m_clear_efb_max_label = nullptr;
  QSpinBox* m_clear_efb_max_spin = nullptr;
  QComboBox* m_texture_mode_combo = nullptr;
  QComboBox* m_selected_match_mode_combo = nullptr;
  QPushButton* m_view_textures_button = nullptr;
  QWidget* m_texture_hash_container = nullptr;
  QScrollArea* m_texture_hash_scroll = nullptr;
  QVBoxLayout* m_texture_hash_layout = nullptr;
  std::vector<QLineEdit*> m_texture_hash_edits;
  QListWidget* m_selected_match_list = nullptr;
  QPushButton* m_add_current_match_button = nullptr;
  QPushButton* m_remove_selected_match_button = nullptr;

  ElementsGroupManager::RuntimeElementSignature m_runtime_element;
  std::vector<ElementsGroupManager::SelectedSubgroupSignature> m_selected_match_filters;
  bool m_selected_match_filters_excluded = false;
};
