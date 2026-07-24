// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/TextureElementOverrideAddEditDialog.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStringList>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <fmt/format.h>

#include "Common/FileUtil.h"

#include "DolphinQt/Config/TextureHashBrowserDialog.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "VideoCommon/TextureInfo.h"

namespace
{
constexpr double UPM_OVERRIDE_MIN = 0.01;
constexpr double UPM_OVERRIDE_MAX = 1000.0;
constexpr double UPM_OVERRIDE_STEP = 0.01;
constexpr int MAX_VISIBLE_TEXTURE_HASH_ROWS = 6;
constexpr int MAX_VISIBLE_COMMENT_LINES = 5;
constexpr int TEXTURE_HASH_PREVIEW_SIZE = 40;  // Thumbnail square, in pixels.
constexpr int TEXTURE_HASH_ROW_HEIGHT = TEXTURE_HASH_PREVIEW_SIZE + 6;

bool HasLocalFileUrls(const QMimeData* mime_data)
{
  if (!mime_data->hasUrls())
    return false;

  for (const QUrl& url : mime_data->urls())
  {
    if (url.isLocalFile())
      return true;
  }
  return false;
}

class TextureHashLineEdit final : public QLineEdit
{
public:
  using DropHandler = std::function<void(const QStringList&)>;

  explicit TextureHashLineEdit(DropHandler drop_handler)
      : m_drop_handler(std::move(drop_handler))
  {
    setAcceptDrops(true);
  }

protected:
  void dragEnterEvent(QDragEnterEvent* event) override
  {
    if (HasLocalFileUrls(event->mimeData()))
      event->acceptProposedAction();
    else
      QLineEdit::dragEnterEvent(event);
  }

  void dragMoveEvent(QDragMoveEvent* event) override
  {
    if (HasLocalFileUrls(event->mimeData()))
      event->acceptProposedAction();
    else
      QLineEdit::dragMoveEvent(event);
  }

  void dropEvent(QDropEvent* event) override
  {
    if (!HasLocalFileUrls(event->mimeData()))
    {
      QLineEdit::dropEvent(event);
      return;
    }

    QStringList files;
    for (const QUrl& url : event->mimeData()->urls())
    {
      if (url.isLocalFile())
        files.push_back(url.toLocalFile());
    }

    m_drop_handler(files);
    event->acceptProposedAction();
  }

private:
  DropHandler m_drop_handler;
};

}  // namespace

using HandlingType = TextureElementManager::HandlingType;
using AnchorRotationMode = TextureElementManager::AnchorRotationMode;
using TextureElementOverride = TextureElementManager::TextureElementOverride;

