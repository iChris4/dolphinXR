// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <QDialog>
#include <QStringList>

#include "VideoCommon/TextureElementManager.h"

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

// Add/Edit dialog for a single Texture Element Override (a named group of texture hashes that
// share one handling). Modeled on ShaderOverrideAddEditDialog, minus the shader-specific fields.
class TextureElementOverrideAddEditDialog : public QDialog
{
  Q_OBJECT
public:
  // edit_override != nullptr → edit mode (pre-fills fields); nullptr → add mode.
  // game_id is used to locate the game's texture dump directory for the Import Texture button.
  explicit TextureElementOverrideAddEditDialog(
      QWidget* parent, std::string game_id,
      const TextureElementManager::TextureElementOverride* edit_override = nullptr,
      const std::vector<std::string>& available_flags = {});

  TextureElementManager::TextureElementOverride GetResult() const;

private:
  std::vector<std::string> CollectTextureHashTokens() const;
  std::vector<u64> CollectTextureHashValues() const;
  void SetTextureHashFields(const std::vector<u64>& hashes);
  void AddTextureHashField(const QString& text);
  void EnsureTextureHashFieldRows();
  void SanitizeTextureHashField(QLineEdit* edit);
  void ShowTextureBrowser();
  void OnImportTextures();
  void ImportTextureFiles(const QStringList& files);
  void EnsureDumpIndex();
  void UpdateTextureHashPreview(QLineEdit* edit, QLabel* preview);

  void OnAccept();
  void OnHandlingChanged();

  QLineEdit* m_name_edit;
  QPlainTextEdit* m_comments_edit;
  QComboBox* m_handling_combo;
  QLabel* m_flag_label;
  QLineEdit* m_flag_edit;
  QLabel* m_condition_label;
  QComboBox* m_condition_combo;
  QLabel* m_condition_mode_label;
  QComboBox* m_condition_mode_combo;
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
  QPushButton* m_view_textures_button;
  QPushButton* m_import_textures_button;
  QWidget* m_texture_hash_container;
  QScrollArea* m_texture_hash_scroll;
  QVBoxLayout* m_texture_hash_layout;
  std::vector<QLineEdit*> m_texture_hash_edits;
  std::vector<QLabel*> m_texture_hash_previews;
  std::vector<QWidget*> m_texture_hash_rows;
  bool m_updating_texture_hash_fields = false;
  std::string m_game_id;

  // Lazily-built index of the game's dumped textures: hash -> absolute file path, used to show a
  // thumbnail beside each hash field.
  std::unordered_map<u64, std::string> m_dump_hash_to_path;
  bool m_dump_index_built = false;
};
