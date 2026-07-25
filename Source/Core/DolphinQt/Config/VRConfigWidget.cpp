// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/VRConfigWidget.h"

#include <fstream>
#include <functional>
#include <sstream>
#include <utility>

#include <fmt/format.h>

#include <QCheckBox>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/Config/Layer.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"

#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "Core/ConfigManager.h"
#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"
#include "DolphinQt/Config/ConfigControls/ConfigFloatSlider.h"
#include "DolphinQt/Config/ConfigControls/ConfigSlider.h"
#include "DolphinQt/Config/GameConfigEdit.h"
#include "DolphinQt/QtUtils/QtUtils.h"
#include "DolphinQt/QtUtils/WrapInScrollArea.h"
#include "DolphinQt/Settings.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
constexpr std::string_view VR_SECTION_NAME = "Graphics.VR";
constexpr std::string_view LEGACY_VR_SECTION_NAME = "GFX.VR";

Config::Location GetVRLocation(const std::string& key)
{
  return {Config::System::GFX, "VR", key};
}

// Common::IniFile cannot round-trip the repeated `$Name` records used by the custom VR override
// sections. Rewriting a per-game VR setting through IniFile therefore strips the names and merges
// the override bodies. Replace only the two VR settings sections as text and preserve every other
// section byte-for-byte.
std::string ReadFileWithoutVRSettingsSections(const std::string& path)
{
  std::ifstream file(path);
  if (!file.is_open())
    return {};

  std::ostringstream out;
  bool skipping = false;
  std::string line;
  while (std::getline(file, line))
  {
    std::string trimmed = line;
    if (!trimmed.empty() && trimmed.back() == '\r')
      trimmed.pop_back();

    if (trimmed == "[Graphics.VR]" || trimmed == "[GFX.VR]")
    {
      skipping = true;
      continue;
    }
    if (skipping && !trimmed.empty() && trimmed.front() == '[')
      skipping = false;

    if (!skipping)
      out << line << '\n';
  }
  return out.str();
}

class VRGameConfigLayerLoader final : public Config::ConfigLayerLoader
{
public:
  VRGameConfigLayerLoader(std::string game_id, std::optional<u16> revision, bool global)
      : ConfigLayerLoader(global ? Config::LayerType::GlobalGame : Config::LayerType::LocalGame),
        m_game_id(std::move(game_id)), m_revision(revision), m_global(global)
  {
  }

  void Load(Config::Layer* layer) override
  {
    Common::IniFile ini;
    const std::string directory = m_global ? File::GetSysDirectory() + GAMESETTINGSVR_DIR DIR_SEP :
                                             File::GetUserPath(D_GAMESETTINGSVR_IDX);

    for (const std::string& filename : ConfigLoaders::GetGameIniFilenames(m_game_id, m_revision))
    {
      ini.Load(directory + filename, true);
    }

    LoadSection(layer, ini.GetSection(LEGACY_VR_SECTION_NAME));
    LoadSection(layer, ini.GetSection(VR_SECTION_NAME));

    // Keep old per-game files useful in the new controls. They are written back using the current
    // names the next time the user changes a setting.
    const Config::Location passthrough = GetVRLocation("Passthrough");
    const Config::Location legacy_ar_mode = GetVRLocation("ARMode");
    if (!layer->Exists(passthrough))
    {
      if (const std::optional<bool> value = layer->Get<bool>(legacy_ar_mode))
        layer->Set(passthrough, *value);
    }

    const Config::Location forced_vbi = GetVRLocation("ForcedVBIFrequency");
    const Config::Location legacy_auto_vbi = GetVRLocation("AutoVBIFromHMD");
    if (!layer->Exists(forced_vbi))
    {
      if (const std::optional<bool> value = layer->Get<bool>(legacy_auto_vbi))
      {
        layer->Set(forced_vbi, *value ? Config::GFX_VR_FORCED_VBI_FREQUENCY_90 :
                                        Config::GFX_VR_FORCED_VBI_FREQUENCY_OFF);
      }
    }
  }

