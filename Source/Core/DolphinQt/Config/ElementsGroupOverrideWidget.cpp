// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/ElementsGroupOverrideWidget.h"

#include <algorithm>
#include <string_view>

#include <QCursor>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QStringList>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "Common/Logging/Log.h"
#include "DolphinQt/Config/ElementsGroupOverrideAddEditDialog.h"
#include "DolphinQt/Config/ElementsHuntingDialog.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/TextureElementManager.h"

namespace
{
QString ToQString(std::string_view value)
{
  return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}
}  // namespace

ElementsGroupOverrideWidget::ElementsGroupOverrideWidget(std::string game_id,
                                                         std::optional<u16> revision)
    : m_game_id(std::move(game_id)), m_revision(revision)
{
  CreateWidgets();
  ConnectWidgets();
  LoadOverrides();
}

ElementsGroupOverrideWidget::~ElementsGroupOverrideWidget() = default;

void ElementsGroupOverrideWidget::CreateWidgets()
{
  m_code_list = new QListWidget;
  m_code_list->setContextMenuPolicy(Qt::CustomContextMenu);

  auto* info_label = new QLabel(
      tr("Elements Group Overrides use captured runtime draw signatures.\n"
         "Use Elements Hunting to isolate frame-local draw groups, then save a new override."));
  info_label->setWordWrap(true);

  m_code_add = new NonDefaultQPushButton(tr("&Add Override"));
  m_code_edit = new NonDefaultQPushButton(tr("&Edit Override"));
  m_code_remove = new NonDefaultQPushButton(tr("&Remove Override"));
  m_code_refresh = new NonDefaultQPushButton(tr("Re&fresh List"));
  m_code_reload = new NonDefaultQPushButton(tr("Reload &Overrides"));
  m_elements_hunting = new NonDefaultQPushButton(tr("Elements Hunting"));

  m_code_edit->setEnabled(false);
  m_code_remove->setEnabled(false);

  auto* button_layout = new QHBoxLayout;
  button_layout->addWidget(m_elements_hunting);
  button_layout->addWidget(m_code_add);
  button_layout->addWidget(m_code_edit);
  button_layout->addWidget(m_code_remove);
  button_layout->addStretch();
  button_layout->addWidget(m_code_refresh);
  button_layout->addWidget(m_code_reload);

  auto* layout = new QVBoxLayout(this);
  layout->addWidget(info_label);
  layout->addWidget(m_code_list);
  layout->addLayout(button_layout);
}

void ElementsGroupOverrideWidget::ConnectWidgets()
{
  connect(m_code_list, &QListWidget::itemChanged, this, &ElementsGroupOverrideWidget::OnItemChanged);
  connect(m_code_list, &QListWidget::itemSelectionChanged, this,
          &ElementsGroupOverrideWidget::OnSelectionChanged);
  connect(m_code_list, &QListWidget::customContextMenuRequested, this,
          &ElementsGroupOverrideWidget::OnContextMenuRequested);
  connect(m_code_add, &QPushButton::clicked, this, &ElementsGroupOverrideWidget::OnAddClicked);
  connect(m_code_edit, &QPushButton::clicked, this, &ElementsGroupOverrideWidget::OnEditClicked);
  connect(m_code_remove, &QPushButton::clicked, this, &ElementsGroupOverrideWidget::OnRemoveClicked);
  connect(m_code_refresh, &QPushButton::clicked, this, &ElementsGroupOverrideWidget::OnRefreshClicked);
  connect(m_code_reload, &QPushButton::clicked, this, &ElementsGroupOverrideWidget::OnReloadClicked);
  connect(m_elements_hunting, &QPushButton::clicked, this, &ElementsGroupOverrideWidget::OnHuntClicked);
}

void ElementsGroupOverrideWidget::LoadOverrides()
{
  m_overrides = ElementsGroupManager::LoadOverridesFromINI(m_game_id, m_revision);
  UpdateList();
  m_code_edit->setEnabled(false);
  m_code_remove->setEnabled(false);
}

