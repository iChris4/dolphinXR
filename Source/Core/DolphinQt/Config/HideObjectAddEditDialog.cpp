// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/HideObjectAddEditDialog.h"

#include <utility>
#include <vector>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

#include "VideoCommon/HideObjectEngine.h"

namespace
{
QString StripHexPrefix(QString text)
{
  text = text.trimmed();
  if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
    text.remove(0, 2);
  return text;
}

QString FormatEntryValue(const HideObjectEngine::HideObjectEntry& entry)
{
  const int char_len = HideObjectEngine::GetByteCount(entry.type) * 2;

  if (char_len <= 16)
  {
    return QStringLiteral("%1")
        .arg(entry.value_lower, char_len, 16, QLatin1Char('0'))
        .toUpper();
  }

  const int upper_chars = char_len - 16;
  return QStringLiteral("%1%2")
      .arg(entry.value_upper, upper_chars, 16, QLatin1Char('0'))
      .arg(entry.value_lower, 16, 16, QLatin1Char('0'))
      .toUpper();
}

bool TryParseEntryValue(const QString& text, HideObjectEngine::HideObjectType type,
                        HideObjectEngine::HideObjectEntry* entry)
{
  const QString value = StripHexPrefix(text);
  const int expected_chars = HideObjectEngine::GetByteCount(type) * 2;
  if (value.isEmpty() || value.length() > expected_chars)
    return false;

  for (const QChar c : value)
  {
    const ushort ch = c.toUpper().unicode();
    if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F')))
      return false;
  }

  HideObjectEngine::HideObjectEntry parsed;
  parsed.type = type;

  bool ok = false;
  if (expected_chars <= 16)
  {
    parsed.value_lower = value.toULongLong(&ok, 16);
    parsed.value_upper = 0;
  }
  else
  {
    if (value.length() > 16)
    {
      const QString upper_str = value.left(value.length() - 16);
      const QString lower_str = value.right(16);
      parsed.value_upper = upper_str.toULongLong(&ok, 16);
      if (ok)
        parsed.value_lower = lower_str.toULongLong(&ok, 16);
    }
    else
    {
      parsed.value_upper = 0;
      parsed.value_lower = value.toULongLong(&ok, 16);
    }
  }

  if (!ok)
    return false;

  *entry = parsed;
  return true;
}

std::vector<u8> EntryToBytes(const HideObjectEngine::HideObjectEntry& entry)
{
  const int byte_count = HideObjectEngine::GetByteCount(entry.type);
  std::vector<u8> bytes;
  bytes.reserve(static_cast<size_t>(byte_count));

  if (byte_count > 8)
  {
    const int upper_bytes = byte_count - 8;
    for (int j = upper_bytes; j > 0; --j)
      bytes.push_back(static_cast<u8>((entry.value_upper >> ((j - 1) * 8)) & 0xFF));
    for (int j = 8; j > 0; --j)
      bytes.push_back(static_cast<u8>((entry.value_lower >> ((j - 1) * 8)) & 0xFF));
  }
  else
  {
    for (int j = byte_count; j > 0; --j)
      bytes.push_back(static_cast<u8>((entry.value_lower >> ((j - 1) * 8)) & 0xFF));
  }

  return bytes;
}

HideObjectEngine::HideObjectEntry EntryFromBytes(HideObjectEngine::HideObjectType type,
                                                 const std::vector<u8>& bytes)
{
  HideObjectEngine::HideObjectEntry entry;
  entry.type = type;

  const int byte_count = static_cast<int>(bytes.size());
  if (byte_count > 8)
  {
    const int upper_bytes = byte_count - 8;
    for (int i = 0; i < upper_bytes; ++i)
      entry.value_upper = (entry.value_upper << 8) | bytes[i];
    for (int i = upper_bytes; i < byte_count; ++i)
      entry.value_lower = (entry.value_lower << 8) | bytes[i];
  }
  else
  {
    for (const u8 byte : bytes)
      entry.value_lower = (entry.value_lower << 8) | byte;
  }

  return entry;
}

int LastByteSliderValueFromEntry(const HideObjectEngine::HideObjectEntry& entry)
{
  const std::vector<u8> bytes = EntryToBytes(entry);
  return bytes.empty() ? 0 : bytes.back();
}

HideObjectEngine::HideObjectEntry EntryFromLastByteSliderValue(
    HideObjectEngine::HideObjectType type, int slider_value,
    const HideObjectEngine::HideObjectEntry& base)
{
  const int byte_count = HideObjectEngine::GetByteCount(type);
  std::vector<u8> bytes = EntryToBytes(base);
  if (static_cast<int>(bytes.size()) != byte_count)
    bytes.assign(static_cast<size_t>(byte_count), 0);

  bytes.back() = static_cast<u8>(slider_value & 0xFF);

  return EntryFromBytes(type, bytes);
}

HideObjectEngine::HideObjectEntry ResizeEntryForType(const HideObjectEngine::HideObjectEntry& entry,
                                                     HideObjectEngine::HideObjectType new_type,
                                                     u8 expand_fill_byte)
{
  std::vector<u8> bytes = EntryToBytes(entry);
  const size_t new_size = static_cast<size_t>(HideObjectEngine::GetByteCount(new_type));

  if (bytes.size() < new_size)
    bytes.resize(new_size, expand_fill_byte);
  else if (bytes.size() > new_size)
    bytes.resize(new_size);

  return EntryFromBytes(new_type, bytes);
}

}  // namespace