  void Save(Config::Layer* layer) override
  {
    if (m_global)
      return;

    const std::string path = GetSavePath();
    std::string base = ReadFileWithoutVRSettingsSections(path);
    while (!base.empty() &&
           (base.back() == '\n' || base.back() == '\r' || base.back() == ' '))
    {
      base.pop_back();
    }
    if (!base.empty())
      base += '\n';

    std::ostringstream out;
    out << base;
    bool wrote_header = false;

    for (const auto& [location, value] : layer->GetLayerMap())
    {
      if (location.system != Config::System::GFX || location.section != "VR" || !value ||
          location.key == "ARMode" || location.key == "AutoVBIFromHMD")
      {
        continue;
      }

      if (!wrote_header)
      {
        out << '[' << VR_SECTION_NAME << "]\n";
        wrote_header = true;
      }
      out << location.key << " = " << *value << '\n';
    }

    File::CreateFullPath(path);
    const std::string contents = out.str();
    if (contents.empty())
    {
      File::Delete(path, File::IfAbsentBehavior::NoConsoleWarning);
      return;
    }

    std::ofstream file(path, std::ios::trunc);
    file << contents;
  }

private:
  static void LoadSection(Config::Layer* layer, const Common::IniFile::Section* section)
  {
    if (!section)
      return;

    for (const auto& [key, value] : section->GetValues())
      layer->Set(GetVRLocation(key), value);
  }

  std::string GetSavePath() const
  {
    const std::string directory = File::GetUserPath(D_GAMESETTINGSVR_IDX);
    if (m_revision)
    {
      const std::string revision_path =
          fmt::format("{}{}r{}.ini", directory, m_game_id, *m_revision);
      if (File::Exists(revision_path))
        return revision_path;
    }
    return fmt::format("{}{}.ini", directory, m_game_id);
  }

  const std::string m_game_id;
  const std::optional<u16> m_revision;
  const bool m_global;
};

void ResetTabPages(QTabWidget* tab)
{
  while (tab->count() > 0)
  {
    QWidget* const page = tab->widget(0);
    tab->removeTab(0);
    delete page;
  }
}

void PopulateVRConfigTab(QTabWidget* tab, const std::string& path, const std::string& game_id,
                         std::optional<u16> revision, bool read_only)
{
  for (const std::string& filename : ConfigLoaders::GetGameIniFilenames(game_id, revision))
  {
    const std::string ini_path = path + filename;
    if (File::Exists(ini_path))
    {
      auto* edit = new GameConfigEdit(nullptr, QString::fromStdString(ini_path), read_only);
      tab->addTab(edit, QString::fromStdString(filename));
    }
  }
}

bool HasTab(const QTabWidget* tab, const QString& name)
{
  for (int i = 0; i < tab->count(); ++i)
  {
    if (tab->tabText(i) == name)
      return true;
  }
  return false;
}
}  // namespace

VRConfigWidget::VRConfigWidget(std::string game_id, std::optional<u16> revision, QWidget* parent)
    : QWidget(parent), m_game_id(std::move(game_id)), m_revision(revision),
      m_layer(std::make_unique<Config::Layer>(
          std::make_unique<VRGameConfigLayerLoader>(m_game_id, m_revision, false))),
      m_global_layer(std::make_unique<Config::Layer>(
          std::make_unique<VRGameConfigLayerLoader>(m_game_id, m_revision, true)))
{
  CreateWidgets();
  m_saved_layer_state = GetLayerState();

  connect(&Settings::Instance(), &Settings::ConfigChanged, this, [this] { SaveSettings(); });

  // Config controls update their bold state on ConfigChanged. Delay this for the same reason as
  // Game Config: fonts are not reliably propagated until the widget hierarchy has been created.
  QTimer::singleShot(100, this, [] { Config::OnConfigChanged(); });
}

VRConfigWidget::~VRConfigWidget()
{
  m_layer->Save();
  if (SConfig::GetInstance().GetGameID() == m_game_id)
    SConfig::GetInstance().ReloadGameVRConfigOverrides();
}