void ElementsGroupOverrideWidget::SaveOverrides()
{
  ElementsGroupManager::SaveOverridesToINI(m_game_id, m_overrides);
}

void ElementsGroupOverrideWidget::UpdateList()
{
  const QSignalBlocker blocker(m_code_list);
  m_code_list->clear();

  for (size_t i = 0; i < m_overrides.size(); ++i)
  {
    const auto& entry = m_overrides[i];
    QString label = QStringLiteral("%1  (%2)")
                        .arg(QString::fromStdString(entry.name))
                        .arg(QString::fromLatin1(entry.handling == ShaderHunter::HandlingType::Screen       ? "screen" :
                                                 entry.handling == ShaderHunter::HandlingType::ScreenPane   ? "screen_pane" :
                                                 entry.handling == ShaderHunter::HandlingType::Fullscreen   ? "fullscreen" :
                                                 entry.handling == ShaderHunter::HandlingType::FullscreenMono ? "fullscreen_mono" :
                                                 entry.handling == ShaderHunter::HandlingType::HeadLocked   ? "headlocked" :
                                                 entry.handling == ShaderHunter::HandlingType::Flag         ? "flag" :
                                                 entry.handling == ShaderHunter::HandlingType::UnitsPerMeter ? "units_per_meter" :
                                                 entry.handling == ShaderHunter::HandlingType::Passthrough ? "passthrough" :
                                                 entry.handling == ShaderHunter::HandlingType::CameraAnchor ? "camera_anchor" :
                                                 entry.handling == ShaderHunter::HandlingType::ControllerAnchor ? "controller_anchor" :
                                                                                                             "skip"));
    if (entry.handling == ShaderHunter::HandlingType::ScreenPane)
    {
      label += entry.screen_pane_depth == ElementsGroupManager::ScreenPaneDepthMode::VR ?
                   QStringLiteral(" depth:vr") :
               entry.screen_pane_depth == ElementsGroupManager::ScreenPaneDepthMode::Flat ?
                   QStringLiteral(" depth:flat") :
                   QStringLiteral(" depth:game");
    }
    if (entry.match_kind == ElementsGroupManager::MatchKind::ProfileLayer)
    {
      label += QStringLiteral(" profile:%1")
                   .arg(ToQString(MetroidElementProfileToININame(entry.profile_id)));
      if (!entry.profile_layers.empty())
      {
        QStringList layers;
        const int shown = std::min<int>(3, static_cast<int>(entry.profile_layers.size()));
        for (int layer_index = 0; layer_index < shown; ++layer_index)
          layers << ToQString(MetroidElementLayerToDisplayName(entry.profile_layers[layer_index]));
        if (static_cast<int>(entry.profile_layers.size()) > shown)
          layers << tr("+%1 more").arg(static_cast<int>(entry.profile_layers.size()) - shown);
        label += QStringLiteral(" [%1]").arg(layers.join(QStringLiteral(", ")));
      }
    }
    else
    {
      if (entry.runtime_element.use_projection)
        label += QStringLiteral(" proj");
      if (entry.runtime_element.use_projection_type)
        label += QStringLiteral(" proj_type");
      if (entry.runtime_element.use_layer)
        label += QStringLiteral(" layer");
      if (entry.runtime_element.use_viewport)
        label += QStringLiteral(" vp");
      if (entry.runtime_element.use_scissor)
        label += QStringLiteral(" scissor");
      if (entry.runtime_element.use_render_state)
        label += QStringLiteral(" state");
    }
    if (!entry.texture_hashes.empty())
      label += QStringLiteral(" tex");
    if (entry.clear_efb)
      label += QStringLiteral(" clear_efb");
    if (!entry.selected_match_filter.empty())
    {
      label += QStringLiteral(" %1[%2]")
                   .arg(entry.selected_match_filter_excluded ? QStringLiteral("match_exclude") :
                                                               QStringLiteral("matches"))
                   .arg(entry.selected_match_filter.size());
      const auto legacy_count = std::count_if(
          entry.selected_match_filter.begin(), entry.selected_match_filter.end(),
          [](const ElementsGroupManager::SelectedSubgroupSignature& filter) {
            return filter.family_version < ShaderHunter::FAMILY_SCHEME_VERSION;
          });
      if (legacy_count > 0)
        label += tr(" [%1 legacy - re-capture to survive shader updates]").arg(legacy_count);
    }
    if (!entry.condition_flag.empty())
      label += QStringLiteral(" when(%1%2)")
                   .arg(entry.condition_inverted ? QStringLiteral("!") : QString())
                   .arg(QString::fromStdString(entry.condition_flag));

    auto* item = new QListWidgetItem(label);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    item->setCheckState(entry.enabled ? Qt::Checked : Qt::Unchecked);
    item->setData(Qt::UserRole, static_cast<int>(i));
    m_code_list->addItem(item);
  }
}

