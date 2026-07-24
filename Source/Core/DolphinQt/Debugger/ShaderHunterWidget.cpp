// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Debugger/ShaderHunterWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QBrush>
#include <QColor>
#include <QDir>
#include <QDirIterator>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSize>
#include <QStringList>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/CommonTypes.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigManager.h"
#include "DolphinQt/Settings.h"
#include "VideoCommon/ShaderHunter.h"

namespace
{
static QString ToHashHex(u64 hash)
{
  return QStringLiteral("%1").arg(static_cast<qulonglong>(hash), 16, 16, QLatin1Char('0'));
}

static std::unordered_map<u64, QString> FindTexturePreviewPaths(const std::vector<u64>& hashes)
{
  std::unordered_map<u64, QString> result;
  if (hashes.empty())
    return result;

  std::unordered_set<u64> remaining(hashes.begin(), hashes.end());
  std::unordered_map<u64, QString> patterns;
  patterns.reserve(remaining.size());
  for (u64 hash : remaining)
    patterns.emplace(hash, QStringLiteral("_%1_").arg(ToHashHex(hash)));

  const QString dump_root = QString::fromStdString(File::GetUserPath(D_DUMPTEXTURES_IDX));
  QStringList roots;
  const std::string game_id = SConfig::GetInstance().GetGameID();
  if (!game_id.empty())
    roots.push_back(QDir::cleanPath(dump_root + QString::fromStdString(game_id)));
  roots.push_back(QDir::cleanPath(dump_root));

  for (const QString& root : roots)
  {
    if (!QDir(root).exists())
      continue;

    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && !remaining.empty())
    {
      const QString path = it.next();
      const QFileInfo info(path);
      const QString suffix = info.suffix().toLower();
      if (suffix != QStringLiteral("png") && suffix != QStringLiteral("jpg") &&
          suffix != QStringLiteral("jpeg") && suffix != QStringLiteral("bmp") &&
          suffix != QStringLiteral("tga"))
      {
        continue;
      }

      const QString filename = info.fileName().toLower();
      for (auto iter = remaining.begin(); iter != remaining.end();)
      {
        const u64 hash = *iter;
        if (filename.contains(patterns[hash]))
        {
          result.emplace(hash, path);
          iter = remaining.erase(iter);
        }
        else
        {
          ++iter;
        }
      }
    }

    if (remaining.empty())
      break;
  }

  return result;
}

static QPixmap LoadPreviewPixmapFresh(const QString& path)
{
  QImageReader reader(path);
  reader.setAutoTransform(true);
  const QImage image = reader.read();
  if (image.isNull())
    return {};
  return QPixmap::fromImage(image);
}
}  // namespace

ShaderHunterWidget::ShaderHunterWidget(QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("Shader Hunter"));
  setWindowFlags(windowFlags() | Qt::Window);
  setAttribute(Qt::WA_DeleteOnClose);
  setMinimumWidth(300);

  CreateWidgets();
  ConnectSignals();

  m_update_timer = new QTimer(this);
  connect(m_update_timer, &QTimer::timeout, this, &ShaderHunterWidget::UpdateDisplay);
  m_update_timer->start(100);
}

