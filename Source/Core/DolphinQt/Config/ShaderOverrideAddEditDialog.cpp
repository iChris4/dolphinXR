// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/ShaderOverrideAddEditDialog.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include <QCheckBox>
#include <QComboBox>
#include <QBrush>
#include <QColor>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QWidget>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigManager.h"
#include "DolphinQt/Config/TextureHashBrowserDialog.h"

namespace
{
constexpr double UPM_OVERRIDE_MIN = 0.01;
constexpr double UPM_OVERRIDE_MAX = 1000.0;
constexpr double UPM_OVERRIDE_STEP = 0.01;
constexpr int MAX_VISIBLE_TEXTURE_HASH_ROWS = 10;
constexpr int TEXTURE_HASH_ROW_HEIGHT = 28;

}  // namespace

ShaderOverrideAddEditDialog::ShaderOverrideAddEditDialog(
    QWidget* parent, const ShaderHunter::ShaderOverride* edit_override,
    const std::vector<std::string>& available_flags)
    : QDialog(parent)
{
  setWindowTitle(edit_override ? tr("Edit Shader Override") : tr("Add Shader Override"));
  setMinimumWidth(350);

  m_name_edit = new QLineEdit;
  m_name_edit->setPlaceholderText(tr("Override name..."));

  m_comments_edit = new QPlainTextEdit;
  m_comments_edit->setPlaceholderText(tr("Optional notes..."));
  m_comments_edit->setTabChangesFocus(true);
  m_comments_edit->setMinimumHeight(70);

  m_credits_edit = new QLineEdit;
  m_credits_edit->setPlaceholderText(tr("Optional author/credits..."));

  m_hash_edit = new QLineEdit;
  m_hash_edit->setPlaceholderText(tr("Hex hash (e.g. 0012abcd)"));

  m_type_combo = new QComboBox;
  m_type_combo->addItem(tr("Pixel Shader"), static_cast<int>(ShaderHunter::ShaderType::Pixel));
  m_type_combo->addItem(tr("Vertex Shader"), static_cast<int>(ShaderHunter::ShaderType::Vertex));
  m_type_combo->addItem(tr("Geometry Shader"),
                         static_cast<int>(ShaderHunter::ShaderType::Geometry));

  m_match_mode_label = new QLabel(tr("Match Mode:"));
  m_match_mode_combo = new QComboBox;
  m_match_mode_combo->addItem(tr("Exact Hash"),
                              static_cast<int>(ShaderHunter::MatchMode::ExactHash));
  m_match_mode_combo->addItem(tr("Shader Family"),
                              static_cast<int>(ShaderHunter::MatchMode::ShaderFamily));
  m_match_mode_combo->setToolTip(
      tr("Exact Hash: match only this exact shader hash.\n"
         "Shader Family: use the relaxed shader family signature."));

  m_handling_combo = new QComboBox;
  m_handling_combo->addItem(tr("Skip"), static_cast<int>(ShaderHunter::HandlingType::Skip));
  m_handling_combo->addItem(tr("Screen"), static_cast<int>(ShaderHunter::HandlingType::Screen));
  m_handling_combo->addItem(tr("Fullscreen"),
                             static_cast<int>(ShaderHunter::HandlingType::Fullscreen));
  m_handling_combo->addItem(tr("Head Locked"),
                             static_cast<int>(ShaderHunter::HandlingType::HeadLocked));
  m_handling_combo->addItem(tr("Flag"), static_cast<int>(ShaderHunter::HandlingType::Flag));
  m_handling_combo->addItem(tr("Units per Meter"),
                            static_cast<int>(ShaderHunter::HandlingType::UnitsPerMeter));
  m_handling_combo->addItem(tr("Passthrough"),
                            static_cast<int>(ShaderHunter::HandlingType::Passthrough));
  m_handling_combo->addItem(tr("Camera Anchor"),
                            static_cast<int>(ShaderHunter::HandlingType::CameraAnchor));
  m_handling_combo->addItem(tr("Controller Anchor"),
                            static_cast<int>(ShaderHunter::HandlingType::ControllerAnchor));

  m_element_depth_label = new QLabel(tr("Element Depth:"));
  m_element_depth_spin = new QDoubleSpinBox;
  m_element_depth_spin->setRange(-1.0, 0.01);
  m_element_depth_spin->setDecimals(4);
  m_element_depth_spin->setSingleStep(0.0001);
  m_element_depth_spin->setSpecialValueText(tr("Default"));
  m_element_depth_spin->setValue(-1.0);
  m_element_depth_spin->setToolTip(tr("Within-element depth range for this shader (legacy fallback "
                                       "when Exact Screen Depth is off).\n"
                                       "-1 (Default) = use the built-in default.\n"
                                       "Higher values fix Z-fighting inside the element."));

  m_units_per_meter_label = new QLabel(tr("Units per Meter:"));
  m_units_per_meter_spin = new QDoubleSpinBox;
  m_units_per_meter_spin->setRange(UPM_OVERRIDE_MIN, UPM_OVERRIDE_MAX);
  m_units_per_meter_spin->setDecimals(2);
  m_units_per_meter_spin->setSingleStep(UPM_OVERRIDE_STEP);
  m_units_per_meter_spin->setValue(1.0);
  m_units_per_meter_spin->setToolTip(tr("Temporary per-shader scale override for VR.\n"
                                         "Higher values make this shader appear larger."));

  m_passthrough_opacity_label = new QLabel(tr("Opacity:"));
  m_passthrough_opacity_spin = new QDoubleSpinBox;
  m_passthrough_opacity_spin->setRange(0.0, 1.0);
  m_passthrough_opacity_spin->setDecimals(2);
  m_passthrough_opacity_spin->setSingleStep(0.05);
  m_passthrough_opacity_spin->setValue(0.0);
  m_passthrough_opacity_spin->setToolTip(
      tr("How opaque this element stays over the headset camera feed.\n"
         "0.00 = fully see-through (pure passthrough window).\n"
         "1.00 = fully opaque (no passthrough).\n"
         "Requires the VR Passthrough setting to be enabled."));

  const auto make_anchor_offset_spin = []() {
    auto* spin = new QDoubleSpinBox;
    spin->setRange(-10.0, 10.0);
    spin->setDecimals(2);
    spin->setSingleStep(0.05);
    spin->setValue(0.0);
    spin->setSuffix(QStringLiteral(" m"));
    return spin;
  };
  m_anchor_right_label = new QLabel(tr("Anchor Right:"));
  m_anchor_right_spin = make_anchor_offset_spin();
  m_anchor_right_spin->setToolTip(
      tr("Sideways offset of the VR camera from the anchor element's origin, in meters.\n"
         "Positive = right in camera space."));
  m_anchor_up_label = new QLabel(tr("Anchor Up:"));
  m_anchor_up_spin = make_anchor_offset_spin();
  m_anchor_up_spin->setToolTip(
      tr("Height offset of the VR camera above the anchor element's origin, in meters.\n"
         "Use this when the element's matrix sits at the character's root instead of the head."));
  m_anchor_forward_label = new QLabel(tr("Anchor Forward:"));
  m_anchor_forward_spin = make_anchor_offset_spin();
  m_anchor_forward_spin->setToolTip(
      tr("Forward offset of the VR camera from the anchor element's origin, in meters.\n"
         "Positive = further ahead in the game camera's view direction."));
  m_anchor_rotation_label = new QLabel(tr("Anchor Rotation:"));
  m_anchor_rotation_combo = new QComboBox;
  m_anchor_rotation_combo->addItem(tr("Off"),
                                   static_cast<int>(ShaderHunter::AnchorRotationMode::Off));
  m_anchor_rotation_combo->addItem(tr("Yaw Only"),
                                   static_cast<int>(ShaderHunter::AnchorRotationMode::YawOnly));
  m_anchor_rotation_combo->addItem(tr("Full"),
                                   static_cast<int>(ShaderHunter::AnchorRotationMode::Full));
  m_anchor_rotation_combo->setToolTip(
      tr("Rotate the VR camera with the anchor element.\n"
         "Off = camera keeps the game camera's orientation (position only).\n"
         "Yaw Only = follow the element's heading but keep the horizon level (comfortable).\n"
         "Full = follow the complete orientation including pitch and roll (intense).\n"
         "Head tracking still works on top in every mode."));
  m_anchor_yaw_label = new QLabel(tr("Rotation Yaw Offset:"));
  m_anchor_yaw_spin = new QDoubleSpinBox;
  m_anchor_yaw_spin->setRange(-180.0, 180.0);
  m_anchor_yaw_spin->setDecimals(0);
  m_anchor_yaw_spin->setSingleStep(15.0);
  m_anchor_yaw_spin->setValue(0.0);
  m_anchor_yaw_spin->setSuffix(QStringLiteral("°"));
  m_anchor_yaw_spin->setToolTip(
      tr("Fixed heading correction for the rotation modes. Models differ in which way\n"
         "they face: if the anchored view comes up looking backward use 180, if it looks\n"
         "sideways use 90 or -90."));
  m_anchor_upm_label = new QLabel(tr("Anchor Units per Meter:"));
  m_anchor_upm_spin = new QDoubleSpinBox;
  m_anchor_upm_spin->setRange(0.0, UPM_OVERRIDE_MAX);
  m_anchor_upm_spin->setDecimals(2);
  m_anchor_upm_spin->setSingleStep(UPM_OVERRIDE_STEP);
  m_anchor_upm_spin->setSpecialValueText(tr("Default"));
  m_anchor_upm_spin->setValue(0.0);
  m_anchor_upm_spin->setToolTip(
      tr("World scale to use while this anchor is active, so a first-person view can have a\n"
         "different scale than the game's global Units per Meter setting, which is left\n"
         "untouched. Default = keep the global value."));
  m_anchor_hide_check = new QCheckBox(tr("Hide Anchor Element"));
  m_anchor_hide_check->setChecked(true);
  m_anchor_hide_check->setToolTip(
      tr("Skip drawing the anchor element itself so its mesh (e.g. the character's head)\n"
         "does not block the first-person view. Other body parts need their own Skip\n"
         "overrides. Disabled while the Camera Anchor toggle in the VR pane is off."));
  m_anchor_hand_label = new QLabel(tr("Controller:"));
  m_anchor_hand_combo = new QComboBox;
  m_anchor_hand_combo->addItem(tr("Right"), 1);
  m_anchor_hand_combo->addItem(tr("Left"), 0);
  m_anchor_hand_combo->setToolTip(
      tr("Which VR controller the element follows. The element's position is replaced by\n"
         "the controller's aim pose (plus the anchor offsets).\n"
         "Disabled while the Controller Anchor toggle in the VR pane is off."));
  m_anchor_follow_rotation_check = new QCheckBox(tr("Follow Controller Rotation"));
  m_anchor_follow_rotation_check->setChecked(false);
  m_anchor_follow_rotation_check->setToolTip(
      tr("Rotate the element with the controller so it stays rigidly in hand (sword,\n"
         "cannon). Off = the element only moves with the controller and keeps its\n"
         "game orientation. The anchor offsets follow the controller's frame when on."));
  const auto make_anchor_angle_spin = []() {
    auto* spin = new QDoubleSpinBox;
    spin->setRange(-180.0, 180.0);
    spin->setDecimals(0);
    spin->setSingleStep(15.0);
    spin->setValue(0.0);
    spin->setSuffix(QStringLiteral("°"));
    return spin;
  };
  m_anchor_ctrl_yaw_label = new QLabel(tr("Model Yaw:"));
  m_anchor_ctrl_yaw_spin = make_anchor_angle_spin();
  m_anchor_ctrl_pitch_label = new QLabel(tr("Model Pitch:"));
  m_anchor_ctrl_pitch_spin = make_anchor_angle_spin();
  m_anchor_ctrl_roll_label = new QLabel(tr("Model Roll:"));
  m_anchor_ctrl_roll_spin = make_anchor_angle_spin();
  const QString anchor_angle_tooltip =
      tr("Fixed correction for the model's native orientation, applied in the\n"
         "controller's frame (yaw, then pitch, then roll). Models differ in which way\n"
         "they point: if the element comes up backward use yaw 180, if it lies flat\n"
         "use pitch ±90, and roll spins it about the aim direction.");
  m_anchor_ctrl_yaw_spin->setToolTip(anchor_angle_tooltip);
  m_anchor_ctrl_pitch_spin->setToolTip(anchor_angle_tooltip);
  m_anchor_ctrl_roll_spin->setToolTip(anchor_angle_tooltip);
  m_anchor_ctrl_yaw_spin->setEnabled(false);
  m_anchor_ctrl_pitch_spin->setEnabled(false);
  m_anchor_ctrl_roll_spin->setEnabled(false);
  connect(m_anchor_follow_rotation_check, &QCheckBox::toggled, this, [this](bool checked) {
    m_anchor_ctrl_yaw_spin->setEnabled(checked);
    m_anchor_ctrl_pitch_spin->setEnabled(checked);
    m_anchor_ctrl_roll_spin->setEnabled(checked);
  });

  m_flag_label = new QLabel(tr("Flag Group:"));
  m_flag_edit = new QLineEdit;
  m_flag_edit->setPlaceholderText(tr("e.g. gameplay"));
  m_flag_edit->setToolTip(tr("When this shader is drawn, it sets this named flag.\n"
                              "Other overrides can be conditional on this flag.\n"
                              "Optional for non-Flag handling (shader acts as both override and flag)."));

  m_condition_label = new QLabel(tr("Condition:"));
  m_condition_combo = new QComboBox;
  m_condition_combo->addItem(tr("(None)"), QString());
  for (const auto& flag : available_flags)
    m_condition_combo->addItem(QString::fromStdString(flag), QString::fromStdString(flag));
  m_condition_combo->setToolTip(
      tr("Select a flag condition for this override.\n"
         "Use Condition Mode to choose active or inactive behavior.\n"
         "Flags are detected from the current and previous frame."));

  m_condition_mode_label = new QLabel(tr("Condition Mode:"));
  m_condition_mode_combo = new QComboBox;
  m_condition_mode_combo->addItem(tr("Activate"), false);
  m_condition_mode_combo->addItem(tr("Deactivate"), true);
  m_condition_mode_combo->setToolTip(
      tr("Activate: apply when the selected flag is active.\n"
         "Deactivate: apply when the selected flag is NOT active."));

  m_hash_family_check = new QCheckBox(tr("Match Shader Family"));
  m_hash_family_check->setToolTip(
      tr("Match this override against a relaxed shader family signature instead of the exact hash.\n"
         "Useful when the same effect changes hash across scenes or game revisions."));
  m_hash_family_check->hide();

  m_view_textures_button = new QPushButton(tr("View Textures"));
  m_view_textures_button->setToolTip(
      tr("Show captured textures for this shader hash and type.\n"
         "Check textures in the popup and apply them to Texture Filters."));

  m_texture_mode_label = new QLabel(tr("Texture Filter Mode:"));
  m_texture_mode_combo = new QComboBox;
  m_texture_mode_combo->addItem(tr("Include"), false);
  m_texture_mode_combo->addItem(tr("Exclude"), true);
  m_texture_mode_combo->setToolTip(
      tr("Include: apply override only when any listed texture hash is bound.\n"
         "Exclude: apply override only when none of the listed texture hashes are bound."));

  m_texture_hash_container = new QWidget;
  m_texture_hash_layout = new QVBoxLayout(m_texture_hash_container);
  m_texture_hash_layout->setContentsMargins(0, 0, 0, 0);
  m_texture_hash_layout->setSpacing(4);
  m_texture_hash_scroll = new QScrollArea;
  m_texture_hash_scroll->setWidgetResizable(true);
  m_texture_hash_scroll->setFrameShape(QFrame::NoFrame);
  m_texture_hash_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_texture_hash_scroll->setWidget(m_texture_hash_container);
  AddTextureHashField(QString());

  // New overrides default to Shader Family matching: family signatures are semantic (computed
  // from game-derived shader configuration) and survive shader-generator updates, while exact
  // hashes embed code_version and break on every bump.
  if (!edit_override)
  {
    const int family_idx =
        m_match_mode_combo->findData(static_cast<int>(ShaderHunter::MatchMode::ShaderFamily));
    if (family_idx >= 0)
      m_match_mode_combo->setCurrentIndex(family_idx);
  }

  // Pre-fill fields in edit mode
  if (edit_override)
  {
    m_edit_family_signature = edit_override->family_signature;
    m_edit_family_version = edit_override->family_version;
    m_edit_original_hash = edit_override->hash;
    m_edit_original_type = edit_override->type;
    m_name_edit->setText(QString::fromStdString(edit_override->name));
    m_comments_edit->setPlainText(QString::fromStdString(edit_override->comments));
    m_credits_edit->setText(QString::fromStdString(edit_override->credits));
    m_hash_edit->setText(
        QString::fromStdString(fmt::format("{:016x}", edit_override->hash)));

    const int type_idx = m_type_combo->findData(static_cast<int>(edit_override->type));
    if (type_idx >= 0)
      m_type_combo->setCurrentIndex(type_idx);

    const ShaderHunter::MatchMode initial_match_mode =
        edit_override->match_mode == ShaderHunter::MatchMode::RuntimeElement ?
            ShaderHunter::MatchMode::ExactHash :
            edit_override->match_mode;
    const int match_mode_idx =
        m_match_mode_combo->findData(static_cast<int>(initial_match_mode));
    if (match_mode_idx >= 0)
      m_match_mode_combo->setCurrentIndex(match_mode_idx);

    const int handling_idx = m_handling_combo->findData(static_cast<int>(edit_override->handling));
    if (handling_idx >= 0)
      m_handling_combo->setCurrentIndex(handling_idx);

    m_element_depth_spin->setValue(edit_override->element_depth);
    if (edit_override->units_per_meter > 0.0f)
      m_units_per_meter_spin->setValue(edit_override->units_per_meter);
    m_passthrough_opacity_spin->setValue(edit_override->passthrough_opacity);
    m_anchor_right_spin->setValue(edit_override->anchor_right);
    m_anchor_up_spin->setValue(edit_override->anchor_up);
    m_anchor_forward_spin->setValue(edit_override->anchor_forward);
    {
      const int idx =
          m_anchor_rotation_combo->findData(static_cast<int>(edit_override->anchor_rotation));
      if (idx >= 0)
        m_anchor_rotation_combo->setCurrentIndex(idx);
    }
    m_anchor_yaw_spin->setValue(edit_override->anchor_yaw_deg);
    if (edit_override->anchor_units_per_meter > 0.0f)
      m_anchor_upm_spin->setValue(edit_override->anchor_units_per_meter);
    m_anchor_hide_check->setChecked(edit_override->anchor_hide);
    {
      const int idx = m_anchor_hand_combo->findData(edit_override->anchor_hand == 0 ? 0 : 1);
      if (idx >= 0)
        m_anchor_hand_combo->setCurrentIndex(idx);
    }
    if (edit_override->handling == ShaderHunter::HandlingType::ControllerAnchor)
    {
      m_anchor_follow_rotation_check->setChecked(edit_override->anchor_rotation !=
                                                 ShaderHunter::AnchorRotationMode::Off);
      m_anchor_ctrl_yaw_spin->setValue(edit_override->anchor_yaw_deg);
      m_anchor_ctrl_pitch_spin->setValue(edit_override->anchor_pitch_deg);
      m_anchor_ctrl_roll_spin->setValue(edit_override->anchor_roll_deg);
    }
    m_flag_edit->setText(QString::fromStdString(edit_override->flag_group));

    m_hash_family_check->setChecked(edit_override->hash_family_match);

    m_updating_texture_hash_fields = true;
    while (m_texture_hash_edits.size() < edit_override->texture_hashes.size())
      AddTextureHashField(QString());
    for (size_t i = 0; i < edit_override->texture_hashes.size(); i++)
    {
      m_texture_hash_edits[i]->setText(
          QString::fromStdString(fmt::format("{:016x}", edit_override->texture_hashes[i])));
    }
    for (size_t i = edit_override->texture_hashes.size(); i < m_texture_hash_edits.size(); i++)
      m_texture_hash_edits[i]->clear();
    m_updating_texture_hash_fields = false;
    EnsureTextureHashFieldRows();
    const int texture_mode_idx =
        m_texture_mode_combo->findData(edit_override->texture_hashes_excluded);
    if (texture_mode_idx >= 0)
      m_texture_mode_combo->setCurrentIndex(texture_mode_idx);

    if (!edit_override->condition_flag.empty())
    {
      const int cond_idx =
          m_condition_combo->findData(QString::fromStdString(edit_override->condition_flag));
      if (cond_idx >= 0)
        m_condition_combo->setCurrentIndex(cond_idx);
      else
      {
        // Flag not in available list — add it dynamically
        m_condition_combo->addItem(QString::fromStdString(edit_override->condition_flag),
                                   QString::fromStdString(edit_override->condition_flag));
        m_condition_combo->setCurrentIndex(m_condition_combo->count() - 1);
      }
    }
    const int mode_idx = m_condition_mode_combo->findData(edit_override->condition_inverted);
    if (mode_idx >= 0)
      m_condition_mode_combo->setCurrentIndex(mode_idx);
  }

  auto* form = new QFormLayout;
  form->addRow(tr("Name:"), m_name_edit);
  form->addRow(tr("Hash:"), m_hash_edit);
  form->addRow(tr("Shader Type:"), m_type_combo);
  form->addRow(m_match_mode_label, m_match_mode_combo);
  form->addRow(tr("Handling:"), m_handling_combo);
  form->addRow(m_element_depth_label, m_element_depth_spin);
  form->addRow(m_units_per_meter_label, m_units_per_meter_spin);
  form->addRow(m_passthrough_opacity_label, m_passthrough_opacity_spin);
  form->addRow(m_anchor_right_label, m_anchor_right_spin);
  form->addRow(m_anchor_up_label, m_anchor_up_spin);
  form->addRow(m_anchor_forward_label, m_anchor_forward_spin);
  form->addRow(m_anchor_rotation_label, m_anchor_rotation_combo);
  form->addRow(m_anchor_yaw_label, m_anchor_yaw_spin);
  form->addRow(m_anchor_upm_label, m_anchor_upm_spin);
  form->addRow(QString(), m_anchor_hide_check);
  form->addRow(m_anchor_hand_label, m_anchor_hand_combo);
  form->addRow(QString(), m_anchor_follow_rotation_check);
  form->addRow(m_anchor_ctrl_yaw_label, m_anchor_ctrl_yaw_spin);
  form->addRow(m_anchor_ctrl_pitch_label, m_anchor_ctrl_pitch_spin);
  form->addRow(m_anchor_ctrl_roll_label, m_anchor_ctrl_roll_spin);
  form->addRow(m_flag_label, m_flag_edit);
  form->addRow(m_condition_label, m_condition_combo);
  form->addRow(m_condition_mode_label, m_condition_mode_combo);
  form->addRow(QString(), m_view_textures_button);
  form->addRow(m_texture_mode_label, m_texture_mode_combo);
  form->addRow(tr("Texture Filters:"), m_texture_hash_scroll);
  form->addRow(tr("Comments:"), m_comments_edit);
  form->addRow(tr("Credits:"), m_credits_edit);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, this, &ShaderOverrideAddEditDialog::OnAccept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_handling_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &ShaderOverrideAddEditDialog::OnHandlingChanged);
  connect(m_condition_combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
    const bool has_condition = !m_condition_combo->currentData().toString().isEmpty();
    m_condition_mode_label->setEnabled(has_condition);
    m_condition_mode_combo->setEnabled(has_condition);
  });
  connect(m_view_textures_button, &QPushButton::clicked, this,
          &ShaderOverrideAddEditDialog::ShowTextureBrowser);

  auto* layout = new QVBoxLayout;
  layout->addLayout(form);
  layout->addWidget(buttons);
  setLayout(layout);

  // Show/hide fields based on initial handling selection
  OnHandlingChanged();
  const bool has_condition = !m_condition_combo->currentData().toString().isEmpty();
  m_condition_mode_label->setEnabled(has_condition);
  m_condition_mode_combo->setEnabled(has_condition);
}

