// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/ElementsGroupOverrideAddEditDialog.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <unordered_map>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "DolphinQt/Config/TextureHashBrowserDialog.h"

namespace
{
constexpr double UPM_OVERRIDE_MIN = 0.01;
constexpr double UPM_OVERRIDE_MAX = 1000.0;
constexpr double UPM_OVERRIDE_STEP = 0.01;
constexpr int MAX_VISIBLE_TEXTURE_HASH_ROWS = 10;
constexpr int TEXTURE_HASH_ROW_HEIGHT = 28;

QString ToHashHex(u64 hash)
{
  return QStringLiteral("%1").arg(static_cast<qulonglong>(hash), 16, 16, QLatin1Char('0'));
}

QString FormatRuntimeElementSummary(const ShaderHunter::RuntimeElementSignature& sig)
{
  if (!sig.valid)
    return QObject::tr("No runtime element captured.");

  QStringList parts;
  parts << QObject::tr("Projection: %1")
               .arg(sig.perspective ? QObject::tr("Perspective") : QObject::tr("Orthographic"));

  if (sig.perspective)
  {
    parts << QObject::tr("FOV: %1 / %2")
                 .arg(sig.perspective_hfov_x100 / 100.0, 0, 'f', 2)
                 .arg(sig.perspective_vfov_x100 / 100.0, 0, 'f', 2);
    parts << QObject::tr("Near/Far: %1 / %2")
                 .arg(sig.perspective_near_x1000 / 1000.0, 0, 'f', 3)
                 .arg(sig.perspective_far_x100 / 100.0, 0, 'f', 2);
  }
  else
  {
    parts << QObject::tr("Ortho: L%1 R%2 T%3 B%4")
                 .arg(sig.ortho_left_x100 / 100.0, 0, 'f', 2)
                 .arg(sig.ortho_right_x100 / 100.0, 0, 'f', 2)
                 .arg(sig.ortho_top_x100 / 100.0, 0, 'f', 2)
                 .arg(sig.ortho_bottom_x100 / 100.0, 0, 'f', 2);
    parts << QObject::tr("Layer: %1").arg(sig.ortho_layer);
  }

  parts << QObject::tr("Viewport: %1,%2 %3x%4")
               .arg(sig.viewport_x)
               .arg(sig.viewport_y)
               .arg(sig.viewport_width)
               .arg(sig.viewport_height);
  parts << QObject::tr("Scissor: %1,%2 %3,%4")
               .arg(sig.scissor_left)
               .arg(sig.scissor_top)
               .arg(sig.scissor_right)
               .arg(sig.scissor_bottom);
  parts << QObject::tr("State: alpha=%1 ztest=%2 zupdate=%3 zfunc=%4 blend=%5/%6")
               .arg(QStringLiteral("0x%1").arg(sig.alpha_test_hex, 8, 16, QLatin1Char('0')))
               .arg(sig.ztest ? QObject::tr("on") : QObject::tr("off"))
               .arg(sig.zupdate ? QObject::tr("on") : QObject::tr("off"))
               .arg(sig.zfunc)
               .arg(sig.blend_color_update ? QObject::tr("col") : QObject::tr("-"))
               .arg(sig.blend_alpha_update ? QObject::tr("alpha") : QObject::tr("-"));

  return parts.join(QStringLiteral("\n"));
}

QString FormatTextureSummary(const std::vector<u64>& textures)
{
  if (textures.empty())
    return QObject::tr("No textures");

  QStringList parts;
  const int shown = std::min<int>(2, static_cast<int>(textures.size()));
  for (int i = 0; i < shown; ++i)
  {
    parts << QStringLiteral("%1")
                 .arg(static_cast<qulonglong>(textures[static_cast<size_t>(i)]), 16, 16,
                      QLatin1Char('0'));
  }
  if (static_cast<int>(textures.size()) > shown)
    parts << QObject::tr("+%1 more").arg(static_cast<int>(textures.size()) - shown);
  return parts.join(QStringLiteral(", "));
}

bool SelectedSubgroupSignaturesEqual(const ElementsGroupManager::SelectedSubgroupSignature& lhs,
                                     const ElementsGroupManager::SelectedSubgroupSignature& rhs)
{
  return lhs.vs_family == rhs.vs_family && lhs.ps_family == rhs.ps_family &&
         lhs.gs_family == rhs.gs_family && lhs.texture_hashes == rhs.texture_hashes;
}

QString FormatSelectedMatchSummary(const ElementsGroupManager::SelectedSubgroupSignature& sig)
{
  QString label = QObject::tr("F %1/%2/%3")
               .arg(QStringLiteral("%1")
                        .arg(static_cast<qulonglong>(sig.vs_family), 16, 8, QLatin1Char('0')))
               .arg(QStringLiteral("%1")
                        .arg(static_cast<qulonglong>(sig.ps_family), 16, 8, QLatin1Char('0')))
               .arg(QStringLiteral("%1")
                        .arg(static_cast<qulonglong>(sig.gs_family), 16, 8, QLatin1Char('0')));
  label += QObject::tr(" | Tex %1").arg(FormatTextureSummary(sig.texture_hashes));
  if (sig.family_version < ShaderHunter::FAMILY_SCHEME_VERSION)
    label += QObject::tr(" [legacy - re-capture to survive shader updates]");
  return label;
}

std::vector<TextureHashBrowserEntry> CollectTextureBrowserEntries(
    const std::vector<ElementsGroupManager::DrawRecord>& draws)
{
  std::vector<TextureHashBrowserEntry> entries;
  std::unordered_map<u64, size_t> indices;

  for (const auto& draw : draws)
  {
    for (size_t i = 0; i < draw.textures.size(); ++i)
    {
      const u64 hash = draw.textures[i];
      if (hash == 0)
        continue;

      const auto it = indices.find(hash);
      if (it == indices.end())
      {
        indices.emplace(hash, entries.size());
        entries.push_back(TextureHashBrowserEntry{.hash = hash, .name = draw.texture_names[i]});
      }
      else if (entries[it->second].name.empty() && !draw.texture_names[i].empty())
      {
        entries[it->second].name = draw.texture_names[i];
      }
    }
  }

  std::sort(entries.begin(), entries.end(),
            [](const TextureHashBrowserEntry& a, const TextureHashBrowserEntry& b) {
              return a.hash < b.hash;
            });
  return entries;
}
}  // namespace