HideObjectAddEditDialog::HideObjectAddEditDialog(
    QWidget* parent, const HideObjectEngine::HideObject* existing_code,
    const std::vector<HideObjectEngine::HideObject>& all_codes)
    : QDialog(parent), m_all_codes(all_codes)
{
  if (existing_code)
  {
    m_is_edit = true;
    m_original_name = existing_code->name;
    m_result = *existing_code;
    if (!m_result.entries.empty())
      m_current_entry = m_result.entries[0];
    else
      m_current_entry.type = HideObjectEngine::HideObjectType::Bits8;

    for (size_t i = 0; i < m_all_codes.size(); ++i)
    {
      if (&m_all_codes[i] == existing_code)
      {
        m_existing_code_index = i;
        break;
      }
    }
  }
  else
  {
    m_current_entry.type = HideObjectEngine::HideObjectType::Bits8;
    m_current_entry.value_upper = 0;
    m_current_entry.value_lower = 0;
  }

  if (m_result.entries.empty())
    m_result.entries.push_back(m_current_entry);
  m_entry_enabled.assign(m_result.entries.size(), true);

  setWindowTitle(m_is_edit ? tr("Edit Hide Object Code") : tr("Add Hide Object Code"));
  setMinimumWidth(560);

  CreateWidgets();
  ConnectWidgets();
  UpdateEntryList();
  UpdateValueDisplay();
}