ShaderHunter::ShaderOverride ShaderOverrideAddEditDialog::GetResult() const
{
  ShaderHunter::ShaderOverride result;
  result.name = m_name_edit->text().toStdString();
  result.comments = m_comments_edit->toPlainText().trimmed().toStdString();
  result.credits = m_credits_edit->text().trimmed().toStdString();
  result.hash = std::strtoull(m_hash_edit->text().toStdString().c_str(), nullptr, 16);
  result.type =
      static_cast<ShaderHunter::ShaderType>(m_type_combo->currentData().toInt());
  result.match_mode =
      static_cast<ShaderHunter::MatchMode>(m_match_mode_combo->currentData().toInt());
  result.hash_family_match = result.match_mode == ShaderHunter::MatchMode::ShaderFamily;
  // Keep the stored family signature/scheme version only while the shader identity is unchanged.
  // A signature belongs to both the hash's captured UID and its shader type.
  const bool shader_identity_unchanged =
      result.hash == m_edit_original_hash && result.type == m_edit_original_type;
  result.family_signature = shader_identity_unchanged ? m_edit_family_signature : 0;
  result.family_version = shader_identity_unchanged ?
                              m_edit_family_version :
                              ShaderHunter::FAMILY_SCHEME_VERSION;

  // When the game is running, re-resolve the family live — that always yields a current-scheme
  // signature and upgrades legacy entries in place.
  if (const auto signature =
          ShaderHunter::GetInstance().GetShaderFamilySignature(result.type, result.hash);
      signature.has_value())
  {
    result.family_signature = *signature;
    result.family_version = ShaderHunter::FAMILY_SCHEME_VERSION;
  }
  result.handling =
      static_cast<ShaderHunter::HandlingType>(m_handling_combo->currentData().toInt());
  result.element_depth = (result.handling == ShaderHunter::HandlingType::Screen ||
                          result.handling == ShaderHunter::HandlingType::HeadLocked) ?
                             static_cast<float>(m_element_depth_spin->value()) : -1.0f;
  result.units_per_meter = result.handling == ShaderHunter::HandlingType::UnitsPerMeter ?
                               static_cast<float>(m_units_per_meter_spin->value()) :
                               -1.0f;
  result.passthrough_opacity = result.handling == ShaderHunter::HandlingType::Passthrough ?
                                   static_cast<float>(m_passthrough_opacity_spin->value()) :
                                   0.0f;
  if (result.handling == ShaderHunter::HandlingType::CameraAnchor)
  {
    result.anchor_right = static_cast<float>(m_anchor_right_spin->value());
    result.anchor_up = static_cast<float>(m_anchor_up_spin->value());
    result.anchor_forward = static_cast<float>(m_anchor_forward_spin->value());
    result.anchor_rotation = static_cast<ShaderHunter::AnchorRotationMode>(
        m_anchor_rotation_combo->currentData().toInt());
    result.anchor_yaw_deg = static_cast<float>(m_anchor_yaw_spin->value());
    result.anchor_units_per_meter = m_anchor_upm_spin->value() >= UPM_OVERRIDE_MIN ?
                                        static_cast<float>(m_anchor_upm_spin->value()) :
                                        -1.0f;
    result.anchor_hide = m_anchor_hide_check->isChecked();
  }
  if (result.handling == ShaderHunter::HandlingType::ControllerAnchor)
  {
    result.anchor_hand = m_anchor_hand_combo->currentData().toInt();
    result.anchor_right = static_cast<float>(m_anchor_right_spin->value());
    result.anchor_up = static_cast<float>(m_anchor_up_spin->value());
    result.anchor_forward = static_cast<float>(m_anchor_forward_spin->value());
    result.anchor_rotation = m_anchor_follow_rotation_check->isChecked() ?
                                 ShaderHunter::AnchorRotationMode::Full :
                                 ShaderHunter::AnchorRotationMode::Off;
    result.anchor_yaw_deg = static_cast<float>(m_anchor_ctrl_yaw_spin->value());
    result.anchor_pitch_deg = static_cast<float>(m_anchor_ctrl_pitch_spin->value());
    result.anchor_roll_deg = static_cast<float>(m_anchor_ctrl_roll_spin->value());
  }
  const auto texture_tokens = CollectTextureHashTokens();
  for (const std::string& token : texture_tokens)
  {
    const u64 parsed = std::strtoull(token.c_str(), nullptr, 16);
    if (parsed != 0)
      result.texture_hashes.push_back(parsed);
  }
  std::sort(result.texture_hashes.begin(), result.texture_hashes.end());
  result.texture_hashes.erase(
      std::unique(result.texture_hashes.begin(), result.texture_hashes.end()),
      result.texture_hashes.end());
  result.texture_hashes_excluded =
      !result.texture_hashes.empty() && m_texture_mode_combo->currentData().toBool();
  result.enabled = true;
  result.user_defined = true;

  // flag_group is available on all handling types (optional except for Flag)
  result.flag_group = m_flag_edit->text().trimmed().toStdString();

  if (result.handling != ShaderHunter::HandlingType::Flag)
  {
    result.condition_flag = m_condition_combo->currentData().toString().toStdString();
    result.condition_inverted =
        !result.condition_flag.empty() && m_condition_mode_combo->currentData().toBool();
  }

  return result;
}