ElementsGroupOverrideAddEditDialog::ElementsGroupOverrideAddEditDialog(
    QWidget* parent, const ElementsGroupManager::ElementGroupOverride* edit_override,
    const std::vector<std::string>& available_flags)
    : QDialog(parent), m_available_flags(available_flags)
{
  setWindowTitle(edit_override ? tr("Edit Elements Group Override") :
                                 tr("Add Elements Group Override"));
  setMinimumWidth(420);

  m_name_edit = new QLineEdit;
  m_name_edit->setPlaceholderText(tr("Override name..."));

  m_comments_edit = new QPlainTextEdit;
  m_comments_edit->setPlaceholderText(tr("Optional notes..."));
  m_comments_edit->setTabChangesFocus(true);
  m_comments_edit->setMinimumHeight(70);

  m_credits_edit = new QLineEdit;
  m_credits_edit->setPlaceholderText(tr("Optional author/credits..."));

  m_match_kind_combo = new QComboBox;
  m_match_kind_combo->addItem(tr("Runtime Signature"),
                              static_cast<int>(ElementsGroupManager::MatchKind::RuntimeSignature));
  m_match_kind_combo->addItem(tr("Profile Layer"),
                              static_cast<int>(ElementsGroupManager::MatchKind::ProfileLayer));

  m_profile_label = new QLabel(tr("Profile:"));
  m_profile_combo = new QComboBox;
  for (MetroidElementProfile profile : GetMetroidElementProfiles())
  {
    const std::string_view label = MetroidElementProfileToDisplayName(profile);
    m_profile_combo->addItem(QString::fromUtf8(label.data(), static_cast<int>(label.size())),
                             static_cast<int>(profile));
  }

  m_profile_layers_label = new QLabel(tr("Profile Layers:"));
  m_profile_layers_list = new QListWidget;
  m_profile_layers_list->setMinimumHeight(160);
  for (MetroidElementLayer layer : GetMetroidElementLayers())
  {
    if (layer == MetroidElementLayer::Unknown)
      continue;
    const std::string_view label = MetroidElementLayerToDisplayName(layer);
    auto* item = new QListWidgetItem(
        QString::fromUtf8(label.data(), static_cast<int>(label.size())));
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
    item->setData(Qt::UserRole, static_cast<int>(layer));
    m_profile_layers_list->addItem(item);
  }

  m_handling_combo = new QComboBox;
  m_handling_combo->addItem(tr("Skip"),
                            static_cast<int>(ElementsGroupManager::HandlingType::Skip));
  m_handling_combo->addItem(tr("Screen"),
                            static_cast<int>(ElementsGroupManager::HandlingType::Screen));
  m_handling_combo->addItem(tr("Screen Pane"),
                            static_cast<int>(ElementsGroupManager::HandlingType::ScreenPane));
  m_handling_combo->addItem(tr("Fullscreen"),
                            static_cast<int>(ElementsGroupManager::HandlingType::Fullscreen));
  m_handling_combo->addItem(tr("Head Locked"),
                            static_cast<int>(ElementsGroupManager::HandlingType::HeadLocked));
  m_handling_combo->addItem(tr("Flag"),
                            static_cast<int>(ElementsGroupManager::HandlingType::Flag));
  m_handling_combo->addItem(
      tr("Units per Meter"),
      static_cast<int>(ElementsGroupManager::HandlingType::UnitsPerMeter));
  m_handling_combo->addItem(
      tr("Passthrough"), static_cast<int>(ElementsGroupManager::HandlingType::Passthrough));
  m_handling_combo->addItem(
      tr("Camera Anchor"), static_cast<int>(ElementsGroupManager::HandlingType::CameraAnchor));
  m_handling_combo->addItem(
      tr("Controller Anchor"),
      static_cast<int>(ElementsGroupManager::HandlingType::ControllerAnchor));

  m_preserve_stereo_efb_check = new QCheckBox(tr("Preserve Stereo EFB"));
  m_preserve_stereo_efb_check->setToolTip(
      tr("Preserve separate left- and right-eye EFB layers when this fullscreen draw uses a "
         "one-layer texture partially updated from a stereo EFB copy.\n"
         "Enable this only for effects that incorrectly duplicate the left eye into the right "
         "eye."));

  m_screen_pane_depth_label = new QLabel(tr("Screen Pane Depth:"));
  m_screen_pane_depth_combo = new QComboBox;
  m_screen_pane_depth_combo->addItem(
      tr("Original Game Depth"),
      static_cast<int>(ElementsGroupManager::ScreenPaneDepthMode::Game));
  m_screen_pane_depth_combo->addItem(
      tr("Physical VR Depth"),
      static_cast<int>(ElementsGroupManager::ScreenPaneDepthMode::VR));
  m_screen_pane_depth_combo->addItem(
      tr("Flat"), static_cast<int>(ElementsGroupManager::ScreenPaneDepthMode::Flat));
  m_screen_pane_depth_combo->setToolTip(
      tr("Original Game Depth preserves the game's depth-buffer ordering between separate draws.\n"
         "Physical VR Depth keeps one shared Z anchor for the complete matched element, preserving\n"
         "its multipart depth composition while X/Y remain fixed to the original screen pane.\n"
         "Flat uses the world-fixed 2D screen plane at the pane's original X/Y position, with\n"
         "zero visual thickness while retaining game depth for stable ordering."));

  m_element_depth_label = new QLabel(tr("Element Depth:"));
  m_element_depth_spin = new QDoubleSpinBox;
  m_element_depth_spin->setRange(-1.0, 1000.0);
  m_element_depth_spin->setDecimals(3);
  m_element_depth_spin->setSingleStep(0.1);
  m_element_depth_spin->setSpecialValueText(tr("Default"));
  m_element_depth_spin->setValue(-1.0);

  m_units_per_meter_label = new QLabel(tr("Units per Meter:"));
  m_units_per_meter_spin = new QDoubleSpinBox;
  m_units_per_meter_spin->setRange(0.0, UPM_OVERRIDE_MAX);
  m_units_per_meter_spin->setDecimals(2);
  m_units_per_meter_spin->setSingleStep(UPM_OVERRIDE_STEP);
  m_units_per_meter_spin->setSpecialValueText(tr("Default"));
  m_units_per_meter_spin->setValue(0.0);

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
         "untouched. Default = keep the global value. The scale eases in when the anchor\n"
         "engages and back out when it is released."));

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
  m_flag_edit->setPlaceholderText(tr("e.g. thermal_visor"));

  m_condition_label = new QLabel(tr("Condition Flag:"));
  m_condition_combo = new QComboBox;
  m_condition_combo->setEditable(true);
  m_condition_combo->addItem(QString());
  for (const std::string& flag : m_available_flags)
    m_condition_combo->addItem(QString::fromStdString(flag));

  m_condition_mode_label = new QLabel(tr("Condition Mode:"));
  m_condition_mode_combo = new QComboBox;
  m_condition_mode_combo->addItem(tr("Activate"), false);
  m_condition_mode_combo->addItem(tr("Deactivate"), true);

  m_capture_seed_button = new QPushButton(tr("Use Current Hunt Seed"));
  m_runtime_element_summary_label = new QLabel;
  m_runtime_element_summary_label->setWordWrap(true);
  m_runtime_use_projection_check = new QCheckBox(tr("Projection"));
  m_runtime_use_projection_check->setToolTip(
      tr("Match the full projection: type plus FOV (perspective) or bounds (orthographic)."));
  m_runtime_use_projection_type_check = new QCheckBox(tr("Projection Type"));
  m_runtime_use_projection_type_check->setToolTip(
      tr("Match only whether the draw is Perspective or Orthographic, ignoring the exact "
         "FOV/bounds values. More robust when the detailed projection drifts."));
  m_runtime_use_layer_check = new QCheckBox(tr("Layer"));
  m_runtime_use_viewport_check = new QCheckBox(tr("Viewport"));
  m_runtime_use_scissor_check = new QCheckBox(tr("Scissor"));
  m_runtime_use_render_state_check = new QCheckBox(tr("Render State"));

  m_clear_efb_check = new QCheckBox(tr("Clear EFB Copy"));
  m_clear_efb_check->setToolTip(
      tr("When this element is drawn, clear the next EFB-to-texture copy to transparent instead of\n"
         "copying the framebuffer content. Use to remove an effect captured via EFB copy.\n"
         "Independent of handling (can combine with Skip, Screen, etc.). Matching is on the full\n"
         "element signature, so only this element arms the clear."));
  m_clear_efb_min_label = new QLabel(tr("EFB Min Width:"));
  m_clear_efb_min_spin = new QSpinBox;
  m_clear_efb_min_spin->setRange(0, 640);
  m_clear_efb_min_spin->setSingleStep(10);
  m_clear_efb_min_spin->setSpecialValueText(tr("Any"));
  m_clear_efb_min_spin->setValue(0);
  m_clear_efb_min_spin->setToolTip(tr("Only clear EFB copies whose native width >= this value.\n"
                                      "0 (Any) = no lower bound. Full EFB = 640 wide."));
  m_clear_efb_max_label = new QLabel(tr("EFB Max Width:"));
  m_clear_efb_max_spin = new QSpinBox;
  m_clear_efb_max_spin->setRange(0, 640);
  m_clear_efb_max_spin->setSingleStep(10);
  m_clear_efb_max_spin->setSpecialValueText(tr("Any"));
  m_clear_efb_max_spin->setValue(0);
  m_clear_efb_max_spin->setToolTip(tr("Only clear EFB copies whose native width <= this value.\n"
                                      "0 (Any) = no upper bound."));

  m_texture_mode_combo = new QComboBox;
  m_texture_mode_combo->addItem(tr("Include"), false);
  m_texture_mode_combo->addItem(tr("Exclude"), true);
  m_selected_match_mode_combo = new QComboBox;
  m_selected_match_mode_combo->addItem(tr("Include"), false);
  m_selected_match_mode_combo->addItem(tr("Exclude"), true);
  m_view_textures_button = new QPushButton(tr("View Textures"));
  m_texture_hash_container = new QWidget;
  m_texture_hash_layout = new QVBoxLayout(m_texture_hash_container);
  m_texture_hash_layout->setContentsMargins(0, 0, 0, 0);
  m_texture_hash_layout->setSpacing(4);
  m_texture_hash_scroll = new QScrollArea;
  m_texture_hash_scroll->setWidgetResizable(true);
  m_texture_hash_scroll->setFrameShape(QFrame::NoFrame);
  m_texture_hash_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_texture_hash_scroll->setWidget(m_texture_hash_container);
  AddTextureHashField();
  EnsureTextureHashFieldRows();

  m_selected_match_list = new QListWidget;
  m_add_current_match_button = new QPushButton(tr("Add Current Hunt Match"));
  m_remove_selected_match_button = new QPushButton(tr("Remove Selected Match"));

  auto* form = new QFormLayout;
  form->addRow(tr("Name:"), m_name_edit);
  form->addRow(tr("Match Type:"), m_match_kind_combo);
  form->addRow(m_profile_label, m_profile_combo);
  form->addRow(m_profile_layers_label, m_profile_layers_list);
  form->addRow(tr("Handling:"), m_handling_combo);
  form->addRow(QString(), m_preserve_stereo_efb_check);
  form->addRow(m_screen_pane_depth_label, m_screen_pane_depth_combo);
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
  form->addRow(tr("Texture Mode:"), m_texture_mode_combo);
  form->addRow(tr("Texture Filters:"), m_texture_hash_scroll);
  form->addRow(tr("Selected Match Mode:"), m_selected_match_mode_combo);
  form->addRow(tr("Selected Matches:"), m_selected_match_list);
  form->addRow(QString(), m_clear_efb_check);
  form->addRow(m_clear_efb_min_label, m_clear_efb_min_spin);
  form->addRow(m_clear_efb_max_label, m_clear_efb_max_spin);

  auto* runtime_group_row = new QHBoxLayout;
  runtime_group_row->addWidget(m_runtime_use_projection_check);
  runtime_group_row->addWidget(m_runtime_use_projection_type_check);
  runtime_group_row->addWidget(m_runtime_use_layer_check);
  runtime_group_row->addWidget(m_runtime_use_viewport_check);
  runtime_group_row->addWidget(m_runtime_use_scissor_check);
  runtime_group_row->addWidget(m_runtime_use_render_state_check);

  auto* layout = new QVBoxLayout(this);
  layout->addLayout(form);
  layout->addWidget(m_capture_seed_button);
  layout->addWidget(m_runtime_element_summary_label);
  layout->addLayout(runtime_group_row);
  auto* selected_match_buttons = new QHBoxLayout;
  selected_match_buttons->addWidget(m_add_current_match_button);
  selected_match_buttons->addWidget(m_remove_selected_match_button);
  layout->addLayout(selected_match_buttons);
  auto* notes_form = new QFormLayout;
  notes_form->addRow(tr("Comments:"), m_comments_edit);
  notes_form->addRow(tr("Credits:"), m_credits_edit);
  layout->addLayout(notes_form);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  layout->addWidget(buttons);

  connect(buttons, &QDialogButtonBox::accepted, this,
          &ElementsGroupOverrideAddEditDialog::OnAccept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_match_kind_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &ElementsGroupOverrideAddEditDialog::RefreshMatchKindUi);
  connect(m_clear_efb_check, &QCheckBox::toggled, this,
          &ElementsGroupOverrideAddEditDialog::RefreshClearEFBUi);
  connect(m_handling_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &ElementsGroupOverrideAddEditDialog::RefreshHandlingUi);
  connect(m_capture_seed_button, &QPushButton::clicked, this,
          &ElementsGroupOverrideAddEditDialog::CaptureCurrentSeed);
  connect(m_view_textures_button, &QPushButton::clicked, this,
          &ElementsGroupOverrideAddEditDialog::ShowTextureBrowser);
  connect(m_add_current_match_button, &QPushButton::clicked, this,
          &ElementsGroupOverrideAddEditDialog::AddCurrentHuntMatch);
  connect(m_remove_selected_match_button, &QPushButton::clicked, this,
          &ElementsGroupOverrideAddEditDialog::RemoveSelectedMatchFilter);
  connect(m_selected_match_list, &QListWidget::itemSelectionChanged, this, [this]() {
    m_remove_selected_match_button->setEnabled(!m_selected_match_list->selectedItems().empty());
  });
  for (QCheckBox* checkbox : {m_runtime_use_projection_check, m_runtime_use_projection_type_check,
                              m_runtime_use_layer_check, m_runtime_use_viewport_check,
                              m_runtime_use_scissor_check, m_runtime_use_render_state_check})
  {
    connect(checkbox, &QCheckBox::toggled, this, [this]() {
      m_runtime_element.use_projection = m_runtime_use_projection_check->isChecked();
      m_runtime_element.use_projection_type = m_runtime_use_projection_type_check->isChecked();
      m_runtime_element.use_layer = m_runtime_use_layer_check->isChecked();
      m_runtime_element.use_viewport = m_runtime_use_viewport_check->isChecked();
      m_runtime_element.use_scissor = m_runtime_use_scissor_check->isChecked();
      m_runtime_element.use_render_state = m_runtime_use_render_state_check->isChecked();
      RefreshRuntimeElementSummary();
    });
  }

  if (edit_override)
  {
    m_name_edit->setText(QString::fromStdString(edit_override->name));
    m_comments_edit->setPlainText(QString::fromStdString(edit_override->comments));
    m_credits_edit->setText(QString::fromStdString(edit_override->credits));
    {
      const int idx = m_match_kind_combo->findData(static_cast<int>(edit_override->match_kind));
      if (idx >= 0)
        m_match_kind_combo->setCurrentIndex(idx);
    }
    {
      const int idx = m_profile_combo->findData(static_cast<int>(edit_override->profile_id));
      if (idx >= 0)
        m_profile_combo->setCurrentIndex(idx);
      SetProfileLayers(edit_override->profile_layers);
    }
    {
      const auto handling =
          edit_override->handling == ElementsGroupManager::HandlingType::FullscreenMono ?
              ElementsGroupManager::HandlingType::Fullscreen :
              edit_override->handling;
      const int handling_idx = m_handling_combo->findData(static_cast<int>(handling));
      if (handling_idx >= 0)
        m_handling_combo->setCurrentIndex(handling_idx);
    }
    m_preserve_stereo_efb_check->setChecked(edit_override->preserve_stereo_efb);
    {
      const int depth_idx =
          m_screen_pane_depth_combo->findData(static_cast<int>(edit_override->screen_pane_depth));
      if (depth_idx >= 0)
        m_screen_pane_depth_combo->setCurrentIndex(depth_idx);
    }
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
    if (edit_override->handling == ElementsGroupManager::HandlingType::ControllerAnchor)
    {
      m_anchor_follow_rotation_check->setChecked(edit_override->anchor_rotation !=
                                                 ShaderHunter::AnchorRotationMode::Off);
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
    {
      const int idx = m_texture_mode_combo->findData(edit_override->texture_hashes_excluded);
      if (idx >= 0)
        m_texture_mode_combo->setCurrentIndex(idx);
    }
    SetTextureHashValues(edit_override->texture_hashes);
    m_runtime_element = edit_override->runtime_element;
    m_clear_efb_check->setChecked(edit_override->clear_efb);
    m_clear_efb_min_spin->setValue(edit_override->clear_efb_min_width);
    m_clear_efb_max_spin->setValue(edit_override->clear_efb_max_width);
    m_selected_match_filters = edit_override->selected_match_filter;
    m_selected_match_filters_excluded = edit_override->selected_match_filter_excluded;
  }
  else
  {
    m_name_edit->setText(tr("Element Override"));
  }

  for (const auto& filter : m_selected_match_filters)
    m_selected_match_list->addItem(FormatSelectedMatchSummary(filter));
  {
    const int idx = m_selected_match_mode_combo->findData(m_selected_match_filters_excluded);
    if (idx >= 0)
      m_selected_match_mode_combo->setCurrentIndex(idx);
  }
  m_remove_selected_match_button->setEnabled(false);

  RefreshHandlingUi();
  RefreshMatchKindUi();
  RefreshClearEFBUi();
  RefreshRuntimeElementSummary();
}

