// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/VRPatchesWidget.h"

#include <algorithm>
#include <cmath>
#include <variant>

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "Common/FileUtil.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "Core/VRPatch/PatchScript.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"

namespace
{
enum Column
{
  COLUMN_ENABLED,
  COLUMN_TITLE,
  COLUMN_ORIGIN,
  COLUMN_STATUS,
  COLUMN_COUNT,
};

constexpr int SLIDER_STEPS = 1000;

QString StatusText(const VRPatch::PatchInfo& info)
{
  if (!info.manifest_error.empty())
    return QObject::tr("Invalid: %1").arg(QString::fromStdString(info.manifest_error));
  if (!info.for_this_game)
    return QObject::tr("Not for this game or revision");
  QString text = QString::fromUtf8(VRPatch::PatchStateName(info.state));
  if (!info.status.empty())
    text += QStringLiteral(": ") + QString::fromStdString(info.status);
  if (info.state == VRPatch::PatchState::Running)
    text += QObject::tr(" (%1 µs per frame)").arg(std::round(info.average_us));
  return text;
}

// After a change while emulation is paused, the CPU thread would apply it only on resume.
void ApplyNowIfPaused()
{
  Core::System& system = Core::System::GetInstance();
  if (Core::GetState(system) == Core::State::Paused)
    VRPatch::ApplyPendingNow(system);
}

int Decimals(double step)
{
  if (step <= 0)
    return 3;
  int decimals = 0;
  while (decimals < 4 && std::abs(step * std::pow(10.0, decimals) -
                                  std::round(step * std::pow(10.0, decimals))) > 1e-6)
  {
    ++decimals;
  }
  return decimals;
}
}  // namespace

VRPatchesWidget::VRPatchesWidget(std::string game_id, std::optional<u16> revision, QWidget* parent)
    : QWidget(parent), m_game_id(std::move(game_id)), m_revision(revision)
{
  CreateWidgets();
  Refresh(true);

  // The status of a running patch changes on its own (started, stopped by an error).
  m_timer = new QTimer(this);
  connect(m_timer, &QTimer::timeout, this, &VRPatchesWidget::UpdateStatus);
  m_timer->start(1000);
}

void VRPatchesWidget::CreateWidgets()
{
  auto* layout = new QVBoxLayout(this);

  auto* intro = new QLabel(
      tr("VR patches add VR features to this game (an object that follows your controller, "
         "motion controls...). Each is a .lua file you can share: put it in your "
         "VRPatches/%1 folder, or use Import.")
          .arg(QString::fromStdString(m_game_id)));
  intro->setWordWrap(true);
  layout->addWidget(intro);

  m_table = new QTableWidget(0, COLUMN_COUNT);
  m_table->setHorizontalHeaderLabels({tr("On"), tr("Patch"), tr("From"), tr("Status")});
  m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table->setSelectionMode(QAbstractItemView::SingleSelection);
  m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_table->verticalHeader()->hide();
  m_table->horizontalHeader()->setSectionResizeMode(COLUMN_ENABLED, QHeaderView::ResizeToContents);
  m_table->horizontalHeader()->setSectionResizeMode(COLUMN_TITLE, QHeaderView::Stretch);
  m_table->horizontalHeader()->setSectionResizeMode(COLUMN_ORIGIN, QHeaderView::ResizeToContents);
  m_table->horizontalHeader()->setSectionResizeMode(COLUMN_STATUS, QHeaderView::Stretch);
  layout->addWidget(m_table, 1);

  m_details = new QLabel;
  m_details->setWordWrap(true);
  m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(m_details);

  m_settings_group = new QGroupBox(tr("Settings"));
  auto* settings_layout = new QVBoxLayout(m_settings_group);
  m_settings_widget = new QWidget;
  m_settings_form = new QFormLayout(m_settings_widget);
  settings_layout->addWidget(m_settings_widget);
  auto* reset = new QPushButton(tr("Reset to Defaults"));
  settings_layout->addWidget(reset, 0, Qt::AlignRight);
  layout->addWidget(m_settings_group);

  auto* buttons = new QHBoxLayout;
  auto* reload = new QPushButton(tr("Reload"));
  auto* open_folder = new QPushButton(tr("Open Folder"));
  auto* import = new QPushButton(tr("Import..."));
  buttons->addWidget(reload);
  buttons->addWidget(open_folder);
  buttons->addWidget(import);
  buttons->addStretch();
  layout->addLayout(buttons);

  connect(m_table, &QTableWidget::cellChanged, this, &VRPatchesWidget::OnItemChanged);
  connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] { Refresh(true); });
  connect(reset, &QPushButton::clicked, this, &VRPatchesWidget::ResetSettings);
  connect(reload, &QPushButton::clicked, this, [this] {
    VRPatch::ReloadPatches();
    ApplyNowIfPaused();
    Refresh(true);
  });
  connect(open_folder, &QPushButton::clicked, this, &VRPatchesWidget::OpenFolder);
  connect(import, &QPushButton::clicked, this, &VRPatchesWidget::Import);
}