TextureElementOverrideAddEditDialog::TextureElementOverrideAddEditDialog(
    QWidget* parent, std::string game_id, const TextureElementOverride* edit_override,
    const std::vector<std::string>& available_flags)
    : QDialog(parent), m_game_id(std::move(game_id))
{
  setWindowTitle(edit_override ? tr("Edit Texture Element Override") :
                                 tr("Add Texture Element Override"));
  setMinimumWidth(350);

  m_name_edit = new QLineEdit;
  m_name_edit->setPlaceholderText(tr("Override name..."));

  m_comments_edit = new QPlainTextEdit;
  m_comments_edit->setPlaceholderText(tr("Optional notes..."));
  m_comments_edit->setTabChangesFocus(true);
  const int comments_document_margins =
      static_cast<int>(2.0 * m_comments_edit->document()->documentMargin());
  const int comments_max_height =
      MAX_VISIBLE_COMMENT_LINES * m_comments_edit->fontMetrics().lineSpacing() +
      2 * m_comments_edit->frameWidth() + comments_document_margins;
  m_comments_edit->setMaximumHeight(comments_max_height);
  m_comments_edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

  m_handling_combo = new QComboBox;
  m_handling_combo->addItem(tr("Skip"), static_cast<int>(HandlingType::Skip));
  m_handling_combo->addItem(tr("Screen"), static_cast<int>(HandlingType::Screen));
  m_handling_combo->addItem(tr("Fullscreen"), static_cast<int>(HandlingType::Fullscreen));
  m_handling_combo->addItem(tr("Head Locked"), static_cast<int>(HandlingType::HeadLocked));
  m_handling_combo->addItem(tr("Flag"), static_cast<int>(HandlingType::Flag));
  m_handling_combo->addItem(tr("Units per Meter"), static_cast<int>(HandlingType::UnitsPerMeter));
  m_handling_combo->addItem(tr("Passthrough"), static_cast<int>(HandlingType::Passthrough));
  m_handling_combo->addItem(tr("Camera Anchor"), static_cast<int>(HandlingType::CameraAnchor));
  m_handling_combo->addItem(tr("Controller Anchor"),
                            static_cast<int>(HandlingType::ControllerAnchor));
  m_handling_combo->setToolTip(
      tr("How every draw that binds a listed texture is handled in VR.\n"
         "Skip = hide, Screen = world-fixed, Head Locked = follows head, Fullscreen = no VR,\n"
         "Flag = detect texture presence to conditionally enable overrides in any tool,\n"
         "Passthrough = pixels become a see-through window to the headset camera,\n"
         "Camera Anchor = the VR camera moves to the drawn element (first-person view)."));

  m_flag_label = new QLabel(tr("Flag Group:"));
  m_flag_edit = new QLineEdit;
  m_flag_edit->setPlaceholderText(tr("e.g. gameplay"));
  m_flag_edit->setToolTip(
      tr("When a listed texture is bound, this named flag is set.\n"
         "Shader, Elements Group, and Texture Element overrides can use it as a condition.\n"
         "Optional for non-Flag handling (the texture acts as both override and flag)."));

  m_condition_label = new QLabel(tr("Condition Flag:"));
  m_condition_combo = new QComboBox;
  m_condition_combo->setEditable(true);
  m_condition_combo->addItem(QString());
  for (const std::string& flag : available_flags)
    m_condition_combo->addItem(QString::fromStdString(flag));
  m_condition_combo->setToolTip(
      tr("Only apply this texture override when the shared flag is active or inactive.\n"
         "Flags may be produced by Shader, Elements Group, or Texture Element overrides."));

  m_condition_mode_label = new QLabel(tr("Condition Mode:"));
  m_condition_mode_combo = new QComboBox;
  m_condition_mode_combo->addItem(tr("Activate"), false);
  m_condition_mode_combo->addItem(tr("Deactivate"), true);

  m_element_depth_label = new QLabel(tr("Element Depth:"));
  m_element_depth_spin = new QDoubleSpinBox;
  m_element_depth_spin->setRange(-1.0, 0.01);
  m_element_depth_spin->setDecimals(4);
  m_element_depth_spin->setSingleStep(0.0001);
  m_element_depth_spin->setSpecialValueText(tr("Default"));
  m_element_depth_spin->setValue(-1.0);
  m_element_depth_spin->setToolTip(tr("Within-element depth range for these textures (legacy "
                                      "fallback when Exact Screen Depth is off).\n"
                                      "-1 (Default) = use the built-in default."));

  m_units_per_meter_label = new QLabel(tr("Units per Meter:"));
  m_units_per_meter_spin = new QDoubleSpinBox;
  m_units_per_meter_spin->setRange(UPM_OVERRIDE_MIN, UPM_OVERRIDE_MAX);
  m_units_per_meter_spin->setDecimals(2);
  m_units_per_meter_spin->setSingleStep(UPM_OVERRIDE_STEP);
  m_units_per_meter_spin->setValue(1.0);
  m_units_per_meter_spin->setToolTip(tr("Temporary per-texture scale override for VR.\n"
                                        "Higher values make these textures appear larger."));

  m_passthrough_opacity_label = new QLabel(tr("Opacity:"));
  m_passthrough_opacity_spin = new QDoubleSpinBox;
  m_passthrough_opacity_spin->setRange(0.0, 1.0);
  m_passthrough_opacity_spin->setDecimals(2);
  m_passthrough_opacity_spin->setSingleStep(0.05);
  m_passthrough_opacity_spin->setValue(0.0);
  m_passthrough_opacity_spin->setToolTip(
      tr("How opaque these textures stay over the headset camera feed.\n"
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
  m_anchor_rotation_combo->addItem(tr("Off"), static_cast<int>(AnchorRotationMode::Off));
  m_anchor_rotation_combo->addItem(tr("Yaw Only"), static_cast<int>(AnchorRotationMode::YawOnly));
  m_anchor_rotation_combo->addItem(tr("Full"), static_cast<int>(AnchorRotationMode::Full));
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

  m_view_textures_button = new QPushButton(tr("Texture Hunter"));
  m_view_textures_button->setToolTip(
      tr("Browse currently-loaded textures and check the ones to add to this override."));

  m_import_textures_button = new QPushButton(tr("Import Texture"));
  m_import_textures_button->setToolTip(
      tr("Add textures by picking dumped texture files. Their hashes are read from the file names\n"
         "and merged into the list below. Opens the game's texture dump folder by default."));

  m_texture_hash_container = new QWidget;
  m_texture_hash_layout = new QVBoxLayout(m_texture_hash_container);
  m_texture_hash_layout->setContentsMargins(0, 0, 0, 0);
  m_texture_hash_layout->setSpacing(4);
  m_texture_hash_layout->setAlignment(Qt::AlignTop);
  m_texture_hash_scroll = new QScrollArea;
  m_texture_hash_scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  m_texture_hash_scroll->setWidgetResizable(true);
  m_texture_hash_scroll->setFrameShape(QFrame::NoFrame);
  m_texture_hash_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_texture_hash_scroll->setWidget(m_texture_hash_container);
  AddTextureHashField(QString());

  // Pre-fill fields in edit mode.
  if (edit_override)
  {
    m_name_edit->setText(QString::fromStdString(edit_override->name));
    m_comments_edit->setPlainText(QString::fromStdString(edit_override->comments));

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
    if (edit_override->handling == HandlingType::ControllerAnchor)
    {
      m_anchor_follow_rotation_check->setChecked(edit_override->anchor_rotation !=
                                                 AnchorRotationMode::Off);
      m_anchor_ctrl_yaw_spin->setValue(edit_override->anchor_yaw_deg);
      m_anchor_ctrl_pitch_spin->setValue(edit_override->anchor_pitch_deg);
      m_anchor_ctrl_roll_spin->setValue(edit_override->anchor_roll_deg);
    }
    m_flag_edit->setText(QString::fromStdString(edit_override->flag_group));
    {
      const QString condition = QString::fromStdString(edit_override->condition_flag);
      int idx = m_condition_combo->findText(condition);
      if (idx < 0 && !condition.isEmpty())
      {
        m_condition_combo->addItem(condition);
        idx = m_condition_combo->findText(condition);
      }
      m_condition_combo->setCurrentIndex(std::max(idx, 0));
    }
    {
      const int idx = m_condition_mode_combo->findData(edit_override->condition_inverted);
      if (idx >= 0)
        m_condition_mode_combo->setCurrentIndex(idx);
    }

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
  }

  auto* form = new QFormLayout;
  form->addRow(tr("Name:"), m_name_edit);
  form->addRow(tr("Handling:"), m_handling_combo);
  form->addRow(m_flag_label, m_flag_edit);
  form->addRow(m_condition_label, m_condition_combo);
  form->addRow(m_condition_mode_label, m_condition_mode_combo);
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
  form->addRow(QString(), m_view_textures_button);
  form->addRow(QString(), m_import_textures_button);
  form->addRow(tr("Texture Hashes:"), m_texture_hash_scroll);
  form->addRow(tr("Comments:"), m_comments_edit);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, this,
          &TextureElementOverrideAddEditDialog::OnAccept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_handling_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &TextureElementOverrideAddEditDialog::OnHandlingChanged);
  connect(m_condition_combo, &QComboBox::currentTextChanged, this, [this](const QString& text) {
    const bool has_condition = !text.trimmed().isEmpty();
    m_condition_mode_label->setEnabled(has_condition);
    m_condition_mode_combo->setEnabled(has_condition);
  });
  connect(m_view_textures_button, &QPushButton::clicked, this,
          &TextureElementOverrideAddEditDialog::ShowTextureBrowser);
  connect(m_import_textures_button, &QPushButton::clicked, this,
          &TextureElementOverrideAddEditDialog::OnImportTextures);

  auto* layout = new QVBoxLayout;
  layout->addLayout(form);
  layout->addWidget(buttons);
  setLayout(layout);

  const bool has_condition = !m_condition_combo->currentText().trimmed().isEmpty();
  m_condition_mode_label->setEnabled(has_condition);
  m_condition_mode_combo->setEnabled(has_condition);
  OnHandlingChanged();
}

TextureElementOverride TextureElementOverrideAddEditDialog::GetResult() const
{
  TextureElementOverride result;
  result.name = m_name_edit->text().toStdString();
  result.comments = m_comments_edit->toPlainText().trimmed().toStdString();
  result.handling = static_cast<HandlingType>(m_handling_combo->currentData().toInt());
  result.flag_group = m_flag_edit->text().trimmed().toStdString();
  if (result.handling != HandlingType::Flag)
  {
    result.condition_flag = m_condition_combo->currentText().trimmed().toStdString();
    result.condition_inverted =
        !result.condition_flag.empty() && m_condition_mode_combo->currentData().toBool();
  }
  result.element_depth = (result.handling == HandlingType::Screen ||
                          result.handling == HandlingType::HeadLocked) ?
                             static_cast<float>(m_element_depth_spin->value()) :
                             -1.0f;
  result.units_per_meter = result.handling == HandlingType::UnitsPerMeter ?
                               static_cast<float>(m_units_per_meter_spin->value()) :
                               -1.0f;
  result.passthrough_opacity = result.handling == HandlingType::Passthrough ?
                                   static_cast<float>(m_passthrough_opacity_spin->value()) :
                                   0.0f;
  if (result.handling == HandlingType::CameraAnchor)
  {
    result.anchor_right = static_cast<float>(m_anchor_right_spin->value());
    result.anchor_up = static_cast<float>(m_anchor_up_spin->value());
    result.anchor_forward = static_cast<float>(m_anchor_forward_spin->value());
    result.anchor_rotation =
        static_cast<AnchorRotationMode>(m_anchor_rotation_combo->currentData().toInt());
    result.anchor_yaw_deg = static_cast<float>(m_anchor_yaw_spin->value());
    result.anchor_units_per_meter = m_anchor_upm_spin->value() >= UPM_OVERRIDE_MIN ?
                                        static_cast<float>(m_anchor_upm_spin->value()) :
                                        -1.0f;
    result.anchor_hide = m_anchor_hide_check->isChecked();
  }
  if (result.handling == HandlingType::ControllerAnchor)
  {
    result.anchor_hand = m_anchor_hand_combo->currentData().toInt();
    result.anchor_right = static_cast<float>(m_anchor_right_spin->value());
    result.anchor_up = static_cast<float>(m_anchor_up_spin->value());
    result.anchor_forward = static_cast<float>(m_anchor_forward_spin->value());
    result.anchor_rotation = m_anchor_follow_rotation_check->isChecked() ?
                                 AnchorRotationMode::Full :
                                 AnchorRotationMode::Off;
    result.anchor_yaw_deg = static_cast<float>(m_anchor_ctrl_yaw_spin->value());
    result.anchor_pitch_deg = static_cast<float>(m_anchor_ctrl_pitch_spin->value());
    result.anchor_roll_deg = static_cast<float>(m_anchor_ctrl_roll_spin->value());
  }

  for (const std::string& token : CollectTextureHashTokens())
  {
    const u64 parsed = std::strtoull(token.c_str(), nullptr, 16);
    if (parsed != 0)
      result.texture_hashes.push_back(parsed);
  }
  std::sort(result.texture_hashes.begin(), result.texture_hashes.end());
  result.texture_hashes.erase(
      std::unique(result.texture_hashes.begin(), result.texture_hashes.end()),
      result.texture_hashes.end());

  result.enabled = true;
  return result;
}

void TextureElementOverrideAddEditDialog::OnAccept()
{
  if (m_name_edit->text().trimmed().isEmpty())
  {
    QMessageBox::warning(this, tr("Validation Error"), tr("Name cannot be empty."));
    return;
  }

  const auto texture_tokens = CollectTextureHashTokens();
  if (texture_tokens.empty())
  {
    QMessageBox::warning(this, tr("Validation Error"),
                         tr("Add at least one texture hash."));
    return;
  }
  for (const std::string& token : texture_tokens)
  {
    for (char c : token)
    {
      if (!std::isxdigit(static_cast<unsigned char>(c)))
      {
        QMessageBox::warning(this, tr("Validation Error"),
                             tr("Texture hashes must contain only hexadecimal characters "
                                "(0-9, a-f)."));
        return;
      }
    }
    if (token.size() > 16)
    {
      QMessageBox::warning(this, tr("Validation Error"),
                           tr("Each texture hash must be at most 16 hex digits."));
      return;
    }
  }

  const auto handling = static_cast<HandlingType>(m_handling_combo->currentData().toInt());
  if (handling == HandlingType::Flag && m_flag_edit->text().trimmed().isEmpty())
  {
    QMessageBox::warning(this, tr("Validation Error"),
                         tr("Flag handling requires a flag group name."));
    return;
  }
  if (handling == HandlingType::UnitsPerMeter && m_units_per_meter_spin->value() <= 0.0)
  {
    QMessageBox::warning(this, tr("Validation Error"),
                         tr("Units per Meter must be greater than 0."));
    return;
  }

  accept();
}

void TextureElementOverrideAddEditDialog::OnHandlingChanged()
{
  const auto handling = static_cast<HandlingType>(m_handling_combo->currentData().toInt());
  const bool show_element_depth =
      (handling == HandlingType::Screen || handling == HandlingType::HeadLocked);
  const bool show_units_per_meter = (handling == HandlingType::UnitsPerMeter);
  const bool show_passthrough = (handling == HandlingType::Passthrough);
  const bool show_anchor = (handling == HandlingType::CameraAnchor);
  const bool show_controller_anchor = (handling == HandlingType::ControllerAnchor);
  const bool show_anchor_offsets = show_anchor || show_controller_anchor;
  const bool is_flag = (handling == HandlingType::Flag);

  m_element_depth_label->setVisible(show_element_depth);
  m_element_depth_spin->setVisible(show_element_depth);
  m_units_per_meter_label->setVisible(show_units_per_meter);
  m_units_per_meter_spin->setVisible(show_units_per_meter);
  m_passthrough_opacity_label->setVisible(show_passthrough);
  m_passthrough_opacity_spin->setVisible(show_passthrough);
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
  // A flag group can be combined with any handling. Conditions are hidden for Flag-only entries,
  // matching the Shader and Elements Group editors.
  m_flag_label->setVisible(true);
  m_flag_edit->setVisible(true);
  m_condition_label->setVisible(!is_flag);
  m_condition_combo->setVisible(!is_flag);
  m_condition_mode_label->setVisible(!is_flag);
  m_condition_mode_combo->setVisible(!is_flag);
}

void TextureElementOverrideAddEditDialog::ShowTextureBrowser()
{
  TextureHashBrowserConfig browser_config;
  browser_config.title = tr("Texture Hunter");
  browser_config.empty_info_text =
      tr("No textures captured yet.\n"
         "Start a game in VR so its textures can be enumerated.");
  browser_config.current_label = tr("currently loaded");
  browser_config.initial_selected_hashes = CollectTextureHashValues();
  browser_config.fetch_current_entries = []() {
    std::vector<TextureHashBrowserEntry> entries;
    for (const auto& tex : TextureElementManager::GetInstance().GetCurrentTextures())
      entries.push_back(TextureHashBrowserEntry{.hash = tex.hash, .name = tex.name});
    return entries;
  };
  browser_config.apply_selected_hashes = [this](const std::vector<u64>& hashes) {
    SetTextureHashFields(hashes);
  };
  // Live preview: as textures are checked, skip/pink-highlight their draws in-game.
  browser_config.live_selection_changed = [](const std::vector<u64>& hashes) {
    TextureElementManager::GetInstance().SetPreviewTextures(hashes);
  };
  // Preview mode (Skip/Pink) toggle shown in the browser window.
  browser_config.preview_mode_changed = [](bool pink) {
    TextureElementManager::GetInstance().SetPreviewPink(pink);
  };

  // Enable global texture capture while the browser is open; restore on close.
  TextureElementManager::GetInstance().SetHunterActive(true);
  auto* dlg = ShowTextureHashBrowserDialog(this, browser_config);
  connect(dlg, &QObject::destroyed, this,
          []() { TextureElementManager::GetInstance().SetHunterActive(false); });
}

void TextureElementOverrideAddEditDialog::OnImportTextures()
{
  // Default to the game's texture dump folder (…/Dump/Textures/<GameID>) when it exists, falling
  // back to the base dump folder otherwise.
  QString start_dir;
  if (!m_game_id.empty())
  {
    const std::string game_dump_dir = File::GetUserPath(D_DUMPTEXTURES_IDX) + m_game_id;
    if (File::IsDirectory(game_dump_dir))
      start_dir = QString::fromStdString(game_dump_dir);
  }
  if (start_dir.isEmpty())
    start_dir = QString::fromStdString(File::GetUserPath(D_DUMPTEXTURES_IDX));

  const QStringList files = DolphinFileDialog::getOpenFileNames(
      this, tr("Import Textures"), start_dir,
      QStringLiteral("%1 (*.png *.dds);;%2 (*)").arg(tr("Texture Files")).arg(tr("All Files")));
  if (files.isEmpty())
    return;

  ImportTextureFiles(files);
}

void TextureElementOverrideAddEditDialog::ImportTextureFiles(const QStringList& files)
{
  std::vector<u64> imported_hashes;
  QStringList unparsed_files;
  for (const QString& path : files)
  {
    const std::string filename = QFileInfo(path).fileName().toStdString();
    const auto hash = TextureInfo::ParseTextureHash(filename);
    if (hash && *hash != 0)
      imported_hashes.push_back(*hash);
    else
      unparsed_files.append(QFileInfo(path).fileName());
  }

  // Merge the imported hashes into whatever is already in the fields, then de-duplicate.
  std::vector<u64> merged = CollectTextureHashValues();
  merged.insert(merged.end(), imported_hashes.begin(), imported_hashes.end());
  std::sort(merged.begin(), merged.end());
  merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
  SetTextureHashFields(merged);

  if (!unparsed_files.isEmpty())
  {
    QMessageBox::warning(
        this, tr("Import Textures"),
        tr("Could not read a texture hash from the following file name(s):\n\n%1")
            .arg(unparsed_files.join(QLatin1Char('\n'))));
  }
}

std::vector<std::string> TextureElementOverrideAddEditDialog::CollectTextureHashTokens() const
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

std::vector<u64> TextureElementOverrideAddEditDialog::CollectTextureHashValues() const
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

void TextureElementOverrideAddEditDialog::SetTextureHashFields(const std::vector<u64>& hashes)
{
  m_updating_texture_hash_fields = true;
  while (m_texture_hash_edits.size() < hashes.size())
    AddTextureHashField(QString());

  for (size_t i = 0; i < hashes.size(); i++)
  {
    m_texture_hash_edits[i]->setText(QString::fromStdString(fmt::format("{:016x}", hashes[i])));
  }
  for (size_t i = hashes.size(); i < m_texture_hash_edits.size(); i++)
    m_texture_hash_edits[i]->clear();

  m_updating_texture_hash_fields = false;
  EnsureTextureHashFieldRows();
}

void TextureElementOverrideAddEditDialog::AddTextureHashField(const QString& text)
{
  auto* edit = new TextureHashLineEdit([this](const QStringList& files) {
    // SetTextureHashFields can add or remove rows, including the drop target. Wait until Qt has
    // finished dispatching the drop event before changing the row widgets.
    QTimer::singleShot(0, this, [this, files] { ImportTextureFiles(files); });
  });
  edit->setPlaceholderText(tr("Hex hash"));
  edit->setToolTip(
      tr("Texture hash to match.\n"
         "Enter one hash per line; a new line is added automatically.\n"
         "You can paste multiple hashes separated by comma, semicolon, or spaces,\n"
         "or drop one or more dumped PNG/DDS texture files here."));
  edit->setText(text);

  // Thumbnail of the matching dumped texture, shown beside the hash.
  auto* preview = new QLabel;
  preview->setFixedSize(TEXTURE_HASH_PREVIEW_SIZE, TEXTURE_HASH_PREVIEW_SIZE);
  preview->setAlignment(Qt::AlignCenter);

  auto* row = new QWidget;
  row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  auto* row_layout = new QHBoxLayout(row);
  row_layout->setContentsMargins(0, 0, 0, 0);
  row_layout->setSpacing(6);
  row_layout->addWidget(edit, 1);
  row_layout->addWidget(preview);

  m_texture_hash_layout->addWidget(row);
  m_texture_hash_edits.push_back(edit);
  m_texture_hash_previews.push_back(preview);
  m_texture_hash_rows.push_back(row);

  connect(edit, &QLineEdit::textChanged, this, [this, edit, preview](const QString&) {
    SanitizeTextureHashField(edit);
    EnsureTextureHashFieldRows();
    UpdateTextureHashPreview(edit, preview);
  });

  UpdateTextureHashPreview(edit, preview);
}

void TextureElementOverrideAddEditDialog::EnsureTextureHashFieldRows()
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
    QWidget* row = m_texture_hash_rows.back();
    m_texture_hash_rows.pop_back();
    m_texture_hash_edits.pop_back();
    m_texture_hash_previews.pop_back();
    m_texture_hash_layout->removeWidget(row);
    delete row;  // Deletes its child edit and preview label too.
  }

  const int visible_rows = std::min(desired_rows, MAX_VISIBLE_TEXTURE_HASH_ROWS);
  const int viewport_height = visible_rows * TEXTURE_HASH_ROW_HEIGHT;
  m_texture_hash_scroll->setMinimumHeight(viewport_height);

  m_updating_texture_hash_fields = false;
}