ElementsGroupManager::ElementGroupOverride ElementsGroupOverrideAddEditDialog::GetResult() const
{
  ElementsGroupManager::ElementGroupOverride result;
  result.name = m_name_edit->text().trimmed().toStdString();
  result.comments = m_comments_edit->toPlainText().trimmed().toStdString();
  result.credits = m_credits_edit->text().trimmed().toStdString();
  result.match_kind = static_cast<ElementsGroupManager::MatchKind>(
      m_match_kind_combo->currentData().toInt());
  result.handling = static_cast<ElementsGroupManager::HandlingType>(
      m_handling_combo->currentData().toInt());
  result.preserve_stereo_efb =
      result.handling == ElementsGroupManager::HandlingType::Fullscreen &&
      m_preserve_stereo_efb_check->isChecked();
  result.screen_pane_depth = static_cast<ElementsGroupManager::ScreenPaneDepthMode>(
      m_screen_pane_depth_combo->currentData().toInt());
  result.runtime_element = m_runtime_element;
  result.profile_id = static_cast<MetroidElementProfile>(m_profile_combo->currentData().toInt());
  result.profile_layers = CollectProfileLayers();
  result.element_depth = m_element_depth_spin->value();
  result.units_per_meter = m_units_per_meter_spin->value() > 0.0 ? m_units_per_meter_spin->value() :
                                                                  -1.0f;
  result.passthrough_opacity =
      result.handling == ElementsGroupManager::HandlingType::Passthrough ?
          static_cast<float>(m_passthrough_opacity_spin->value()) :
          0.0f;
  if (result.handling == ElementsGroupManager::HandlingType::CameraAnchor)
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
  if (result.handling == ElementsGroupManager::HandlingType::ControllerAnchor)
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
  result.texture_hashes = CollectTextureHashValues();
  result.texture_hashes_excluded =
      !result.texture_hashes.empty() && m_texture_mode_combo->currentData().toBool();
  result.selected_match_filter = m_selected_match_filters;
  result.selected_match_filter_excluded =
      !result.selected_match_filter.empty() && m_selected_match_mode_combo->currentData().toBool();
  result.clear_efb = m_clear_efb_check->isChecked();
  result.clear_efb_min_width = result.clear_efb ? m_clear_efb_min_spin->value() : 0;
  result.clear_efb_max_width = result.clear_efb ? m_clear_efb_max_spin->value() : 0;
  result.flag_group = m_flag_edit->text().trimmed().toStdString();
  result.condition_flag = m_condition_combo->currentText().trimmed().toStdString();
  result.condition_inverted =
      !result.condition_flag.empty() && m_condition_mode_combo->currentData().toBool();
  if (result.match_kind == ElementsGroupManager::MatchKind::ProfileLayer)
  {
    result.runtime_element = {};
    result.selected_match_filter.clear();
    result.selected_match_filter_excluded = false;
  }
  else
  {
    result.profile_id = MetroidElementProfile::None;
    result.profile_layers.clear();
  }
  return result;
}

