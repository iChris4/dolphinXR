// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Settings/VRPane.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

#include "Common/Config/Config.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/System.h"

#include "DolphinQt/Config/HideObjectAddEditDialog.h"
#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"
#include "DolphinQt/Config/ConfigControls/ConfigFloatSlider.h"
#include "DolphinQt/Config/ConfigControls/ConfigSlider.h"
#include "DolphinQt/Debugger/CullingCodeFinderWidget.h"
#include "DolphinQt/Debugger/ShaderHunterWidget.h"
#include "DolphinQt/Settings.h"
#include "VideoCommon/HideObjectEngine.h"
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/VideoConfig.h"

VRPane::VRPane(QWidget* parent) : QWidget(parent)
{
  auto* main_layout = new QVBoxLayout;
  auto* tabs = new QTabWidget(this);
  auto* general_tab = new QWidget(this);
  auto* general_layout = new QVBoxLayout;
  auto* hack_tab = new QWidget(this);
  auto* hack_layout = new QVBoxLayout;
  auto* tools_tab = new QWidget(this);
  auto* tools_tab_layout = new QVBoxLayout;

  general_tab->setLayout(general_layout);
  hack_tab->setLayout(hack_layout);
  tools_tab->setLayout(tools_tab_layout);
  tabs->addTab(general_tab, tr("General"));
  tabs->addTab(hack_tab, tr("Advanced"));
  tabs->addTab(tools_tab, tr("Tools"));

  auto* openxr_group = new QGroupBox(tr("OpenXR"));
  auto* openxr_layout = new QGridLayout;
  openxr_group->setLayout(openxr_layout);
  auto* camera_group = new QGroupBox(tr("Camera"));
  auto* camera_layout = new QGridLayout;
  camera_group->setLayout(camera_layout);
  auto* virtual_screen_group = new QGroupBox(tr("Virtual Screen"));
  auto* virtual_screen_layout = new QGridLayout;
  virtual_screen_group->setLayout(virtual_screen_layout);
  auto* framerate_group = new QGroupBox(tr("Framerate"));
  auto* framerate_layout = new QGridLayout;
  framerate_group->setLayout(framerate_layout);
  auto* rendering_group = new QGroupBox(tr("Rendering"));
  auto* rendering_layout = new QGridLayout;
  rendering_group->setLayout(rendering_layout);
  auto* passthrough_group = new QGroupBox(tr("Passthrough"));
  auto* passthrough_layout = new QGridLayout;
  passthrough_group->setLayout(passthrough_layout);

  m_enable_openxr = new ConfigBool(tr("Enable VR"), Config::GFX_VR_ENABLE_OPENXR);
  m_flat_screen = new ConfigBool(tr("Flat Screen (2D, no stereo)"), Config::GFX_VR_FLAT_SCREEN);
  m_reference_space_mode = new ConfigChoiceMap<OpenXRReferenceSpaceMode>(
      {{tr("LOCAL"), OpenXRReferenceSpaceMode::Local},
       {tr("STAGE + Height"), OpenXRReferenceSpaceMode::StageHeight},
       {tr("STAGE"), OpenXRReferenceSpaceMode::Stage}},
      Config::GFX_VR_REFERENCE_SPACE_MODE);
  m_tracking_mode = new ConfigChoiceMap<OpenXRTrackingMode>(
      {{tr("6DoF"), OpenXRTrackingMode::Full6DoF},
       {tr("3DoF"), OpenXRTrackingMode::Rotation3DoF},
       {tr("None"), OpenXRTrackingMode::None}},
      Config::GFX_VR_TRACKING_MODE);
  m_use_vulkan_multiview = new ConfigBool(tr("Use Vulkan Multiview"),
                                          Config::GFX_VR_USE_VULKAN_MULTIVIEW);
  // Exponential scale: precise near the minimum, while still reaching large values at the far end.
  m_units_per_meter = new ConfigFloatSlider(Config::GFX_VR_UNITS_PER_METER_MIN,
                                            Config::GFX_VR_UNITS_PER_METER_MAX,
                                            Config::GFX_VR_UNITS_PER_METER,
                                            Config::GFX_VR_UNITS_PER_METER_STEP, nullptr,
                                            ConfigFloatSlider::ScaleMode::Exponential);
  m_units_per_meter_value = new QLabel();
  m_enable_lean_back_angle =
      new ConfigBool(tr("Lean Back Angle (deg)"), Config::GFX_VR_ENABLE_LEAN_BACK_ANGLE);
  m_enable_lean_back_angle->setToolTip(
      tr("When unchecked, ignores the Lean Back Angle value below and applies no pitch offset."));
  m_lean_back_angle = new ConfigFloatSlider(Config::GFX_VR_LEAN_BACK_ANGLE_MIN,
                                            Config::GFX_VR_LEAN_BACK_ANGLE_MAX,
                                            Config::GFX_VR_LEAN_BACK_ANGLE,
                                            Config::GFX_VR_LEAN_BACK_ANGLE_STEP);
  m_lean_back_angle_value = new QLabel();
  m_enable_camera_forward =
      new ConfigBool(tr("Camera Forward (m)"), Config::GFX_VR_ENABLE_CAMERA_FORWARD);
  m_enable_camera_forward->setToolTip(
      tr("When unchecked, ignores the Camera Forward value below and applies no "
         "forward/backward offset."));
  m_camera_forward = new ConfigFloatSlider(Config::GFX_VR_CAMERA_FORWARD_MIN,
                                           Config::GFX_VR_CAMERA_FORWARD_MAX,
                                           Config::GFX_VR_CAMERA_FORWARD,
                                           Config::GFX_VR_CAMERA_FORWARD_STEP);
  m_camera_forward_value = new QLabel();
  m_enable_camera_height =
      new ConfigBool(tr("Camera Height (m)"), Config::GFX_VR_ENABLE_CAMERA_HEIGHT);
  m_enable_camera_height->setToolTip(
      tr("When unchecked, ignores the Camera Height value below and applies no vertical "
         "offset."));
  m_camera_height = new ConfigFloatSlider(Config::GFX_VR_CAMERA_HEIGHT_MIN,
                                          Config::GFX_VR_CAMERA_HEIGHT_MAX,
                                          Config::GFX_VR_CAMERA_HEIGHT,
                                          Config::GFX_VR_CAMERA_HEIGHT_STEP);
  m_camera_height_value = new QLabel();
  m_enable_camera_anchor = new ConfigBool(tr("Camera Anchor"), Config::GFX_VR_ENABLE_CAMERA_ANCHOR);
  m_enable_camera_anchor->setToolTip(
      tr("Let a \"Camera Anchor\" Elements Group Override move the VR camera to a game\n"
         "element's position (e.g. a character's head for first-person view). When no\n"
         "anchor override matches, or this is disabled, the camera stays at the default\n"
         "position."));
  m_camera_anchor_smoothing = new ConfigFloatSlider(Config::GFX_VR_CAMERA_ANCHOR_SMOOTHING_MIN,
                                                    Config::GFX_VR_CAMERA_ANCHOR_SMOOTHING_MAX,
                                                    Config::GFX_VR_CAMERA_ANCHOR_SMOOTHING,
                                                    Config::GFX_VR_CAMERA_ANCHOR_SMOOTHING_STEP);
  m_camera_anchor_smoothing->setToolTip(
      tr("How much the anchored camera resists the element's movement each frame.\n"
         "0.00 follows the element rigidly (transmits every animation bob);\n"
         "higher values smooth the motion for comfort."));
  m_camera_anchor_smoothing_value = new QLabel();
  m_enable_controller_anchor =
      new ConfigBool(tr("Controller Anchor"), Config::GFX_VR_ENABLE_CONTROLLER_ANCHOR);
  m_enable_controller_anchor->setToolTip(
      tr("Let \"Controller Anchor\" Elements Group Overrides reposition game elements to a\n"
         "VR controller (e.g. a sword on the right hand). Position only; the element keeps\n"
         "its game orientation. When no override matches, or this is disabled, elements\n"
         "render where the game put them."));

  openxr_layout->addWidget(m_enable_openxr, 0, 0, 1, 3);

  m_passthrough = new ConfigBool(tr("Enable Passthrough"), Config::GFX_VR_PASSTHROUGH);
  m_passthrough_remove_black_bg =
      new ConfigBool(tr("Reveal Unrendered Areas"), Config::GFX_VR_PASSTHROUGH_REMOVE_BLACK_BG);
  m_passthrough_remove_black_clears = new ConfigBool(
      tr("Remove Black EFB Clears"), Config::GFX_VR_PASSTHROUGH_REMOVE_BLACK_CLEARS);
  m_passthrough_scene_opacity =
      new ConfigFloatSlider(Config::GFX_VR_PASSTHROUGH_SCENE_OPACITY_MIN,
                            Config::GFX_VR_PASSTHROUGH_SCENE_OPACITY_MAX,
                            Config::GFX_VR_PASSTHROUGH_SCENE_OPACITY,
                            Config::GFX_VR_PASSTHROUGH_SCENE_OPACITY_STEP);
  m_passthrough_scene_opacity_value = new QLabel();
  m_passthrough_scene_opacity_value->setText(
      QString::asprintf("%.2f", m_passthrough_scene_opacity->GetValue()));
  connect(m_passthrough_scene_opacity, &ConfigFloatSlider::valueChanged, this, [this] {
    m_passthrough_scene_opacity_value->setText(
        QString::asprintf("%.2f", m_passthrough_scene_opacity->GetValue()));
  });
  m_passthrough_coverage_mode = new ConfigChoiceMap<VRPassthroughCoverageMode>(
      {{tr("Exact"), VRPassthroughCoverageMode::Exact},
       {tr("Fast"), VRPassthroughCoverageMode::Fast}},
      Config::GFX_VR_PASSTHROUGH_COVERAGE_MODE);

  openxr_layout->addWidget(new ConfigFloatLabel(tr("Units per Meter:"), m_units_per_meter), 1, 0);
  openxr_layout->addWidget(m_units_per_meter, 1, 1);
  openxr_layout->addWidget(m_units_per_meter_value, 1, 2);

  m_mirror_view = new ConfigChoiceMap<OpenXRMirrorView>(
      {{tr("Both Eyes"), OpenXRMirrorView::BothEyes},
       {tr("Left Eye"), OpenXRMirrorView::LeftEye},
       {tr("Right Eye"), OpenXRMirrorView::RightEye},
       {tr("None"), OpenXRMirrorView::None}},
      Config::GFX_VR_MIRROR_VIEW);
  openxr_layout->addWidget(new QLabel(tr("Desktop Mirror View:")), 2, 0);
  openxr_layout->addWidget(m_mirror_view, 2, 1, 1, 2);

  openxr_layout->addWidget(m_flat_screen, 3, 0, 1, 3);

  camera_layout->addWidget(m_enable_lean_back_angle, 0, 0);
  camera_layout->addWidget(m_lean_back_angle, 0, 1);
  camera_layout->addWidget(m_lean_back_angle_value, 0, 2);
  camera_layout->addWidget(m_enable_camera_forward, 1, 0);
  camera_layout->addWidget(m_camera_forward, 1, 1);
  camera_layout->addWidget(m_camera_forward_value, 1, 2);
  camera_layout->addWidget(m_enable_camera_height, 2, 0);
  camera_layout->addWidget(m_camera_height, 2, 1);
  camera_layout->addWidget(m_camera_height_value, 2, 2);
  camera_layout->addWidget(m_enable_camera_anchor, 3, 0);
  camera_layout->addWidget(m_camera_anchor_smoothing, 3, 1);
  camera_layout->addWidget(m_camera_anchor_smoothing_value, 3, 2);
  camera_layout->addWidget(m_enable_controller_anchor, 4, 0);

  // With the exponential scale, small values need more decimals to read meaningfully while large
  // values don't; adapt the precision to the magnitude.
  const auto units_per_meter_text = [](float value) {
    return QString::asprintf(value < 10.0f ? "%.2f" : (value < 100.0f ? "%.1f" : "%.0f"), value);
  };
  m_units_per_meter_value->setText(units_per_meter_text(m_units_per_meter->GetValue()));
  connect(m_units_per_meter, &ConfigFloatSlider::valueChanged, this,
          [this, units_per_meter_text] {
            m_units_per_meter_value->setText(units_per_meter_text(m_units_per_meter->GetValue()));
          });
  m_lean_back_angle_value->setText(QString::asprintf("%.1f", m_lean_back_angle->GetValue()));
  connect(m_lean_back_angle, &ConfigFloatSlider::valueChanged, this, [this] {
    m_lean_back_angle_value->setText(QString::asprintf("%.1f", m_lean_back_angle->GetValue()));
  });
  m_camera_forward_value->setText(QString::asprintf("%.1f", m_camera_forward->GetValue()));
  connect(m_camera_forward, &ConfigFloatSlider::valueChanged, this, [this] {
    m_camera_forward_value->setText(QString::asprintf("%.1f", m_camera_forward->GetValue()));
  });
  m_camera_height_value->setText(QString::asprintf("%.1f", m_camera_height->GetValue()));
  connect(m_camera_height, &ConfigFloatSlider::valueChanged, this, [this] {
    m_camera_height_value->setText(QString::asprintf("%.1f", m_camera_height->GetValue()));
  });
  m_camera_anchor_smoothing_value->setText(
      QString::asprintf("%.2f", m_camera_anchor_smoothing->GetValue()));
  connect(m_camera_anchor_smoothing, &ConfigFloatSlider::valueChanged, this, [this] {
    m_camera_anchor_smoothing_value->setText(
        QString::asprintf("%.2f", m_camera_anchor_smoothing->GetValue()));
  });

  // Virtual screen settings
  m_virtual_screen =
      new ConfigBool(tr("Virtual Screen for 2D Content"), Config::GFX_VR_VIRTUAL_SCREEN);
  virtual_screen_layout->addWidget(m_virtual_screen, 0, 0, 1, 3);

  m_screen_distance = new ConfigFloatSlider(Config::GFX_VR_SCREEN_DISTANCE_MIN,
                                            Config::GFX_VR_SCREEN_DISTANCE_MAX,
                                            Config::GFX_VR_SCREEN_DISTANCE,
                                            Config::GFX_VR_SCREEN_DISTANCE_STEP);
  m_screen_distance_value = new QLabel();
  m_screen_size = new ConfigFloatSlider(Config::GFX_VR_SCREEN_SIZE_MIN,
                                        Config::GFX_VR_SCREEN_SIZE_MAX,
                                        Config::GFX_VR_SCREEN_SIZE,
                                        Config::GFX_VR_SCREEN_SIZE_STEP);
  m_screen_size_value = new QLabel();
  m_head_locked_curvature =
      new ConfigFloatSlider(Config::GFX_VR_HEAD_LOCKED_CURVATURE_MIN,
                            Config::GFX_VR_HEAD_LOCKED_CURVATURE_MAX,
                            Config::GFX_VR_HEAD_LOCKED_CURVATURE,
                            Config::GFX_VR_HEAD_LOCKED_CURVATURE_STEP);
  m_head_locked_curvature_value = new QLabel();

  virtual_screen_layout->addWidget(
      new ConfigFloatLabel(tr("Screen Distance (m):"), m_screen_distance), 3, 0);
  virtual_screen_layout->addWidget(m_screen_distance, 3, 1);
  virtual_screen_layout->addWidget(m_screen_distance_value, 3, 2);
  virtual_screen_layout->addWidget(new ConfigFloatLabel(tr("Screen Size (m):"), m_screen_size), 4,
                                   0);
  virtual_screen_layout->addWidget(m_screen_size, 4, 1);
  virtual_screen_layout->addWidget(m_screen_size_value, 4, 2);
  virtual_screen_layout->addWidget(
      new ConfigFloatLabel(tr("Head Locked Curvature:"), m_head_locked_curvature), 6, 0);
  virtual_screen_layout->addWidget(m_head_locked_curvature, 6, 1);
  virtual_screen_layout->addWidget(m_head_locked_curvature_value, 6, 2);

  m_screen_distance_value->setText(QString::asprintf("%.1f", m_screen_distance->GetValue()));
  connect(m_screen_distance, &ConfigFloatSlider::valueChanged, this, [this] {
    m_screen_distance_value->setText(QString::asprintf("%.1f", m_screen_distance->GetValue()));
  });
  m_screen_size_value->setText(QString::asprintf("%.1f", m_screen_size->GetValue()));
  connect(m_screen_size, &ConfigFloatSlider::valueChanged, this, [this] {
    m_screen_size_value->setText(QString::asprintf("%.1f", m_screen_size->GetValue()));
  });
  m_head_locked_curvature_value->setText(
      QString::asprintf("%.2f", m_head_locked_curvature->GetValue()));
  connect(m_head_locked_curvature, &ConfigFloatSlider::valueChanged, this, [this] {
    m_head_locked_curvature_value->setText(
        QString::asprintf("%.2f", m_head_locked_curvature->GetValue()));
  });

  // Sub-refresh-rate games are paced by the XR pacing thread, which re-submits the
  // last frame at HMD cadence for ATW reprojection (replaced the legacy Opcode Replay).
  m_forced_vbi_frequency = new ConfigChoiceMap<int>(
      {{tr("Auto"), Config::GFX_VR_FORCED_VBI_FREQUENCY_AUTO},
       {tr("Off"), Config::GFX_VR_FORCED_VBI_FREQUENCY_OFF},
       {tr("72 Hz"), Config::GFX_VR_FORCED_VBI_FREQUENCY_72},
       {tr("90 Hz"), Config::GFX_VR_FORCED_VBI_FREQUENCY_90},
       {tr("120 Hz"), Config::GFX_VR_FORCED_VBI_FREQUENCY_120}},
      Config::GFX_VR_FORCED_VBI_FREQUENCY);
  framerate_layout->addWidget(new QLabel(tr("Forced VBI Frequency:")), 2, 0);
  framerate_layout->addWidget(m_forced_vbi_frequency, 2, 1, 1, 2);
  connect(m_forced_vbi_frequency, &QComboBox::currentIndexChanged, this,
          [](int) { Config::SetBaseOrCurrent(Config::GFX_VR_AUTO_VBI_FROM_HMD, false); });

  m_eager_heartbeat =
      new ConfigBool(tr("Eager Frame Heartbeat"), Config::GFX_VR_EAGER_HEARTBEAT);
  m_eager_heartbeat->setToolTip(
      tr("Controls how the VR pacing thread fills the headset's refresh rate when the game "
         "renders slower than the display.<br><br>"
         "<b>On</b> resubmits the last frame every headset refresh so every slot is filled. "
         "Best for standalone runtimes with no motion smoothing (Quest). On PC, this also "
         "prevents the runtime's own motion smoothing (SteamVR reprojection, Virtual Desktop "
         "SSW, Meta Link ASW) from engaging in the first place, which avoids that smoothing's "
         "own warping/ghosting artifacts — useful if you find those more distracting than the "
         "judder it's meant to hide.<br><br>"
         "<b>Off</b> paces submissions to the game's real frame rate so the PC runtime's own "
         "motion smoothing can engage — this removes head-tracking judder on PC at the cost of "
         "possible smoothing artifacts.<br><br>"
         "Can be toggled while a game is running."));
  framerate_layout->addWidget(m_eager_heartbeat, 3, 1, 1, 2);

  m_clear_efb_slider = new ConfigSlider(Config::GFX_VR_CLEAR_EFB_MIN,
                                        Config::GFX_VR_CLEAR_EFB_MAX,
                                        Config::GFX_VR_CLEAR_EFB_COPIES,
                                        Config::GFX_VR_CLEAR_EFB_STEP);
  m_clear_efb_value = new QLabel();
  rendering_layout->addWidget(
      new ConfigSliderLabel(tr("Clear EFB (min width):"), m_clear_efb_slider), 0, 0);
  rendering_layout->addWidget(m_clear_efb_slider, 0, 1);
  rendering_layout->addWidget(m_clear_efb_value, 0, 2);

  auto update_clear_efb_label = [this] {
    const int val = m_clear_efb_slider->value();
    m_clear_efb_value->setText(val == 0 ? tr("Off") : QString::number(val));
  };
  update_clear_efb_label();
  connect(m_clear_efb_slider, &ConfigSlider::valueChanged, this, update_clear_efb_label);

  // VR Gamma
  m_vr_gamma = new ConfigFloatSlider(Config::GFX_VR_GAMMA_MIN, Config::GFX_VR_GAMMA_MAX,
                                     Config::GFX_VR_GAMMA, Config::GFX_VR_GAMMA_STEP);
  m_vr_gamma_value = new QLabel();
  rendering_layout->addWidget(new ConfigFloatLabel(tr("VR Gamma:"), m_vr_gamma), 1, 0);
  rendering_layout->addWidget(m_vr_gamma, 1, 1);
  rendering_layout->addWidget(m_vr_gamma_value, 1, 2);

  auto update_gamma_label = [this] {
    const float val = m_vr_gamma->GetValue();
    m_vr_gamma_value->setText(val <= 1.01f ? tr("Off") : QString::asprintf("%.1f", val));
  };
  update_gamma_label();
  connect(m_vr_gamma, &ConfigFloatSlider::valueChanged, this, update_gamma_label);

  // Eye buffer resolution scale (applied at session start)
  m_resolution_scale = new ConfigFloatSlider(Config::GFX_VR_RESOLUTION_SCALE_MIN,
                                             Config::GFX_VR_RESOLUTION_SCALE_MAX,
                                             Config::GFX_VR_RESOLUTION_SCALE,
                                             Config::GFX_VR_RESOLUTION_SCALE_STEP);
  m_resolution_scale_value = new QLabel();
  rendering_layout->addWidget(new ConfigFloatLabel(tr("Resolution Scale:"), m_resolution_scale),
                              2, 0);
  rendering_layout->addWidget(m_resolution_scale, 2, 1);
  rendering_layout->addWidget(m_resolution_scale_value, 2, 2);

  auto update_resolution_scale_label = [this] {
    m_resolution_scale_value->setText(
        QString::asprintf("%.2fx", m_resolution_scale->GetValue()));
  };
  update_resolution_scale_label();
  connect(m_resolution_scale, &ConfigFloatSlider::valueChanged, this,
          update_resolution_scale_label);

  // Fixed foveated rendering (XR_FB_foveation; needs a runtime that exposes it, applied
  // at session start)
  m_foveation_level = new ConfigChoiceMap<int>(
      {{tr("Off"), 0}, {tr("Low"), 1}, {tr("Medium"), 2}, {tr("High"), 3}},
      Config::GFX_VR_FOVEATION_LEVEL);
  m_dynamic_foveation = new ConfigBool(tr("Dynamic"), Config::GFX_VR_FOVEATION_DYNAMIC);
  rendering_layout->addWidget(new QLabel(tr("Foveated Rendering:")), 3, 0);
  rendering_layout->addWidget(m_foveation_level, 3, 1);
  rendering_layout->addWidget(m_dynamic_foveation, 3, 2);

  m_foveate_efb = new ConfigBool(tr("Foveate Game Render (EFB)"), Config::GFX_VR_EFB_FOVEATION);
  m_foveate_efb->setToolTip(
      tr("Applies foveation to the game's own render pass, not just the eye buffers. Helps "
         "games with long uninterrupted render passes, but hurts games that interrupt "
         "rendering with many EFB copies per frame (it forces tiled GPUs out of direct "
         "rendering mode). Leave off unless a specific game benefits."));
  rendering_layout->addWidget(m_foveate_efb, 4, 1, 1, 2);

  // Exact Screen Depth
  m_exact_screen_depth =
      new ConfigBool(tr("Exact Screen Depth"), Config::GFX_VR_EXACT_SCREEN_DEPTH);
  m_exact_screen_depth->setToolTip(
      tr("Writes the game's original depth values on the 2D Virtual Screen instead of "
         "synthesized per-draw layers.<br><br>"
         "This restores the console's exact depth-test behavior for menus and HUDs, which "
         "eliminates z-fighting between overlapping 2D elements by construction. With this "
         "enabled, Auto Layer Spread / Layer Offset / Element Depth are no longer needed for "
         "correctness.<br><br>"
         "Disable only to fall back to the legacy layer system for troubleshooting."));
  virtual_screen_layout->addWidget(m_exact_screen_depth, 1, 0, 1, 3);

  // Auto-Exclude EFB Effects
  m_auto_native_efb_effects =
      new ConfigBool(tr("Auto-Exclude EFB Effects"), Config::GFX_VR_AUTO_NATIVE_EFB_EFFECTS);
  m_auto_native_efb_effects->setToolTip(
      tr("Automatically keeps full-screen EFB effects (bloom, motion blur, heat distortion and "
         "other post-processing) off the 2D Virtual Screen.<br><br>"
         "These effects draw as 2D quads sampling a copy of the rendered frame, so the virtual "
         "screen would otherwise capture them and smear the whole-scene effect across the 2D "
         "screen. Detected draws render natively over each eye instead, exactly as if they had "
         "a manual Fullscreen override.<br><br>"
         "Genuine 2D content that samples a frame copy opaquely (e.g. a frozen pause-menu "
         "background) stays on the virtual screen. Disable this if a game's 2D element is "
         "misdetected and leaves the screen."));
  virtual_screen_layout->addWidget(m_auto_native_efb_effects, 2, 0, 1, 3);

  m_hud_thickness = new ConfigFloatSlider(Config::GFX_VR_HUD_THICKNESS_MIN,
                                          Config::GFX_VR_HUD_THICKNESS_MAX,
                                          Config::GFX_VR_HUD_THICKNESS,
                                          Config::GFX_VR_HUD_THICKNESS_STEP);
  m_hud_thickness_value = new QLabel();
  virtual_screen_layout->addWidget(new ConfigFloatLabel(tr("Screen Depth:"), m_hud_thickness), 5,
                                   0);
  virtual_screen_layout->addWidget(m_hud_thickness, 5, 1);
  virtual_screen_layout->addWidget(m_hud_thickness_value, 5, 2);

  auto update_hud_thickness_label = [this] {
    const float val = m_hud_thickness->GetValue();
    m_hud_thickness_value->setText(val <= 0.001f ? tr("Off") : QString::asprintf("%.2f m", val));
  };
  update_hud_thickness_label();
  connect(m_hud_thickness, &ConfigFloatSlider::valueChanged, this, update_hud_thickness_label);

  general_layout->addWidget(openxr_group);
  general_layout->addWidget(camera_group);
  general_layout->addWidget(virtual_screen_group);
  general_layout->addWidget(rendering_group);
  auto* general_actions_layout = new QHBoxLayout;
  general_actions_layout->addStretch();
  m_reset_general_settings = new QPushButton(tr("Reset VR Settings"));
  connect(m_reset_general_settings, &QPushButton::clicked, this, &VRPane::ResetGeneralSettings);
  general_actions_layout->addWidget(m_reset_general_settings);
  general_layout->addLayout(general_actions_layout);

  auto* hacks_group = new QGroupBox(tr("VR Hacks"));
  auto* hacks_group_layout = new QVBoxLayout;
  hacks_group->setLayout(hacks_group_layout);

  m_dont_clear_screen =
      new ConfigBool(tr("Don't Clear Screen"), Config::GFX_VR_DONT_CLEAR_SCREEN);
  m_disable_cpu_cull =
      new ConfigBool(tr("Disable CPU Culling in VR"), Config::GFX_VR_DISABLE_CPU_CULL);
  m_remove_bars = new ConfigBool(tr("Remove Cinematic Bars"), Config::GFX_VR_REMOVE_BARS);
  m_frame_size_from_xfb =
      new ConfigBool(tr("Frame Size from XFB Copy"), Config::GFX_VR_FRAME_SIZE_FROM_XFB);
  m_frame_size_from_xfb->setToolTip(
      tr("Match the composed scene to the EFB-to-XFB display copy without changing the game's\n"
         "EFB viewport. This preserves Metroid Prime Trilogy blur/visor effects and corrects\n"
         "Mario Golf's 480-line scene to 448-line display mismatch. Turn off for the legacy\n"
         "clear-based behavior."));
  m_panes_on_screen =
      new ConfigBool(tr("Small 3D Viewports on Screen"), Config::GFX_VR_PANES_ON_SCREEN);
  m_panes_on_screen->setToolTip(
      tr("Draw 3D models rendered into small menu panes (e.g. the Mario Kart: Double Dash!!\n"
         "character select boxes) on the 2D virtual screen instead of head-tracking them.\n"
         "Stops them swaying and stretching when you move your head."));
  m_detect_render_targets =
      new ConfigBool(tr("Detect Render-to-Texture Viewports"), Config::GFX_VR_DETECT_RENDER_TARGETS);
  m_detect_render_targets->setToolTip(
      tr("Exclude square render-to-texture passes (shadow/environment maps) from VR\n"
         "reprojection so their output textures don't bake in the head pose. Experimental."));
  m_ortho_scissor_fix =
      new ConfigBool(tr("Ortho Scissor Fix"), Config::GFX_VR_ORTHO_SCISSOR_FIX);
  m_detect_skybox = new ConfigBool(tr("Detect Skybox"), Config::GFX_VR_DETECT_SKYBOX);
  m_layered_palette_conversion_path =
      new ConfigBool(tr("Layered Palette Conversion Path"),
                     Config::GFX_VR_METROID_THERMAL_VISOR_FIX);
  const auto update_layered_palette_conversion_path = [](bool checked) {
    if (Config::Get(Config::GFX_VR_METROID_D3D_THERMAL_PALETTE_FIX) != checked)
      Config::SetBaseOrCurrent(Config::GFX_VR_METROID_D3D_THERMAL_PALETTE_FIX, checked);
  };
  connect(m_layered_palette_conversion_path, &QCheckBox::toggled, this,
          [update_layered_palette_conversion_path](bool checked) {
            update_layered_palette_conversion_path(checked);
          });
  update_layered_palette_conversion_path(m_layered_palette_conversion_path->isChecked());

  hacks_group_layout->addWidget(m_use_vulkan_multiview);
  hacks_group_layout->addWidget(m_dont_clear_screen);
  hacks_group_layout->addWidget(m_disable_cpu_cull);
  hacks_group_layout->addWidget(m_remove_bars);
  hacks_group_layout->addWidget(m_frame_size_from_xfb);
  hacks_group_layout->addWidget(m_panes_on_screen);
  hacks_group_layout->addWidget(m_detect_render_targets);
  hacks_group_layout->addWidget(m_ortho_scissor_fix);
  hacks_group_layout->addWidget(m_detect_skybox);
  hacks_group_layout->addWidget(m_layered_palette_conversion_path);
  hack_layout->addWidget(hacks_group);
  hack_layout->addWidget(framerate_group);

  auto* shaders_group = new QGroupBox(tr("Shaders"));
  auto* shaders_group_layout = new QVBoxLayout;
  shaders_group->setLayout(shaders_group_layout);

  m_load_custom_shaders =
      new ConfigBool(tr("Load Custom Shaders"), Config::GFX_VR_LOAD_CUSTOM_SHADERS);
  shaders_group_layout->addWidget(m_load_custom_shaders);
  hack_layout->addWidget(shaders_group);

  passthrough_layout->addWidget(m_passthrough, 0, 0, 1, 3);
  passthrough_layout->addWidget(m_passthrough_remove_black_bg, 1, 0, 1, 3);
  passthrough_layout->addWidget(m_passthrough_remove_black_clears, 2, 0, 1, 3);
  passthrough_layout->addWidget(
      new ConfigFloatLabel(tr("Scene Opacity:"), m_passthrough_scene_opacity), 3, 0);
  passthrough_layout->addWidget(m_passthrough_scene_opacity, 3, 1);
  passthrough_layout->addWidget(m_passthrough_scene_opacity_value, 3, 2);
  passthrough_layout->addWidget(new QLabel(tr("Coverage Mode:")), 4, 0);
  passthrough_layout->addWidget(m_passthrough_coverage_mode, 4, 1, 1, 2);
#if defined(_WIN32)
  const auto update_passthrough_backend = [passthrough_group] {
    passthrough_group->setEnabled(Config::Get(Config::MAIN_GFX_BACKEND) == "Vulkan");
  };
  connect(&Settings::Instance(), &Settings::ConfigChanged, passthrough_group,
          update_passthrough_backend);
  update_passthrough_backend();
#else
  passthrough_group->setVisible(false);
#endif
  hack_layout->addWidget(passthrough_group);

  auto* tools_group = new QGroupBox(tr("Tools"));
  auto* tools_layout = new QGridLayout;
  tools_group->setLayout(tools_layout);

  auto* add_hide_object_code_btn = new QPushButton(tr("Add Hide Object Code"));
  connect(add_hide_object_code_btn, &QPushButton::clicked, this, [this] {
    const std::string game_id = SConfig::GetInstance().GetGameID();
    if (game_id.empty() || game_id == "00000000")
    {
      QMessageBox::information(this, tr("No Game Running"),
                               tr("Start a game before adding Hide Object codes."));
      return;
    }

    auto codes = HideObjectEngine::LoadFromINI(game_id);
    HideObjectAddEditDialog dialog(this, nullptr, codes);
    if (dialog.exec() == QDialog::Accepted)
    {
      codes.push_back(dialog.GetResult());
      HideObjectEngine::SaveToINI(game_id, codes);
      HideObjectEngine::Engine::GetInstance().ApplyCodes(codes);
    }
    else
    {
      // Restore the active code set if temporary brute-force values were applied.
      HideObjectEngine::Engine::GetInstance().ApplyCodes(codes);
    }
  });
  tools_layout->addWidget(add_hide_object_code_btn, 0, 0);

  auto* shader_hunter_btn = new QPushButton(tr("Shader Hunter"));
  connect(shader_hunter_btn, &QPushButton::clicked, this, [this] {
    const std::string game_id = SConfig::GetInstance().GetGameID();
    if (game_id.empty() || game_id == "00000000")
    {
      QMessageBox::information(this, tr("No Game Running"),
                               tr("Start a game before searching for shaders."));
      return;
    }

    if (m_shader_hunter_widget)
    {
      m_shader_hunter_widget->show();
      m_shader_hunter_widget->raise();
      m_shader_hunter_widget->activateWindow();
      return;
    }

    m_shader_hunter_widget = new ShaderHunterWidget(this);
    m_shader_hunter_widget->setAttribute(Qt::WA_DeleteOnClose, true);
    m_shader_hunter_widget->show();
    connect(m_shader_hunter_widget, &QObject::destroyed, this,
            [this] { m_shader_hunter_widget = nullptr; });
  });
  tools_layout->addWidget(shader_hunter_btn, 1, 0);

  auto* culling_finder_btn = new QPushButton(tr("Culling Code Finder"));
  connect(culling_finder_btn, &QPushButton::clicked, this, [this] {
    const std::string game_id = SConfig::GetInstance().GetGameID();
    if (game_id.empty() || game_id == "00000000")
    {
      QMessageBox::information(this, tr("No Game Running"),
                               tr("Start a game before searching for culling codes."));
      return;
    }

    if (m_culling_finder_widget)
    {
      m_culling_finder_widget->show();
      m_culling_finder_widget->raise();
      m_culling_finder_widget->activateWindow();
      return;
    }

    m_culling_finder_widget = new CullingCodeFinderWidget(this);
    m_culling_finder_widget->setAttribute(Qt::WA_DeleteOnClose, true);
    m_culling_finder_widget->show();
    connect(m_culling_finder_widget, &QObject::destroyed, this,
            [this] { m_culling_finder_widget = nullptr; });
  });
  tools_layout->addWidget(culling_finder_btn, 2, 0);

  auto* debug_group = new QGroupBox(tr("Debug"));
  auto* debug_layout = new QGridLayout;
  debug_group->setLayout(debug_layout);

  debug_layout->addWidget(new QLabel(tr("Default VR Position:")), 0, 0);
  debug_layout->addWidget(m_reference_space_mode, 0, 1);
  debug_layout->addWidget(new QLabel(tr("Tracking Mode:")), 1, 0);
  debug_layout->addWidget(m_tracking_mode, 1, 1);

  m_debug_log_draws = new QCheckBox(tr("Debug Log Draws"));
  m_debug_log_draws->setToolTip(
      tr("Log every draw call with projection, viewport, scissor, and shader hashes.\n"
         "Also logs EFB clear operations. Use to identify how visual elements are drawn.\n"
         "Open Log Configuration and enable INFO for Video to see the output.\n"
         "WARNING: generates a LOT of log output — enable briefly, then disable."));
  m_debug_log_draws->setChecked(ShaderHunter::GetInstance().IsDebugLogging());
  connect(m_debug_log_draws, &QCheckBox::toggled, this,
          [](bool checked) { ShaderHunter::GetInstance().SetDebugLogging(checked); });
  debug_layout->addWidget(m_debug_log_draws, 2, 0, 1, 2);

  general_layout->addStretch();
  hack_layout->addStretch();
  tools_tab_layout->addWidget(tools_group);
  tools_tab_layout->addWidget(debug_group);
  tools_tab_layout->addStretch();

  main_layout->addWidget(tabs);

  setLayout(main_layout);

  AddDescriptions();

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &VRPane::OnEmulationStateChanged);
  OnEmulationStateChanged(Core::GetState(Core::System::GetInstance()));
}

