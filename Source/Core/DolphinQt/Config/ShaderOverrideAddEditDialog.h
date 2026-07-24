// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDialog>

#include "VideoCommon/ShaderHunter.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QString;
class QSpinBox;
class QVBoxLayout;
class QWidget;

class ShaderOverrideAddEditDialog : public QDialog
{
  Q_OBJECT
public:
  // edit_override != nullptr → edit mode (pre-fills fields); nullptr → add mode.
  explicit ShaderOverrideAddEditDialog(
      QWidget* parent, const ShaderHunter::ShaderOverride* edit_override = nullptr,
      const std::vector<std::string>& available_flags = {});

  ShaderHunter::ShaderOverride GetResult() const;

private:
  std::vector<std::string> CollectTextureHashTokens() const;
  std::vector<u64> CollectTextureHashValues() const;
  void SetTextureHashFields(const std::vector<u64>& hashes);
  void AddTextureHashField(const QString& text);
  void EnsureTextureHashFieldRows();
  void SanitizeTextureHashField(QLineEdit* edit);
  void ShowTextureBrowser();

  void OnAccept();
  void OnHandlingChanged();

  QLineEdit* m_name_edit;
  QPlainTextEdit* m_comments_edit;
  QLineEdit* m_credits_edit;
  QLineEdit* m_hash_edit;
  QComboBox* m_type_combo;
  QComboBox* m_match_mode_combo;
  QLabel* m_match_mode_label;
  QComboBox* m_handling_combo;
  QDoubleSpinBox* m_element_depth_spin;
  QLabel* m_element_depth_label;
  QDoubleSpinBox* m_units_per_meter_spin;
  QLabel* m_units_per_meter_label;
  QDoubleSpinBox* m_passthrough_opacity_spin;
  QLabel* m_passthrough_opacity_label;
  QLabel* m_anchor_right_label;
  QDoubleSpinBox* m_anchor_right_spin;
  QLabel* m_anchor_up_label;
  QDoubleSpinBox* m_anchor_up_spin;
  QLabel* m_anchor_forward_label;
  QDoubleSpinBox* m_anchor_forward_spin;
  QLabel* m_anchor_rotation_label;
  QComboBox* m_anchor_rotation_combo;
  QLabel* m_anchor_yaw_label;
  QDoubleSpinBox* m_anchor_yaw_spin;
  QLabel* m_anchor_upm_label;
  QDoubleSpinBox* m_anchor_upm_spin;
  QCheckBox* m_anchor_hide_check;
  QLabel* m_anchor_hand_label = nullptr;
  QComboBox* m_anchor_hand_combo = nullptr;
  QCheckBox* m_anchor_follow_rotation_check = nullptr;
  QLabel* m_anchor_ctrl_yaw_label = nullptr;
  QDoubleSpinBox* m_anchor_ctrl_yaw_spin = nullptr;
  QLabel* m_anchor_ctrl_pitch_label = nullptr;
  QDoubleSpinBox* m_anchor_ctrl_pitch_spin = nullptr;
  QLabel* m_anchor_ctrl_roll_label = nullptr;
  QDoubleSpinBox* m_anchor_ctrl_roll_spin = nullptr;
  QLineEdit* m_flag_edit;
  QLabel* m_flag_label;
  QComboBox* m_condition_combo;
  QLabel* m_condition_label;
  QComboBox* m_condition_mode_combo;
  QLabel* m_condition_mode_label;
  QCheckBox* m_hash_family_check;
  QPushButton* m_view_textures_button;
  QComboBox* m_texture_mode_combo;
  QLabel* m_texture_mode_label;
  QWidget* m_texture_hash_container;
  QScrollArea* m_texture_hash_scroll;
  QVBoxLayout* m_texture_hash_layout;
  std::vector<QLineEdit*> m_texture_hash_edits;
  bool m_updating_texture_hash_fields = false;
  u64 m_edit_family_signature = 0;
  u32 m_edit_family_version = ShaderHunter::FAMILY_SCHEME_VERSION;
  u64 m_edit_original_hash = 0;
  ShaderHunter::ShaderType m_edit_original_type = ShaderHunter::ShaderType::Pixel;
};