void ShaderOverrideAddEditDialog::OnAccept()
{
  const std::string name = m_name_edit->text().toStdString();
  if (name.empty())
  {
    QMessageBox::warning(this, tr("Validation Error"), tr("Name cannot be empty."));
    return;
  }

  const std::string hash_str = m_hash_edit->text().toStdString();
  if (hash_str.empty())
  {
    QMessageBox::warning(this, tr("Validation Error"), tr("Hash cannot be empty."));
    return;
  }

  // Validate hex string
  for (char c : hash_str)
  {
    if (!std::isxdigit(static_cast<unsigned char>(c)))
    {
      QMessageBox::warning(this, tr("Validation Error"),
                           tr("Hash must contain only hexadecimal characters (0-9, a-f)."));
      return;
    }
  }

  if (hash_str.size() > 16)
  {
    QMessageBox::warning(this, tr("Validation Error"),
                         tr("Hash must be at most 16 hex digits."));
    return;
  }

  const u64 hash = std::strtoull(hash_str.c_str(), nullptr, 16);
  const auto shader_type =
      static_cast<ShaderHunter::ShaderType>(m_type_combo->currentData().toInt());
  const auto match_mode =
      static_cast<ShaderHunter::MatchMode>(m_match_mode_combo->currentData().toInt());
  if (match_mode == ShaderHunter::MatchMode::ShaderFamily)
  {
    const bool can_reuse_stored_signature =
        m_edit_family_signature != 0 && hash == m_edit_original_hash &&
        shader_type == m_edit_original_type;
    if (!can_reuse_stored_signature &&
        !ShaderHunter::GetInstance().GetShaderFamilySignature(shader_type, hash).has_value())
    {
      QMessageBox::warning(
          this, tr("Validation Error"),
          tr("No family signature has been captured for this shader hash and type. Run the game "
             "and let this shader render, or capture it with Shader Hunter, then retry. You can "
             "also select Exact Hash, but that override may not survive shader-generator "
             "updates."));
      return;
    }
  }

  // Validate texture hash hex string (if provided)
  const auto texture_tokens = CollectTextureHashTokens();
  for (const std::string& token : texture_tokens)
  {
    for (char c : token)
    {
      if (!std::isxdigit(static_cast<unsigned char>(c)))
      {
        QMessageBox::warning(this, tr("Validation Error"),
                             tr("Texture filter values must contain only hexadecimal characters "
                                "(0-9, a-f)."));
        return;
      }
    }
    if (token.size() > 16)
    {
      QMessageBox::warning(this, tr("Validation Error"),
                           tr("Each texture filter value must be at most 16 hex digits."));
      return;
    }
  }

  // Validate flag group name for Flag handling
  const auto handling = static_cast<ShaderHunter::HandlingType>(
      m_handling_combo->currentData().toInt());
  if (handling == ShaderHunter::HandlingType::Flag && m_flag_edit->text().trimmed().isEmpty())
  {
    QMessageBox::warning(this, tr("Validation Error"),
                         tr("Flag Group name cannot be empty for Flag handling."));
    return;
  }
  if (handling == ShaderHunter::HandlingType::UnitsPerMeter &&
      m_units_per_meter_spin->value() <= 0.0)
  {
    QMessageBox::warning(this, tr("Validation Error"),
                         tr("Units per Meter must be greater than 0."));
    return;
  }

  accept();
}