std::vector<u64> ElementsGroupOverrideAddEditDialog::CollectTextureHashValues() const
{
  std::vector<u64> hashes;
  hashes.reserve(m_texture_hash_edits.size());
  for (QLineEdit* edit : m_texture_hash_edits)
  {
    const QString text = edit->text().trimmed();
    if (text.isEmpty())
      continue;
    const u64 parsed = std::strtoull(text.toStdString().c_str(), nullptr, 16);
    if (parsed != 0)
      hashes.push_back(parsed);
  }
  std::sort(hashes.begin(), hashes.end());
  hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
  return hashes;
}

void ElementsGroupOverrideAddEditDialog::SetTextureHashValues(const std::vector<u64>& hashes)
{
  m_updating_texture_hash_fields = true;
  while (m_texture_hash_edits.size() < hashes.size())
    AddTextureHashField();
  for (size_t i = 0; i < hashes.size(); ++i)
    m_texture_hash_edits[i]->setText(ToHashHex(hashes[i]));
  for (size_t i = hashes.size(); i < m_texture_hash_edits.size(); ++i)
    m_texture_hash_edits[i]->clear();
  m_updating_texture_hash_fields = false;
  EnsureTextureHashFieldRows();
}

void ElementsGroupOverrideAddEditDialog::AddTextureHashField(const QString& text)
{
  auto* edit = new QLineEdit;
  edit->setPlaceholderText(tr("0000000000000000"));
  edit->setText(text);
  connect(edit, &QLineEdit::editingFinished, this,
          [this, edit]() { SanitizeTextureHashField(edit); });
  connect(edit, &QLineEdit::textChanged, this,
          [this]() { EnsureTextureHashFieldRows(); });
  m_texture_hash_layout->addWidget(edit);
  m_texture_hash_edits.push_back(edit);
}