void ShaderHunterWidget::CreateWidgets()
{
  auto* layout = new QVBoxLayout;
  m_main_layout = layout;

  m_enable_checkbox = new QCheckBox(tr("Enable Shader Hunting"));
  layout->addWidget(m_enable_checkbox);

  auto* hunting_option_layout = new QHBoxLayout;
  hunting_option_layout->addWidget(new QLabel(tr("Hunting Option:")));
  m_hunting_option_combo = new QComboBox;
  m_hunting_option_combo->addItem(tr("Skip"),
                                  static_cast<int>(ShaderHunter::HuntingOption::Skip));
  m_hunting_option_combo->addItem(tr("Pink"),
                                  static_cast<int>(ShaderHunter::HuntingOption::Pink));
  hunting_option_layout->addWidget(m_hunting_option_combo);
  layout->addLayout(hunting_option_layout);

  auto* match_mode_layout = new QHBoxLayout;
  match_mode_layout->addWidget(new QLabel(tr("Match Type:")));
  m_match_mode_combo = new QComboBox;
  m_match_mode_combo->addItem(tr("Shader Family"),
                              static_cast<int>(ShaderHunter::MatchMode::ShaderFamily));
  m_match_mode_combo->addItem(tr("Exact Hash"),
                              static_cast<int>(ShaderHunter::MatchMode::ExactHash));
  m_match_mode_combo->setToolTip(
      tr("Controls both the live Skip/Pink preview and the saved override.\n"
         "Shader Family matches semantic shader variants and is more resilient to Dolphin "
         "shader-generator updates."));
  match_mode_layout->addWidget(m_match_mode_combo);
  layout->addLayout(match_mode_layout);

  auto* type_layout = new QHBoxLayout;
  type_layout->addWidget(new QLabel(tr("Shader Type:")));
  m_type_combo = new QComboBox;
  m_type_combo->addItem(tr("Pixel Shader"));
  m_type_combo->addItem(tr("Vertex Shader"));
  m_type_combo->addItem(tr("Geometry Shader"));
  type_layout->addWidget(m_type_combo);
  layout->addLayout(type_layout);

  m_hash_label = new QLabel(tr("Hash: (none)"));
  m_hash_label->setFont(QFont(QStringLiteral("Courier")));
  layout->addWidget(m_hash_label);

  m_family_signature_label = new QLabel(tr("Family: (none)"));
  m_family_signature_label->setFont(QFont(QStringLiteral("Courier")));
  layout->addWidget(m_family_signature_label);

  m_position_label = new QLabel(tr("- / -"));
  layout->addWidget(m_position_label);

  auto* nav_layout = new QHBoxLayout;
  m_prev_button = new QPushButton(tr("<< Previous"));
  m_next_button = new QPushButton(tr("Next >>"));
  nav_layout->addWidget(m_prev_button);
  nav_layout->addWidget(m_next_button);
  layout->addLayout(nav_layout);

  // Save shader override section
  m_shader_name_edit = new QLineEdit;
  m_shader_name_edit->setPlaceholderText(tr("Enter shader name..."));
  layout->addWidget(m_shader_name_edit);

  auto* handling_layout = new QHBoxLayout;
  handling_layout->addWidget(new QLabel(tr("Override Handling:")));
  m_handling_combo = new QComboBox;
  m_handling_combo->addItem(tr("Skip Draw"), static_cast<int>(ShaderHunter::HandlingType::Skip));
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
  handling_layout->addWidget(m_handling_combo);
  layout->addLayout(handling_layout);

  auto* upm_layout = new QHBoxLayout;
  m_units_per_meter_label = new QLabel(tr("Units per Meter:"));
  m_units_per_meter_spin = new QDoubleSpinBox;
  m_units_per_meter_spin->setRange(Config::GFX_VR_UNITS_PER_METER_MIN,
                                   Config::GFX_VR_UNITS_PER_METER_MAX);
  m_units_per_meter_spin->setDecimals(2);
  m_units_per_meter_spin->setSingleStep(Config::GFX_VR_UNITS_PER_METER_STEP);
  m_units_per_meter_spin->setValue(1.0);
  m_units_per_meter_spin->setToolTip(
      tr("Temporary per-shader scale override for VR when handling is Units per Meter."));
  upm_layout->addWidget(m_units_per_meter_label);
  upm_layout->addWidget(m_units_per_meter_spin);
  layout->addLayout(upm_layout);

  m_save_button = new QPushButton(tr("Save Shader"));
  layout->addWidget(m_save_button);

  m_dump_button = new QPushButton(tr("Dump Shader"));
  layout->addWidget(m_dump_button);

  m_textures_button = new QPushButton(tr("View Textures"));
  m_textures_button->setToolTip(
      tr("Show texture hashes used by the currently selected shader.\n"
         "You can toggle texture-based skipping to isolate specific textures."));
  layout->addWidget(m_textures_button);

  auto* texture_mode_layout = new QHBoxLayout;
  texture_mode_layout->addWidget(new QLabel(tr("Texture Filter Mode:")));
  m_texture_filter_mode_combo = new QComboBox;
  m_texture_filter_mode_combo->addItem(tr("Include"), false);
  m_texture_filter_mode_combo->addItem(tr("Exclude"), true);
  m_texture_filter_mode_combo->setToolTip(
      tr("Include: apply override only when selected textures are present.\n"
         "Exclude: apply override only when selected textures are absent."));
  texture_mode_layout->addWidget(m_texture_filter_mode_combo);
  layout->addLayout(texture_mode_layout);

  m_selected_texture_label = new QLabel;
  m_selected_texture_label->setFont(QFont(QStringLiteral("Courier")));
  m_selected_texture_label->setVisible(false);
  layout->addWidget(m_selected_texture_label);

  setLayout(m_main_layout);
}