void ShaderOverrideAddEditDialog::OnHandlingChanged()
{
  const auto handling = static_cast<ShaderHunter::HandlingType>(
      m_handling_combo->currentData().toInt());
  const bool show_element_depth = (handling == ShaderHunter::HandlingType::Screen ||
                                   handling == ShaderHunter::HandlingType::HeadLocked);
  const bool show_units_per_meter = (handling == ShaderHunter::HandlingType::UnitsPerMeter);
  const bool show_passthrough = (handling == ShaderHunter::HandlingType::Passthrough);
  const bool is_flag = (handling == ShaderHunter::HandlingType::Flag);

  m_element_depth_label->setVisible(show_element_depth);
  m_element_depth_spin->setVisible(show_element_depth);
  m_units_per_meter_label->setVisible(show_units_per_meter);
  m_units_per_meter_spin->setVisible(show_units_per_meter);
  m_passthrough_opacity_label->setVisible(show_passthrough);
  m_passthrough_opacity_spin->setVisible(show_passthrough);
  const bool show_anchor = (handling == ShaderHunter::HandlingType::CameraAnchor);
  const bool show_controller_anchor =
      (handling == ShaderHunter::HandlingType::ControllerAnchor);
  const bool show_anchor_offsets = show_anchor || show_controller_anchor;
  m_anchor_right_label->setVisible(show_anchor_offsets);
  m_anchor_right_spin->setVisible(show_anchor_offsets);
  m_anchor_up_label->setVisible(show_anchor_offsets);
  m_anchor_up_spin->setVisible(show_anchor_offsets);
  m_anchor_forward_label->setVisible(show_anchor_offsets);
  m_anchor_forward_spin->setVisible(show_anchor_offsets);
  m_anchor_rotation_label->setVisible(show_anchor);
  m_anchor_rotation_combo->setVisible(show_anchor);
  m_anchor_yaw_label->setVisible(show_anchor);
  m_anchor_yaw_spin->setVisible(show_anchor);
  m_anchor_upm_label->setVisible(show_anchor);
  m_anchor_upm_spin->setVisible(show_anchor);
  m_anchor_hide_check->setVisible(show_anchor);
  m_anchor_hand_label->setVisible(show_controller_anchor);
  m_anchor_hand_combo->setVisible(show_controller_anchor);
  m_anchor_follow_rotation_check->setVisible(show_controller_anchor);
  m_anchor_ctrl_yaw_label->setVisible(show_controller_anchor);
  m_anchor_ctrl_yaw_spin->setVisible(show_controller_anchor);
  m_anchor_ctrl_pitch_label->setVisible(show_controller_anchor);
  m_anchor_ctrl_pitch_spin->setVisible(show_controller_anchor);
  m_anchor_ctrl_roll_label->setVisible(show_controller_anchor);
  m_anchor_ctrl_roll_spin->setVisible(show_controller_anchor);
  // Flag group is always visible (optional for non-Flag handling, required for Flag)
  m_flag_label->setVisible(true);
  m_flag_edit->setVisible(true);
  m_condition_label->setVisible(!is_flag);
  m_condition_combo->setVisible(!is_flag);
  m_condition_mode_label->setVisible(!is_flag);
  m_condition_mode_combo->setVisible(!is_flag);
}