void VRConfigWidget::CreateWidgets()
{
  Config::Layer* const layer = m_layer.get();

  const auto mark_default = [this](auto* control) {
    if (!m_global_layer->Exists(control->GetLocation()))
      return control;

    QFont font = control->font();
    font.setItalic(true);
    control->setFont(font);
    return control;
  };

  const auto make_bool = [this, layer, &mark_default](const QString& text,
                                                      const Config::Info<bool>& setting) {
    return mark_default(new ConfigBool(text, setting, layer, false, m_global_layer.get()));
  };

  const auto add_float =
      [this, layer, &mark_default](
          QGridLayout* layout, int row, const QString& text, float minimum, float maximum,
          const Config::Info<float>& setting, float step, std::function<QString(float)> format,
          ConfigFloatSlider::ScaleMode scale = ConfigFloatSlider::ScaleMode::Linear) {
        auto* slider = mark_default(new ConfigFloatSlider(minimum, maximum, setting, step, layer,
                                                          scale, m_global_layer.get()));
        auto* value = new QLabel;
        const auto update = [slider, value, format = std::move(format)] {
          value->setText(format(slider->GetValue()));
        };
        update();
        QObject::connect(slider, &ConfigFloatSlider::valueChanged, slider, update);
        if (!text.isEmpty())
          layout->addWidget(new ConfigFloatLabel(text, slider), row, 0);
        layout->addWidget(slider, row, 1);
        layout->addWidget(value, row, 2);
        return slider;
      };

  const auto add_int = [this, layer, &mark_default](QGridLayout* layout, int row,
                                                    const QString& text, int minimum, int maximum,
                                                    const Config::Info<int>& setting, int step,
                                                    std::function<QString(int)> format) {
    auto* slider = mark_default(
        new ConfigSlider(minimum, maximum, setting, layer, step, m_global_layer.get()));
    auto* value = new QLabel;
    const auto update = [slider, value, format = std::move(format)] {
      value->setText(format(slider->value()));
    };
    update();
    QObject::connect(slider, &ConfigSlider::valueChanged, slider, update);
    layout->addWidget(new ConfigSliderLabel(text, slider), row, 0);
    layout->addWidget(slider, row, 1);
    layout->addWidget(value, row, 2);
    return slider;
  };

  auto* general_page = new QWidget;
  auto* general_layout = new QVBoxLayout(general_page);

  auto* openxr_group = new QGroupBox(tr("OpenXR"));
  auto* openxr_layout = new QGridLayout(openxr_group);
  openxr_layout->addWidget(make_bool(tr("Enable VR"), Config::GFX_VR_ENABLE_OPENXR), 0, 0, 1, 3);
  add_float(
      openxr_layout, 1, tr("Units per Meter:"), Config::GFX_VR_UNITS_PER_METER_MIN,
      Config::GFX_VR_UNITS_PER_METER_MAX, Config::GFX_VR_UNITS_PER_METER,
      Config::GFX_VR_UNITS_PER_METER_STEP,
      [](float value) {
        return QString::asprintf(value < 10.0f ? "%.2f" : (value < 100.0f ? "%.1f" : "%.0f"),
                                 value);
      },
      ConfigFloatSlider::ScaleMode::Exponential);

  auto* mirror_view = mark_default(new ConfigChoiceMap<OpenXRMirrorView>(
      {{tr("Both Eyes"), OpenXRMirrorView::BothEyes},
       {tr("Left Eye"), OpenXRMirrorView::LeftEye},
       {tr("Right Eye"), OpenXRMirrorView::RightEye},
       {tr("None"), OpenXRMirrorView::None}},
      Config::GFX_VR_MIRROR_VIEW, layer, m_global_layer.get()));
  openxr_layout->addWidget(new QLabel(tr("Desktop Mirror View:")), 2, 0);
  openxr_layout->addWidget(mirror_view, 2, 1, 1, 2);
  openxr_layout->addWidget(make_bool(tr("Flat Screen (2D, no stereo)"), Config::GFX_VR_FLAT_SCREEN),
                           3, 0, 1, 3);

  auto* camera_group = new QGroupBox(tr("Camera"));
  auto* camera_layout = new QGridLayout(camera_group);
  camera_layout->addWidget(
      make_bool(tr("Lean Back Angle (deg)"), Config::GFX_VR_ENABLE_LEAN_BACK_ANGLE), 0, 0);
  add_float(camera_layout, 0, QString{}, Config::GFX_VR_LEAN_BACK_ANGLE_MIN,
            Config::GFX_VR_LEAN_BACK_ANGLE_MAX, Config::GFX_VR_LEAN_BACK_ANGLE,
            Config::GFX_VR_LEAN_BACK_ANGLE_STEP,
            [](float value) { return QString::asprintf("%.1f", value); });
  camera_layout->addWidget(
      make_bool(tr("Camera Forward (m)"), Config::GFX_VR_ENABLE_CAMERA_FORWARD), 1, 0);
  add_float(camera_layout, 1, QString{}, Config::GFX_VR_CAMERA_FORWARD_MIN,
            Config::GFX_VR_CAMERA_FORWARD_MAX, Config::GFX_VR_CAMERA_FORWARD,
            Config::GFX_VR_CAMERA_FORWARD_STEP,
            [](float value) { return QString::asprintf("%.1f", value); });
  camera_layout->addWidget(
      make_bool(tr("Camera Height (m)"), Config::GFX_VR_ENABLE_CAMERA_HEIGHT), 2, 0);
  add_float(camera_layout, 2, QString{}, Config::GFX_VR_CAMERA_HEIGHT_MIN,
            Config::GFX_VR_CAMERA_HEIGHT_MAX, Config::GFX_VR_CAMERA_HEIGHT,
            Config::GFX_VR_CAMERA_HEIGHT_STEP,
            [](float value) { return QString::asprintf("%.1f", value); });
  camera_layout->addWidget(make_bool(tr("Camera Anchor"), Config::GFX_VR_ENABLE_CAMERA_ANCHOR), 3,
                           0);
  add_float(camera_layout, 3, QString{}, Config::GFX_VR_CAMERA_ANCHOR_SMOOTHING_MIN,
            Config::GFX_VR_CAMERA_ANCHOR_SMOOTHING_MAX, Config::GFX_VR_CAMERA_ANCHOR_SMOOTHING,
            Config::GFX_VR_CAMERA_ANCHOR_SMOOTHING_STEP,
            [](float value) { return QString::asprintf("%.2f", value); });
  camera_layout->addWidget(
      make_bool(tr("Controller Anchor"), Config::GFX_VR_ENABLE_CONTROLLER_ANCHOR), 4, 0, 1, 3);

  auto* virtual_screen_group = new QGroupBox(tr("Virtual Screen"));
  auto* virtual_screen_layout = new QGridLayout(virtual_screen_group);
  virtual_screen_layout->addWidget(
      make_bool(tr("Virtual Screen for 2D Content"), Config::GFX_VR_VIRTUAL_SCREEN), 0, 0, 1, 3);
  virtual_screen_layout->addWidget(
      make_bool(tr("Exact Screen Depth"), Config::GFX_VR_EXACT_SCREEN_DEPTH), 1, 0, 1, 3);
  virtual_screen_layout->addWidget(
      make_bool(tr("Auto-Exclude EFB Effects"), Config::GFX_VR_AUTO_NATIVE_EFB_EFFECTS), 2, 0, 1,
      3);
  add_float(virtual_screen_layout, 3, tr("Screen Distance (m):"),
            Config::GFX_VR_SCREEN_DISTANCE_MIN, Config::GFX_VR_SCREEN_DISTANCE_MAX,
            Config::GFX_VR_SCREEN_DISTANCE, Config::GFX_VR_SCREEN_DISTANCE_STEP,
            [](float value) { return QString::asprintf("%.1f", value); });
  add_float(virtual_screen_layout, 4, tr("Screen Size (m):"), Config::GFX_VR_SCREEN_SIZE_MIN,
            Config::GFX_VR_SCREEN_SIZE_MAX, Config::GFX_VR_SCREEN_SIZE,
            Config::GFX_VR_SCREEN_SIZE_STEP,
            [](float value) { return QString::asprintf("%.1f", value); });
  add_float(virtual_screen_layout, 5, tr("Screen Depth:"), Config::GFX_VR_HUD_THICKNESS_MIN,
            Config::GFX_VR_HUD_THICKNESS_MAX, Config::GFX_VR_HUD_THICKNESS,
            Config::GFX_VR_HUD_THICKNESS_STEP, [this](float value) {
              return value <= 0.001f ? tr("Off") : QString::asprintf("%.2f m", value);
            });
  add_float(virtual_screen_layout, 6, tr("Head Locked Curvature:"),
            Config::GFX_VR_HEAD_LOCKED_CURVATURE_MIN, Config::GFX_VR_HEAD_LOCKED_CURVATURE_MAX,
            Config::GFX_VR_HEAD_LOCKED_CURVATURE, Config::GFX_VR_HEAD_LOCKED_CURVATURE_STEP,
            [](float value) { return QString::asprintf("%.2f", value); });

  auto* rendering_group = new QGroupBox(tr("Rendering"));
  auto* rendering_layout = new QGridLayout(rendering_group);
  add_int(rendering_layout, 0, tr("Clear EFB (min width):"), Config::GFX_VR_CLEAR_EFB_MIN,
          Config::GFX_VR_CLEAR_EFB_MAX, Config::GFX_VR_CLEAR_EFB_COPIES,
          Config::GFX_VR_CLEAR_EFB_STEP,
          [this](int value) { return value == 0 ? tr("Off") : QString::number(value); });
  add_float(rendering_layout, 1, tr("VR Gamma:"), Config::GFX_VR_GAMMA_MIN,
            Config::GFX_VR_GAMMA_MAX, Config::GFX_VR_GAMMA, Config::GFX_VR_GAMMA_STEP,
            [this](float value) {
              return value <= 1.01f ? tr("Off") : QString::asprintf("%.1f", value);
            });
  add_float(rendering_layout, 2, tr("Resolution Scale:"), Config::GFX_VR_RESOLUTION_SCALE_MIN,
            Config::GFX_VR_RESOLUTION_SCALE_MAX, Config::GFX_VR_RESOLUTION_SCALE,
            Config::GFX_VR_RESOLUTION_SCALE_STEP,
            [](float value) { return QString::asprintf("%.2fx", value); });

  auto* foveation_level = mark_default(
      new ConfigChoiceMap<int>({{tr("Off"), 0}, {tr("Low"), 1}, {tr("Medium"), 2}, {tr("High"), 3}},
                               Config::GFX_VR_FOVEATION_LEVEL, layer, m_global_layer.get()));
  rendering_layout->addWidget(new QLabel(tr("Foveated Rendering:")), 3, 0);
  rendering_layout->addWidget(foveation_level, 3, 1);
  rendering_layout->addWidget(make_bool(tr("Dynamic"), Config::GFX_VR_FOVEATION_DYNAMIC), 3, 2);
  rendering_layout->addWidget(
      make_bool(tr("Foveate Game Render (EFB)"), Config::GFX_VR_EFB_FOVEATION), 4, 1, 1, 2);

  general_layout->addWidget(openxr_group);
  general_layout->addWidget(camera_group);
  general_layout->addWidget(virtual_screen_group);
  general_layout->addWidget(rendering_group);
  general_layout->addStretch();

  auto* hacks_page = new QWidget;
  auto* hacks_layout = new QVBoxLayout(hacks_page);

  auto* hacks_group = new QGroupBox(tr("VR Hacks"));
  auto* hacks_group_layout = new QVBoxLayout(hacks_group);
  hacks_group_layout->addWidget(
      make_bool(tr("Use Vulkan Multiview"), Config::GFX_VR_USE_VULKAN_MULTIVIEW));
  hacks_group_layout->addWidget(
      make_bool(tr("Don't Clear Screen"), Config::GFX_VR_DONT_CLEAR_SCREEN));
  hacks_group_layout->addWidget(
      make_bool(tr("Disable CPU Culling in VR"), Config::GFX_VR_DISABLE_CPU_CULL));
  hacks_group_layout->addWidget(make_bool(tr("Remove Cinematic Bars"), Config::GFX_VR_REMOVE_BARS));
  hacks_group_layout->addWidget(
      make_bool(tr("Frame Size from XFB Copy"), Config::GFX_VR_FRAME_SIZE_FROM_XFB));
  hacks_group_layout->addWidget(
      make_bool(tr("Small 3D Viewports on Screen"), Config::GFX_VR_PANES_ON_SCREEN));
  hacks_group_layout->addWidget(
      make_bool(tr("Detect Render-to-Texture Viewports"), Config::GFX_VR_DETECT_RENDER_TARGETS));
  hacks_group_layout->addWidget(
      make_bool(tr("Ortho Scissor Fix"), Config::GFX_VR_ORTHO_SCISSOR_FIX));
  hacks_group_layout->addWidget(make_bool(tr("Detect Skybox"), Config::GFX_VR_DETECT_SKYBOX));
  hacks_group_layout->addWidget(
      make_bool(tr("Layered Palette Conversion Path"), Config::GFX_VR_METROID_THERMAL_VISOR_FIX));

  auto* framerate_group = new QGroupBox(tr("Framerate"));
  auto* framerate_layout = new QGridLayout(framerate_group);
  auto* forced_vbi = mark_default(
      new ConfigChoiceMap<int>({{tr("Auto"), Config::GFX_VR_FORCED_VBI_FREQUENCY_AUTO},
                                {tr("Off"), Config::GFX_VR_FORCED_VBI_FREQUENCY_OFF},
                                {tr("72 Hz"), Config::GFX_VR_FORCED_VBI_FREQUENCY_72},
                                {tr("90 Hz"), Config::GFX_VR_FORCED_VBI_FREQUENCY_90},
                                {tr("120 Hz"), Config::GFX_VR_FORCED_VBI_FREQUENCY_120}},
                               Config::GFX_VR_FORCED_VBI_FREQUENCY, layer, m_global_layer.get()));
  framerate_layout->addWidget(new QLabel(tr("Forced VBI Frequency:")), 0, 0);
  framerate_layout->addWidget(forced_vbi, 0, 1);
  framerate_layout->addWidget(
      make_bool(tr("Eager Frame Heartbeat"), Config::GFX_VR_EAGER_HEARTBEAT), 1, 0, 1, 2);

  auto* shaders_group = new QGroupBox(tr("Shaders"));
  auto* shaders_layout = new QVBoxLayout(shaders_group);
  shaders_layout->addWidget(
      make_bool(tr("Load Custom Shaders"), Config::GFX_VR_LOAD_CUSTOM_SHADERS));

  auto* passthrough_group = new QGroupBox(tr("Passthrough"));
  auto* passthrough_layout = new QGridLayout(passthrough_group);
  passthrough_layout->addWidget(make_bool(tr("Enable Passthrough"), Config::GFX_VR_PASSTHROUGH), 0,
                                0, 1, 3);
  passthrough_layout->addWidget(
      make_bool(tr("Reveal Unrendered Areas"), Config::GFX_VR_PASSTHROUGH_REMOVE_BLACK_BG), 1, 0, 1,
      3);
  passthrough_layout->addWidget(
      make_bool(tr("Remove Black EFB Clears"), Config::GFX_VR_PASSTHROUGH_REMOVE_BLACK_CLEARS), 2,
      0, 1, 3);
  add_float(passthrough_layout, 3, tr("Scene Opacity:"),
            Config::GFX_VR_PASSTHROUGH_SCENE_OPACITY_MIN,
            Config::GFX_VR_PASSTHROUGH_SCENE_OPACITY_MAX, Config::GFX_VR_PASSTHROUGH_SCENE_OPACITY,
            Config::GFX_VR_PASSTHROUGH_SCENE_OPACITY_STEP,
            [](float value) { return QString::asprintf("%.2f", value); });
  auto* coverage_mode = mark_default(new ConfigChoiceMap<VRPassthroughCoverageMode>(
      {{tr("Exact"), VRPassthroughCoverageMode::Exact},
       {tr("Fast"), VRPassthroughCoverageMode::Fast}},
      Config::GFX_VR_PASSTHROUGH_COVERAGE_MODE, layer, m_global_layer.get()));
  passthrough_layout->addWidget(new QLabel(tr("Coverage Mode:")), 4, 0);
  passthrough_layout->addWidget(coverage_mode, 4, 1, 1, 2);

  auto* tracking_group = new QGroupBox(tr("Tracking"));
  auto* tracking_layout = new QGridLayout(tracking_group);
  auto* reference_space = mark_default(new ConfigChoiceMap<OpenXRReferenceSpaceMode>(
      {{tr("LOCAL"), OpenXRReferenceSpaceMode::Local},
       {tr("STAGE + Height"), OpenXRReferenceSpaceMode::StageHeight},
       {tr("STAGE"), OpenXRReferenceSpaceMode::Stage}},
      Config::GFX_VR_REFERENCE_SPACE_MODE, layer, m_global_layer.get()));
  auto* tracking_mode = mark_default(new ConfigChoiceMap<OpenXRTrackingMode>(
      {{tr("6DoF"), OpenXRTrackingMode::Full6DoF},
       {tr("3DoF"), OpenXRTrackingMode::Rotation3DoF},
       {tr("None"), OpenXRTrackingMode::None}},
      Config::GFX_VR_TRACKING_MODE, layer, m_global_layer.get()));
  tracking_layout->addWidget(new QLabel(tr("Default VR Position:")), 0, 0);
  tracking_layout->addWidget(reference_space, 0, 1);
  tracking_layout->addWidget(new QLabel(tr("Tracking Mode:")), 1, 0);
  tracking_layout->addWidget(tracking_mode, 1, 1);

  hacks_layout->addWidget(hacks_group);
  hacks_layout->addWidget(framerate_group);
  hacks_layout->addWidget(shaders_group);
  hacks_layout->addWidget(passthrough_group);
  hacks_layout->addWidget(tracking_group);
  hacks_layout->addStretch();

  auto* editor_page = new QWidget;
  auto* editor_layout = new QVBoxLayout(editor_page);
  auto* default_group = new QGroupBox(tr("VR Default Config (Read Only)"));
  auto* default_layout = new QVBoxLayout(default_group);
  m_default_tab = new QTabWidget;
  default_layout->addWidget(m_default_tab);
  auto* local_group = new QGroupBox(tr("VR User Config"));
  auto* local_layout = new QVBoxLayout(local_group);
  m_local_tab = new QTabWidget;
  local_layout->addWidget(m_local_tab);
  editor_layout->addWidget(default_group);
  editor_layout->addWidget(local_group);

  auto* tabs = new QTabWidget;
  tabs->addTab(GetWrappedWidget(general_page), tr("General"));
  tabs->addTab(GetWrappedWidget(hacks_page), tr("Hacks"));
  const int editor_index = tabs->addTab(editor_page, tr("Editor"));

  connect(tabs, &QTabWidget::currentChanged, this, [this, editor_index](int index) {
    if (index == editor_index)
    {
      m_layer->Save();
      RefreshEditorTabs();
    }

    if (m_prev_tab_index == editor_index)
      ReloadSettings();

    m_prev_tab_index = index;
  });

  const QString help_message =
      tr("Italics mark default game settings and bold marks user settings. "
         "Right-click a control to remove its user setting.");
  auto* help_label = new QLabel(tr("These VR settings override core Dolphin settings."));
  auto* help_widget = QtUtils::CreateIconWarning(this, QStyle::SP_MessageBoxQuestion, help_label);
  help_widget->setToolTip(help_message);

  auto* root_layout = new QVBoxLayout;
  root_layout->addWidget(help_widget);
  root_layout->addWidget(tabs);
  setLayout(root_layout);

  RefreshEditorTabs();
}