void VRPane::AddDescriptions()
{
  static constexpr char TR_ENABLE_OPENXR_DESCRIPTION[] = QT_TR_NOOP(
      "Enables VR rendering through OpenXR."
      "<br><br>When enabled, Dolphin uses OpenXR instead of Stereoscopic 3D output modes."
      "<br><br>This setting cannot be changed while emulation is active."
      "<br><br><dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static constexpr char TR_FLAT_SCREEN_DESCRIPTION[] = QT_TR_NOOP(
      "Shows the game on a flat 2D screen floating in the VR scene instead of rendering in "
      "stereoscopic 3D."
      "<br><br>Requires Enable VR. Uses the Screen Distance and Screen Size settings below to "
      "position and size the panel. The screen is world-locked; use Recenter to bring it back "
      "in front of you."
      "<br><br>This setting cannot be changed while emulation is active."
      "<br><br><dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static constexpr char TR_USE_VULKAN_MULTIVIEW_DESCRIPTION[] = QT_TR_NOOP(
      "Uses Vulkan multiview for OpenXR stereo rendering when the GPU and runtime support it."
      "<br><br>This renders both eyes through a multiview render pass instead of using "
      "geometry-shader stereo expansion, which can be much faster on Quest/Adreno."
      "<br><br>This setting only affects the Vulkan backend and requires restarting emulation."
      "<br><br><dolphin_emphasis>If unsure on Quest, leave this checked.</dolphin_emphasis>");
  static constexpr char TR_UNITS_PER_METER_DESCRIPTION[] = QT_TR_NOOP(
      "Sets how many game world units correspond to one real-world meter."
      "<br><br>Higher values increase stereo scale and make the world appear smaller."
      "<br><br>Lower values decrease stereo scale and make the world appear larger.");
  static constexpr char TR_REFERENCE_SPACE_MODE_DESCRIPTION[] = QT_TR_NOOP(
      "Selects how Dolphin sets the default VR position when OpenXR starts."
      "<br><br>LOCAL uses OpenXR <code>LOCAL</code> space and keeps the current behavior: Dolphin "
      "sets home from the initial headset position."
      "<br><br>STAGE + Height uses OpenXR <code>STAGE</code> space, keeps the play-space center "
      "for horizontal position, and offsets height by the initial headset height."
      "<br><br>STAGE uses OpenXR <code>STAGE</code> space and uses the play-space origin directly."
      "<br><br>If <code>STAGE</code> is unavailable, Dolphin falls back to <code>LOCAL</code>."
      "<br><br>This setting requires restarting emulation."
      "<br><br><dolphin_emphasis>If unsure, use LOCAL.</dolphin_emphasis>");
  static constexpr char TR_TRACKING_MODE_DESCRIPTION[] = QT_TR_NOOP(
      "Selects how OpenXR headset tracking affects the VR camera."
      "<br><br>6DoF uses full rotation and position tracking."
      "<br><br>3DoF uses head rotation tracking only and ignores headset position movement."
      "<br><br>None disables both head rotation and position tracking."
      "<br><br>This only affects the emulated VR camera; OpenXR still receives frames normally."
      "<br><br><dolphin_emphasis>If unsure, use 6DoF.</dolphin_emphasis>");
  static constexpr char TR_LEAN_BACK_ANGLE_DESCRIPTION[] = QT_TR_NOOP(
      "Applies a constant pitch offset (in degrees) to the VR camera."
      "<br><br>Use this to compensate games where the default viewpoint feels tilted."
      "<br><br>Positive values lean the camera back, negative values lean it forward.");
  static constexpr char TR_CAMERA_FORWARD_DESCRIPTION[] = QT_TR_NOOP(
      "Applies a constant forward/backward offset (in meters) to the VR camera."
      "<br><br>Positive values move the camera forward, negative values move it backward.");
  static constexpr char TR_CAMERA_HEIGHT_DESCRIPTION[] = QT_TR_NOOP(
      "Applies a constant vertical offset (in meters) to the VR camera."
      "<br><br>Positive values move the camera up, negative values move it down.");
  static constexpr char TR_VIRTUAL_SCREEN_DESCRIPTION[] = QT_TR_NOOP(
      "Places 2D content (menus, FMV, in-game HUD) on a virtual screen floating in 3D space."
      "<br><br>Disable for games that rely on full-screen EFB effects that don't work with a virtual screen.");
  static constexpr char TR_SCREEN_DISTANCE_DESCRIPTION[] = QT_TR_NOOP(
      "Distance in meters to the virtual screen used for 2D content (menus, FMV, HUD)."
      "<br><br>The screen is fixed in space like a TV — it stays in place when you turn your head.");
  static constexpr char TR_SCREEN_SIZE_DESCRIPTION[] = QT_TR_NOOP(
      "Height in meters of the virtual screen used for 2D content."
      "<br><br>Larger values make the screen bigger, smaller values make it more compact.");
  static constexpr char TR_HEAD_LOCKED_CURVATURE_DESCRIPTION[] = QT_TR_NOOP(
      "Curves the virtual screen used by Head Locked shader overrides."
      "<br><br>0 keeps the screen flat. Higher values increase curvature."
      "<br><br>This only affects shaders using Head Locked handling.");
  static constexpr char TR_DONT_CLEAR_SCREEN_DESCRIPTION[] = QT_TR_NOOP(
      "Prevents clearing the VR eye framebuffers before rendering each frame."
      "<br><br>By default the screen is cleared to black, which prevents image trails when "
      "looking outside the rendered area. Enable this if clearing causes visual glitches "
      "in specific games."
      "<br><br><dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static constexpr char TR_DISABLE_CPU_CULL_DESCRIPTION[] = QT_TR_NOOP(
      "Disables CPU-side primitive culling when OpenXR VR is active."
      "<br><br>This may fix missing geometry in some games at the cost of a small performance hit."
      "<br><br>This only affects Dolphin's CPU culling optimization. It does not override "
      "the game's own backface culling state.");
  static constexpr char TR_MIRROR_VIEW_DESCRIPTION[] = QT_TR_NOOP(
      "Selects what the desktop render window shows while OpenXR is active."
      "<br><br>Both Eyes shows the current side-by-side mirror. Left Eye and Right Eye fill the "
      "window with a single eye. None leaves the desktop window blank while continuing to render "
      "normally to the headset."
      "<br><br>This only affects the desktop mirror view; it does not affect the OpenXR headset "
      "output.");
  static constexpr char TR_FORCED_VBI_FREQUENCY_DESCRIPTION[] = QT_TR_NOOP(
      "Forces Dolphin's VBI frequency to the selected rate while OpenXR VR is enabled."
      "<br><br>Auto samples the headset refresh rate once at OpenXR session startup and uses "
      "the closest supported value: 72, 90, or 120 Hz."
      "<br><br>This overrides the Advanced tab's VBI percentage during VR sessions."
      "<br><br><dolphin_emphasis>If unsure, leave this set to Off.</dolphin_emphasis>");
  static constexpr char TR_HUD_THICKNESS_DESCRIPTION[] = QT_TR_NOOP(
      "Gives 2D HUD/menu layers real 3D depth in VR by spreading their elements across this "
      "much world-space thickness (in metres), instead of drawing them flat on a single plane."
      "<br><br>Recreates the layered HUD effect from Dolphin VR Hydra (e.g. the Metroid Prime "
      "visor). Only affects 2D layers whose elements are drawn at different depths; flat menus "
      "and full-motion video are unaffected. Set to 0 (Off) for a completely flat HUD."
      "<br><br><dolphin_emphasis>If unsure, leave this at 0 (off).</dolphin_emphasis>");
  static constexpr char TR_CLEAR_EFB_COPIES_DESCRIPTION[] = QT_TR_NOOP(
      "Clears EFB (Embedded Framebuffer) copy textures to transparent when the copy's native "
      "width is at or above this threshold. Set to 0 to disable."
      "<br><br>Full-screen post-processing effects (bloom, blur) typically use full EFB width "
      "(640). HUD and small UI elements use smaller partial copies. Increasing this value "
      "preserves smaller EFB copies (like HUD textures) while removing large full-screen effects."
      "<br><br>Recommended: 400-640 to clear only full-screen effects, or 0 to disable."
      "<br><br><dolphin_emphasis>If unsure, leave this at 0 (off).</dolphin_emphasis>");
  static constexpr char TR_REMOVE_BARS_DESCRIPTION[] = QT_TR_NOOP(
      "Expands the scissor and viewport to fill the full active rendering area for perspective "
      "draws, removing cinematic letterbox bars."
      "<br><br>Some games (e.g. Metroid Prime) use GX scissor clipping to create cinematic "
      "bars that mask the top and bottom of the screen. In VR these bars are distracting. "
      "This option removes them by expanding the rendered area."
      "<br><br>Disable this if it causes visual artifacts in a specific game."
      "<br><br><dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static constexpr char TR_ORTHO_SCISSOR_FIX_DESCRIPTION[] = QT_TR_NOOP(
      "Expands the scissor to the full active rendering area for orthographic VR draws."
      "<br><br>This fixes 2D content that is reprojected onto the VR virtual screen but still "
      "uses game-side EFB scissor coordinates, such as the item roulette in Mario Kart: "
      "Double Dash!!."
      "<br><br>Disable this if a game needs its original orthographic scissor clipping."
      "<br><br><dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static constexpr char TR_DETECT_SKYBOX_DESCRIPTION[] = QT_TR_NOOP(
      "Assumes any object drawn at the camera origin (translation 0,0,0) is a skybox and locks it "
      "to head rotation only, so it renders at infinity instead of appearing too close."
      "<br><br>Some games (e.g. Metroid Prime) draw the sky as a small box centred on the camera. "
      "In VR the per-eye offset makes that box look only a metre or two away. This option removes "
      "the positional/IPD offset for those draws so both eyes see the same distant sky, which "
      "rotates with your head but does not move when you lean."
      "<br><br>Disable this if it incorrectly affects non-sky geometry in a specific game."
      "<br><br><dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static constexpr char TR_LAYERED_PALETTE_CONVERSION_PATH_DESCRIPTION[] = QT_TR_NOOP(
      "Uses a layered palette conversion path for Metroid Prime's thermal visor EFB copy when "
      "running the Vulkan or D3D11 backend in OpenXR."
      "<br><br>This keeps the thermal source stereo during palette conversion instead of "
      "flattening it to one eye."
      "<br><br>This only changes palette conversion for Metroid Prime thermal-sized stereo EFB "
      "copies. It does not change normal game shaders, X-Ray handling, or non-Metroid games."
      "<br><br><dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static constexpr char TR_VR_GAMMA_DESCRIPTION[] = QT_TR_NOOP(
      "Adjusts gamma correction for VR eye output."
      "<br><br>1.0 = no correction (default). 2.2 = standard sRGB gamma. "
      "Higher values darken the image further."
      "<br><br>Some headsets expect sRGB-encoded content and appear "
      "too bright without correction. Adjust this slider until the brightness matches "
      "the desktop mirror."
      "<br><br><dolphin_emphasis>If unsure, leave this at 1.0 (Off).</dolphin_emphasis>");
  static constexpr char TR_LOAD_CUSTOM_SHADERS_DESCRIPTION[] = QT_TR_NOOP(
      "Loads custom shader replacements from the Load/Shaders/&lt;GameID&gt;/ directory."
      "<br><br>Place edited shader files (dumped via Shader Hunter) in this folder to override "
      "the game's original shaders. Files must be named &lt;hash&gt;-&lt;vs|ps|gs&gt;.txt."
      "<br><br><dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static constexpr char TR_PASSTHROUGH_DESCRIPTION[] = QT_TR_NOOP(
      "Shows the headset camera feed through the dedicated coverage mask. This is supported "
      "only by Windows Vulkan OpenXR."
      "<br><br>Meta Horizon Link uses <code>XR_FB_passthrough</code> when <b>Passthrough over "
      "Meta Horizon Link</b> is enabled. Other runtimes may use the <code>ALPHA_BLEND</code> "
      "environment blend mode."
      "<br><br><dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static constexpr char TR_PASSTHROUGH_REMOVE_BLACK_BG_DESCRIPTION[] = QT_TR_NOOP(
      "Reveals only EFB regions that no accepted game color fragment or color clear touched. "
      "Black game clears remain opaque unless <b>Remove Black EFB Clears</b> is enabled."
      "<br><br>Disable this to keep the whole scene opaque and control passthrough "
      "manually — only elements marked with a <b>Passthrough</b> override (Shader / "
      "Elements Group / Texture Element tools) become camera windows. Useful for games "
      "that explicitly draw their menu backgrounds."
      "<br><br><dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static constexpr char TR_PASSTHROUGH_REMOVE_BLACK_CLEARS_DESCRIPTION[] = QT_TR_NOOP(
      "Treats (near-)black EFB color clears as unrendered instead of covered."
      "<br><br>Most games clear the whole screen to black every frame, which normally "
      "marks everything as covered — the void around menus or behind a removed skybox "
      "stays opaque black. With this enabled, those cleared regions reveal the camera "
      "feed until the game draws over them. Colored clears (skies, white menu "
      "backgrounds) always stay opaque."
      "<br><br>Content the game actually draws — even pure black art, FMVs or letterbox "
      "bars drawn as geometry — is unaffected."
      "<br><br><dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static constexpr char TR_PASSTHROUGH_SCENE_OPACITY_DESCRIPTION[] = QT_TR_NOOP(
      "Blends the entire rendered scene over the camera feed."
      "<br><br>1.00 shows the game fully opaque; lower values make everything on screen "
      "progressively see-through until only the real world remains at 0.00. Passthrough "
      "windows and the black void are unaffected (they are already fully transparent)."
      "<br><br><dolphin_emphasis>If unsure, leave this at 1.00.</dolphin_emphasis>");
  static constexpr char TR_PASSTHROUGH_COVERAGE_MODE_DESCRIPTION[] = QT_TR_NOOP(
      "Exact preserves game blending and logic operations by using a coverage prepass when "
      "the draw cannot safely write the coverage MRT."
      "<br><br>Fast may disable dual-source blending where required. Incompatible logic and "
      "custom draws still use a prepass."
      "<br><br><dolphin_emphasis>If unsure, select Exact.</dolphin_emphasis>");

  m_enable_openxr->SetDescription(tr(TR_ENABLE_OPENXR_DESCRIPTION));
  m_flat_screen->SetDescription(tr(TR_FLAT_SCREEN_DESCRIPTION));
  m_use_vulkan_multiview->SetDescription(tr(TR_USE_VULKAN_MULTIVIEW_DESCRIPTION));
  m_reference_space_mode->SetDescription(tr(TR_REFERENCE_SPACE_MODE_DESCRIPTION));
  m_tracking_mode->SetDescription(tr(TR_TRACKING_MODE_DESCRIPTION));
  m_units_per_meter->SetDescription(tr(TR_UNITS_PER_METER_DESCRIPTION));
  m_lean_back_angle->SetDescription(tr(TR_LEAN_BACK_ANGLE_DESCRIPTION));
  m_camera_forward->SetDescription(tr(TR_CAMERA_FORWARD_DESCRIPTION));
  m_camera_height->SetDescription(tr(TR_CAMERA_HEIGHT_DESCRIPTION));
  m_virtual_screen->SetDescription(tr(TR_VIRTUAL_SCREEN_DESCRIPTION));
  m_screen_distance->SetDescription(tr(TR_SCREEN_DISTANCE_DESCRIPTION));
  m_screen_size->SetDescription(tr(TR_SCREEN_SIZE_DESCRIPTION));
  m_head_locked_curvature->SetDescription(tr(TR_HEAD_LOCKED_CURVATURE_DESCRIPTION));
  m_dont_clear_screen->SetDescription(tr(TR_DONT_CLEAR_SCREEN_DESCRIPTION));
  m_disable_cpu_cull->SetDescription(tr(TR_DISABLE_CPU_CULL_DESCRIPTION));
  m_mirror_view->SetDescription(tr(TR_MIRROR_VIEW_DESCRIPTION));
  m_forced_vbi_frequency->SetDescription(tr(TR_FORCED_VBI_FREQUENCY_DESCRIPTION));
  m_clear_efb_slider->SetDescription(tr(TR_CLEAR_EFB_COPIES_DESCRIPTION));
  m_remove_bars->SetDescription(tr(TR_REMOVE_BARS_DESCRIPTION));
  m_ortho_scissor_fix->SetDescription(tr(TR_ORTHO_SCISSOR_FIX_DESCRIPTION));
  m_detect_skybox->SetDescription(tr(TR_DETECT_SKYBOX_DESCRIPTION));
  m_layered_palette_conversion_path->SetDescription(
      tr(TR_LAYERED_PALETTE_CONVERSION_PATH_DESCRIPTION));
  m_vr_gamma->SetDescription(tr(TR_VR_GAMMA_DESCRIPTION));
  m_hud_thickness->SetDescription(tr(TR_HUD_THICKNESS_DESCRIPTION));
  m_load_custom_shaders->SetDescription(tr(TR_LOAD_CUSTOM_SHADERS_DESCRIPTION));
  m_passthrough->SetDescription(tr(TR_PASSTHROUGH_DESCRIPTION));
  m_passthrough_remove_black_bg->SetDescription(tr(TR_PASSTHROUGH_REMOVE_BLACK_BG_DESCRIPTION));
  m_passthrough_remove_black_clears->SetDescription(
      tr(TR_PASSTHROUGH_REMOVE_BLACK_CLEARS_DESCRIPTION));
  m_passthrough_scene_opacity->SetDescription(tr(TR_PASSTHROUGH_SCENE_OPACITY_DESCRIPTION));
  m_passthrough_coverage_mode->SetDescription(
      tr(TR_PASSTHROUGH_COVERAGE_MODE_DESCRIPTION));
}

