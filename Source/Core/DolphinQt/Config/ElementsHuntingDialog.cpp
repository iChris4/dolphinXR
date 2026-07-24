// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/ElementsHuntingDialog.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QAbstractItemView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "DolphinQt/Config/ElementsGroupOverrideAddEditDialog.h"
#include "DolphinQt/Config/TextureHashBrowserDialog.h"
#include "VideoCommon/ElementsGroupManager.h"
#include "VideoCommon/HideObjectEngine.h"
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/TextureElementManager.h"

namespace
{
QString GetProjectionLabel(const ShaderHunter::RuntimeElementSignature& sig)
{
  if (sig.perspective)
  {
    return QObject::tr("Perspective %1x%2")
        .arg(sig.perspective_hfov_x100 / 100.0, 0, 'f', 2)
        .arg(sig.perspective_vfov_x100 / 100.0, 0, 'f', 2);
  }

  return QObject::tr("Ortho L%1")
      .arg(sig.ortho_layer);
}

QString FormatSeedCandidateLabel(const ElementsGroupManager::SeedCandidate& candidate,
                                 const ShaderHunter::RuntimeElementSignature& active_mask)
{
  const auto& group = candidate.group_signature;
  const auto& rep = candidate.representative_draw.signature;
  QStringList parts;

  if (!active_mask.use_projection && !active_mask.use_layer && !active_mask.use_viewport &&
      !active_mask.use_scissor && !active_mask.use_render_state)
  {
    parts << GetProjectionLabel(candidate.signature);
    parts << QObject::tr("VP %1,%2 %3x%4")
                 .arg(candidate.signature.viewport_x)
                 .arg(candidate.signature.viewport_y)
                 .arg(candidate.signature.viewport_width)
                 .arg(candidate.signature.viewport_height);
    parts << QObject::tr("SC %1,%2 %3,%4")
                 .arg(candidate.signature.scissor_left)
                 .arg(candidate.signature.scissor_top)
                 .arg(candidate.signature.scissor_right)
                 .arg(candidate.signature.scissor_bottom);
  }
  else
  {
    if (active_mask.use_projection)
      parts << GetProjectionLabel(group);
    else if (active_mask.use_layer)
      parts << (rep.perspective ? QObject::tr("Perspective") :
                                  QObject::tr("Ortho L%1").arg(group.ortho_layer));

    if (active_mask.use_viewport)
    {
      parts << QObject::tr("VP %1,%2 %3x%4")
                   .arg(group.viewport_x)
                   .arg(group.viewport_y)
                   .arg(group.viewport_width)
                   .arg(group.viewport_height);
    }

    if (active_mask.use_scissor)
    {
      parts << QObject::tr("SC %1,%2 %3,%4")
                   .arg(group.scissor_left)
                   .arg(group.scissor_top)
                   .arg(group.scissor_right)
                   .arg(group.scissor_bottom);
    }

    if (active_mask.use_render_state)
      parts << QObject::tr("State");
  }

  parts << QObject::tr("Draw #%1").arg(candidate.representative_draw.draw_index + 1);
  parts << QObject::tr("Seen %1x").arg(candidate.occurrence_count);
  return parts.join(QStringLiteral(" | "));
}

QString FormatSeedSummary(const ElementsGroupManager::SeedCandidate& candidate,
                          const ShaderHunter::RuntimeElementSignature& active_mask)
{
  return QObject::tr("%1\nRepresentative Draw: #%2\nOccurrences: %3")
      .arg(FormatSeedCandidateLabel(candidate, active_mask))
      .arg(candidate.representative_draw.draw_index + 1)
      .arg(candidate.occurrence_count);
}

QString FormatMatchLabel(const ElementsGroupManager::DrawRecord& draw)
{
  QString label = QObject::tr("Draw #%1 | VS %2 | PS %3 | GS %4")
      .arg(draw.draw_index + 1)
      .arg(static_cast<qulonglong>(draw.vs_hash), 16, 16, QLatin1Char('0'))
      .arg(static_cast<qulonglong>(draw.ps_hash), 16, 16, QLatin1Char('0'))
      .arg(static_cast<qulonglong>(draw.gs_hash), 16, 16, QLatin1Char('0'));
  if (draw.profile_id != MetroidElementProfile::None)
  {
    label += QObject::tr(" | Profile %1")
                 .arg(QString::fromStdString(draw.profile_layer_name.empty() ?
                                                  std::string("Unknown") :
                                                  draw.profile_layer_name));
  }
  return label;
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

QString FormatCurrentMatchLabel(const ElementsGroupManager::CurrentMatchCandidate& candidate)
{
  QString label = GetProjectionLabel(candidate.representative_draw.signature);
  label += QObject::tr(" | F %1/%2/%3")
               .arg(QStringLiteral("%1")
                        .arg(static_cast<qulonglong>(candidate.subgroup.vs_family), 16, 8,
                             QLatin1Char('0')))
               .arg(QStringLiteral("%1")
                        .arg(static_cast<qulonglong>(candidate.subgroup.ps_family), 16, 8,
                             QLatin1Char('0')))
               .arg(QStringLiteral("%1")
                        .arg(static_cast<qulonglong>(candidate.subgroup.gs_family), 16, 8,
                             QLatin1Char('0')));
  label += QObject::tr(" | Tex %1").arg(FormatTextureSummary(candidate.subgroup.texture_hashes));
  label += QObject::tr(" | x%1").arg(candidate.raw_draw_count);
  if (candidate.representative_draw.draw_index >= 0)
    label += QObject::tr(" | Draw #%1").arg(candidate.representative_draw.draw_index + 1);
  if (candidate.representative_draw.profile_id != MetroidElementProfile::None)
  {
    label += QObject::tr(" | Profile %1")
                 .arg(QString::fromStdString(
                     candidate.representative_draw.profile_layer_name.empty() ?
                         std::string("Unknown") :
                         candidate.representative_draw.profile_layer_name));
  }
  if (!candidate.active_this_frame)
    label += QObject::tr(" | Inactive");
  return label;
}

QString GetPreviewScopeLabel(const ElementsGroupManager::Status& status)
{
  if (status.selected_match_filter_count <= 0)
    return QObject::tr("Whole current group");

  return status.selected_match_filter_excluded ? QObject::tr("Unchecked matches only") :
                                                 QObject::tr("Checked matches only");
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

std::vector<std::string> CollectAvailableFlags(const std::string& game_id)
{
  std::vector<std::string> flags;
  const auto shader_overrides = ShaderHunter::LoadOverridesFromINI(game_id);
  for (const auto& entry : shader_overrides)
  {
    if (!entry.flag_group.empty() &&
        std::find(flags.begin(), flags.end(), entry.flag_group) == flags.end())
    {
      flags.push_back(entry.flag_group);
    }
  }
  const auto element_overrides = ElementsGroupManager::LoadOverridesFromINI(game_id);
  for (const auto& entry : element_overrides)
  {
    if (!entry.flag_group.empty() &&
        std::find(flags.begin(), flags.end(), entry.flag_group) == flags.end())
    {
      flags.push_back(entry.flag_group);
    }
  }
  const auto texture_overrides = TextureElementManager::LoadOverridesFromINI(game_id);
  for (const auto& entry : texture_overrides)
  {
    if (!entry.flag_group.empty() &&
        std::find(flags.begin(), flags.end(), entry.flag_group) == flags.end())
    {
      flags.push_back(entry.flag_group);
    }
  }
  std::sort(flags.begin(), flags.end());
  return flags;
}

bool HideObjectNameExists(const std::vector<HideObjectEngine::HideObject>& codes,
                          const QString& name)
{
  const std::string candidate = name.toStdString();
  return std::any_of(codes.begin(), codes.end(), [&candidate](const auto& code) {
    return code.name == candidate;
  });
}

QString MakeUniqueHideObjectName(int draw_count,
                                 const std::vector<HideObjectEngine::HideObject>& codes)
{
  const QString base = QObject::tr("Element Hide %1 Draws").arg(draw_count);
  if (!HideObjectNameExists(codes, base))
    return base;

  for (int suffix = 2;; ++suffix)
  {
    const QString candidate = QStringLiteral("%1 %2").arg(base).arg(suffix);
    if (!HideObjectNameExists(codes, candidate))
      return candidate;
  }
}
}  // namespace

ElementsHuntingDialog::ElementsHuntingDialog(std::string game_id, QWidget* parent)
    : QDialog(parent), m_game_id(std::move(game_id))
{
  setWindowTitle(tr("Elements Hunting"));
  setAttribute(Qt::WA_DeleteOnClose);
  setMinimumWidth(420);

  ElementsGroupManager::GetInstance().SetPopupOpen(true);
  HideObjectEngine::Engine::GetInstance().SetCaptureEnabled(true);
  if (!m_game_id.empty())
    ElementsGroupManager::GetInstance().LoadOverridesIfNeeded(m_game_id);

  CreateWidgets();
  ConnectWidgets();
  RefreshPendingTextureSummary();

  m_update_timer = new QTimer(this);
  connect(m_update_timer, &QTimer::timeout, this, &ElementsHuntingDialog::UpdateDisplay);
  m_update_timer->start(100);
}

ElementsHuntingDialog::~ElementsHuntingDialog() = default;

void ElementsHuntingDialog::CreateWidgets()
{
  auto* layout = new QVBoxLayout(this);

  auto* info = new QLabel(
      tr("Check \"Enable Group Hunt\" to start collecting seed candidates from the current frame, "
         "then select a runtime-signature seed candidate and enable signature groups to cycle "
         "matching elements.\n"
         "Preview affects the whole current group until one or more matches are checked below. "
         "After that, preview and saved overrides affect only the checked matches.\n"
         "Seen matches stay in the list even when not drawn and are shown in grey."));
  info->setWordWrap(true);
  layout->addWidget(info);

  m_enable_check = new QCheckBox(tr("Enable Group Hunt"));
  layout->addWidget(m_enable_check);

  auto* option_layout = new QHBoxLayout;
  option_layout->addWidget(new QLabel(tr("Preview:")));
  m_option_combo = new QComboBox;
  m_option_combo->addItem(tr("Skip"), static_cast<int>(ShaderHunter::HuntingOption::Skip));
  m_option_combo->addItem(tr("Pink"), static_cast<int>(ShaderHunter::HuntingOption::Pink));
  option_layout->addWidget(m_option_combo);
  option_layout->addWidget(new QLabel(tr("Match Mode:")));
  m_match_filter_mode_combo = new QComboBox;
  m_match_filter_mode_combo->addItem(tr("Include"), false);
  m_match_filter_mode_combo->addItem(tr("Exclude"), true);
  option_layout->addWidget(m_match_filter_mode_combo);
  option_layout->addWidget(new QLabel(tr("Texture Mode:")));
  m_texture_mode_combo = new QComboBox;
  m_texture_mode_combo->addItem(tr("Include"), false);
  m_texture_mode_combo->addItem(tr("Exclude"), true);
  option_layout->addWidget(m_texture_mode_combo);
  option_layout->addStretch();
  layout->addLayout(option_layout);

  layout->addWidget(new QLabel(tr("Seed Candidates:")));
  m_seed_list = new QListWidget;
  m_seed_list->setSelectionMode(QAbstractItemView::SingleSelection);
  layout->addWidget(m_seed_list);

  m_seed_label = new QLabel(tr("Seed: (none)"));
  m_seed_label->setWordWrap(true);
  layout->addWidget(m_seed_label);

  auto* groups_layout = new QHBoxLayout;
  m_projection_check = new QCheckBox(tr("Projection"));
  m_layer_check = new QCheckBox(tr("Layer"));
  m_viewport_check = new QCheckBox(tr("Viewport"));
  m_scissor_check = new QCheckBox(tr("Scissor"));
  m_render_state_check = new QCheckBox(tr("Render State"));
  groups_layout->addWidget(m_projection_check);
  groups_layout->addWidget(m_layer_check);
  groups_layout->addWidget(m_viewport_check);
  groups_layout->addWidget(m_scissor_check);
  groups_layout->addWidget(m_render_state_check);
  layout->addLayout(groups_layout);

  m_match_label = new QLabel(tr("Matches: - / -"));
  layout->addWidget(m_match_label);

  auto* match_nav = new QHBoxLayout;
  m_prev_match_button = new QPushButton(tr("<< Previous Match"));
  m_next_match_button = new QPushButton(tr("Next Match >>"));
  match_nav->addWidget(m_prev_match_button);
  match_nav->addWidget(m_next_match_button);
  layout->addLayout(match_nav);

  layout->addWidget(new QLabel(tr("Current Matches:")));
  m_current_match_list = new QListWidget;
  m_current_match_list->setSelectionMode(QAbstractItemView::SingleSelection);
  layout->addWidget(m_current_match_list);

  auto* texture_layout = new QHBoxLayout;
  m_view_textures_button = new QPushButton(tr("View Textures"));
  texture_layout->addWidget(m_view_textures_button);
  texture_layout->addStretch();
  layout->addLayout(texture_layout);

  m_pending_texture_label = new QLabel;
  m_pending_texture_label->setWordWrap(true);
  layout->addWidget(m_pending_texture_label);

  auto* refresh_layout = new QHBoxLayout;
  m_auto_refresh_check = new QCheckBox(tr("Auto Refresh"));
  m_auto_refresh_check->setToolTip(
      tr("Continuously refresh the current matches list while this window is open.\n"
         "Disable this when the window becomes sluggish and use Refresh manually."));
  m_refresh_button = new QPushButton(tr("Refresh"));
  refresh_layout->addWidget(m_auto_refresh_check);
  refresh_layout->addStretch();
  refresh_layout->addWidget(m_refresh_button);
  layout->addLayout(refresh_layout);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  m_save_button = buttons->addButton(tr("Save To Elements Group Override"),
                                     QDialogButtonBox::ActionRole);
  m_save_hide_objects_button =
      buttons->addButton(tr("Save To Hide Objects"), QDialogButtonBox::ActionRole);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
  layout->addWidget(buttons);
}

void ElementsHuntingDialog::ConnectWidgets()
{
  auto& manager = ElementsGroupManager::GetInstance();
  connect(m_enable_check, &QCheckBox::toggled, this, [&manager, this](bool checked) {
    manager.SetHuntEnabled(checked);
    // Refresh so candidates appear right after enabling (collection only runs while enabled) and so
    // stale leftovers clear on disable, matching how the group/filter toggles behave.
    RequestRefresh();
  });
  connect(m_option_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [&manager, this]() {
            manager.SetHuntingOption(static_cast<ShaderHunter::HuntingOption>(
                m_option_combo->currentData().toInt()));
          });
  connect(m_match_filter_mode_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [&manager, this]() {
            manager.SetSelectedMatchFilterExcluded(m_match_filter_mode_combo->currentData().toBool());
            RequestRefresh();
          });
  connect(m_texture_mode_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this]() { RefreshPendingTextureSummary(); });
  connect(m_refresh_button, &QPushButton::clicked, this, &ElementsHuntingDialog::RequestRefresh);
  connect(m_auto_refresh_check, &QCheckBox::toggled, this, [this](bool checked) {
    if (checked)
      RequestRefresh();
  });
  connect(m_seed_list, &QListWidget::itemSelectionChanged, this,
          &ElementsHuntingDialog::OnSeedSelectionChanged);
  connect(m_view_textures_button, &QPushButton::clicked, this,
          &ElementsHuntingDialog::ShowTextureBrowser);

  const auto apply_groups = [this, &manager]() {
    manager.SetSeedGroupMask(m_projection_check->isChecked(), m_layer_check->isChecked(),
                             m_viewport_check->isChecked(), m_scissor_check->isChecked(),
                             m_render_state_check->isChecked());
    RequestRefresh();
  };
  for (QCheckBox* checkbox : {m_projection_check, m_layer_check, m_viewport_check,
                              m_scissor_check, m_render_state_check})
  {
    connect(checkbox, &QCheckBox::toggled, this, apply_groups);
  }

  connect(m_prev_match_button, &QPushButton::clicked, this,
          [this, &manager]() {
            manager.PrevMatch();
            RequestRefresh();
          });
  connect(m_next_match_button, &QPushButton::clicked, this,
          [this, &manager]() {
            manager.NextMatch();
            RequestRefresh();
          });
  connect(m_current_match_list, &QListWidget::itemSelectionChanged, this,
          &ElementsHuntingDialog::OnCurrentMatchSelectionChanged);
  connect(m_current_match_list, &QListWidget::itemChanged, this,
          &ElementsHuntingDialog::OnCurrentMatchItemChanged);
  connect(m_save_button, &QPushButton::clicked, this, &ElementsHuntingDialog::SaveCurrentOverride);
  connect(m_save_hide_objects_button, &QPushButton::clicked, this,
          &ElementsHuntingDialog::SaveCurrentHideObjectCode);
}

void ElementsHuntingDialog::UpdateDisplay()
{
  auto& manager = ElementsGroupManager::GetInstance();
  const auto status = manager.GetStatus();
  bool refresh_lists = m_refresh_pending || (m_auto_refresh_check && m_auto_refresh_check->isChecked());

  {
    const QSignalBlocker blocker(m_enable_check);
    m_enable_check->setChecked(status.hunt_enabled);
  }
  {
    const QSignalBlocker blocker(m_option_combo);
    const int idx = m_option_combo->findData(static_cast<int>(status.option));
    if (idx >= 0)
      m_option_combo->setCurrentIndex(idx);
  }
  {
    const QSignalBlocker blocker(m_match_filter_mode_combo);
    const int idx = m_match_filter_mode_combo->findData(status.selected_match_filter_excluded);
    if (idx >= 0)
      m_match_filter_mode_combo->setCurrentIndex(idx);
  }

  const auto candidates = manager.GetSeedCandidates();
  // Auto-heal: if the hunt is enabled and the backend has candidates but the list is still empty
  // (e.g. the refresh tick fired before the first frame was collected), force a refresh.
  if (!refresh_lists && status.hunt_enabled && m_seed_list->count() == 0 && !candidates.empty())
    refresh_lists = true;
  if (refresh_lists)
  {
    {
      const QSignalBlocker blocker(m_seed_list);
      if (m_seed_list->count() != static_cast<int>(candidates.size()))
      {
        m_seed_list->clear();
        for (size_t i = 0; i < candidates.size(); ++i)
        {
          auto* item = new QListWidgetItem(FormatSeedCandidateLabel(candidates[i], status.seed_signature));
          item->setData(Qt::UserRole, static_cast<int>(i));
          m_seed_list->addItem(item);
        }
      }
      else
      {
        for (int i = 0; i < m_seed_list->count(); ++i)
          m_seed_list->item(i)->setText(
              FormatSeedCandidateLabel(candidates[static_cast<size_t>(i)], status.seed_signature));
      }

      if (status.selected_seed_index >= 0 && status.selected_seed_index < m_seed_list->count())
        m_seed_list->setCurrentRow(status.selected_seed_index);
      else
        m_seed_list->clearSelection();
    }

    const auto current_matches = manager.GetCurrentMatches();
    {
      const QSignalBlocker blocker(m_current_match_list);
      // Preserve the scroll position across rebuilds. With fluctuating matches the count changes,
      // forcing a clear()+re-add that otherwise snaps the view back to the top while the user is
      // toggling checkboxes. Restored last so it overrides any auto-scroll from setCurrentRow.
      const int match_scroll = m_current_match_list->verticalScrollBar()->value();
      if (m_current_match_list->count() != static_cast<int>(current_matches.size()))
      {
        m_current_match_list->clear();
        for (size_t i = 0; i < current_matches.size(); ++i)
        {
          auto* item = new QListWidgetItem(FormatCurrentMatchLabel(current_matches[i]));
          item->setData(Qt::UserRole, static_cast<int>(i));
          item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
          item->setCheckState(manager.IsCurrentMatchFilterEnabled(static_cast<int>(i)) ? Qt::Checked :
                                                                                        Qt::Unchecked);
          if (!current_matches[i].active_this_frame)
            item->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
          m_current_match_list->addItem(item);
        }
      }
      else
      {
        for (int i = 0; i < m_current_match_list->count(); ++i)
        {
          auto* item = m_current_match_list->item(i);
          item->setText(FormatCurrentMatchLabel(current_matches[static_cast<size_t>(i)]));
          item->setCheckState(manager.IsCurrentMatchFilterEnabled(i) ? Qt::Checked : Qt::Unchecked);
          item->setForeground(current_matches[static_cast<size_t>(i)].active_this_frame ?
                                  palette().color(QPalette::Text) :
                                  palette().color(QPalette::Disabled, QPalette::Text));
        }
      }

      if (status.selected_match >= 0 && status.selected_match < m_current_match_list->count())
        m_current_match_list->setCurrentRow(status.selected_match);
      else
        m_current_match_list->clearSelection();

      m_current_match_list->verticalScrollBar()->setValue(match_scroll);
    }

    m_refresh_pending = false;
  }

  if (status.seed_valid)
  {
    QStringList groups;
    if (status.seed_signature.use_projection)
      groups << tr("Projection");
    if (status.seed_signature.use_layer)
      groups << tr("Layer");
    if (status.seed_signature.use_viewport)
      groups << tr("Viewport");
    if (status.seed_signature.use_scissor)
      groups << tr("Scissor");
    if (status.seed_signature.use_render_state)
      groups << tr("Render State");
    QString seed_summary = tr("Seed: %1\nGroups: %2\nChecked Match Filters: %3")
                              .arg(status.selected_seed_index >= 0 &&
                                           status.selected_seed_index < static_cast<int>(candidates.size()) ?
                                       FormatSeedSummary(candidates[static_cast<size_t>(status.selected_seed_index)],
                                                         status.seed_signature) :
                                       tr("selected"))
                              .arg(groups.isEmpty() ? tr("(none)") :
                                                      groups.join(QStringLiteral(", ")))
                              .arg(status.selected_match_filter_count);
    if (status.selected_match_filter_count > 0)
    {
      seed_summary += tr("\nMatch Mode: %1")
                          .arg(status.selected_match_filter_excluded ? tr("Exclude") :
                                                                       tr("Include"));
    }
    m_seed_label->setText(seed_summary);
  }
  else
  {
    m_seed_label->setText(tr("Seed: (none)"));
  }

  {
    const QSignalBlocker p(m_projection_check);
    const QSignalBlocker l(m_layer_check);
    const QSignalBlocker v(m_viewport_check);
    const QSignalBlocker s(m_scissor_check);
    const QSignalBlocker r(m_render_state_check);
    m_projection_check->setChecked(status.seed_signature.use_projection);
    m_layer_check->setChecked(status.seed_signature.use_layer);
    m_viewport_check->setChecked(status.seed_signature.use_viewport);
    m_scissor_check->setChecked(status.seed_signature.use_scissor);
    m_render_state_check->setChecked(status.seed_signature.use_render_state);
  }

  if (status.highlighted_draw)
  {
    m_match_label->setText(tr("Matches: %1 / %2 (Active: %3)\nCurrent Match: %4\nPreview Scope: %5")
                               .arg(status.total_matches > 0 ? status.selected_match + 1 : 0)
                               .arg(status.total_matches)
                               .arg(status.active_matches)
                               .arg(tr("%1 | Covers %2 draw(s)")
                                        .arg(FormatMatchLabel(*status.highlighted_draw))
                                        .arg(status.highlighted_match_raw_draw_count))
                               .arg(GetPreviewScopeLabel(status)));
  }
  else
  {
    m_match_label->setText(tr("Matches: %1 / %2 (Active: %3)\nPreview Scope: %4")
                               .arg(status.total_matches > 0 ? status.selected_match + 1 : 0)
                               .arg(status.total_matches)
                               .arg(status.active_matches)
                               .arg(GetPreviewScopeLabel(status)));
  }
}

void ElementsHuntingDialog::RequestRefresh()
{
  m_refresh_pending = true;
  UpdateDisplay();
}

void ElementsHuntingDialog::RefreshPendingTextureSummary()
{
  if (m_pending_texture_hashes.empty())
  {
    m_pending_texture_label->setText(tr("Pending Texture Filters: (none)"));
    return;
  }

  QStringList hashes;
  const int shown = std::min<int>(3, static_cast<int>(m_pending_texture_hashes.size()));
  for (int i = 0; i < shown; ++i)
  {
    hashes << QStringLiteral("%1")
                  .arg(static_cast<qulonglong>(m_pending_texture_hashes[static_cast<size_t>(i)]), 16,
                       16, QLatin1Char('0'));
  }
  if (static_cast<int>(m_pending_texture_hashes.size()) > shown)
  {
    hashes << tr("+%1 more").arg(static_cast<int>(m_pending_texture_hashes.size()) - shown);
  }

  m_pending_texture_label->setText(
      tr("Pending Texture %1 Filter(s): %2")
          .arg(m_texture_mode_combo->currentData().toBool() ? tr("Exclude") : tr("Include"))
          .arg(hashes.join(QStringLiteral(", "))));
}

void ElementsHuntingDialog::OnSeedSelectionChanged()
{
  const auto items = m_seed_list->selectedItems();
  if (items.empty())
    return;

  const int index = items[0]->data(Qt::UserRole).toInt();
  ElementsGroupManager::GetInstance().SelectSeedCandidate(index);
  m_pending_texture_hashes.clear();
  RefreshPendingTextureSummary();
  RequestRefresh();
}

void ElementsHuntingDialog::ShowTextureBrowser()
{
  const auto status = ElementsGroupManager::GetInstance().GetStatus();
  if (!status.seed_valid)
  {
    QMessageBox::warning(this, tr("View Textures"),
                         tr("No active Elements Hunting preview scope is available."));
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
  browser_config.initial_selected_hashes = m_pending_texture_hashes;
  browser_config.fetch_current_entries = []() {
    return CollectTextureBrowserEntries(
        ElementsGroupManager::GetInstance().GetCurrentTextureSourceDraws());
  };
  browser_config.apply_selected_hashes = [this](const std::vector<u64>& hashes) {
    m_pending_texture_hashes = hashes;
    RefreshPendingTextureSummary();
  };

  ShowTextureHashBrowserDialog(this, browser_config);
}

void ElementsHuntingDialog::SaveCurrentOverride()
{
  if (m_game_id.empty())
  {
    QMessageBox::warning(this, tr("Elements Hunting"), tr("No game is currently running."));
    return;
  }

  auto& manager = ElementsGroupManager::GetInstance();
  const auto status = manager.GetStatus();
  if (!status.highlighted_draw)
  {
    QMessageBox::warning(this, tr("Elements Hunting"),
                         tr("No highlighted match is available in the current frame."));
    return;
  }
  if (!status.seed_valid)
  {
    QMessageBox::warning(this, tr("Elements Hunting"), tr("Seed a draw first."));
    return;
  }

  ElementsGroupManager::ElementGroupOverride initial;
  initial.name =
      fmt::format("Element Draw {}", status.highlighted_draw->draw_index >= 0 ?
                                           status.highlighted_draw->draw_index + 1 :
                                           0);
  initial.handling = ShaderHunter::HandlingType::Skip;
  initial.runtime_element = status.seed_signature;
  initial.texture_hashes = m_pending_texture_hashes;
  initial.texture_hashes_excluded =
      !m_pending_texture_hashes.empty() && m_texture_mode_combo->currentData().toBool();
  initial.selected_match_filter = manager.GetSelectedMatchFilters();
  initial.selected_match_filter_excluded = manager.GetSelectedMatchFilterExcluded();

  ElementsGroupOverrideAddEditDialog dialog(this, &initial, CollectAvailableFlags(m_game_id));
  if (dialog.exec() != QDialog::Accepted)
    return;

  auto overrides = ElementsGroupManager::LoadOverridesFromINI(m_game_id);
  overrides.push_back(dialog.GetResult());
  ElementsGroupManager::SaveOverridesToINI(m_game_id, overrides);
  manager.LoadOverrides(m_game_id);
  emit OverridesChanged();
}

void ElementsHuntingDialog::SaveCurrentHideObjectCode()
{
  if (m_game_id.empty())
  {
    QMessageBox::warning(this, tr("Elements Hunting"), tr("No game is currently running."));
    return;
  }

  auto& manager = ElementsGroupManager::GetInstance();
  const auto draws = manager.GetCurrentTextureSourceDraws();
  if (draws.empty())
  {
    QMessageBox::warning(this, tr("Elements Hunting"),
                         tr("No current Element Hunter matches are available."));
    return;
  }

  std::vector<u32> draw_sequences;
  draw_sequences.reserve(draws.size());
  for (const auto& draw : draws)
  {
    if (draw.draw_sequence != 0)
      draw_sequences.push_back(draw.draw_sequence);
  }

  auto& hide_engine = HideObjectEngine::Engine::GetInstance();
  const auto captured = hide_engine.GetCapturedEntriesForDrawSequences(draw_sequences);
  if (captured.entries.empty())
  {
    QMessageBox::warning(
        this, tr("Elements Hunting"),
        tr("No captured vertex prefixes were found for the current matches.\n"
           "Let the game run with this window open, refresh the matches, then try again."));
    return;
  }

  auto codes = HideObjectEngine::LoadFromINI(m_game_id);
  QString suggested_name =
      MakeUniqueHideObjectName(static_cast<int>(draw_sequences.size()), codes);
  QString code_name;
  for (;;)
  {
    bool accepted = false;
    code_name = QInputDialog::getText(this, tr("Save To Hide Objects"), tr("Code name:"),
                                      QLineEdit::Normal, suggested_name, &accepted)
                    .trimmed();
    if (!accepted)
      return;
    if (code_name.isEmpty())
    {
      QMessageBox::warning(this, tr("Save To Hide Objects"), tr("Code name cannot be empty."));
      continue;
    }
    if (HideObjectNameExists(codes, code_name))
    {
      QMessageBox::warning(this, tr("Save To Hide Objects"),
                           tr("A Hide Object code already uses that name."));
      suggested_name = code_name;
      continue;
    }
    break;
  }

  HideObjectEngine::HideObject code;
  code.name = code_name.toStdString();
  code.entries = captured.entries;
  code.active = true;
  code.user_defined = true;
  codes.push_back(std::move(code));

  HideObjectEngine::SaveToINI(m_game_id, codes);
  hide_engine.ApplyCodes(codes);

  QString message =
      tr("Saved '%1' with %2 hide entries from %3 matched draws.")
          .arg(code_name)
          .arg(captured.entries.size())
          .arg(captured.matched_draws);
  if (captured.matched_draws < captured.requested_draws)
  {
    message += QStringLiteral("\n");
    message += tr("%1 selected draws did not have captured vertex prefixes yet.")
                   .arg(captured.requested_draws - captured.matched_draws);
  }
  if (captured.shorter_than_128bit > 0)
  {
    message += QStringLiteral("\n");
    message += tr("%1 captured prefixes used less than 128 bits because their vertex data was "
                  "shorter than 16 bytes.")
                   .arg(captured.shorter_than_128bit);
  }

  QMessageBox::information(this, tr("Save To Hide Objects"), message);
}

void ElementsHuntingDialog::OnCurrentMatchSelectionChanged()
{
  const auto items = m_current_match_list->selectedItems();
  if (items.empty())
    return;

  ElementsGroupManager::GetInstance().SelectMatch(items[0]->data(Qt::UserRole).toInt());
  RequestRefresh();
}

void ElementsHuntingDialog::OnCurrentMatchItemChanged(QListWidgetItem* item)
{
  if (!item)
    return;

  ElementsGroupManager::GetInstance().SetCurrentMatchFilterEnabled(
      item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
  RequestRefresh();
}

void ElementsHuntingDialog::closeEvent(QCloseEvent* event)
{
  auto& manager = ElementsGroupManager::GetInstance();
  manager.SetHuntEnabled(false);
  manager.SetPopupOpen(false);
  HideObjectEngine::Engine::GetInstance().SetCaptureEnabled(false);
  QDialog::closeEvent(event);
}
