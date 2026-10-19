// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/AgentServer/LogBuffer.h"

#include <string_view>

namespace AgentServer
{
void LogBuffer::Log(Common::Log::LogLevel level, const char* msg)
{
  std::string_view text(msg);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    text.remove_suffix(1);

  std::lock_guard lock(m_mutex);
  m_lines.push_back({m_next_sequence++, level, std::string(text)});
  if (m_lines.size() > CAPACITY)
    m_lines.pop_front();
}

std::vector<LogBuffer::Line> LogBuffer::GetLines(u64 after_sequence) const
{
  std::lock_guard lock(m_mutex);
  std::vector<Line> lines;
  for (const Line& line : m_lines)
  {
    if (line.sequence > after_sequence)
      lines.push_back(line);
  }
  return lines;
}

u64 LogBuffer::GetLastSequence() const
{
  std::lock_guard lock(m_mutex);
  return m_next_sequence - 1;
}
}  // namespace AgentServer