void ElementsGroupOverrideAddEditDialog::EnsureTextureHashFieldRows()
{
  if (m_updating_texture_hash_fields)
    return;

  m_updating_texture_hash_fields = true;
  int filled = 0;
  for (QLineEdit* edit : m_texture_hash_edits)
  {
    if (!edit->text().trimmed().isEmpty())
      ++filled;
  }

  const int desired_rows = std::max(1, filled + 1);
  while (static_cast<int>(m_texture_hash_edits.size()) < desired_rows)
    AddTextureHashField();
  while (static_cast<int>(m_texture_hash_edits.size()) > desired_rows)
  {
    QLineEdit* edit = m_texture_hash_edits.back();
    if (!edit->text().trimmed().isEmpty())
      break;
    m_texture_hash_edits.pop_back();
    m_texture_hash_layout->removeWidget(edit);
    delete edit;
  }
  const int visible_rows = std::min<int>(desired_rows, MAX_VISIBLE_TEXTURE_HASH_ROWS);
  const int viewport_height = visible_rows * TEXTURE_HASH_ROW_HEIGHT;
  m_texture_hash_scroll->setMinimumHeight(viewport_height);
  m_texture_hash_scroll->setMaximumHeight(viewport_height);
  m_updating_texture_hash_fields = false;
}