void ShaderOverrideAddEditDialog::ShowTextureBrowser()
{
  const std::string hash_str = m_hash_edit->text().trimmed().toStdString();
  if (hash_str.empty())
  {
    QMessageBox::warning(this, tr("View Textures"),
                         tr("Hash cannot be empty. Enter the shader hash first."));
    return;
  }
  if (hash_str.size() > 16)
  {
    QMessageBox::warning(this, tr("View Textures"),
                         tr("Hash must be at most 16 hex digits."));
    return;
  }
  for (char c : hash_str)
  {
    if (!std::isxdigit(static_cast<unsigned char>(c)))
    {
      QMessageBox::warning(this, tr("View Textures"),
                           tr("Hash must contain only hexadecimal characters (0-9, a-f)."));
      return;
    }
  }

  const u64 shader_hash = std::strtoull(hash_str.c_str(), nullptr, 16);
  const auto shader_type =
      static_cast<ShaderHunter::ShaderType>(m_type_combo->currentData().toInt());
  auto& hunter = ShaderHunter::GetInstance();
  const bool was_hunting_enabled = hunter.IsEnabled();
  const auto previous_active_type = hunter.GetActiveType();
  const int previous_selected_pos = hunter.GetSelectedPosition();
  const u64 previous_selected_hash = hunter.GetSelectedHash();

  const char* type_str = shader_type == ShaderHunter::ShaderType::Pixel    ? "PS" :
                         shader_type == ShaderHunter::ShaderType::Vertex   ? "VS" :
                                                                              "GS";

  if (!was_hunting_enabled)
    hunter.SetEnabled(true);
  hunter.SetTextureToolActive(true);

  const auto initial_hashes = CollectTextureHashValues();
  const auto sync_selected_hashes = [shader_hash, shader_type](const std::vector<u64>& hashes) {
    auto& sync_hunter = ShaderHunter::GetInstance();
    if (!sync_hunter.SelectShader(shader_type, shader_hash))
      return;

    sync_hunter.ClearTextureSkipFilters();
    for (u64 selected_hash : hashes)
      sync_hunter.SetTextureSkipEnabled(selected_hash, true);
  };

  TextureHashBrowserConfig browser_config;
  browser_config.title = tr("Textures for %1 %2")
                             .arg(QString::fromLatin1(type_str))
                             .arg(static_cast<qulonglong>(shader_hash), 16, 16,
                                  QLatin1Char('0'));
  browser_config.empty_info_text =
      tr("No textures captured yet for this shader.\n"
         "Shader Hunter was enabled temporarily for this window.");
  browser_config.current_label = tr("%1 %2")
                                     .arg(QString::fromLatin1(type_str))
                                     .arg(static_cast<qulonglong>(shader_hash), 16, 16,
                                          QLatin1Char('0'));
  browser_config.initial_selected_hashes = initial_hashes;
  browser_config.fetch_current_entries = [shader_hash, shader_type]() {
    auto& populate_hunter = ShaderHunter::GetInstance();
    if (!populate_hunter.SelectShader(shader_type, shader_hash))
      return std::vector<TextureHashBrowserEntry>{};

    std::vector<TextureHashBrowserEntry> entries;
    for (const auto& tex : populate_hunter.GetTexturesForHash(shader_type, shader_hash))
      entries.push_back(TextureHashBrowserEntry{.hash = tex.hash, .name = tex.name});
    return entries;
  };
  browser_config.apply_selected_hashes = [this](const std::vector<u64>& hashes) {
    SetTextureHashFields(hashes);
  };
  browser_config.live_selection_changed = sync_selected_hashes;

  auto* dlg = ShowTextureHashBrowserDialog(this, browser_config);

  connect(dlg, &QObject::destroyed, this,
          [was_hunting_enabled, previous_active_type, previous_selected_pos,
           previous_selected_hash]() {
            auto& restore_hunter = ShaderHunter::GetInstance();
            restore_hunter.SetTextureToolActive(false);
            restore_hunter.ClearTextureSkipFilters();
            if (previous_selected_pos >= 0)
            {
              if (!restore_hunter.SelectShader(previous_active_type, previous_selected_hash))
                restore_hunter.SetActiveType(previous_active_type);
            }
            else
            {
              restore_hunter.SetActiveType(previous_active_type);
            }
            if (!was_hunting_enabled)
              restore_hunter.SetEnabled(false);
          });
}