void HideObjectAddEditDialog::CreateWidgets()
{
  auto* name_label = new QLabel(tr("Name:"));
  m_name_edit = new QLineEdit;
  if (m_is_edit)
    m_name_edit->setText(QString::fromStdString(m_result.name));
  else
    m_name_edit->setPlaceholderText(tr("Enter code name..."));

  auto* entries_label = new QLabel(tr("Code lines:"));
  entries_label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  m_entry_list = new QListWidget;
  m_entry_list->setFont(QFont(QStringLiteral("Courier New"), 10));
  m_entry_list->setMinimumHeight(120);
  m_entry_list->setToolTip(
      tr("Unchecked lines are excluded while testing and are discarded when you click OK."));

  m_entry_add = new QPushButton(tr("Add Line"));
  m_entry_remove = new QPushButton(tr("Remove Line"));

  auto* entry_button_layout = new QHBoxLayout;
  entry_button_layout->addWidget(m_entry_add);
  entry_button_layout->addWidget(m_entry_remove);
  entry_button_layout->addStretch();

  auto* type_label = new QLabel(tr("Size:"));
  m_type_combo = new QComboBox;
  for (int i = 0; i < static_cast<int>(HideObjectEngine::HideObjectType::Count); i++)
    m_type_combo->addItem(QString::fromLatin1(
        HideObjectEngine::GetTypeName(static_cast<HideObjectEngine::HideObjectType>(i))));
  m_type_combo->setCurrentIndex(static_cast<int>(m_current_entry.type));

  auto* value_label = new QLabel(tr("Value (hex):"));
  m_value_edit = new QLineEdit;
  m_value_edit->setFont(QFont(QStringLiteral("Courier New"), 10));
  m_value_slider = new QSlider(Qt::Horizontal);
  m_value_slider->setRange(0, 0xFF);
  m_value_slider->setSingleStep(1);
  m_value_slider->setPageStep(0x10);
  m_value_slider->setToolTip(tr("Adjusts the last byte of the value from 00 to FF."));

  m_up_button = new QPushButton(tr("Up"));
  m_down_button = new QPushButton(tr("Down"));

  const QString tooltip =
      tr("The Up/Down buttons can be used to find new codes.\n"
         "While the game is playing, find an object you want to hide and select '8bits'.\n"
         "Keep clicking Up until the object disappears.\n"
         "Now choose '16bits' and continue clicking Up until the object is hidden again.\n"
         "Repeat until the code is long enough to be unique.\n"
         "Warning: Too short of a code may hide other objects too.");
  m_up_button->setToolTip(tooltip);
  m_down_button->setToolTip(tooltip);

  auto* button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);

  auto* grid = new QGridLayout;
  grid->addWidget(name_label, 0, 0);
  grid->addWidget(m_name_edit, 0, 1, 1, 3);
  grid->addWidget(entries_label, 1, 0);
  grid->addWidget(m_entry_list, 1, 1, 1, 3);
  grid->addLayout(entry_button_layout, 2, 1, 1, 3);
  grid->addWidget(type_label, 3, 0);
  grid->addWidget(m_type_combo, 3, 1, 1, 3);
  grid->addWidget(value_label, 4, 0);
  grid->addWidget(m_value_edit, 4, 1);
  grid->addWidget(m_up_button, 4, 2);
  grid->addWidget(m_down_button, 4, 3);
  grid->addWidget(m_value_slider, 5, 1, 1, 3);

  auto* layout = new QVBoxLayout{this};
  layout->addLayout(grid);
  layout->addWidget(button_box);

  connect(button_box, &QDialogButtonBox::accepted, this, &HideObjectAddEditDialog::OnAccept);
  connect(button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void HideObjectAddEditDialog::ConnectWidgets()
{
  connect(m_type_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &HideObjectAddEditDialog::OnTypeChanged);
  connect(m_value_edit, &QLineEdit::textChanged, this,
          &HideObjectAddEditDialog::OnValueTextChanged);
  connect(m_value_slider, &QSlider::valueChanged, this,
          &HideObjectAddEditDialog::OnValueSliderChanged);
  connect(m_entry_list, &QListWidget::itemSelectionChanged, this,
          &HideObjectAddEditDialog::OnEntrySelectionChanged);
  connect(m_entry_list, &QListWidget::itemChanged, this,
          &HideObjectAddEditDialog::OnEntryCheckStateChanged);
  connect(m_entry_add, &QPushButton::clicked, this, &HideObjectAddEditDialog::OnAddEntryClicked);
  connect(m_entry_remove, &QPushButton::clicked, this,
          &HideObjectAddEditDialog::OnRemoveEntryClicked);
  connect(m_up_button, &QPushButton::clicked, this, &HideObjectAddEditDialog::OnUpClicked);
  connect(m_down_button, &QPushButton::clicked, this, &HideObjectAddEditDialog::OnDownClicked);
}

void HideObjectAddEditDialog::UpdateEntryList()
{
  const QSignalBlocker blocker(m_entry_list);
  m_entry_list->clear();

  for (size_t i = 0; i < m_result.entries.size(); ++i)
  {
    const auto& entry = m_result.entries[i];
    auto* item = new QListWidgetItem(QStringLiteral("[%1] %2").arg(
        QString::fromLatin1(HideObjectEngine::GetTypeName(entry.type)), FormatEntryValue(entry)));
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(m_entry_enabled[i] ? Qt::Checked : Qt::Unchecked);
    m_entry_list->addItem(item);
  }

  m_entry_list->setCurrentRow(static_cast<int>(m_current_entry_index));
  m_entry_remove->setEnabled(m_result.entries.size() > 1);
}

void HideObjectAddEditDialog::UpdateCurrentEntryListItem()
{
  QListWidgetItem* const item = m_entry_list->item(static_cast<int>(m_current_entry_index));
  if (!item)
    return;

  item->setText(QStringLiteral("[%1] %2").arg(
      QString::fromLatin1(HideObjectEngine::GetTypeName(m_current_entry.type)),
      FormatEntryValue(m_current_entry)));
}

void HideObjectAddEditDialog::UpdateValueDisplay()
{
  {
    const QSignalBlocker blocker(m_value_edit);
    m_value_edit->setText(FormatEntryValue(m_current_entry));
  }
  UpdateValueSliderFromText();
}

void HideObjectAddEditDialog::UpdateValueSliderFromText()
{
  HideObjectEngine::HideObjectEntry parsed;
  if (!TryParseEntryValue(m_value_edit->text(), m_current_entry.type, &parsed))
    return;

  const QSignalBlocker blocker(m_value_slider);
  m_value_slider->setValue(LastByteSliderValueFromEntry(parsed));
}

bool HideObjectAddEditDialog::ParseValueFromUI()
{
  const QString text = m_value_edit->text().trimmed();
  if (text.isEmpty())
  {
    QMessageBox::warning(this, tr("Error"), tr("Value cannot be empty."));
    return false;
  }

  HideObjectEngine::HideObjectEntry parsed;
  if (!TryParseEntryValue(text, m_current_entry.type, &parsed))
  {
    QMessageBox::warning(this, tr("Error"),
                         tr("Invalid hex value. Use characters 0-9 and A-F only, with no more "
                            "digits than the selected size."));
    return false;
  }

  m_current_entry = parsed;
  return true;
}

void HideObjectAddEditDialog::StoreCurrentEntry()
{
  m_result.entries[m_current_entry_index] = m_current_entry;
  UpdateCurrentEntryListItem();
}

void HideObjectAddEditDialog::OnTypeChanged()
{
  if (!ParseValueFromUI())
    return;

  const auto new_type = static_cast<HideObjectEngine::HideObjectType>(m_type_combo->currentIndex());
  m_current_entry = ResizeEntryForType(m_current_entry, new_type, 0x00);

  StoreCurrentEntry();
  UpdateValueDisplay();
  ApplyTemporarily();
}

void HideObjectAddEditDialog::OnUpClicked()
{
  if (!ParseValueFromUI())
    return;

  // Brute-force only the last byte (two hex chars), without carrying into higher bytes.
  const u8 low_byte = static_cast<u8>(m_current_entry.value_lower & 0xFFULL);
  const u8 next_low_byte = static_cast<u8>(low_byte + 1);
  m_current_entry.value_lower = (m_current_entry.value_lower & ~0xFFULL) | next_low_byte;

  StoreCurrentEntry();
  UpdateValueDisplay();
  ApplyTemporarily();
}

void HideObjectAddEditDialog::OnDownClicked()
{
  if (!ParseValueFromUI())
    return;

  // Brute-force only the last byte (two hex chars), without borrowing from higher bytes.
  const u8 low_byte = static_cast<u8>(m_current_entry.value_lower & 0xFFULL);
  const u8 prev_low_byte = static_cast<u8>(low_byte - 1);
  m_current_entry.value_lower = (m_current_entry.value_lower & ~0xFFULL) | prev_low_byte;

  StoreCurrentEntry();
  UpdateValueDisplay();
  ApplyTemporarily();
}

void HideObjectAddEditDialog::OnValueTextChanged()
{
  UpdateValueSliderFromText();

  HideObjectEngine::HideObjectEntry parsed;
  if (TryParseEntryValue(m_value_edit->text(), m_current_entry.type, &parsed))
  {
    m_current_entry = parsed;
    StoreCurrentEntry();
  }
}

void HideObjectAddEditDialog::OnValueSliderChanged(int value)
{
  HideObjectEngine::HideObjectEntry current_entry;
  if (!TryParseEntryValue(m_value_edit->text(), m_current_entry.type, &current_entry))
    return;

  m_current_entry = EntryFromLastByteSliderValue(m_current_entry.type, value, current_entry);
  StoreCurrentEntry();
  UpdateValueDisplay();
  ApplyTemporarily();
}

void HideObjectAddEditDialog::OnEntrySelectionChanged()
{
  const int selected_row = m_entry_list->currentRow();
  if (selected_row < 0 || static_cast<size_t>(selected_row) == m_current_entry_index)
    return;

  if (!ParseValueFromUI())
  {
    const QSignalBlocker blocker(m_entry_list);
    m_entry_list->setCurrentRow(static_cast<int>(m_current_entry_index));
    return;
  }

  StoreCurrentEntry();
  m_current_entry_index = static_cast<size_t>(selected_row);
  m_current_entry = m_result.entries[m_current_entry_index];

  {
    const QSignalBlocker blocker(m_type_combo);
    m_type_combo->setCurrentIndex(static_cast<int>(m_current_entry.type));
  }
  UpdateValueDisplay();
}

void HideObjectAddEditDialog::OnEntryCheckStateChanged(QListWidgetItem* item)
{
  const int row = m_entry_list->row(item);
  if (row < 0 || row >= static_cast<int>(m_entry_enabled.size()))
    return;

  const bool enabled = item->checkState() == Qt::Checked;
  if (m_entry_enabled[static_cast<size_t>(row)] == enabled)
    return;

  m_entry_enabled[static_cast<size_t>(row)] = enabled;
  ApplyTemporarily();
}

void HideObjectAddEditDialog::OnAddEntryClicked()
{
  if (!ParseValueFromUI())
    return;

  StoreCurrentEntry();

  HideObjectEngine::HideObjectEntry entry;
  entry.type = HideObjectEngine::HideObjectType::Bits8;
  m_result.entries.push_back(entry);
  m_entry_enabled.push_back(true);
  m_current_entry_index = m_result.entries.size() - 1;
  m_current_entry = entry;

  {
    const QSignalBlocker blocker(m_type_combo);
    m_type_combo->setCurrentIndex(static_cast<int>(m_current_entry.type));
  }
  UpdateEntryList();
  UpdateValueDisplay();
}

void HideObjectAddEditDialog::OnRemoveEntryClicked()
{
  if (m_result.entries.size() <= 1)
    return;

  m_result.entries.erase(m_result.entries.begin() + m_current_entry_index);
  m_entry_enabled.erase(m_entry_enabled.begin() + m_current_entry_index);
  if (m_current_entry_index >= m_result.entries.size())
    m_current_entry_index = m_result.entries.size() - 1;
  m_current_entry = m_result.entries[m_current_entry_index];

  {
    const QSignalBlocker blocker(m_type_combo);
    m_type_combo->setCurrentIndex(static_cast<int>(m_current_entry.type));
  }
  UpdateEntryList();
  UpdateValueDisplay();
  ApplyTemporarily();
}

void HideObjectAddEditDialog::ApplyTemporarily()
{
  std::vector<HideObjectEngine::HideObject> temp_list;
  temp_list.reserve(m_all_codes.size() + 1);

  // Keep saved active codes applied while overlaying the search candidate.
  for (size_t i = 0; i < m_all_codes.size(); ++i)
  {
    if (m_existing_code_index && i == *m_existing_code_index)
      continue;

    temp_list.push_back(m_all_codes[i]);
  }

  HideObjectEngine::HideObject temp_code;
  temp_code.name = "temp_brute_force";
  temp_code.entries.reserve(m_result.entries.size());
  for (size_t i = 0; i < m_result.entries.size(); ++i)
  {
    if (m_entry_enabled[i])
      temp_code.entries.push_back(m_result.entries[i]);
  }
  temp_code.active = true;
  temp_code.user_defined = false;
  if (!temp_code.entries.empty())
    temp_list.push_back(std::move(temp_code));

  HideObjectEngine::Engine::GetInstance().ApplyCodes(temp_list);
}

void HideObjectAddEditDialog::OnAccept()
{
  const QString name = m_name_edit->text().trimmed();
  if (name.isEmpty())
  {
    QMessageBox::warning(this, tr("Error"), tr("Please enter a name for this code."));
    return;
  }

  // Check name uniqueness
  for (const auto& code : m_all_codes)
  {
    if (code.name == name.toStdString() && code.name != m_original_name)
    {
      QMessageBox::warning(this, tr("Error"),
                           tr("Name is already in use. Please choose a unique name."));
      return;
    }
  }

  if (!ParseValueFromUI())
    return;

  StoreCurrentEntry();

  std::vector<HideObjectEngine::HideObjectEntry> enabled_entries;
  enabled_entries.reserve(m_result.entries.size());
  for (size_t i = 0; i < m_result.entries.size(); ++i)
  {
    if (m_entry_enabled[i])
      enabled_entries.push_back(m_result.entries[i]);
  }
  if (enabled_entries.empty())
  {
    QMessageBox::warning(this, tr("Error"), tr("Please enable at least one code line to save."));
    return;
  }

  if (enabled_entries.size() != m_result.entries.size() &&
      QMessageBox::warning(this, tr("Unchecked Code Lines"),
                           tr("Unchecked code lines will not be saved and will be removed from "
                              "this code. Do you want to continue?"),
                           QMessageBox::Yes | QMessageBox::No, QMessageBox::No) !=
          QMessageBox::Yes)
  {
    return;
  }

  m_result.entries = std::move(enabled_entries);
  m_result.name = name.toStdString();
  m_result.active = true;
  m_result.user_defined = true;

  accept();
}