void TextureElementOverrideAddEditDialog::EnsureDumpIndex()
{
  if (m_dump_index_built)
    return;
  m_dump_index_built = true;  // Build once; keep true even on failure so we don't rescan.

  if (m_game_id.empty())
    return;

  const QString dump_dir =
      QString::fromStdString(File::GetUserPath(D_DUMPTEXTURES_IDX) + m_game_id);
  QDir dir(dump_dir);
  if (!dir.exists())
    return;

  static const QStringList filters{QStringLiteral("*.png"), QStringLiteral("*.dds")};
  const QStringList files = dir.entryList(filters, QDir::Files, QDir::Name);
  for (const QString& filename : files)
  {
    const auto hash = TextureInfo::ParseTextureHash(filename.toStdString());
    if (!hash || *hash == 0)
      continue;
    // entryList is sorted by name, so the base texture ("…_14.png") is seen before its "_mip"/
    // "_arb" variants; emplace keeps that first (base) match.
    m_dump_hash_to_path.emplace(*hash, dir.absoluteFilePath(filename).toStdString());
  }
}

void TextureElementOverrideAddEditDialog::UpdateTextureHashPreview(QLineEdit* edit, QLabel* preview)
{
  if (edit == nullptr || preview == nullptr)
    return;

  EnsureDumpIndex();

  // Preview the first hash-looking token in the field.
  u64 hash = 0;
  const QRegularExpression hex_token(QStringLiteral("[0-9A-Fa-f]{1,16}"));
  const auto match = hex_token.match(edit->text());
  if (match.hasMatch())
    hash = std::strtoull(match.captured(0).toLower().toStdString().c_str(), nullptr, 16);

  const auto it = hash != 0 ? m_dump_hash_to_path.find(hash) : m_dump_hash_to_path.end();
  if (it == m_dump_hash_to_path.end())
  {
    preview->clear();
    preview->setToolTip(hash != 0 ? tr("No dumped texture found for this hash.") : QString());
    return;
  }

  const QString path = QString::fromStdString(it->second);
  const QPixmap pixmap(path);
  if (pixmap.isNull())
  {
    preview->clear();
    preview->setToolTip(QFileInfo(path).fileName());
    return;
  }

  preview->setPixmap(
      pixmap.scaled(preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
  preview->setToolTip(QFileInfo(path).fileName());
}

void TextureElementOverrideAddEditDialog::SanitizeTextureHashField(QLineEdit* edit)
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