std::optional<VRPatch::PatchInfo> VRPatchesWidget::SelectedPatch() const
{
  const int row = m_table->currentRow();
  if (row < 0 || row >= static_cast<int>(m_patches.size()))
    return std::nullopt;
  return m_patches[row];
}

void VRPatchesWidget::Refresh(bool rebuild_settings)
{
  const std::optional<VRPatch::PatchInfo> selected = SelectedPatch();
  const std::string selected_id = selected ? selected->id : m_settings_for;
  m_patches = VRPatch::ListPatches(m_game_id, m_revision);

  m_updating = true;
  m_table->setRowCount(static_cast<int>(m_patches.size()));
  int selected_row = m_patches.empty() ? -1 : 0;
  for (int row = 0; row < static_cast<int>(m_patches.size()); ++row)
  {
    const VRPatch::PatchInfo& info = m_patches[row];
    auto* enabled = new QTableWidgetItem;
    enabled->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    enabled->setCheckState(info.enabled ? Qt::Checked : Qt::Unchecked);
    m_table->setItem(row, COLUMN_ENABLED, enabled);
    const QString title = info.manifest ? QString::fromStdString(info.manifest->title) :
                                          QString::fromStdString(info.id);
    m_table->setItem(row, COLUMN_TITLE, new QTableWidgetItem(title));
    QString origin = QString::fromUtf8(VRPatch::PatchOriginName(info.origin));
    if (info.overrides_sys)
      origin += tr(" (replaces Sys)");
    m_table->setItem(row, COLUMN_ORIGIN, new QTableWidgetItem(origin));
    m_table->setItem(row, COLUMN_STATUS, new QTableWidgetItem(StatusText(info)));
    if (info.id == selected_id)
      selected_row = row;
  }
  if (selected_row >= 0 && m_table->currentRow() != selected_row)
  {
    const QSignalBlocker blocker(m_table);
    m_table->selectRow(selected_row);
  }
  m_updating = false;

  if (rebuild_settings)
    BuildSettings();
  UpdateStatus();
}

void VRPatchesWidget::UpdateStatus()
{
  const std::vector<VRPatch::PatchInfo> patches = VRPatch::ListPatches(m_game_id, m_revision);
  if (patches.size() != m_patches.size() ||
      !std::ranges::equal(patches, m_patches,
                          [](const auto& a, const auto& b) { return a.id == b.id; }))
  {
    // A file appeared or went away.
    Refresh(true);
    return;
  }
  m_patches = patches;

  m_updating = true;
  for (int row = 0; row < static_cast<int>(m_patches.size()); ++row)
  {
    const VRPatch::PatchInfo& info = m_patches[row];
    if (QTableWidgetItem* item = m_table->item(row, COLUMN_ENABLED))
      item->setCheckState(info.enabled ? Qt::Checked : Qt::Unchecked);
    if (QTableWidgetItem* item = m_table->item(row, COLUMN_STATUS))
      item->setText(StatusText(info));
  }
  m_updating = false;

  const std::optional<VRPatch::PatchInfo> selected = SelectedPatch();
  if (!selected)
  {
    m_details->setText(
        m_patches.empty() ?
            tr("No VR patch for this game yet. Patches for it go in %1")
                .arg(QString::fromStdString(VRPatch::GetUserPatchDirectory(m_game_id))) :
            QString());
    return;
  }
  QString text;
  if (selected->manifest)
  {
    const VRPatch::Manifest& m = *selected->manifest;
    text = QStringLiteral("<b>%1</b>").arg(QString::fromStdString(m.title).toHtmlEscaped());
    if (!m.version.empty())
      text += QStringLiteral(" %1").arg(QString::fromStdString(m.version).toHtmlEscaped());
    if (!m.author.empty())
      text += tr(" by %1").arg(QString::fromStdString(m.author).toHtmlEscaped());
    if (!m.description.empty())
      text += QStringLiteral("<br>") + QString::fromStdString(m.description).toHtmlEscaped();
    for (const VRPatch::OverrideRef& conflict : m.conflicts)
    {
      text += QStringLiteral("<br>") +
              tr("Turns off the %1 override \"%2\" while it runs.")
                  .arg(QString::fromUtf8(VRPatch::OverrideKindName(conflict.kind)),
                       QString::fromStdString(conflict.name).toHtmlEscaped());
    }
  }
  else
  {
    text = QString::fromStdString(selected->manifest_error).toHtmlEscaped();
  }
  if (!selected->path.empty())
  {
    text +=
        QStringLiteral("<br><i>%1</i>").arg(QString::fromStdString(selected->path).toHtmlEscaped());
  }
  m_details->setText(text);
}

