// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/HideObjectWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "DolphinQt/Config/HideObjectAddEditDialog.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "VideoCommon/HideObjectEngine.h"

HideObjectWidget::HideObjectWidget(std::string game_id, std::optional<u16> revision)
    : m_game_id(std::move(game_id)), m_revision(revision)
{
  CreateWidgets();
  ConnectWidgets();
  LoadCodes();
}

HideObjectWidget::~HideObjectWidget() = default;

void HideObjectWidget::CreateWidgets()
{
  m_code_list = new QListWidget;

  m_code_add = new NonDefaultQPushButton(tr("&Add..."));
  m_code_edit = new NonDefaultQPushButton(tr("&Edit..."));
  m_code_remove = new NonDefaultQPushButton(tr("&Remove"));
  m_code_refresh = new NonDefaultQPushButton(tr("Re&fresh List"));
  m_code_reload = new NonDefaultQPushButton(tr("Reload &Codes"));

  m_code_edit->setEnabled(false);
  m_code_remove->setEnabled(false);

  auto* info_label = new QLabel(
      tr("Hide object codes suppress rendering of specific vertex data patterns.\n"
         "Use Add to create a new code, or use the Up/Down buttons in the editor\n"
         "to discover codes while the game is running."));
  info_label->setWordWrap(true);

  auto* button_layout = new QHBoxLayout;
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

void HideObjectWidget::ConnectWidgets()
{
  connect(m_code_list, &QListWidget::itemChanged, this, &HideObjectWidget::OnItemChanged);
  connect(m_code_list, &QListWidget::itemSelectionChanged, this,
          &HideObjectWidget::OnSelectionChanged);
  connect(m_code_add, &QPushButton::clicked, this, &HideObjectWidget::OnAddClicked);
  connect(m_code_edit, &QPushButton::clicked, this, &HideObjectWidget::OnEditClicked);
  connect(m_code_remove, &QPushButton::clicked, this, &HideObjectWidget::OnRemoveClicked);
  connect(m_code_refresh, &QPushButton::clicked, this, &HideObjectWidget::OnRefreshClicked);
  connect(m_code_reload, &QPushButton::clicked, this, &HideObjectWidget::OnReloadClicked);
}

void HideObjectWidget::LoadCodes()
{
  m_codes = HideObjectEngine::LoadFromINI(m_game_id, m_revision);

  m_code_list->setEnabled(!m_game_id.empty());
  m_code_edit->setEnabled(false);
  m_code_remove->setEnabled(false);

  UpdateList();
}

void HideObjectWidget::SaveCodes()
{
  HideObjectEngine::SaveToINI(m_game_id, m_codes);
}

void HideObjectWidget::UpdateList()
{
  const QSignalBlocker blocker(m_code_list);
  m_code_list->clear();

  for (size_t i = 0; i < m_codes.size(); i++)
  {
    const auto& code = m_codes[i];

    QString display;
    if (!code.entries.empty())
    {
      const auto& entry = code.entries[0];
      const int char_len = HideObjectEngine::GetByteCount(entry.type) * 2;
      QString value_str;
      if (char_len <= 16)
      {
        value_str = QStringLiteral("%1")
                        .arg(entry.value_lower, char_len, 16, QLatin1Char('0'))
                        .toUpper();
      }
      else
      {
        const int upper_chars = char_len - 16;
        value_str = QStringLiteral("%1%2")
                        .arg(entry.value_upper, upper_chars, 16, QLatin1Char('0'))
                        .arg(entry.value_lower, 16, 16, QLatin1Char('0'))
                        .toUpper();
      }

      display = QStringLiteral("%1  [%2 %3]")
                    .arg(QString::fromStdString(code.name))
                    .arg(QString::fromLatin1(HideObjectEngine::GetTypeName(entry.type)))
                    .arg(value_str);
    }
    else
    {
      display = QString::fromStdString(code.name);
    }

    auto* item = new QListWidgetItem(display);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    item->setCheckState(code.active ? Qt::Checked : Qt::Unchecked);
    item->setData(Qt::UserRole, static_cast<int>(i));

    m_code_list->addItem(item);
  }
}

void HideObjectWidget::OnItemChanged(QListWidgetItem* item)
{
  const int row = m_code_list->row(item);
  if (row < 0 || row >= static_cast<int>(m_codes.size()))
    return;

  m_codes[row].active = (item->checkState() == Qt::Checked);
  SaveCodes();
  HideObjectEngine::Engine::GetInstance().ApplyCodes(m_codes);
}

void HideObjectWidget::OnSelectionChanged()
{
  const auto items = m_code_list->selectedItems();
  const bool has_selection = !items.empty();
  m_code_edit->setEnabled(has_selection);
  m_code_remove->setEnabled(has_selection);
}

void HideObjectWidget::OnAddClicked()
{
  HideObjectAddEditDialog dialog(this, nullptr, m_codes);
  if (dialog.exec() == QDialog::Accepted)
  {
    m_codes.push_back(dialog.GetResult());
    SaveCodes();
    UpdateList();

    // Re-apply codes to runtime engine so the new code takes effect immediately
    HideObjectEngine::Engine::GetInstance().ApplyCodes(m_codes);
  }
  else
  {
    // Dialog was cancelled — restore the full code set to the runtime engine
    // (in case brute-force Up/Down applied temporary codes)
    HideObjectEngine::Engine::GetInstance().ApplyCodes(m_codes);
  }
}

void HideObjectWidget::OnEditClicked()
{
  const auto items = m_code_list->selectedItems();
  if (items.empty())
    return;

  const int row = m_code_list->row(items[0]);
  if (row < 0 || row >= static_cast<int>(m_codes.size()))
    return;

  HideObjectAddEditDialog dialog(this, &m_codes[row], m_codes);
  if (dialog.exec() == QDialog::Accepted)
  {
    m_codes[row] = dialog.GetResult();
    SaveCodes();
    UpdateList();

    HideObjectEngine::Engine::GetInstance().ApplyCodes(m_codes);
  }
  else
  {
    HideObjectEngine::Engine::GetInstance().ApplyCodes(m_codes);
  }
}

void HideObjectWidget::OnRemoveClicked()
{
  const auto items = m_code_list->selectedItems();
  if (items.empty())
    return;

  const int row = m_code_list->row(items[0]);
  if (row < 0 || row >= static_cast<int>(m_codes.size()))
    return;

  m_codes.erase(m_codes.begin() + row);

  SaveCodes();
  UpdateList();
  HideObjectEngine::Engine::GetInstance().ApplyCodes(m_codes);

  m_code_edit->setEnabled(false);
  m_code_remove->setEnabled(false);
}

void HideObjectWidget::OnRefreshClicked()
{
  LoadCodes();
}

void HideObjectWidget::showEvent(QShowEvent* event)
{
  QWidget::showEvent(event);
  // The VR Config editor writes the same file: show what it holds now, or the next change here
  // would save the old list over the codes pasted there.
  LoadCodes();
}

void HideObjectWidget::OnReloadClicked()
{
  HideObjectEngine::Engine::GetInstance().LoadCodes(m_game_id);
}