void ElementsGroupOverrideAddEditDialog::SanitizeTextureHashField(QLineEdit* edit)
{
  if (edit == nullptr || m_updating_texture_hash_fields)
    return;

  QString text = edit->text().trimmed().toLower();
  QString filtered;
  filtered.reserve(text.size());
  for (const QChar c : text)
  {
    if (c.isDigit() || (c >= QLatin1Char('a') && c <= QLatin1Char('f')))
      filtered.append(c);
  }
  if (filtered.size() > 16)
    filtered = filtered.right(16);
  edit->setText(filtered);
}

void ElementsGroupOverrideAddEditDialog::RefreshRuntimeElementSummary()
{
  QString summary = FormatRuntimeElementSummary(m_runtime_element);
  QStringList groups;
  if (m_runtime_element.use_projection)
    groups << tr("Projection");
  if (m_runtime_element.use_projection_type)
    groups << tr("Projection Type");
  if (m_runtime_element.use_layer)
    groups << tr("Layer");
  if (m_runtime_element.use_viewport)
    groups << tr("Viewport");
  if (m_runtime_element.use_scissor)
    groups << tr("Scissor");
  if (m_runtime_element.use_render_state)
    groups << tr("Render State");

  summary += tr("\nActive Groups: %1")
                 .arg(groups.isEmpty() ? tr("(none)") : groups.join(QStringLiteral(", ")));
  m_runtime_element_summary_label->setText(summary);

  const QSignalBlocker projection_blocker(m_runtime_use_projection_check);
  const QSignalBlocker projection_type_blocker(m_runtime_use_projection_type_check);
  const QSignalBlocker layer_blocker(m_runtime_use_layer_check);
  const QSignalBlocker viewport_blocker(m_runtime_use_viewport_check);
  const QSignalBlocker scissor_blocker(m_runtime_use_scissor_check);
  const QSignalBlocker render_blocker(m_runtime_use_render_state_check);
  m_runtime_use_projection_check->setChecked(m_runtime_element.use_projection);
  m_runtime_use_projection_type_check->setChecked(m_runtime_element.use_projection_type);
  m_runtime_use_layer_check->setChecked(m_runtime_element.use_layer);
  m_runtime_use_viewport_check->setChecked(m_runtime_element.use_viewport);
  m_runtime_use_scissor_check->setChecked(m_runtime_element.use_scissor);
  m_runtime_use_render_state_check->setChecked(m_runtime_element.use_render_state);
}