void VRPatchesWidget::BuildSettings()
{
  // Rebuild the form for the selected patch.
  delete m_settings_widget;
  m_settings_widget = new QWidget;
  m_settings_form = new QFormLayout(m_settings_widget);
  m_settings_group->layout()->addWidget(m_settings_widget);
  // Keep the reset button last.
  QLayoutItem* reset = m_settings_group->layout()->takeAt(0);
  m_settings_group->layout()->addItem(reset);

  const std::optional<VRPatch::PatchInfo> selected = SelectedPatch();
  m_settings_for = selected ? selected->id : std::string();
  if (!selected || !selected->manifest || selected->manifest->settings.empty())
  {
    m_settings_group->setVisible(false);
    return;
  }
  m_settings_group->setVisible(true);
  m_settings_group->setTitle(
      tr("Settings of %1").arg(QString::fromStdString(selected->manifest->title)));

  const std::string id = selected->id;
  for (const VRPatch::SettingDef& def : selected->manifest->settings)
  {
    const auto value_it = selected->values.find(def.name);
    const double value = value_it == selected->values.end() ? def.default_value : value_it->second;
    QString label = QString::fromStdString(def.label);
    if (!def.unit.empty())
      label += QStringLiteral(" (%1)").arg(QString::fromStdString(def.unit));
    QWidget* editor = nullptr;
    switch (def.type)
    {
    case VRPatch::SettingType::Bool:
    {
      auto* check = new QCheckBox;
      check->setChecked(value != 0);
      connect(check, &QCheckBox::toggled, this,
              [this, id, name = def.name](bool on) { SetSetting(id, name, on ? 1 : 0); });
      editor = check;
      break;
    }
    case VRPatch::SettingType::Choice:
    {
      auto* combo = new QComboBox;
      for (const std::string& option : def.options)
        combo->addItem(QString::fromStdString(option));
      combo->setCurrentIndex(static_cast<int>(value));
      connect(combo, &QComboBox::currentIndexChanged, this,
              [this, id, name = def.name](int index) { SetSetting(id, name, index); });
      editor = combo;
      break;
    }
    case VRPatch::SettingType::Number:
    default:
    {
      auto* row = new QWidget;
      auto* row_layout = new QHBoxLayout(row);
      row_layout->setContentsMargins(0, 0, 0, 0);
      auto* slider = new QSlider(Qt::Horizontal);
      slider->setRange(0, SLIDER_STEPS);
      auto* spin = new QDoubleSpinBox;
      spin->setRange(def.min, def.max);
      spin->setDecimals(Decimals(def.step));
      spin->setSingleStep(def.step > 0 ? def.step : (def.max - def.min) / 100);
      spin->setValue(value);
      const double span = def.max > def.min ? def.max - def.min : 1;
      slider->setValue(static_cast<int>(std::round((value - def.min) / span * SLIDER_STEPS)));
      row_layout->addWidget(slider, 1);
      row_layout->addWidget(spin);
      connect(slider, &QSlider::valueChanged, this, [spin, def, span](int position) {
        double v = def.min + span * position / SLIDER_STEPS;
        if (def.step > 0)
          v = def.min + std::round((v - def.min) / def.step) * def.step;
        spin->setValue(v);
      });
      connect(spin, &QDoubleSpinBox::valueChanged, this,
              [this, id, name = def.name, slider, def, span](double v) {
                const QSignalBlocker blocker(slider);
                slider->setValue(static_cast<int>(std::round((v - def.min) / span * SLIDER_STEPS)));
                SetSetting(id, name, v);
              });
      editor = row;
      break;
    }
    }
    if (!def.description.empty())
      editor->setToolTip(QString::fromStdString(def.description));
    m_settings_form->addRow(label, editor);
  }
}

