// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/TextureElementOverrideWidget.h"

#include <algorithm>

#include <QColor>
#include <QCursor>
#include <QDialog>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QShowEvent>
#include <QStringList>
#include <QStyle>
#include <QVBoxLayout>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"

#include "Core/Config/GraphicsSettings.h"

#include "DolphinQt/Config/TextureElementOverrideAddEditDialog.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/QtUtils/QtUtils.h"
#include "DolphinQt/Settings.h"
#include "VideoCommon/ElementsGroupManager.h"
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/TextureElementManager.h"

using HandlingType = TextureElementManager::HandlingType;
using TextureElementOverride = TextureElementManager::TextureElementOverride;

TextureElementOverrideWidget::TextureElementOverrideWidget(std::string game_id,
                                                           std::optional<u16> revision)
    : m_game_id(std::move(game_id)), m_revision(revision)
{
  CreateWidgets();
  ConnectWidgets();
  LoadOverrides();
}

TextureElementOverrideWidget::~TextureElementOverrideWidget() = default;

void TextureElementOverrideWidget::CreateWidgets()
{
  m_code_list = new QListWidget;
  m_code_list->setContextMenuPolicy(Qt::CustomContextMenu);

  auto* info_label = new QLabel(
      tr("Texture Element Overrides reclassify VR draws based purely on the bound texture hash,\n"
         "across all shaders and elements. Use the Texture Hunter to identify a texture, then\n"
         "apply a handling to every draw that binds it.\n"
         "Handling: Skip = hide, Screen = world-fixed, Head Locked = follows head,\n"
         "Fullscreen = no VR, Flag = detect texture presence for shared override conditions.\n"
         "Handling changes are a fallback after Shader and Elements Group overrides; flags are\n"
         "global and can condition overrides in all three tools."));
  info_label->setWordWrap(true);

  // Warning shown when neither the texture preview nor the Import Texture button can find any
  // textures to work with: the game has no dumped textures and Dump Textures is disabled, so the
  // Texture Hunter has nothing to enumerate. See UpdateDumpWarning() for the exact condition.
  m_dump_warning_text = new QLabel(
      tr("To see the texture preview, enable \"Dump Textures\" in the Graphics settings."));
  m_dump_warning_text->setWordWrap(true);
  m_dump_warning =
      QtUtils::CreateIconWarning(this, QStyle::SP_MessageBoxWarning, m_dump_warning_text);
  m_dump_warning->setHidden(true);

  m_code_add = new NonDefaultQPushButton(tr("&Add Texture Hash"));
  m_code_edit = new NonDefaultQPushButton(tr("&Edit Texture"));
  m_code_edit->setEnabled(false);
  m_code_remove = new NonDefaultQPushButton(tr("&Remove Texture"));
  m_code_remove->setEnabled(false);
  m_code_refresh = new NonDefaultQPushButton(tr("Re&fresh List"));
  m_code_reload = new NonDefaultQPushButton(tr("Reload &Override"));

  auto* button_layout = new QHBoxLayout;
  button_layout->addWidget(m_code_add);
  button_layout->addWidget(m_code_edit);
  button_layout->addWidget(m_code_remove);
  button_layout->addStretch();
  button_layout->addWidget(m_code_refresh);
  button_layout->addWidget(m_code_reload);

  auto* layout = new QVBoxLayout{this};
  layout->addWidget(info_label);
  layout->addWidget(m_dump_warning);
  layout->addWidget(m_code_list);
  layout->addLayout(button_layout);
}