void ElementsGroupOverrideAddEditDialog::RefreshHandlingUi()
{
  const auto handling = static_cast<ElementsGroupManager::HandlingType>(
      m_handling_combo->currentData().toInt());
  const bool show_element_depth = (handling == ElementsGroupManager::HandlingType::Screen ||
                                   handling == ElementsGroupManager::HandlingType::HeadLocked);
  const bool show_screen_pane_depth =
      handling == ElementsGroupManager::HandlingType::ScreenPane;
  const bool show_preserve_stereo_efb =
      handling == ElementsGroupManager::HandlingType::Fullscreen;
  const bool show_units_per_meter =
      (handling == ElementsGroupManager::HandlingType::UnitsPerMeter);
  const bool show_passthrough = (handling == ElementsGroupManager::HandlingType::Passthrough);
  const bool show_anchor = (handling == ElementsGroupManager::HandlingType::CameraAnchor);
  const bool show_controller_anchor =
      (handling == ElementsGroupManager::HandlingType::ControllerAnchor);
  const bool show_anchor_offsets = show_anchor || show_controller_anchor;
  const bool is_flag = (handling == ElementsGroupManager::HandlingType::Flag);

  m_element_depth_label->setVisible(show_element_depth);
  m_element_depth_spin->setVisible(show_element_depth);
  m_screen_pane_depth_label->setVisible(show_screen_pane_depth);
  m_screen_pane_depth_combo->setVisible(show_screen_pane_depth);
  m_preserve_stereo_efb_check->setVisible(show_preserve_stereo_efb);
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
  m_condition_label->setVisible(!is_flag);
  m_condition_combo->setVisible(!is_flag);
  m_condition_mode_label->setVisible(!is_flag);
  m_condition_mode_combo->setVisible(!is_flag);
}

std::vector<MetroidElementLayer> ElementsGroupOverrideAddEditDialog::CollectProfileLayers() const
{
  std::vector<MetroidElementLayer> layers;
  for (int i = 0; i < m_profile_layers_list->count(); ++i)
  {
    const QListWidgetItem* item = m_profile_layers_list->item(i);
    if (item->checkState() == Qt::Checked)
      layers.push_back(static_cast<MetroidElementLayer>(item->data(Qt::UserRole).toInt()));
  }
  return layers;
}

void ElementsGroupOverrideAddEditDialog::SetProfileLayers(
    const std::vector<MetroidElementLayer>& layers)
{
  for (int i = 0; i < m_profile_layers_list->count(); ++i)
  {
    QListWidgetItem* item = m_profile_layers_list->item(i);
    const auto layer = static_cast<MetroidElementLayer>(item->data(Qt::UserRole).toInt());
    item->setCheckState(std::find(layers.begin(), layers.end(), layer) != layers.end() ?
                            Qt::Checked :
                            Qt::Unchecked);
  }
}

void ElementsGroupOverrideAddEditDialog::RefreshMatchKindUi()
{
  const auto match_kind = static_cast<ElementsGroupManager::MatchKind>(
      m_match_kind_combo->currentData().toInt());
  const bool profile_layer = match_kind == ElementsGroupManager::MatchKind::ProfileLayer;

  m_profile_label->setVisible(profile_layer);
  m_profile_combo->setVisible(profile_layer);
  m_profile_layers_label->setVisible(profile_layer);
  m_profile_layers_list->setVisible(profile_layer);

  m_capture_seed_button->setVisible(!profile_layer);
  m_runtime_element_summary_label->setVisible(!profile_layer);
  m_runtime_use_projection_check->setVisible(!profile_layer);
  m_runtime_use_projection_type_check->setVisible(!profile_layer);
  m_runtime_use_layer_check->setVisible(!profile_layer);
  m_runtime_use_viewport_check->setVisible(!profile_layer);
  m_runtime_use_scissor_check->setVisible(!profile_layer);
  m_runtime_use_render_state_check->setVisible(!profile_layer);
  m_selected_match_mode_combo->setVisible(!profile_layer);
  m_selected_match_list->setVisible(!profile_layer);
  m_add_current_match_button->setVisible(!profile_layer);
  m_remove_selected_match_button->setVisible(!profile_layer);
}

void ElementsGroupOverrideAddEditDialog::RefreshClearEFBUi()
{
  // Clear EFB works for both runtime-signature and profile-layer matches, so it stays visible in
  // either mode; only the width bounds depend on the checkbox.
  const bool show = m_clear_efb_check->isChecked();
  m_clear_efb_min_label->setVisible(show);
  m_clear_efb_min_spin->setVisible(show);
  m_clear_efb_max_label->setVisible(show);
  m_clear_efb_max_spin->setVisible(show);
}