std::vector<std::string> ElementsGroupOverrideWidget::CollectAvailableFlags() const
{
  std::vector<std::string> flags;
  const auto append_flag = [&flags](const std::string& flag) {
    if (!flag.empty() && std::find(flags.begin(), flags.end(), flag) == flags.end())
      flags.push_back(flag);
  };

  const auto shader_overrides = ShaderHunter::LoadOverridesFromINI(m_game_id, m_revision);
  for (const auto& entry : shader_overrides)
    append_flag(entry.flag_group);
  for (const auto& entry : m_overrides)
    append_flag(entry.flag_group);
  for (const auto& entry : TextureElementManager::LoadOverridesFromINI(m_game_id, m_revision))
    append_flag(entry.flag_group);

  std::sort(flags.begin(), flags.end());
  return flags;
}

void ElementsGroupOverrideWidget::OnItemChanged(QListWidgetItem* item)
{
  const int idx = item->data(Qt::UserRole).toInt();
  if (idx < 0 || idx >= static_cast<int>(m_overrides.size()))
    return;
  m_overrides[idx].enabled = item->checkState() == Qt::Checked;
  INFO_LOG_FMT(VIDEO, "ElementsGroup: override '{}' {}", m_overrides[idx].name,
               m_overrides[idx].enabled ? "enabled" : "disabled");
  SaveOverrides();
  ElementsGroupManager::GetInstance().LoadOverrides(m_game_id);
}

void ElementsGroupOverrideWidget::OnSelectionChanged()
{
  const bool has_selection = !m_code_list->selectedItems().empty();
  m_code_edit->setEnabled(has_selection);
  m_code_remove->setEnabled(has_selection);
}

void ElementsGroupOverrideWidget::OnContextMenuRequested()
{
  QMenu menu;

  menu.addAction(tr("Sort Alphabetically"), this,
                 &ElementsGroupOverrideWidget::SortAlphabetically);
  menu.addAction(tr("Show Enabled Codes First"), this,
                 &ElementsGroupOverrideWidget::SortEnabledCodesFirst);
  menu.addAction(tr("Show Disabled Codes First"), this,
                 &ElementsGroupOverrideWidget::SortDisabledCodesFirst);

  menu.exec(QCursor::pos());
}

void ElementsGroupOverrideWidget::SortAlphabetically()
{
  m_code_list->sortItems();
  OnListReordered();
}

void ElementsGroupOverrideWidget::SortEnabledCodesFirst()
{
  std::ranges::stable_partition(m_overrides, std::identity{},
                                &ElementsGroupManager::ElementGroupOverride::enabled);
  UpdateList();
  SaveOverrides();
  ElementsGroupManager::GetInstance().LoadOverrides(m_game_id);
}

