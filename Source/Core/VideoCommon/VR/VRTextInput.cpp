// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/VRTextInput.h"

#include <atomic>
#include <mutex>
#include <utility>

namespace VR::TextInput
{
namespace
{
std::atomic<ShowKeyboardFunction> s_show_keyboard = nullptr;

std::mutex s_mutex;
// The latest request; typed text always belongs to it.
u32 s_request = 0;
std::string s_text;
bool s_text_changed = false;
bool s_done = false;
}  // namespace

void SetShowKeyboardFunction(ShowKeyboardFunction function)
{
  s_show_keyboard.store(function);
}

bool IsAvailable()
{
  return s_show_keyboard.load() != nullptr;
}

u32 Request(const std::string& text)
{
  u32 request;
  {
    std::lock_guard lock(s_mutex);
    request = ++s_request;
    s_text = text;
    s_text_changed = false;
    s_done = false;
  }
  if (const ShowKeyboardFunction show_keyboard = s_show_keyboard.load())
    show_keyboard(text);
  return request;
}

void SetText(std::string text, bool done)
{
  std::lock_guard lock(s_mutex);
  if (s_request == 0 || (text == s_text && !done))
    return;
  s_text = std::move(text);
  s_text_changed = true;
  s_done = s_done || done;
}

std::optional<Edit> TakeEdit(u32 request)
{
  std::lock_guard lock(s_mutex);
  if (request == 0 || request != s_request || !s_text_changed)
    return std::nullopt;
  Edit edit{s_text, s_done};
  s_text_changed = false;
  s_done = false;
  return edit;
}
}  // namespace VR::TextInput