void VRConfigWidget::SaveSettings()
{
  const std::string layer_state = GetLayerState();
  if (layer_state == m_saved_layer_state)
    return;

  m_layer->Save();
  m_saved_layer_state = layer_state;
  if (SConfig::GetInstance().GetGameID() == m_game_id)
    SConfig::GetInstance().ReloadGameVRConfigOverrides();
}

void VRConfigWidget::ReloadSettings()
{
  m_layer->DeleteAllKeys();
  m_layer->Load();
  m_saved_layer_state = GetLayerState();
  Config::OnConfigChanged();
  if (SConfig::GetInstance().GetGameID() == m_game_id)
    SConfig::GetInstance().ReloadGameVRConfigOverrides();
}

void VRConfigWidget::RefreshEditorTabs()
{
  if (!m_default_tab || !m_local_tab || m_game_id.empty())
    return;

  ResetTabPages(m_default_tab);
  ResetTabPages(m_local_tab);

  PopulateVRConfigTab(m_default_tab, File::GetSysDirectory() + GAMESETTINGSVR_DIR DIR_SEP,
                      m_game_id, m_revision, true);
  PopulateVRConfigTab(m_local_tab, File::GetUserPath(D_GAMESETTINGSVR_IDX), m_game_id, m_revision,
                      false);

  const QString local_tab_name = QString::fromStdString(m_game_id + ".ini");
  if (!HasTab(m_local_tab, local_tab_name))
  {
    m_local_tab->addTab(
        new GameConfigEdit(nullptr, QString::fromStdString(GetLocalINIPath()), false),
        local_tab_name);
  }
}

std::string VRConfigWidget::GetLocalINIPath() const
{
  return File::GetUserPath(D_GAMESETTINGSVR_IDX) + m_game_id + ".ini";
}

std::string VRConfigWidget::GetLayerState() const
{
  std::string state;
  for (const auto& [location, value] : m_layer->GetLayerMap())
  {
    const auto append_part = [&state](std::string_view part) {
      state += std::to_string(part.size());
      state += ':';
      state += part;
    };

    state += std::to_string(static_cast<int>(location.system));
    state += ':';
    append_part(location.section);
    append_part(location.key);
    state += value ? '1' : '0';
    if (value)
      append_part(*value);
  }
  return state;
}