void ElementsGroupOverrideWidget::SortDisabledCodesFirst()
{
  std::ranges::stable_partition(m_overrides, std::logical_not{},
                                &ElementsGroupManager::ElementGroupOverride::enabled);
  UpdateList();
  SaveOverrides();
  ElementsGroupManager::GetInstance().LoadOverrides(m_game_id);
}

void ElementsGroupOverrideWidget::OnListReordered()
{
  std::vector<ElementsGroupManager::ElementGroupOverride> overrides;
  overrides.reserve(m_overrides.size());

  for (int i = 0; i < m_code_list->count(); ++i)
  {
    const int index = m_code_list->item(i)->data(Qt::UserRole).toInt();
    overrides.push_back(std::move(m_overrides[index]));
  }

  m_overrides = std::move(overrides);

  UpdateList();
  SaveOverrides();
  ElementsGroupManager::GetInstance().LoadOverrides(m_game_id);
}

void ElementsGroupOverrideWidget::OnAddClicked()
{
  ElementsGroupOverrideAddEditDialog dialog(this, nullptr, CollectAvailableFlags());
  if (dialog.exec() != QDialog::Accepted)
    return;
  auto result = dialog.GetResult();
  result.enabled = true;
  result.user_defined = true;
  m_overrides.push_back(std::move(result));
  SaveOverrides();
  LoadOverrides();
  ElementsGroupManager::GetInstance().LoadOverrides(m_game_id);
}

void ElementsGroupOverrideWidget::OnEditClicked()
{
  const auto items = m_code_list->selectedItems();
  if (items.empty())
    return;
  const int idx = items[0]->data(Qt::UserRole).toInt();
  if (idx < 0 || idx >= static_cast<int>(m_overrides.size()))
    return;

  ElementsGroupOverrideAddEditDialog dialog(this, &m_overrides[idx], CollectAvailableFlags());
  if (dialog.exec() != QDialog::Accepted)
    return;

  auto result = dialog.GetResult();
  result.enabled = m_overrides[idx].enabled;
  m_overrides[idx] = std::move(result);
  SaveOverrides();
  LoadOverrides();
  ElementsGroupManager::GetInstance().LoadOverrides(m_game_id);
}

void ElementsGroupOverrideWidget::OnRemoveClicked()
{
  const auto items = m_code_list->selectedItems();
  if (items.empty())
    return;
  const int idx = items[0]->data(Qt::UserRole).toInt();
  if (idx < 0 || idx >= static_cast<int>(m_overrides.size()))
    return;

  m_overrides.erase(m_overrides.begin() + idx);
  SaveOverrides();
  LoadOverrides();
  ElementsGroupManager::GetInstance().LoadOverrides(m_game_id);
}

void ElementsGroupOverrideWidget::OnRefreshClicked()
{
  LoadOverrides();
}

void ElementsGroupOverrideWidget::OnReloadClicked()
{
  INFO_LOG_FMT(VIDEO, "ElementsGroup: reload requested for {}", m_game_id);
  ElementsGroupManager::GetInstance().LoadOverrides(m_game_id);
}

void ElementsGroupOverrideWidget::OnHuntClicked()
{
  if (m_hunting_dialog)
  {
    m_hunting_dialog->show();
    m_hunting_dialog->raise();
    m_hunting_dialog->activateWindow();
    return;
  }

  m_hunting_dialog = new ElementsHuntingDialog(m_game_id, this);
  m_hunting_dialog->setAttribute(Qt::WA_DeleteOnClose, true);
  connect(m_hunting_dialog, &ElementsHuntingDialog::OverridesChanged, this,
          [this]() {
            LoadOverrides();
            ElementsGroupManager::GetInstance().LoadOverrides(m_game_id);
          });
  connect(m_hunting_dialog, &QObject::destroyed, this,
          [this]() { m_hunting_dialog = nullptr; });
  m_hunting_dialog->show();
}
