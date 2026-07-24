// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/ShaderOverrideWidget.h"

#include <algorithm>
#include <unordered_map>

#include <QColor>
#include <QCursor>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

#include "DolphinQt/Config/ShaderOverrideAddEditDialog.h"
#include "DolphinQt/Debugger/ShaderHunterWidget.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/Settings.h"
#include "VideoCommon/ElementsGroupManager.h"
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/TextureElementManager.h"

ShaderOverrideWidget::ShaderOverrideWidget(std::string game_id, std::optional<u16> revision)
    : m_game_id(std::move(game_id)), m_revision(revision)
{
  CreateWidgets();
  ConnectWidgets();
  LoadOverrides();
}

ShaderOverrideWidget::~ShaderOverrideWidget() = default;

void ShaderOverrideWidget::CreateWidgets()
{
  m_code_list = new QListWidget;
  m_code_list->setContextMenuPolicy(Qt::CustomContextMenu);

  auto* info_label = new QLabel(
      tr("Shader overrides control how individual shaders are handled.\n"
         "Use Add/Edit to create overrides manually, or add them from the Shader Hunter tool.\n"
         "Handling: Skip = hide, Screen = world-fixed, Head Locked = follows head,\n"
         "Fullscreen = no VR.\n"
         "Units per Meter = temporary per-shader VR scale override.\n"
         "Flag = detect shader presence to conditionally enable other overrides.\n"
         "Clear EFB = clear the next EFB copy to transparent (removes post-processing effects)."));
  info_label->setWordWrap(true);

  m_code_add = new NonDefaultQPushButton(tr("&Add Override"));
  m_code_edit = new NonDefaultQPushButton(tr("&Edit Override"));
  m_code_edit->setEnabled(false);
  m_code_remove = new NonDefaultQPushButton(tr("&Remove Override"));
  m_code_remove->setEnabled(false);

  m_code_refresh = new NonDefaultQPushButton(tr("Re&fresh List"));
  m_code_reload = new NonDefaultQPushButton(tr("Reload &Shaders"));
  m_shader_hunter = new NonDefaultQPushButton(tr("Shader Hunter"));

  auto* button_layout = new QHBoxLayout;
  button_layout->addWidget(m_shader_hunter);
  button_layout->addWidget(m_code_add);
  button_layout->addWidget(m_code_edit);
  button_layout->addWidget(m_code_remove);
  button_layout->addStretch();
  button_layout->addWidget(m_code_refresh);
  button_layout->addWidget(m_code_reload);

  auto* layout = new QVBoxLayout{this};
  layout->addWidget(info_label);
  layout->addWidget(m_code_list);
  layout->addLayout(button_layout);
}

void ShaderOverrideWidget::ConnectWidgets()
{
  connect(m_code_list, &QListWidget::itemChanged, this, &ShaderOverrideWidget::OnItemChanged);
  connect(m_code_list, &QListWidget::itemSelectionChanged, this,
          &ShaderOverrideWidget::OnSelectionChanged);
  connect(m_code_list, &QListWidget::customContextMenuRequested, this,
          &ShaderOverrideWidget::OnContextMenuRequested);
  connect(m_code_add, &QPushButton::clicked, this, &ShaderOverrideWidget::OnAddClicked);
  connect(m_code_edit, &QPushButton::clicked, this, &ShaderOverrideWidget::OnEditClicked);
  connect(m_code_remove, &QPushButton::clicked, this, &ShaderOverrideWidget::OnRemoveClicked);
  connect(m_code_refresh, &QPushButton::clicked, this, &ShaderOverrideWidget::OnRefreshClicked);
  connect(m_code_reload, &QPushButton::clicked, this, &ShaderOverrideWidget::OnReloadClicked);
  connect(&Settings::Instance(), &Settings::ShaderOverridesChanged, this,
          [this] { LoadOverrides(); });
  connect(m_shader_hunter, &QPushButton::clicked, this, [this] {
    if (m_shader_hunter_widget)
    {
      m_shader_hunter_widget->show();
      m_shader_hunter_widget->raise();
      m_shader_hunter_widget->activateWindow();
      return;
    }

    m_shader_hunter_widget = new ShaderHunterWidget(this);
    m_shader_hunter_widget->setAttribute(Qt::WA_DeleteOnClose, true);
    connect(m_shader_hunter_widget, &ShaderHunterWidget::OverridesChanged, this,
            [this] { LoadOverrides(); });
    m_shader_hunter_widget->show();
    connect(m_shader_hunter_widget, &QObject::destroyed, this,
            [this] { m_shader_hunter_widget = nullptr; });
  });
}

