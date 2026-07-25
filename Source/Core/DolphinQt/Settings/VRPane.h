// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QPointer>
#include <QWidget>

class ConfigBool;
class ConfigFloatSlider;
class ConfigSlider;
class QCheckBox;
class QLabel;
class QPushButton;
class CullingCodeFinderWidget;
class ShaderHunterWidget;
template <typename T>
class ConfigChoiceMap;
enum class OpenXRMirrorView : int;
enum class OpenXRReferenceSpaceMode : int;
enum class OpenXRTrackingMode : int;
enum class VRPassthroughCoverageMode : int;

namespace Core
{
enum class State;
}

class VRPane final : public QWidget
{
  Q_OBJECT
public:
  explicit VRPane(QWidget* parent = nullptr);

private:
  void AddDescriptions();
  void OnEmulationStateChanged(Core::State state);
  void ResetGeneralSettings();

  ConfigBool* m_enable_openxr = nullptr;
  ConfigBool* m_flat_screen = nullptr;
  ConfigChoiceMap<OpenXRReferenceSpaceMode>* m_reference_space_mode = nullptr;
  ConfigChoiceMap<OpenXRTrackingMode>* m_tracking_mode = nullptr;
  ConfigFloatSlider* m_units_per_meter = nullptr;
  QLabel* m_units_per_meter_value = nullptr;
  ConfigBool* m_enable_lean_back_angle = nullptr;
  ConfigFloatSlider* m_lean_back_angle = nullptr;
  QLabel* m_lean_back_angle_value = nullptr;
  ConfigBool* m_enable_camera_forward = nullptr;
  ConfigFloatSlider* m_camera_forward = nullptr;
  QLabel* m_camera_forward_value = nullptr;
  ConfigBool* m_enable_camera_height = nullptr;
  ConfigFloatSlider* m_camera_height = nullptr;
  QLabel* m_camera_height_value = nullptr;
  ConfigBool* m_enable_camera_anchor = nullptr;
  ConfigFloatSlider* m_camera_anchor_smoothing = nullptr;
  QLabel* m_camera_anchor_smoothing_value = nullptr;
  ConfigBool* m_enable_controller_anchor = nullptr;
  ConfigBool* m_virtual_screen = nullptr;
  ConfigFloatSlider* m_screen_distance = nullptr;
  QLabel* m_screen_distance_value = nullptr;
  ConfigFloatSlider* m_screen_size = nullptr;
  QLabel* m_screen_size_value = nullptr;
  ConfigFloatSlider* m_head_locked_curvature = nullptr;
  QLabel* m_head_locked_curvature_value = nullptr;
  ConfigBool* m_dont_clear_screen = nullptr;
  ConfigBool* m_disable_cpu_cull = nullptr;
  ConfigBool* m_ortho_scissor_fix = nullptr;
  ConfigBool* m_layered_palette_conversion_path = nullptr;
  ConfigChoiceMap<OpenXRMirrorView>* m_mirror_view = nullptr;
  ConfigChoiceMap<int>* m_forced_vbi_frequency = nullptr;
  ConfigSlider* m_clear_efb_slider = nullptr;
  QLabel* m_clear_efb_value = nullptr;
  ConfigBool* m_remove_bars = nullptr;
  ConfigBool* m_frame_size_from_xfb = nullptr;
  ConfigBool* m_panes_on_screen = nullptr;
  ConfigBool* m_detect_render_targets = nullptr;
  ConfigBool* m_detect_skybox = nullptr;
  ConfigBool* m_use_vulkan_multiview = nullptr;
  ConfigBool* m_passthrough = nullptr;
  ConfigBool* m_passthrough_remove_black_bg = nullptr;
  ConfigBool* m_passthrough_remove_black_clears = nullptr;
  ConfigFloatSlider* m_passthrough_scene_opacity = nullptr;
  QLabel* m_passthrough_scene_opacity_value = nullptr;
  ConfigChoiceMap<VRPassthroughCoverageMode>* m_passthrough_coverage_mode = nullptr;
  ConfigFloatSlider* m_vr_gamma = nullptr;
  QLabel* m_vr_gamma_value = nullptr;
  ConfigFloatSlider* m_resolution_scale = nullptr;
  QLabel* m_resolution_scale_value = nullptr;
  ConfigChoiceMap<int>* m_foveation_level = nullptr;
  ConfigBool* m_dynamic_foveation = nullptr;
  ConfigBool* m_foveate_efb = nullptr;
  ConfigBool* m_eager_heartbeat = nullptr;
  ConfigBool* m_exact_screen_depth = nullptr;
  ConfigBool* m_auto_native_efb_effects = nullptr;
  ConfigFloatSlider* m_hud_thickness = nullptr;
  QLabel* m_hud_thickness_value = nullptr;
  QPushButton* m_reset_general_settings = nullptr;
  ConfigBool* m_load_custom_shaders = nullptr;
  QCheckBox* m_debug_log_draws = nullptr;
  QPointer<CullingCodeFinderWidget> m_culling_finder_widget;
  QPointer<ShaderHunterWidget> m_shader_hunter_widget;
};