std::vector<std::string> ShaderOverrideAddEditDialog::CollectTextureHashTokens() const
{
  std::vector<std::string> tokens;
  const QRegularExpression separators(QStringLiteral("[,;\\s]+"));
  const QRegularExpression hex_exact(QStringLiteral("^[0-9A-Fa-f]{1,16}$"));
  const QRegularExpression hex_16_anywhere(QStringLiteral("([0-9A-Fa-f]{16})"));
  for (QLineEdit* edit : m_texture_hash_edits)
  {
    if (edit == nullptr)
      continue;
    const auto parts = edit->text().trimmed().split(separators, Qt::SkipEmptyParts);
    for (const QString& part : parts)
    {
      if (hex_exact.match(part).hasMatch())
      {
        tokens.push_back(part.toLower().toStdString());
        continue;
      }

      const auto match = hex_16_anywhere.match(part);
      if (match.hasMatch())
      {
        tokens.push_back(match.captured(1).toLower().toStdString());
        continue;
      }

      tokens.push_back(part.toStdString());
    }
  }
  return tokens;
}

std::vector<u64> ShaderOverrideAddEditDialog::CollectTextureHashValues() const
{
  std::vector<u64> hashes;
  for (const std::string& token : CollectTextureHashTokens())
  {
    if (token.empty() || token.size() > 16)
      continue;
    bool valid = true;
    for (char c : token)
    {
      if (!std::isxdigit(static_cast<unsigned char>(c)))
      {
        valid = false;
        break;
      }
    }
    if (!valid)
      continue;
    const u64 parsed = std::strtoull(token.c_str(), nullptr, 16);
    if (parsed != 0)
      hashes.push_back(parsed);
  }
  std::sort(hashes.begin(), hashes.end());
  hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
  return hashes;
}