void ShaderOverrideWidget::LoadOverrides()
{
  m_overrides = ShaderHunter::LoadOverridesFromINI(m_game_id, m_revision);

  m_code_list->setEnabled(!m_game_id.empty());
  m_code_remove->setEnabled(false);
  m_code_edit->setEnabled(false);

  UpdateList();
}

void ShaderOverrideWidget::SaveOverrides()
{
  ShaderHunter::SaveOverridesToINI(m_game_id, m_overrides);
}

void ShaderOverrideWidget::UpdateList()
{
  // Block signals to prevent itemChanged → SaveOverrides() during list construction.
  const QSignalBlocker blocker(m_code_list);
  m_code_list->clear();

  struct HandlingHashKey
  {
    int handling = 0;
    u64 hash = 0;

    bool operator==(const HandlingHashKey& other) const
    {
      return handling == other.handling && hash == other.hash;
    }
  };
  struct HandlingHashKeyHasher
  {
    size_t operator()(const HandlingHashKey& key) const noexcept
    {
      return std::hash<u64>{}(key.hash) ^ (std::hash<int>{}(key.handling) << 1);
    }
  };

  std::unordered_map<u64, size_t> hash_counts;
  std::unordered_map<HandlingHashKey, size_t, HandlingHashKeyHasher> handling_hash_counts;
  hash_counts.reserve(m_overrides.size());
  handling_hash_counts.reserve(m_overrides.size());
  for (const auto& ovr : m_overrides)
  {
    ++hash_counts[ovr.hash];
    ++handling_hash_counts[{static_cast<int>(ovr.handling), ovr.hash}];
  }

  for (size_t idx = 0; idx < m_overrides.size(); ++idx)
  {
    const auto& ovr = m_overrides[idx];

    const char* type_str = ovr.type == ShaderHunter::ShaderType::Vertex   ? "VS" :
                           ovr.type == ShaderHunter::ShaderType::Geometry  ? "GS" :
                                                                             "PS";
    const char* handling_str = ovr.handling == ShaderHunter::HandlingType::Screen      ? "screen" :
                               ovr.handling == ShaderHunter::HandlingType::Fullscreen ||
                                       ovr.handling == ShaderHunter::HandlingType::FullscreenMono ?
                                   "fullscreen" :
                               ovr.handling == ShaderHunter::HandlingType::HeadLocked  ? "headlocked" :
                               ovr.handling == ShaderHunter::HandlingType::Flag        ? "flag" :
                               ovr.handling == ShaderHunter::HandlingType::UnitsPerMeter ?
                                   "units_per_meter" :
                               ovr.handling == ShaderHunter::HandlingType::Passthrough ?
                                   "passthrough" :
                               ovr.handling == ShaderHunter::HandlingType::CameraAnchor ?
                                   "camera_anchor" :
                               ovr.handling == ShaderHunter::HandlingType::ControllerAnchor ?
                                   "controller_anchor" :
                                                                                         "skip";

    QString label;
    if (ovr.handling == ShaderHunter::HandlingType::Flag)
    {
      label = QStringLiteral("%1  [%2 %3] (flag -> '%4')")
                  .arg(QString::fromStdString(ovr.name))
                  .arg(QString::fromLatin1(type_str))
                  .arg(ovr.hash, 16, 16, QLatin1Char('0'))
                  .arg(QString::fromStdString(ovr.flag_group));
    }
    else if (!ovr.flag_group.empty())
    {
      // Non-Flag override that also sets a flag
      label = QStringLiteral("%1  [%2 %3] (%4, flag -> '%5')")
                  .arg(QString::fromStdString(ovr.name))
                  .arg(QString::fromLatin1(type_str))
                  .arg(ovr.hash, 16, 16, QLatin1Char('0'))
                  .arg(QString::fromLatin1(handling_str))
                  .arg(QString::fromStdString(ovr.flag_group));
    }
    else
    {
      label = QStringLiteral("%1  [%2 %3] (%4)")
                  .arg(QString::fromStdString(ovr.name))
                  .arg(QString::fromLatin1(type_str))
                  .arg(ovr.hash, 16, 16, QLatin1Char('0'))
                  .arg(QString::fromLatin1(handling_str));
    }

    if (ovr.handling == ShaderHunter::HandlingType::Screen ||
        ovr.handling == ShaderHunter::HandlingType::HeadLocked)
    {
      if (ovr.element_depth >= 0.0f)
        label += QStringLiteral(" D%1").arg(ovr.element_depth, 0, 'f', 4);
    }
    else if (ovr.handling == ShaderHunter::HandlingType::UnitsPerMeter && ovr.units_per_meter > 0.0f)
    {
      label += QStringLiteral(" UPM%1").arg(ovr.units_per_meter, 0, 'f', 2);
    }
    else if (ovr.handling == ShaderHunter::HandlingType::Passthrough)
    {
      label += QStringLiteral(" O%1").arg(ovr.passthrough_opacity, 0, 'f', 2);
    }
    else if (ovr.handling == ShaderHunter::HandlingType::CameraAnchor)
    {
      if (ovr.anchor_rotation != ShaderHunter::AnchorRotationMode::Off)
      {
        label += ovr.anchor_rotation == ShaderHunter::AnchorRotationMode::Full ?
                     QStringLiteral(" rot:full") :
                     QStringLiteral(" rot:yaw");
      }
      if (ovr.anchor_units_per_meter > 0.0f)
        label += QStringLiteral(" UPM%1").arg(ovr.anchor_units_per_meter, 0, 'f', 2);
      if (ovr.anchor_hide)
        label += QStringLiteral(" hidden");
    }

    if (ovr.hash_family_match)
    {
      label += QStringLiteral(" family");
      // Pre-v2 VS/GS family signatures were raw-UID CRC32s; they still match via the exact-hash
      // fallback but break on the next shader-generator update.
      if (ovr.family_version < ShaderHunter::FAMILY_SCHEME_VERSION &&
          ovr.type != ShaderHunter::ShaderType::Pixel && ovr.family_signature != 0)
      {
        label += tr(" [legacy - re-capture to survive shader updates]");
      }
    }

    if (!ovr.texture_hashes.empty())
    {
      QStringList texture_hashes;
      for (u64 texture_hash : ovr.texture_hashes)
      {
        texture_hashes.append(
            QString::number(texture_hash, 16).rightJustified(16, QLatin1Char('0')));
      }
      label += QStringLiteral(" tex(%1):%2")
                   .arg(ovr.texture_hashes_excluded ? QStringLiteral("exclude") :
                                                     QStringLiteral("include"))
                   .arg(texture_hashes.join(QStringLiteral(",")));
    }

    if (!ovr.condition_flag.empty())
    {
      const QString condition_text = ovr.condition_inverted ?
                                         QStringLiteral("[when not '%1']").arg(
                                             QString::fromStdString(ovr.condition_flag)) :
                                         QStringLiteral("[when '%1']").arg(
                                             QString::fromStdString(ovr.condition_flag));
      label = QStringLiteral("  \u2514 ") + label + QStringLiteral(" ") + condition_text;
    }

    const bool has_duplicate_handling_hash =
        handling_hash_counts[{static_cast<int>(ovr.handling), ovr.hash}] > 1;
    const bool has_duplicate_hash = hash_counts[ovr.hash] > 1;
    const bool has_hash_only_duplicate = has_duplicate_hash && !has_duplicate_handling_hash;
    if (has_duplicate_handling_hash)
      label += QStringLiteral("  [DUPLICATE HANDLING+HASH]");
    else if (has_hash_only_duplicate)
      label += QStringLiteral("  [DUPLICATE HASH]");

    auto* item = new QListWidgetItem(label);

    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    item->setCheckState(ovr.enabled ? Qt::Checked : Qt::Unchecked);
    item->setData(Qt::UserRole, static_cast<int>(idx));
    if (has_duplicate_handling_hash)
    {
      item->setForeground(QColor(220, 85, 85));
      item->setToolTip(
          tr("Duplicate Handling+Hash: another override in this list has the same handling mode and hash."));
    }
    else if (has_hash_only_duplicate)
    {
      item->setForeground(QColor(214, 160, 0));
      item->setToolTip(
          tr("Duplicate Hash: another override in this list has the same hash but a different shader "
             "type."));
    }

    m_code_list->addItem(item);
  }
}

