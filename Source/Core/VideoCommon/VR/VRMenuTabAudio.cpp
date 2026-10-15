// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <memory>
#include <string>

#include "AudioCommon/AudioCommon.h"
#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"

#include "VideoCommon/VR/VRMenuActions.h"
#include "VideoCommon/VR/VRMenuTabs.h"
#include "VideoCommon/VR/VRMenuWidgets.h"

namespace VR
{
namespace
{
class AudioTab final : public VRMenuTab
{
public:
  const char* GetName() const override { return "Audio"; }

  void Draw(VRMenuContext& context) override
  {
    // Audio settings are global (Dolphin.ini), not part of "Save for this game".
    MenuWidgets::SectionHeader("Volume");
    bool changed = MenuWidgets::SliderInt("Volume", nullptr, Config::MAIN_AUDIO_VOLUME, 0, 100,
                                          "%d %%", nullptr);
    changed |= MenuWidgets::Checkbox("Mute", nullptr, Config::MAIN_AUDIO_MUTED, nullptr);
    if (changed)
      ApplyToSoundStream(context);

    MenuWidgets::SectionHeader("Playback");
    MenuWidgets::Checkbox("Fill Audio Gaps",
                          "Repeat existing audio during lag spikes to prevent stuttering.",
                          Config::MAIN_AUDIO_FILL_GAPS, nullptr);
  }

private:
  // The sound stream only picks up volume changes when told to, on the host thread. Dragging
  // the slider changes the config every frame: keep at most one update in flight; it reads
  // the latest value when it runs.
  void ApplyToSoundStream(VRMenuContext& context)
  {
    if (m_update_queued->exchange(true))
      return;
    context.actions.Run({}, [queued = m_update_queued](Core::System& system) {
      queued->store(false);
      AudioCommon::UpdateSoundStream(system);
      return std::string();
    });
  }

  std::shared_ptr<std::atomic<bool>> m_update_queued = std::make_shared<std::atomic<bool>>(false);
};
}  // namespace

std::unique_ptr<VRMenuTab> CreateAudioTab()
{
  return std::make_unique<AudioTab>();
}
}  // namespace VR