void ShaderHunterWidget::ConnectSignals()
{
  connect(m_enable_checkbox, &QCheckBox::toggled, this, [this](bool checked) {
    ShaderHunter::GetInstance().SetEnabled(checked);
    m_hunting_option_combo->setEnabled(checked);
  });
  connect(m_hunting_option_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int) {
            auto& hunter = ShaderHunter::GetInstance();
            const auto option = static_cast<ShaderHunter::HuntingOption>(
                m_hunting_option_combo->currentData().toInt());
            hunter.SetHuntingOption(option);
          });
  connect(m_match_mode_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int) {
            m_saved_texture_filters.clear();
            SetSelectedTextureHashes({});
            auto& hunter = ShaderHunter::GetInstance();
            hunter.SetHuntingMatchMode(static_cast<ShaderHunter::MatchMode>(
                m_match_mode_combo->currentData().toInt()));
            UpdateDisplay();
          });
  connect(m_type_combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
    m_saved_texture_filters.clear();
    SetSelectedTextureHashes({});
    ShaderHunter::GetInstance().SetActiveType(static_cast<ShaderHunter::ShaderType>(index));
  });
  connect(m_prev_button, &QPushButton::clicked, this, [this] {
    m_saved_texture_filters.clear();
    SetSelectedTextureHashes({});
    ShaderHunter::GetInstance().PrevShader();
    UpdateDisplay();
  });
  connect(m_next_button, &QPushButton::clicked, this, [this] {
    m_saved_texture_filters.clear();
    SetSelectedTextureHashes({});
    ShaderHunter::GetInstance().NextShader();
    UpdateDisplay();
  });
  connect(m_save_button, &QPushButton::clicked, this, &ShaderHunterWidget::SaveCurrentShader);
  connect(m_dump_button, &QPushButton::clicked, this, &ShaderHunterWidget::DumpCurrentShader);
  connect(m_textures_button, &QPushButton::clicked, this, &ShaderHunterWidget::ShowTexturesDialog);
  connect(m_texture_filter_mode_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int) { SetSelectedTextureHashes(m_saved_texture_filters); });
  connect(m_handling_combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
    const auto handling =
        static_cast<ShaderHunter::HandlingType>(m_handling_combo->currentData().toInt());
    const bool show_upm = handling == ShaderHunter::HandlingType::UnitsPerMeter;
    m_units_per_meter_label->setVisible(show_upm);
    m_units_per_meter_spin->setVisible(show_upm);
  });
  const auto handling =
      static_cast<ShaderHunter::HandlingType>(m_handling_combo->currentData().toInt());
  const bool show_upm = handling == ShaderHunter::HandlingType::UnitsPerMeter;
  m_units_per_meter_label->setVisible(show_upm);
  m_units_per_meter_spin->setVisible(show_upm);

  const auto hunting_option = ShaderHunter::GetInstance().GetHuntingOption();
  const int hunting_idx = m_hunting_option_combo->findData(static_cast<int>(hunting_option));
  if (hunting_idx >= 0)
    m_hunting_option_combo->setCurrentIndex(hunting_idx);
  const auto match_mode = ShaderHunter::GetInstance().GetHuntingMatchMode();
  const int match_mode_idx = m_match_mode_combo->findData(static_cast<int>(match_mode));
  if (match_mode_idx >= 0)
    m_match_mode_combo->setCurrentIndex(match_mode_idx);
  m_hunting_option_combo->setEnabled(ShaderHunter::GetInstance().IsEnabled());
}