void ShaderOverrideAddEditDialog::SetTextureHashFields(const std::vector<u64>& hashes)
{
  m_updating_texture_hash_fields = true;
  while (m_texture_hash_edits.size() < hashes.size())
    AddTextureHashField(QString());

  for (size_t i = 0; i < hashes.size(); i++)
  {
    m_texture_hash_edits[i]->setText(
        QString::fromStdString(fmt::format("{:016x}", hashes[i])));
  }
  for (size_t i = hashes.size(); i < m_texture_hash_edits.size(); i++)
    m_texture_hash_edits[i]->clear();

  m_updating_texture_hash_fields = false;
  EnsureTextureHashFieldRows();
}

void ShaderOverrideAddEditDialog::AddTextureHashField(const QString& text)
{
  auto* edit = new QLineEdit;
  edit->setPlaceholderText(tr("Hex hash"));
  edit->setToolTip(
      tr("Optional texture filter hash.\n"
         "Enter one hash per line; a new line is added automatically.\n"
         "You can also paste multiple hashes separated by comma, semicolon, or spaces."));
  edit->setText(text);
  m_texture_hash_layout->addWidget(edit);
  m_texture_hash_edits.push_back(edit);

  connect(edit, &QLineEdit::textChanged, this, [this, edit](const QString&) {
    SanitizeTextureHashField(edit);
    EnsureTextureHashFieldRows();
  });
}