void VRPane::OnEmulationStateChanged(Core::State state)
{
  const bool running = state != Core::State::Uninitialized;
  m_enable_openxr->setEnabled(!running);
  m_flat_screen->setEnabled(!running);
  m_reference_space_mode->setEnabled(!running);
  m_use_vulkan_multiview->setEnabled(!running);
}

void VRPane::ResetGeneralSettings()
{
  Config::ConfigChangeCallbackGuard guard;

  Config::SetBaseOrCurrent(Config::GFX_VR_ENABLE_OPENXR,
                           Config::GFX_VR_ENABLE_OPENXR.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_FLAT_SCREEN,
                           Config::GFX_VR_FLAT_SCREEN.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_REFERENCE_SPACE_MODE,
                           Config::GFX_VR_REFERENCE_SPACE_MODE.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_TRACKING_MODE,
                           Config::GFX_VR_TRACKING_MODE.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_UNITS_PER_METER,
                           Config::GFX_VR_UNITS_PER_METER.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_ENABLE_LEAN_BACK_ANGLE,
                           Config::GFX_VR_ENABLE_LEAN_BACK_ANGLE.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_LEAN_BACK_ANGLE,
                           Config::GFX_VR_LEAN_BACK_ANGLE.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_ENABLE_CAMERA_FORWARD,
                           Config::GFX_VR_ENABLE_CAMERA_FORWARD.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_CAMERA_FORWARD,
                           Config::GFX_VR_CAMERA_FORWARD.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_ENABLE_CAMERA_HEIGHT,
                           Config::GFX_VR_ENABLE_CAMERA_HEIGHT.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_CAMERA_HEIGHT,
                           Config::GFX_VR_CAMERA_HEIGHT.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_ENABLE_CAMERA_ANCHOR,
                           Config::GFX_VR_ENABLE_CAMERA_ANCHOR.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_CAMERA_ANCHOR_SMOOTHING,
                           Config::GFX_VR_CAMERA_ANCHOR_SMOOTHING.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_ENABLE_CONTROLLER_ANCHOR,
                           Config::GFX_VR_ENABLE_CONTROLLER_ANCHOR.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_VIRTUAL_SCREEN,
                           Config::GFX_VR_VIRTUAL_SCREEN.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_SCREEN_DISTANCE,
                           Config::GFX_VR_SCREEN_DISTANCE.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_SCREEN_SIZE,
                           Config::GFX_VR_SCREEN_SIZE.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_HEAD_LOCKED_CURVATURE,
                           Config::GFX_VR_HEAD_LOCKED_CURVATURE.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_MIRROR_VIEW,
                           Config::GFX_VR_MIRROR_VIEW.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_FORCED_VBI_FREQUENCY,
                           Config::GFX_VR_FORCED_VBI_FREQUENCY.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_AUTO_VBI_FROM_HMD,
                           Config::GFX_VR_AUTO_VBI_FROM_HMD.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_CLEAR_EFB_COPIES,
                           Config::GFX_VR_CLEAR_EFB_COPIES.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_GAMMA, Config::GFX_VR_GAMMA.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_EXACT_SCREEN_DEPTH,
                           Config::GFX_VR_EXACT_SCREEN_DEPTH.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_AUTO_NATIVE_EFB_EFFECTS,
                           Config::GFX_VR_AUTO_NATIVE_EFB_EFFECTS.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_HUD_THICKNESS,
                           Config::GFX_VR_HUD_THICKNESS.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_METROID_THERMAL_VISOR_FIX,
                           Config::GFX_VR_METROID_THERMAL_VISOR_FIX.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_METROID_D3D_THERMAL_PALETTE_FIX,
                           Config::GFX_VR_METROID_D3D_THERMAL_PALETTE_FIX.GetDefaultValue());

  // Advanced tab
  Config::SetBaseOrCurrent(Config::GFX_VR_USE_VULKAN_MULTIVIEW,
                           Config::GFX_VR_USE_VULKAN_MULTIVIEW.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_DONT_CLEAR_SCREEN,
                           Config::GFX_VR_DONT_CLEAR_SCREEN.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_DISABLE_CPU_CULL,
                           Config::GFX_VR_DISABLE_CPU_CULL.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_REMOVE_BARS,
                           Config::GFX_VR_REMOVE_BARS.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_FRAME_SIZE_FROM_XFB,
                           Config::GFX_VR_FRAME_SIZE_FROM_XFB.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_PANES_ON_SCREEN,
                           Config::GFX_VR_PANES_ON_SCREEN.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_DETECT_RENDER_TARGETS,
                           Config::GFX_VR_DETECT_RENDER_TARGETS.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_ORTHO_SCISSOR_FIX,
                           Config::GFX_VR_ORTHO_SCISSOR_FIX.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_DETECT_SKYBOX,
                           Config::GFX_VR_DETECT_SKYBOX.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_EAGER_HEARTBEAT,
                           Config::GFX_VR_EAGER_HEARTBEAT.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_LOAD_CUSTOM_SHADERS,
                           Config::GFX_VR_LOAD_CUSTOM_SHADERS.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_PASSTHROUGH,
                           Config::GFX_VR_PASSTHROUGH.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_PASSTHROUGH_REMOVE_BLACK_BG,
                           Config::GFX_VR_PASSTHROUGH_REMOVE_BLACK_BG.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_PASSTHROUGH_REMOVE_BLACK_CLEARS,
                           Config::GFX_VR_PASSTHROUGH_REMOVE_BLACK_CLEARS.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_PASSTHROUGH_SCENE_OPACITY,
                           Config::GFX_VR_PASSTHROUGH_SCENE_OPACITY.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_PASSTHROUGH_COVERAGE_MODE,
                           Config::GFX_VR_PASSTHROUGH_COVERAGE_MODE.GetDefaultValue());
}