void ShaderHunterWidget::UpdateDisplay()
{
  auto& hunter = ShaderHunter::GetInstance();
  const int pos = hunter.GetSelectedPosition();
  const int total = hunter.GetTotalCount();
  const u64 hash = hunter.GetSelectedHash();
  const auto family_signature = hunter.GetShaderFamilySignature(hunter.GetActiveType(), hash);

  if (pos >= 0 && total > 0)
  {
    m_hash_label->setText(
        tr("Hash: 0x%1").arg(static_cast<uint>(hash), 8, 16, QLatin1Char('0')).toUpper());
    m_position_label->setText(tr("%1 / %2").arg(pos + 1).arg(total));
    if (family_signature.has_value())
    {
      m_family_signature_label->setText(
          tr("Family: 0x%1")
              .arg(static_cast<qulonglong>(*family_signature), 16, 16, QLatin1Char('0'))
              .toUpper());
    }
    else
    {
      m_family_signature_label->setText(tr("Family: (unavailable)"));
    }
  }
  else
  {
    m_hash_label->setText(tr("Hash: (none)"));
    m_family_signature_label->setText(tr("Family: (none)"));
    m_position_label->setText(tr("- / %1").arg(total));
  }
}
void ShaderHunterWidget::SaveCurrentShader()
{
  auto& hunter = ShaderHunter::GetInstance();
  const int pos = hunter.GetSelectedPosition();
  if (pos < 0)
  {
    QMessageBox::warning(this, tr("Save Shader"), tr("No shader selected. Use hunting to select a shader first."));
    return;
  }

  const std::string game_id = SConfig::GetInstance().GetGameID();
  if (game_id.empty())
  {
    QMessageBox::warning(this, tr("Save Shader"), tr("No game is currently running."));
    return;
  }

  // Load existing overrides first so we can auto-generate a unique name when empty.
  auto all = ShaderHunter::LoadOverridesFromINI(game_id);

  std::string name = m_shader_name_edit->text().toStdString();
  if (name.empty())
  {
    constexpr std::string_view unnamed_prefix = "Unnamed Shader";
    int max_unnamed_index = 0;
    for (const auto& existing : all)
    {
      const std::string& existing_name = existing.name;
      if (existing_name == unnamed_prefix)
      {
        max_unnamed_index = std::max(max_unnamed_index, 1);
        continue;
      }

      if (!existing_name.starts_with(unnamed_prefix) || existing_name.size() <= unnamed_prefix.size())
        continue;

      if (existing_name[unnamed_prefix.size()] != ' ')
        continue;

      const std::string suffix = existing_name.substr(unnamed_prefix.size() + 1);
      if (suffix.empty() ||
          !std::all_of(suffix.begin(), suffix.end(),
                       [](unsigned char c) { return std::isdigit(c) != 0; }))
      {
        continue;
      }

      max_unnamed_index = std::max(max_unnamed_index, std::stoi(suffix));
    }

    name = fmt::format("Unnamed Shader {}", max_unnamed_index + 1);
  }

  u64 hash = hunter.GetSelectedHash();
  auto type = hunter.GetActiveType();
  const auto match_mode = static_cast<ShaderHunter::MatchMode>(
      m_match_mode_combo->currentData().toInt());
  const auto family_signature = hunter.GetShaderFamilySignature(type, hash);
  if (match_mode == ShaderHunter::MatchMode::ShaderFamily && !family_signature.has_value())
  {
    QMessageBox::warning(
        this, tr("Save Shader"),
        tr("The selected shader family signature is not available yet. Let the shader render "
           "again, then retry. The override was not saved as an exact hash because that would "
           "not survive shader-generator updates."));
    return;
  }
  const auto handling =
      static_cast<ShaderHunter::HandlingType>(m_handling_combo->currentData().toInt());

  ShaderHunter::ShaderOverride entry;
  entry.name = name;
  entry.hash = hash;
  entry.type = type;
  entry.handling = handling;
  entry.match_mode = match_mode;
  entry.hash_family_match = match_mode == ShaderHunter::MatchMode::ShaderFamily;
  if (family_signature.has_value())
  {
    entry.family_signature = *family_signature;
    entry.family_version = ShaderHunter::FAMILY_SCHEME_VERSION;
  }
  entry.enabled = true;
  entry.user_defined = true;
  if (handling == ShaderHunter::HandlingType::UnitsPerMeter)
    entry.units_per_meter = static_cast<float>(m_units_per_meter_spin->value());
  // Quick-save defaults Passthrough to fully see-through; adjust via the edit dialog.
  if (handling == ShaderHunter::HandlingType::Passthrough)
    entry.passthrough_opacity = 0.0f;
  if (handling == ShaderHunter::HandlingType::Flag)
    entry.flag_group = name;

  // Include texture filters saved from the Texture Hunter popup (if any).
  entry.texture_hashes.insert(entry.texture_hashes.end(), m_saved_texture_filters.begin(),
                              m_saved_texture_filters.end());
  std::sort(entry.texture_hashes.begin(), entry.texture_hashes.end());
  entry.texture_hashes.erase(
      std::unique(entry.texture_hashes.begin(), entry.texture_hashes.end()),
      entry.texture_hashes.end());
  entry.texture_hashes_excluded =
      !entry.texture_hashes.empty() && m_texture_filter_mode_combo->currentData().toBool();

  // Append and save
  all.push_back(entry);
  ShaderHunter::SaveOverridesToINI(game_id, all);

  // Reload runtime overrides
  hunter.LoadOverrides(game_id);
  emit OverridesChanged();
  Settings::Instance().NotifyShaderOverridesChanged();

  const char* type_str = type == ShaderHunter::ShaderType::Pixel    ? "PS" :
                         type == ShaderHunter::ShaderType::Vertex   ? "VS" :
                                                                      "GS";
  const char* handling_str = handling == ShaderHunter::HandlingType::Screen     ? "screen" :
                             handling == ShaderHunter::HandlingType::Fullscreen ||
                                     handling == ShaderHunter::HandlingType::FullscreenMono ?
                                 "fullscreen" :
                             handling == ShaderHunter::HandlingType::HeadLocked ? "headlocked" :
                             handling == ShaderHunter::HandlingType::Flag       ? "flag" :
                             handling == ShaderHunter::HandlingType::UnitsPerMeter ?
                                 "units_per_meter" :
                             handling == ShaderHunter::HandlingType::Passthrough ? "passthrough" :
                                                                                  "skip";
  const QString match_mode_text = entry.hash_family_match ? tr("Shader Family") : tr("Exact Hash");
  QString msg = tr("Saved shader override '%1' (%2, hash 0x%3, match: %4, handling: %5)")
      .arg(QString::fromStdString(name))
      .arg(QString::fromLatin1(type_str))
      .arg(static_cast<uint>(hash), 8, 16, QLatin1Char('0'))
      .arg(match_mode_text)
      .arg(QString::fromLatin1(handling_str));

  if (entry.family_signature != 0)
  {
    msg += tr("\nFamily signature: 0x%1 (scheme v%2)")
               .arg(static_cast<qulonglong>(entry.family_signature), 16, 16,
                    QLatin1Char('0'))
               .arg(entry.family_version);
  }

  if (handling == ShaderHunter::HandlingType::UnitsPerMeter && entry.units_per_meter > 0.0f)
    msg += tr("\nUnits per Meter: %1").arg(entry.units_per_meter, 0, 'f', 2);

  if (!entry.texture_hashes.empty())
  {
    QStringList hash_list;
    for (u64 texture_hash : entry.texture_hashes)
      hash_list.append(QString::number(texture_hash, 16).rightJustified(16, QLatin1Char('0')));
    const QString mode_text = entry.texture_hashes_excluded ? tr("Exclude") : tr("Include");
    msg += tr("\nTexture filter mode: %1").arg(mode_text);
    msg += tr("\nTexture filter(s): %1").arg(hash_list.join(QStringLiteral(", ")));
  }

  msg += tr("\nSaved to %1.ini").arg(QString::fromStdString(game_id));

  QMessageBox::information(this, tr("Save Shader"), msg);
  m_shader_name_edit->clear();
  m_saved_texture_filters.clear();
  m_texture_filter_mode_combo->setCurrentIndex(0);
  SetSelectedTextureHashes({});
}