void VRPatchesWidget::OnItemChanged(int row, int column)
{
  if (m_updating || column != COLUMN_ENABLED || row < 0 ||
      row >= static_cast<int>(m_patches.size()))
  {
    return;
  }
  const bool enabled = m_table->item(row, column)->checkState() == Qt::Checked;
  if (!VRPatch::SetPatchEnabled(m_game_id, m_patches[row].id, enabled))
  {
    ModalMessageBox::warning(this, tr("Error"),
                             tr("The game's VR settings file could not be written."));
  }
  ApplyNowIfPaused();
  UpdateStatus();
}

void VRPatchesWidget::SetSetting(const std::string& id, const std::string& name, double value)
{
  // Sliders send many values: save the last one shortly after the hand stops.
  const std::string key = id + "/" + name;
  m_pending_values[key] = value;
  QTimer*& timer = m_save_timers[key];
  if (!timer)
  {
    timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, id, name, key] {
      VRPatch::SetPatchSetting(m_game_id, id, name, m_pending_values[key]);
      ApplyNowIfPaused();
    });
  }
  timer->start(150);
}

void VRPatchesWidget::ResetSettings()
{
  const std::optional<VRPatch::PatchInfo> selected = SelectedPatch();
  if (!selected || !selected->manifest)
    return;
  for (const VRPatch::SettingDef& def : selected->manifest->settings)
    VRPatch::SetPatchSetting(m_game_id, selected->id, def.name, def.default_value);
  ApplyNowIfPaused();
  Refresh(true);
}

void VRPatchesWidget::OpenFolder()
{
  const std::string folder = VRPatch::GetUserPatchDirectory(m_game_id);
  File::CreateFullPath(folder);
  QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(folder)));
}

void VRPatchesWidget::Import()
{
  const QString path = DolphinFileDialog::getOpenFileName(
      this, tr("Import a VR Patch"), QString(), QStringLiteral("%1 (*.lua)").arg(tr("VR Patches")));
  if (path.isEmpty())
    return;

  std::string source;
  if (!File::ReadFileToString(path.toStdString(), source))
  {
    ModalMessageBox::warning(this, tr("Error"), tr("The file could not be read."));
    return;
  }
  const std::string id = QFileInfo(path).completeBaseName().toStdString();
  if (!VRPatch::IsValidPatchId(id))
  {
    ModalMessageBox::warning(this, tr("Error"),
                             tr("The file name must be letters, digits, '-', '_' or '.'."));
    return;
  }
  const auto read = VRPatch::ReadManifest(source, id);
  if (const std::string* error = std::get_if<std::string>(&read))
  {
    ModalMessageBox::warning(this, tr("Invalid VR Patch"), QString::fromStdString(*error));
    return;
  }
  const VRPatch::Manifest& manifest = std::get<VRPatch::Manifest>(read);
  if (!manifest.games.empty() &&
      std::ranges::none_of(manifest.games,
                           [this](const std::string& game) { return m_game_id.starts_with(game); }))
  {
    if (ModalMessageBox::question(this, tr("Another Game"),
                                  tr("This patch is for %1, not %2. Import it anyway?")
                                      .arg(QString::fromStdString(manifest.games.front()),
                                           QString::fromStdString(m_game_id))) != QMessageBox::Yes)
    {
      return;
    }
  }
  const std::string destination = VRPatch::GetUserPatchDirectory(m_game_id) + id + ".lua";
  if (File::Exists(destination) &&
      ModalMessageBox::question(
          this, tr("Replace"),
          tr("%1 exists. Replace it?").arg(QString::fromStdString(destination))) !=
          QMessageBox::Yes)
  {
    return;
  }
  if (!File::CreateFullPath(destination) || !File::WriteStringToFile(destination, source))
  {
    ModalMessageBox::warning(this, tr("Error"), tr("The patch could not be copied."));
    return;
  }
  VRPatch::ReloadPatches();
  Refresh(true);
}
