// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <imgui.h>

#include "Common/Timer.h"
#include "Core/State.h"

#include "VideoCommon/VR/VRMenuActions.h"
#include "VideoCommon/VR/VRMenuTabs.h"
#include "VideoCommon/VR/VRMenuWidgets.h"

namespace VR
{
namespace
{
// Slots 1-10, the files the desktop menus and the savestate hotkeys use (<GameID>.s01-.s10).
constexpr u32 FIRST_SLOT = 1;
constexpr u32 SLOT_COUNT = 10;
// A destructive button needs a second press within this time.
constexpr u64 CONFIRM_TIMEOUT_US = 4'000'000;
constexpr u64 REFRESH_INTERVAL_US = 1'000'000;

class SavestatesTab final : public VRMenuTab
{
public:
  const char* GetName() const override { return "Savestates"; }

  void OnOpen(VRMenuContext& context) override
  {
    m_confirm = {};
    Refresh(context);
  }

  void Draw(VRMenuContext& context) override
  {
    const u64 now_us = Common::Timer::NowUs();
    if (now_us - m_last_refresh_us > REFRESH_INTERVAL_US)
      Refresh(context);
    if (m_confirm.slot != 0 && now_us > m_confirm.until_us)
      m_confirm = {};

    SlotList slots;
    bool slots_valid;
    {
      std::lock_guard lock(m_slots->mutex);
      slots = m_slots->slots;
      slots_valid = m_slots->valid;
    }

    MenuWidgets::SectionHeader("Slots");
    const bool busy = context.actions.GetPendingJobs() > 0;
    const float button_width = ImGui::GetFontSize() * 6.5f;
    for (u32 i = 0; i < SLOT_COUNT; ++i)
    {
      const u32 slot = FIRST_SLOT + i;
      const SlotEntry& entry = slots[i];
      ImGui::PushID(static_cast<int>(slot));
      MenuWidgets::BeginRow(fmt::format("Slot {}", slot).c_str(), nullptr,
                            Config::LayerType::Base);

      ImGui::BeginDisabled(busy);
      if (ConfirmButton(Action::Save, slot, entry.used ? "Overwrite?" : nullptr, "Save",
                        button_width, now_us))
      {
        context.actions.Run(fmt::format("Saving slot {}...", slot),
                            [slot](Core::System& system) {
                              State::Save(system, slot);
                              return fmt::format("Saved slot {}", slot);
                            });
        m_last_refresh_us = 0;
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(!entry.used);
      if (ConfirmButton(Action::Load, slot, "Load?", "Load", button_width, now_us))
      {
        context.actions.Run(fmt::format("Loading slot {}...", slot), [slot](Core::System& system) {
          if (State::GetUnixTimeOfSlot(slot) == 0)
            return fmt::format("Slot {} is empty", slot);
          State::Load(system, slot);
          return fmt::format("Loaded slot {}", slot);
        });
      }
      ImGui::EndDisabled();
      ImGui::EndDisabled();

      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      if (entry.used)
        ImGui::TextUnformatted(entry.text.c_str());
      else
        ImGui::TextDisabled("%s", slots_valid ? "Empty" : "...");
      ImGui::PopID();
    }

    MenuWidgets::SectionHeader("Undo");
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Undo Load State"))
    {
      context.actions.Run("Undoing load...", [](Core::System& system) {
        State::UndoLoadState(system);
        return std::string("Restored the state from before the last load");
      });
    }
    ImGui::SameLine();
    if (ImGui::Button("Undo Save State"))
    {
      context.actions.Run("Undoing save...", [](Core::System& system) {
        State::UndoSaveState(system);
        return std::string("Restored the slot overwritten by the last save");
      });
      m_last_refresh_us = 0;
    }
    ImGui::EndDisabled();
  }

private:
  enum class Action
  {
    None,
    Save,
    Load,
  };

  struct SlotEntry
  {
    bool used = false;
    std::string text;
  };
  using SlotList = std::array<SlotEntry, SLOT_COUNT>;

  // Filled by host-thread jobs (savestate headers are file reads).
  struct SlotInfo
  {
    std::mutex mutex;
    SlotList slots;
    bool valid = false;
    std::atomic<bool> refreshing = false;
  };

  struct Confirm
  {
    Action action = Action::None;
    u32 slot = 0;
    u64 until_us = 0;
  };

  void Refresh(VRMenuContext& context)
  {
    m_last_refresh_us = Common::Timer::NowUs();
    if (m_slots->refreshing.exchange(true))
      return;
    context.actions.RunInBackground([slots_info = m_slots](Core::System&) {
      SlotList slots;
      for (u32 i = 0; i < SLOT_COUNT; ++i)
      {
        const u32 slot = FIRST_SLOT + i;
        slots[i].used = State::GetUnixTimeOfSlot(slot) != 0;
        if (slots[i].used)
          slots[i].text = State::GetInfoStringOfSlot(slot);
      }
      {
        std::lock_guard lock(slots_info->mutex);
        slots_info->slots = std::move(slots);
        slots_info->valid = true;
      }
      slots_info->refreshing = false;
    });
  }

  // A button that runs its action right away, or, with `confirm_label`, only on a second press
  // while it shows `confirm_label`.
  bool ConfirmButton(Action action, u32 slot, const char* confirm_label, const char* label,
                     float width, u64 now_us)
  {
    const bool confirming = m_confirm.action == action && m_confirm.slot == slot;
    if (confirming)
    {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.35f, 0.1f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.45f, 0.15f, 1.0f));
    }
    const bool pressed =
        ImGui::Button(confirming ? confirm_label : label, ImVec2(width, 0.0f));
    if (confirming)
      ImGui::PopStyleColor(2);
    if (!pressed)
      return false;

    if (!confirm_label || confirming)
    {
      m_confirm = {};
      return true;
    }
    m_confirm = {action, slot, now_us + CONFIRM_TIMEOUT_US};
    return false;
  }

  std::shared_ptr<SlotInfo> m_slots = std::make_shared<SlotInfo>();
  u64 m_last_refresh_us = 0;
  Confirm m_confirm;
};
}  // namespace

std::unique_ptr<VRMenuTab> CreateSavestatesTab()
{
  return std::make_unique<SavestatesTab>();
}
}  // namespace VR