void ShaderHunterWidget::closeEvent(QCloseEvent* event)
{
  auto& hunter = ShaderHunter::GetInstance();
  hunter.SetEnabled(false);
  m_saved_texture_filters.clear();
  m_texture_filter_mode_combo->setCurrentIndex(0);
  SetSelectedTextureHashes({});
  QDialog::closeEvent(event);
}

void ShaderHunterWidget::DumpCurrentShader()
{
  auto& hunter = ShaderHunter::GetInstance();
  const int pos = hunter.GetSelectedPosition();
  if (pos < 0)
  {
    QMessageBox::warning(this, tr("Dump Shader"),
                         tr("No shader selected. Use hunting to select a shader first."));
    return;
  }

  const std::string game_id = SConfig::GetInstance().GetGameID();
  if (game_id.empty())
  {
    QMessageBox::warning(this, tr("Dump Shader"), tr("No game is currently running."));
    return;
  }

  const u64 hash = hunter.GetSelectedHash();
  const auto type = hunter.GetActiveType();

  if (hunter.DumpShader(game_id, type, hash))
  {
    const char* type_suffix = type == ShaderHunter::ShaderType::Pixel    ? "ps" :
                              type == ShaderHunter::ShaderType::Vertex   ? "vs" :
                                                                           "gs";
    QMessageBox::information(
        this, tr("Dump Shader"),
        tr("Dumped shader to Dump/Shaders/%1/%2-%3.txt")
            .arg(QString::fromStdString(game_id))
            .arg(static_cast<uint>(hash), 8, 16, QLatin1Char('0'))
            .arg(QString::fromLatin1(type_suffix)));
  }
  else
  {
    QMessageBox::warning(this, tr("Dump Shader"),
                         tr("Failed to dump shader. The shader UID may not be cached yet.\n"
                            "Make sure shader hunting is enabled and the shader has been "
                            "seen in the current session."));
  }
}