void ShaderOverrideAddEditDialog::EnsureTextureHashFieldRows()
{
  if (m_updating_texture_hash_fields)
    return;

  m_updating_texture_hash_fields = true;

  int last_non_empty = -1;
  for (int i = 0; i < static_cast<int>(m_texture_hash_edits.size()); i++)
  {
    if (!m_texture_hash_edits[i]->text().trimmed().isEmpty())
      last_non_empty = i;
  }

  const int desired_rows = std::max(1, last_non_empty + 2);

  while (static_cast<int>(m_texture_hash_edits.size()) < desired_rows)
    AddTextureHashField(QString());

  while (static_cast<int>(m_texture_hash_edits.size()) > desired_rows)
  {
    QLineEdit* edit = m_texture_hash_edits.back();
    m_texture_hash_edits.pop_back();
    m_texture_hash_layout->removeWidget(edit);
    delete edit;
  }

  const int visible_rows = std::min(desired_rows, MAX_VISIBLE_TEXTURE_HASH_ROWS);
  const int viewport_height = visible_rows * TEXTURE_HASH_ROW_HEIGHT;
  m_texture_hash_scroll->setMinimumHeight(viewport_height);
  m_texture_hash_scroll->setMaximumHeight(viewport_height);

  m_updating_texture_hash_fields = false;
}

void ShaderOverrideAddEditDialog::SanitizeTextureHashField(QLineEdit* edit)
{
  if (edit == nullptr || m_updating_texture_hash_fields)
    return;

  const QRegularExpression separators(QStringLiteral("[,;\\s]+"));
  const QRegularExpression hex_exact(QStringLiteral("^[0-9A-Fa-f]{1,16}$"));
  const QRegularExpression hex_16_anywhere(QStringLiteral("([0-9A-Fa-f]{16})"));

  const auto parts = edit->text().trimmed().split(separators, Qt::SkipEmptyParts);
  if (parts.isEmpty())
    return;

  QStringList sanitized_parts;
  bool changed = false;
  for (const QString& part : parts)
  {
    if (hex_exact.match(part).hasMatch())
    {
      const QString lowered = part.toLower();
      sanitized_parts.push_back(lowered);
      if (lowered != part)
        changed = true;
      continue;
    }

    const auto match = hex_16_anywhere.match(part);
    if (match.hasMatch())
    {
      sanitized_parts.push_back(match.captured(1).toLower());
      changed = true;
      continue;
    }

    sanitized_parts.push_back(part);
  }

  if (!changed)
    return;

  const QString new_text = sanitized_parts.join(QStringLiteral(" "));
  if (new_text == edit->text())
    return;

  const QSignalBlocker blocker(edit);
  edit->setText(new_text);
}