void TextureElementOverrideWidget::ConnectWidgets()
{
  connect(m_code_list, &QListWidget::itemChanged, this,
          &TextureElementOverrideWidget::OnItemChanged);
  connect(m_code_list, &QListWidget::itemSelectionChanged, this,
          &TextureElementOverrideWidget::OnSelectionChanged);
  connect(m_code_list, &QListWidget::customContextMenuRequested, this,
          &TextureElementOverrideWidget::OnContextMenuRequested);
  connect(m_code_add, &QPushButton::clicked, this, &TextureElementOverrideWidget::OnAddClicked);
  connect(m_code_edit, &QPushButton::clicked, this, &TextureElementOverrideWidget::OnEditClicked);
  connect(m_code_remove, &QPushButton::clicked, this,
          &TextureElementOverrideWidget::OnRemoveClicked);
  connect(m_code_refresh, &QPushButton::clicked, this,
          &TextureElementOverrideWidget::OnRefreshClicked);
  connect(m_code_reload, &QPushButton::clicked, this,
          &TextureElementOverrideWidget::OnReloadClicked);

  // Refresh the warning when Dump Textures (or any other setting) is toggled elsewhere.
  connect(&Settings::Instance(), &Settings::ConfigChanged, this,
          &TextureElementOverrideWidget::UpdateDumpWarning);
  connect(&Settings::Instance(), &Settings::TextureElementOverridesChanged, this,
          &TextureElementOverrideWidget::LoadOverrides);
}

void TextureElementOverrideWidget::LoadOverrides()
{
  m_overrides = TextureElementManager::LoadOverridesFromINI(m_game_id, m_revision);

  m_code_list->setEnabled(!m_game_id.empty());
  m_code_remove->setEnabled(false);
  m_code_edit->setEnabled(false);

  UpdateList();
  UpdateDumpWarning();
}

void TextureElementOverrideWidget::SaveOverrides()
{
  TextureElementManager::SaveOverridesToINI(m_game_id, m_overrides);
}

void TextureElementOverrideWidget::ReloadRuntime()
{
  TextureElementManager::GetInstance().LoadOverrides(m_game_id);
}

void TextureElementOverrideWidget::UpdateList()
{
  // Block signals to prevent itemChanged → SaveOverrides() during list construction.
  const QSignalBlocker blocker(m_code_list);
  m_code_list->clear();

  for (size_t idx = 0; idx < m_overrides.size(); idx++)
  {
    const auto& ovr = m_overrides[idx];

    const char* handling_str =
        ovr.handling == HandlingType::Screen     ? "screen" :
        ovr.handling == HandlingType::Fullscreen ||
                ovr.handling == HandlingType::FullscreenMono ? "fullscreen" :
        ovr.handling == HandlingType::HeadLocked    ? "headlocked" :
        ovr.handling == HandlingType::Flag          ? "flag" :
        ovr.handling == HandlingType::UnitsPerMeter ? "units_per_meter" :
        ovr.handling == HandlingType::Passthrough   ? "passthrough" :
        ovr.handling == HandlingType::CameraAnchor  ? "camera_anchor" :
        ovr.handling == HandlingType::ControllerAnchor ? "controller_anchor" :
                                                      "skip";

    QString label = QStringLiteral("%1  (%2)  [%3 tex]")
                        .arg(QString::fromStdString(ovr.name))
                        .arg(QString::fromLatin1(handling_str))
                        .arg(ovr.texture_hashes.size());

    if (!ovr.flag_group.empty())
      label += QStringLiteral(" flag:%1").arg(QString::fromStdString(ovr.flag_group));
    if (!ovr.condition_flag.empty())
    {
      label += (ovr.condition_inverted ? QStringLiteral(" if-not:%1") :
                                         QStringLiteral(" if:%1"))
                   .arg(QString::fromStdString(ovr.condition_flag));
    }

    if (ovr.handling == HandlingType::Screen || ovr.handling == HandlingType::HeadLocked)
    {
      if (ovr.element_depth >= 0.0f)
        label += QStringLiteral(" D%1").arg(ovr.element_depth, 0, 'f', 4);
    }
    else if (ovr.handling == HandlingType::UnitsPerMeter && ovr.units_per_meter > 0.0f)
    {
      label += QStringLiteral(" UPM%1").arg(ovr.units_per_meter, 0, 'f', 2);
    }
    else if (ovr.handling == HandlingType::Passthrough)
    {
      label += QStringLiteral(" O%1").arg(ovr.passthrough_opacity, 0, 'f', 2);
    }
    else if (ovr.handling == HandlingType::CameraAnchor)
    {
      if (ovr.anchor_rotation != TextureElementManager::AnchorRotationMode::Off)
      {
        label += ovr.anchor_rotation == TextureElementManager::AnchorRotationMode::Full ?
                     QStringLiteral(" rot:full") :
                     QStringLiteral(" rot:yaw");
      }
      if (ovr.anchor_units_per_meter > 0.0f)
        label += QStringLiteral(" UPM%1").arg(ovr.anchor_units_per_meter, 0, 'f', 2);
      if (ovr.anchor_hide)
        label += QStringLiteral(" hidden");
    }

    if (!ovr.texture_hashes.empty())
    {
      QStringList texture_hashes;
      const int shown = std::min<int>(3, static_cast<int>(ovr.texture_hashes.size()));
      for (int i = 0; i < shown; i++)
      {
        texture_hashes.append(
            QString::number(ovr.texture_hashes[static_cast<size_t>(i)], 16)
                .rightJustified(16, QLatin1Char('0')));
      }
      if (static_cast<int>(ovr.texture_hashes.size()) > shown)
        texture_hashes.append(
            tr("+%1 more").arg(static_cast<int>(ovr.texture_hashes.size()) - shown));
      label += QStringLiteral(": %1").arg(texture_hashes.join(QStringLiteral(", ")));
    }

    auto* item = new QListWidgetItem(label);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    item->setCheckState(ovr.enabled ? Qt::Checked : Qt::Unchecked);
    item->setData(Qt::UserRole, static_cast<int>(idx));
    m_code_list->addItem(item);
  }
}

