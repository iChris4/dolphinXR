// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <optional>
#include <string>

#include "Common/CommonTypes.h"

// Text entry for the in-game VR menu through a system keyboard shown over the immersive session
// (Android: the Quest keyboard overlay). The frontend provides the keyboard; without one the menu
// shows no text fields.
namespace VR::TextInput
{
// Frontend: shows the keyboard to edit `text`. Called on the video thread; must not block.
using ShowKeyboardFunction = void (*)(const std::string& text);
void SetShowKeyboardFunction(ShowKeyboardFunction function);
bool IsAvailable();

// Menu (video thread): shows the keyboard to edit `text`. Returns the request TakeEdit reads.
u32 Request(const std::string& text);
// Frontend, any thread: the text of the field after each edit, with `done` when the keyboard's
// Enter (Done) key finished it.
void SetText(std::string text, bool done);

struct Edit
{
  std::string text;
  bool done = false;
};
// Menu: the text typed for `request` since the last call, if any.
std::optional<Edit> TakeEdit(u32 request);
}  // namespace VR::TextInput

#endif  // ENABLE_VR