void ShaderHunterWidget::ShowTexturesDialog()
{
  auto& hunter = ShaderHunter::GetInstance();
  const int pos = hunter.GetSelectedPosition();
  if (pos < 0)
  {
    QMessageBox::warning(this, tr("View Textures"),
                         tr("No shader selected. Enable hunting and select a shader first."));
    return;
  }

  const u64 hash = hunter.GetSelectedHash();
  const auto type = hunter.GetActiveType();
  const char* type_str = type == ShaderHunter::ShaderType::Pixel    ? "PS" :
                         type == ShaderHunter::ShaderType::Vertex   ? "VS" :
                                                                       "GS";
  hunter.SetTextureToolActive(true);

  auto* dlg = new QDialog(this);
  dlg->setWindowTitle(tr("Textures for %1 %2")
      .arg(QString::fromLatin1(type_str))
      .arg(static_cast<uint>(hash), 8, 16, QLatin1Char('0')));
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setMinimumSize(620, 320);

  auto* tree = new QTreeWidget;
  tree->setHeaderLabels({tr("Skip"), tr("Texture Hash"), tr("Name"), tr("Preview")});
  tree->setRootIsDecorated(false);
  tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
  tree->header()->setStretchLastSection(true);
  tree->setIconSize(QSize(64, 64));

  auto* info = new QLabel;
  info->setWordWrap(true);
  auto* continuous_scan_check = new QCheckBox(tr("Continuous Scan"));
  continuous_scan_check->setToolTip(
      tr("Continuously refresh the list while this window is open.\n"
         "Textures seen at least once during scan stay visible in gray."));
  auto* scan_timer = new QTimer(dlg);
  scan_timer->setInterval(250);
  auto seen_textures = std::make_shared<std::unordered_map<u64, std::string>>();

  connect(dlg, &QObject::destroyed, this, []() {
    ShaderHunter::GetInstance().SetTextureToolActive(false);
  });

  const auto update_selected_label = [this, tree]() {
    std::vector<u64> selected_hashes;
    selected_hashes.reserve(tree->topLevelItemCount());
    for (int i = 0; i < tree->topLevelItemCount(); i++)
    {
      QTreeWidgetItem* item = tree->topLevelItem(i);
      if (!item || item->checkState(0) != Qt::Checked)
        continue;
      const u64 texture_hash = item->data(0, Qt::UserRole).toULongLong();
      if (texture_hash != 0)
        selected_hashes.push_back(texture_hash);
    }
    std::sort(selected_hashes.begin(), selected_hashes.end());
    selected_hashes.erase(std::unique(selected_hashes.begin(), selected_hashes.end()),
                          selected_hashes.end());
    SetSelectedTextureHashes(std::vector<uint64_t>(selected_hashes.begin(), selected_hashes.end()));
  };

  const auto populate = [tree, info, continuous_scan_check, seen_textures, &hunter, type_str,
                         hash]() {
    const auto textures = hunter.GetTexturesForSelectedShader();
    std::unordered_map<u64, std::string> current_textures;
    current_textures.reserve(textures.size());
    for (const auto& tex : textures)
      current_textures.emplace(tex.hash, tex.name);

    if (continuous_scan_check->isChecked())
    {
      for (const auto& tex : textures)
      {
        auto& saved_name = (*seen_textures)[tex.hash];
        if (saved_name.empty() && !tex.name.empty())
          saved_name = tex.name;
      }
    }

    std::vector<u64> texture_hashes;
    texture_hashes.reserve(current_textures.size() + seen_textures->size());
    for (const auto& [texture_hash, _] : current_textures)
      texture_hashes.push_back(texture_hash);
    for (const auto& [texture_hash, _] : *seen_textures)
      texture_hashes.push_back(texture_hash);
    std::sort(texture_hashes.begin(), texture_hashes.end());
    texture_hashes.erase(std::unique(texture_hashes.begin(), texture_hashes.end()),
                         texture_hashes.end());
    const auto preview_paths = FindTexturePreviewPaths(texture_hashes);

    const QSignalBlocker blocker(tree);
    tree->clear();

    int seen_only_count = 0;
    for (u64 texture_hash : texture_hashes)
    {
      const auto current_it = current_textures.find(texture_hash);
      const bool in_current_frame = current_it != current_textures.end();
      if (!in_current_frame)
        seen_only_count++;

      auto* item = new QTreeWidgetItem;
      item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
      item->setCheckState(0, hunter.IsTextureSkipEnabled(texture_hash) ? Qt::Checked : Qt::Unchecked);
      item->setText(1,
                    QStringLiteral("%1").arg(static_cast<qulonglong>(texture_hash), 16, 16,
                                             QLatin1Char('0')));
      if (in_current_frame)
      {
        const std::string& name = current_it->second;
        item->setText(2, name.empty() ? QObject::tr("(unknown)") : QString::fromStdString(name));
      }
      else
      {
        const auto seen_it = seen_textures->find(texture_hash);
        const bool has_seen_name = seen_it != seen_textures->end() && !seen_it->second.empty();
        item->setText(2, has_seen_name ? QString::fromStdString(seen_it->second) :
                                         QObject::tr("(seen during scan, not in current frame)"));
        const QBrush gray_brush(QColor(140, 140, 140));
        item->setForeground(1, gray_brush);
        item->setForeground(2, gray_brush);
      }
      item->setData(0, Qt::UserRole, static_cast<qulonglong>(texture_hash));

      auto preview_it = preview_paths.find(texture_hash);
      if (preview_it != preview_paths.end())
      {
        QPixmap pixmap = LoadPreviewPixmapFresh(preview_it->second);
        if (!pixmap.isNull())
        {
          item->setData(3, Qt::DecorationRole,
                        pixmap.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
          item->setSizeHint(3, QSize(72, 72));
        }
      }

      tree->addTopLevelItem(item);
    }

    for (int i = 0; i < 4; i++)
      tree->resizeColumnToContents(i);

    if (current_textures.empty() && seen_only_count == 0)
    {
      info->setText(QObject::tr(
          "No textures captured yet for %1 %2.\n"
          "Enable hunting and let at least one frame render with this shader selected.\n"
          "Then use Refresh or Continuous Scan.")
                        .arg(QString::fromLatin1(type_str))
                        .arg(static_cast<uint>(hash), 8, 16, QLatin1Char('0')));
    }
    else if (current_textures.empty())
    {
      info->setText(QObject::tr(
          "No textures in current frame for %1 %2.\n"
          "%3 texture hash(es) were seen during scan and are shown in gray.")
                        .arg(QString::fromLatin1(type_str))
                        .arg(static_cast<uint>(hash), 8, 16, QLatin1Char('0'))
                        .arg(seen_only_count));
    }
    else
    {
      info->setText(QObject::tr(
          "%1 texture hash(es) in current frame for %3 %4. %2 seen earlier (gray).\n"
          "Check a row to skip draws using that texture for the selected shader.")
                        .arg(current_textures.size())
                        .arg(seen_only_count)
                        .arg(QString::fromLatin1(type_str))
                        .arg(static_cast<uint>(hash), 8, 16, QLatin1Char('0')));
    }
  };

  connect(tree, &QTreeWidget::itemChanged, dlg,
          [&hunter, update_selected_label](QTreeWidgetItem* item, int column) {
    if (!item || column != 0)
      return;
    const u64 texture_hash = item->data(0, Qt::UserRole).toULongLong();
    hunter.SetTextureSkipEnabled(texture_hash, item->checkState(0) == Qt::Checked);
    update_selected_label();
          });

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  auto* refresh_button = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
  auto* clear_button = buttons->addButton(tr("Clear Checked"), QDialogButtonBox::ActionRole);
  auto* save_button = buttons->addButton(tr("Save To Shader"), QDialogButtonBox::ActionRole);

  connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::close);
  connect(refresh_button, &QPushButton::clicked, dlg, [populate, update_selected_label]() {
    populate();
    update_selected_label();
  });
  connect(scan_timer, &QTimer::timeout, dlg, [populate, update_selected_label]() {
    populate();
    update_selected_label();
  });
  connect(continuous_scan_check, &QCheckBox::toggled, dlg,
          [scan_timer, populate, update_selected_label](bool checked) {
    if (checked)
    {
      populate();
      update_selected_label();
      scan_timer->start();
    }
    else
    {
      scan_timer->stop();
    }
  });
  connect(clear_button, &QPushButton::clicked, dlg, [&hunter, populate, update_selected_label]() {
    hunter.ClearTextureSkipFilters();
    populate();
    update_selected_label();
  });
  connect(save_button, &QPushButton::clicked, dlg, [this, tree, dlg]() {
    std::vector<u64> selected_hashes;
    selected_hashes.reserve(tree->topLevelItemCount());
    for (int i = 0; i < tree->topLevelItemCount(); i++)
    {
      QTreeWidgetItem* item = tree->topLevelItem(i);
      if (!item || item->checkState(0) != Qt::Checked)
        continue;
      const u64 texture_hash = item->data(0, Qt::UserRole).toULongLong();
      if (texture_hash != 0)
        selected_hashes.push_back(texture_hash);
    }
    std::sort(selected_hashes.begin(), selected_hashes.end());
    selected_hashes.erase(std::unique(selected_hashes.begin(), selected_hashes.end()),
                          selected_hashes.end());
    m_saved_texture_filters = std::move(selected_hashes);
    SetSelectedTextureHashes(
        std::vector<uint64_t>(m_saved_texture_filters.begin(), m_saved_texture_filters.end()));

    QMessageBox::information(
        dlg, tr("Texture Filters Saved"),
        tr("Saved %1 texture hash(es) to the current shader.\n"
           "Press 'Save Shader' in Shader Hunter to write them to the config.")
            .arg(m_saved_texture_filters.size()));
  });

  auto* layout = new QVBoxLayout;
  layout->addWidget(info);
  layout->addWidget(tree);
  auto* bottom_layout = new QHBoxLayout;
  bottom_layout->addWidget(continuous_scan_check);
  bottom_layout->addStretch();
  bottom_layout->addWidget(buttons);
  layout->addLayout(bottom_layout);
  dlg->setLayout(layout);

  populate();
  update_selected_label();
  QTimer::singleShot(120, dlg, [populate, update_selected_label]() {
    populate();
    update_selected_label();
  });
  dlg->show();
}

void ShaderHunterWidget::SetSelectedTextureHashes(const std::vector<uint64_t>& hashes)
{
  if (hashes.empty())
  {
    m_selected_texture_label->setVisible(false);
    return;
  }

  QStringList hash_list;
  for (uint64_t hash : hashes)
  {
    hash_list.append(
        QStringLiteral("%1").arg(static_cast<qulonglong>(hash), 16, 16, QLatin1Char('0')));
  }

  m_selected_texture_label->setVisible(true);
  const bool exclude_mode = m_texture_filter_mode_combo->currentData().toBool();
  m_selected_texture_label->setText(
      tr("Selected Texture %1:\n%2")
          .arg(exclude_mode ? tr("Exclude Filter(s)") : tr("Include Filter(s)"))
          .arg(hash_list.join(QStringLiteral("\n"))));
}