void TextureElementOverrideWidget::OnItemChanged(QListWidgetItem* item)
{
  const int idx = item->data(Qt::UserRole).toInt();
  if (idx < 0 || idx >= static_cast<int>(m_overrides.size()))
    return;

  m_overrides[idx].enabled = (item->checkState() == Qt::Checked);
  SaveOverrides();
  ReloadRuntime();
}

void TextureElementOverrideWidget::OnSelectionChanged()
{
  const bool has_selection = !m_code_list->selectedItems().empty();
  m_code_remove->setEnabled(has_selection);
  m_code_edit->setEnabled(has_selection);
}

void TextureElementOverrideWidget::OnContextMenuRequested()
{
  QMenu menu;

  menu.addAction(tr("Sort Alphabetically"), this,
                 &TextureElementOverrideWidget::SortAlphabetically);
  menu.addAction(tr("Show Enabled Codes First"), this,
                 &TextureElementOverrideWidget::SortEnabledCodesFirst);
  menu.addAction(tr("Show Disabled Codes First"), this,
                 &TextureElementOverrideWidget::SortDisabledCodesFirst);

  menu.exec(QCursor::pos());
}

void TextureElementOverrideWidget::SortAlphabetically()
{
  m_code_list->sortItems();
  OnListReordered();
}

void TextureElementOverrideWidget::SortEnabledCodesFirst()
{
  std::ranges::stable_partition(m_overrides, std::identity{},
                                &TextureElementOverride::enabled);
  UpdateList();
  SaveOverrides();
  ReloadRuntime();
}

void TextureElementOverrideWidget::SortDisabledCodesFirst()
{
  std::ranges::stable_partition(m_overrides, std::logical_not{},
                                &TextureElementOverride::enabled);
  UpdateList();
  SaveOverrides();
  ReloadRuntime();
}

void TextureElementOverrideWidget::OnListReordered()
{
  std::vector<TextureElementOverride> overrides;
  overrides.reserve(m_overrides.size());

  for (int i = 0; i < m_code_list->count(); i++)
  {
    const int index = m_code_list->item(i)->data(Qt::UserRole).toInt();
    overrides.push_back(std::move(m_overrides[index]));
  }

  m_overrides = std::move(overrides);

  UpdateList();
  SaveOverrides();
  ReloadRuntime();
}