void ShaderOverrideWidget::OnItemChanged(QListWidgetItem* item)
{
  const int idx = item->data(Qt::UserRole).toInt();
  if (idx < 0 || idx >= static_cast<int>(m_overrides.size()))
    return;

  m_overrides[idx].enabled = (item->checkState() == Qt::Checked);
  SaveOverrides();
  ShaderHunter::GetInstance().LoadOverrides(m_game_id);
}

void ShaderOverrideWidget::OnSelectionChanged()
{
  const auto items = m_code_list->selectedItems();
  const bool has_selection = !items.empty();
  m_code_remove->setEnabled(has_selection);
  m_code_edit->setEnabled(has_selection);
}

void ShaderOverrideWidget::OnContextMenuRequested()
{
  QMenu menu;

  menu.addAction(tr("Sort Alphabetically"), this, &ShaderOverrideWidget::SortAlphabetically);
  menu.addAction(tr("Show Enabled Codes First"), this,
                 &ShaderOverrideWidget::SortEnabledCodesFirst);
  menu.addAction(tr("Show Disabled Codes First"), this,
                 &ShaderOverrideWidget::SortDisabledCodesFirst);

  menu.exec(QCursor::pos());
}

void ShaderOverrideWidget::SortAlphabetically()
{
  m_code_list->sortItems();
  OnListReordered();
}

