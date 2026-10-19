// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace HideObjectEngine
{

enum class HideObjectType
{
  Bits8 = 0,
  Bits16,
  Bits24,
  Bits32,
  Bits40,
  Bits48,
  Bits56,
  Bits64,
  Bits72,
  Bits80,
  Bits88,
  Bits96,
  Bits104,
  Bits112,
  Bits120,
  Bits128,
  Count = 16
};

// Returns byte count for a given type (Bits8 = 1 byte, Bits16 = 2, ... Bits128 = 16)
inline int GetByteCount(HideObjectType type)
{
  return static_cast<int>(type) + 1;
}

// String names for each type (for UI and INI serialization)
const char* GetTypeName(HideObjectType type);
HideObjectType ParseTypeName(const std::string& name);

struct HideObjectEntry
{
  HideObjectType type = HideObjectType::Bits16;
  u64 value_upper = 0;  // High bytes for types > 64 bits
  u64 value_lower = 0;  // Low bytes (always used)
};

struct HideObject
{
  std::string name;
  std::vector<HideObjectEntry> entries;  // Usually 1, but format allows multiple per code
  bool active = true;
  // False for codes that ship in Sys (LoadFromINI): they cannot be deleted in the user's file.
  bool user_defined = true;
};

// Flattened byte array for fast memcmp matching
using SkipEntry = std::vector<u8>;

struct CapturedHideObjectEntries
{
  std::vector<HideObjectEntry> entries;
  size_t requested_draws = 0;
  size_t matched_draws = 0;
  size_t prefixes_seen = 0;
  size_t shorter_than_128bit = 0;
};

std::optional<HideObjectEntry> BuildEntryFromPrefix(const u8* data, size_t available_bytes);

// --- Static INI helpers (direct file I/O, bypasses IniFile to avoid corruption) ---
std::vector<HideObject> LoadFromINI(const std::string& game_id,
                                    std::optional<u16> revision = std::nullopt);
void SaveToINI(const std::string& game_id, const std::vector<HideObject>& codes);
// The codes of one INI file (no Sys/User merge), e.g. an entry the agent server wrote.
std::vector<HideObject> LoadCodesFromINIFile(const std::string& path);

// --- Runtime singleton for hide-object matching ---
class Engine
{
public:
  static Engine& GetInstance();

  // Load codes from per-game INI and apply active ones
  void LoadCodes(const std::string& game_id);
  void LoadCodesIfNeeded(const std::string& game_id);

  // Convert HideObject list to flattened byte patterns for matching.
  void ApplyCodes(const std::vector<HideObject>& codes);

  // Runtime-only capture used by Element Hunter to export selected draws to Hide Objects.
  void SetCaptureEnabled(bool enabled);
  bool IsCaptureEnabled() const;
  void CaptureVertexPrefix(const u8* src, size_t available_bytes);
  void CommitCapturedPrefixesForDraw(u32 draw_sequence);
  void DiscardPendingCapturedPrefixes();
  void OnFrameEnd();
  CapturedHideObjectEntries
  GetCapturedEntriesForDrawSequences(const std::vector<u32>& draw_sequences) const;

  // Called from VertexLoaderManager::RunVertices — returns true if src matches any active code
  bool ShouldHide(const u8* src) const;
  bool HasCodes() const;

private:
  Engine() = default;

  mutable std::mutex m_mutex;
  std::vector<SkipEntry> m_active_entries;  // Flattened byte patterns for memcmp
  std::string m_loaded_game_id;

  struct CapturedPrefix
  {
    u32 draw_sequence = 0;
    SkipEntry prefix;
  };

  mutable std::mutex m_capture_mutex;
  std::atomic<bool> m_capture_enabled{false};
  std::vector<SkipEntry> m_pending_capture_prefixes;
  std::vector<CapturedPrefix> m_collecting_capture_prefixes;
  std::vector<CapturedPrefix> m_display_capture_prefixes;

  // Thread-safety coordination between UI updates and render-thread checks
  std::atomic<bool> m_updating{false};
  std::atomic<bool> m_done{true};
};

}  // namespace HideObjectEngine