std::vector<std::string> TextureElementOverrideWidget::CollectAvailableFlags() const
{
  std::vector<std::string> flags;
  const auto append_flag = [&flags](const std::string& flag) {
    if (!flag.empty() && std::find(flags.begin(), flags.end(), flag) == flags.end())
      flags.push_back(flag);
  };

  for (const auto& entry : ShaderHunter::LoadOverridesFromINI(m_game_id, m_revision))
    append_flag(entry.flag_group);
  for (const auto& entry : ElementsGroupManager::LoadOverridesFromINI(m_game_id, m_revision))
    append_flag(entry.flag_group);
  for (const auto& entry : m_overrides)
    append_flag(entry.flag_group);

  std::sort(flags.begin(), flags.end());
  return flags;
}

void TextureElementOverrideWidget::OnAddClicked()
{
  TextureElementOverrideAddEditDialog dialog(this, m_game_id, nullptr, CollectAvailableFlags());
  if (dialog.exec() != QDialog::Accepted)
    return;

  m_overrides.push_back(dialog.GetResult());
  SaveOverrides();
  UpdateList();
  ReloadRuntime();
}

void TextureElementOverrideWidget::OnEditClicked()
{
  const auto items = m_code_list->selectedItems();
  if (items.empty())
    return;

  const int idx = items[0]->data(Qt::UserRole).toInt();
  if (idx < 0 || idx >= static_cast<int>(m_overrides.size()))
    return;

  TextureElementOverrideAddEditDialog dialog(this, m_game_id, &m_overrides[idx],
                                             CollectAvailableFlags());
  if (dialog.exec() != QDialog::Accepted)
    return;

  auto result = dialog.GetResult();
  result.enabled = m_overrides[idx].enabled;  // Preserve enabled state
  m_overrides[idx] = result;
  SaveOverrides();
  UpdateList();
  ReloadRuntime();
}

void TextureElementOverrideWidget::OnRemoveClicked()
{
  const auto items = m_code_list->selectedItems();
  if (items.empty())
    return;

  const int idx = items[0]->data(Qt::UserRole).toInt();
  if (idx < 0 || idx >= static_cast<int>(m_overrides.size()))
    return;

  m_overrides.erase(m_overrides.begin() + idx);

  SaveOverrides();
  UpdateList();
  ReloadRuntime();

  m_code_remove->setEnabled(false);
  m_code_edit->setEnabled(false);
}

void TextureElementOverrideWidget::OnRefreshClicked()
{
  LoadOverrides();
}

void TextureElementOverrideWidget::OnReloadClicked()
{
  ReloadRuntime();
}

void TextureElementOverrideWidget::showEvent(QShowEvent* event)
{
  QWidget::showEvent(event);
  // Re-scan the dump folder each time the pane is shown (dumps may have been created meanwhile).
  UpdateDumpWarning();
}

bool TextureElementOverrideWidget::HasTextureDumps() const
{
  if (m_game_id.empty())
    return false;

  const QString dump_dir =
      QString::fromStdString(File::GetUserPath(D_DUMPTEXTURES_IDX) + m_game_id);
  QDir dir(dump_dir);
  if (!dir.exists())
    return false;

  static const QStringList filters{QStringLiteral("*.png"), QStringLiteral("*.dds")};
  return !dir.entryList(filters, QDir::Files).isEmpty();
}

void TextureElementOverrideWidget::UpdateDumpWarning()
{
  // Show the warning only when there is nothing to preview or import: the game has no dumped
  // textures AND Dump Textures is disabled (so the Texture Hunter can't enumerate any either).
  // If dumps already exist, or dumping is enabled, the tools have textures to work with → no
  // warning.
  const bool dumping_enabled = Config::Get(Config::GFX_DUMP_TEXTURES);
  const bool show_warning = !dumping_enabled && !HasTextureDumps();
  m_dump_warning->setHidden(!show_warning);
}