void ElementsGroupOverrideAddEditDialog::CaptureCurrentSeed()
{
  const auto status = ElementsGroupManager::GetInstance().GetStatus();
  if (!status.seed_valid)
  {
    QMessageBox::warning(this, tr("Elements Group Override"),
                         tr("No active Elements Hunting seed is available."));
    return;
  }

  m_runtime_element = status.seed_signature;
  RefreshRuntimeElementSummary();
}

void ElementsGroupOverrideAddEditDialog::ShowTextureBrowser()
{
  const auto status = ElementsGroupManager::GetInstance().GetStatus();
  if (!status.seed_valid)
  {
    QMessageBox::warning(this, tr("View Textures"),
                         tr("No active Elements Hunting preview scope is available.\n"
                            "Open Elements Hunting and select a seed first."));
    return;
  }

  TextureHashBrowserConfig browser_config;
  browser_config.title = tr("Textures for Current Element Selection");
  browser_config.empty_info_text =
      tr("No textures were captured yet for the current preview scope.");
  browser_config.current_label = tr("current preview scope");
  browser_config.fetch_current_label = []() {
    const auto current_status = ElementsGroupManager::GetInstance().GetStatus();
    return current_status.selected_match_filter_count > 0 ?
               (current_status.selected_match_filter_excluded ? QObject::tr("unchecked matches only") :
                                                                QObject::tr("checked matches only")) :
               QObject::tr("whole current group");
  };
  browser_config.initial_selected_hashes = CollectTextureHashValues();
  browser_config.fetch_current_entries = []() {
    return CollectTextureBrowserEntries(
        ElementsGroupManager::GetInstance().GetCurrentTextureSourceDraws());
  };
  browser_config.apply_selected_hashes = [this](const std::vector<u64>& hashes) {
    SetTextureHashValues(hashes);
  };

  ShowTextureHashBrowserDialog(this, browser_config);
}

void ElementsGroupOverrideAddEditDialog::AddCurrentHuntMatch()
{
  const auto status = ElementsGroupManager::GetInstance().GetStatus();
  if (!status.highlighted_draw)
  {
    QMessageBox::warning(this, tr("Elements Group Override"),
                         tr("No active Elements Hunting match is available."));
    return;
  }

  const auto matches = ElementsGroupManager::GetInstance().GetCurrentMatches();
  if (status.selected_match < 0 || status.selected_match >= static_cast<int>(matches.size()))
  {
    QMessageBox::warning(this, tr("Elements Group Override"),
                         tr("No stable Elements Hunting match is available."));
    return;
  }

  const auto filter = matches[static_cast<size_t>(status.selected_match)].subgroup;
  const auto it = std::find_if(m_selected_match_filters.begin(), m_selected_match_filters.end(),
                               [&filter](const ElementsGroupManager::SelectedSubgroupSignature& existing) {
                                 return SelectedSubgroupSignaturesEqual(existing, filter);
                               });
  if (it != m_selected_match_filters.end())
    return;

  m_selected_match_filters.push_back(filter);
  m_selected_match_list->addItem(FormatSelectedMatchSummary(filter));
}

void ElementsGroupOverrideAddEditDialog::RemoveSelectedMatchFilter()
{
  const auto items = m_selected_match_list->selectedItems();
  if (items.empty())
    return;

  const int row = m_selected_match_list->row(items[0]);
  if (row < 0 || row >= static_cast<int>(m_selected_match_filters.size()))
    return;

  m_selected_match_filters.erase(m_selected_match_filters.begin() + row);
  delete m_selected_match_list->takeItem(row);
}

void ElementsGroupOverrideAddEditDialog::OnAccept()
{
  const auto result = GetResult();
  if (result.name.empty())
  {
    QMessageBox::warning(this, tr("Elements Group Override"), tr("Name cannot be empty."));
    return;
  }
  if (result.match_kind == ElementsGroupManager::MatchKind::ProfileLayer)
  {
    if (result.profile_id == MetroidElementProfile::None)
    {
      QMessageBox::warning(this, tr("Elements Group Override"),
                           tr("A Metroid profile is required."));
      return;
    }
    if (result.profile_layers.empty())
    {
      QMessageBox::warning(this, tr("Elements Group Override"),
                           tr("Select at least one profile layer."));
      return;
    }
  }
  else if (!result.runtime_element.valid)
  {
    QMessageBox::warning(this, tr("Elements Group Override"),
                         tr("A runtime element signature is required."));
    return;
  }
  if (result.match_kind == ElementsGroupManager::MatchKind::RuntimeSignature &&
      !result.runtime_element.use_projection && !result.runtime_element.use_projection_type &&
      !result.runtime_element.use_layer && !result.runtime_element.use_viewport &&
      !result.runtime_element.use_scissor && !result.runtime_element.use_render_state)
  {
    QMessageBox::warning(this, tr("Elements Group Override"),
                         tr("Enable at least one runtime signature group."));
    return;
  }
  if (result.handling == ElementsGroupManager::HandlingType::Flag && result.flag_group.empty())
  {
    QMessageBox::warning(this, tr("Elements Group Override"),
                         tr("Flag handling requires a flag group name."));
    return;
  }

  accept();
}
