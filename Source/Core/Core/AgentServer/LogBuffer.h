// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Logging/LogManager.h"

namespace AgentServer
{
// The last log lines, kept for the read_log tool. Registered as a LogManager listener, so it sees
// what the log types and verbosity let through, like the log window.
class LogBuffer final : public Common::Log::LogListener
{
public:
  struct Line
  {
    u64 sequence;
    Common::Log::LogLevel level;
    std::string text;
  };

  static constexpr size_t CAPACITY = 4000;

  void Log(Common::Log::LogLevel level, const char* msg) override;

  // Lines with a sequence number above `after_sequence`, oldest first.
  std::vector<Line> GetLines(u64 after_sequence) const;
  u64 GetLastSequence() const;

private:
  mutable std::mutex m_mutex;
  std::deque<Line> m_lines;
  u64 m_next_sequence = 1;
};
}  // namespace AgentServer