void ShaderOverrideWidget::SortEnabledCodesFirst()
{
  std::ranges::stable_partition(m_overrides, std::identity{},
                                &ShaderHunter::ShaderOverride::enabled);
  UpdateList();
  SaveOverrides();
  ShaderHunter::GetInstance().LoadOverrides(m_game_id);
}

void ShaderOverrideWidget::SortDisabledCodesFirst()
{
  std::ranges::stable_partition(m_overrides, std::logical_not{},
                                &ShaderHunter::ShaderOverride::enabled);
  UpdateList();
  SaveOverrides();
  ShaderHunter::GetInstance().LoadOverrides(m_game_id);
}

void ShaderOverrideWidget::OnListReordered()
{
  std::vector<ShaderHunter::ShaderOverride> overrides;
  overrides.reserve(m_overrides.size());

  for (int i = 0; i < m_code_list->count(); ++i)
  {
    const int index = m_code_list->item(i)->data(Qt::UserRole).toInt();
    overrides.push_back(std::move(m_overrides[index]));
  }

  m_overrides = std::move(overrides);

  UpdateList();
  SaveOverrides();
  ShaderHunter::GetInstance().LoadOverrides(m_game_id);
}

std::vector<std::string> ShaderOverrideWidget::CollectAvailableFlags() const
{
  std::vector<std::string> flags;
  const auto append_flag = [&flags](const std::string& flag) {
    if (!flag.empty() && std::find(flags.begin(), flags.end(), flag) == flags.end())
      flags.push_back(flag);
  };

  for (const auto& ovr : m_overrides)
    append_flag(ovr.flag_group);
  for (const auto& entry : ElementsGroupManager::LoadOverridesFromINI(m_game_id, m_revision))
    append_flag(entry.flag_group);
  for (const auto& entry : TextureElementManager::LoadOverridesFromINI(m_game_id, m_revision))
    append_flag(entry.flag_group);

  std::sort(flags.begin(), flags.end());
  return flags;
}

void ShaderOverrideWidget::OnAddClicked()
{
  ShaderOverrideAddEditDialog dialog(this, nullptr, CollectAvailableFlags());
  if (dialog.exec() != QDialog::Accepted)
    return;

  m_overrides.push_back(dialog.GetResult());
  SaveOverrides();
  UpdateList();
  ShaderHunter::GetInstance().LoadOverrides(m_game_id);
}

void ShaderOverrideWidget::OnEditClicked()
{
  const auto items = m_code_list->selectedItems();
  if (items.empty())
    return;

  const int idx = items[0]->data(Qt::UserRole).toInt();
  if (idx < 0 || idx >= static_cast<int>(m_overrides.size()))
    return;

  ShaderOverrideAddEditDialog dialog(this, &m_overrides[idx], CollectAvailableFlags());
  if (dialog.exec() != QDialog::Accepted)
    return;

  auto result = dialog.GetResult();
  result.enabled = m_overrides[idx].enabled;  // Preserve enabled state
  m_overrides[idx] = result;
  SaveOverrides();
  UpdateList();
  ShaderHunter::GetInstance().LoadOverrides(m_game_id);
}

void ShaderOverrideWidget::OnRemoveClicked()
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
  ShaderHunter::GetInstance().LoadOverrides(m_game_id);

  m_code_remove->setEnabled(false);
  m_code_edit->setEnabled(false);
}

void ShaderOverrideWidget::OnRefreshClicked()
{
  // Re-read overrides from the INI file and refresh the list display
  LoadOverrides();
}

void ShaderOverrideWidget::OnReloadClicked()
{
  // Force the runtime ShaderHunter to reload overrides from the INI,
  // so enabled/disabled changes take effect in the running game without restarting.
  auto& hunter = ShaderHunter::GetInstance();
  hunter.LoadOverrides(m_game_id);
}
