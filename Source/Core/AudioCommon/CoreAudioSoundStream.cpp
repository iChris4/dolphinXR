// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AudioCommon/CoreAudioSoundStream.h"

#include <algorithm>
#include <atomic>

#include "Common/Logging/Log.h"

OSStatus CoreAudioSound::OutputCallback(void* ref_con, AudioUnitRenderActionFlags* action_flags,
                                        const AudioTimeStamp* timestamp, UInt32 bus_number,
                                        UInt32 number_frames, AudioBufferList* io_data)
{
  auto* const sound = static_cast<CoreAudioSound*>(ref_con);
  if (!sound || !io_data || io_data->mNumberBuffers == 0)
    return noErr;

  // The client format below is interleaved stereo S16, so RemoteIO supplies a
  // single buffer. Mix exactly the hardware-requested frame count instead of
  // pulling audio in large queued bursts.
  AudioBuffer& buffer = io_data->mBuffers[0];
  const UInt32 available_frames = buffer.mDataByteSize / (2 * sizeof(s16));
  const UInt32 frames = std::min(number_frames, available_frames);
  if (buffer.mData && frames > 0)
    sound->GetMixer()->Mix(static_cast<s16*>(buffer.mData), frames);

  static std::atomic<bool> logged_callback = false;
  if (!logged_callback.exchange(true))
  {
    INFO_LOG_FMT(AUDIO, "CoreAudio: RemoteIO callback: {} frames, {} buffers", number_frames,
                 io_data->mNumberBuffers);
  }
  return noErr;
}

CoreAudioSound::~CoreAudioSound()
{
  if (!m_audio_unit)
    return;
  AudioOutputUnitStop(m_audio_unit);
  AudioUnitUninitialize(m_audio_unit);
  AudioComponentInstanceDispose(m_audio_unit);
}

bool CoreAudioSound::Init()
{
  AudioComponentDescription description{};
  description.componentType = kAudioUnitType_Output;
  description.componentSubType = kAudioUnitSubType_RemoteIO;
  description.componentManufacturer = kAudioUnitManufacturer_Apple;

  AudioComponent component = AudioComponentFindNext(nullptr, &description);
  if (!component || AudioComponentInstanceNew(component, &m_audio_unit) != noErr)
  {
    ERROR_LOG_FMT(AUDIO, "Could not create the RemoteIO audio component");
    return false;
  }

  AudioStreamBasicDescription format{};
  FillOutASBDForLPCM(format, GetMixer()->GetSampleRate(), 2, 16, 16, false, false, false);
  if (AudioUnitSetProperty(m_audio_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                           &format, sizeof(format)) != noErr)
  {
    ERROR_LOG_FMT(AUDIO, "Could not set the RemoteIO stereo PCM format");
    return false;
  }

  AURenderCallbackStruct callback{};
  callback.inputProc = OutputCallback;
  callback.inputProcRefCon = this;
  if (AudioUnitSetProperty(m_audio_unit, kAudioUnitProperty_SetRenderCallback,
                           kAudioUnitScope_Input, 0, &callback, sizeof(callback)) != noErr)
  {
    ERROR_LOG_FMT(AUDIO, "Could not set the RemoteIO render callback");
    return false;
  }

  AudioUnitSetParameter(m_audio_unit, kHALOutputParam_Volume, kAudioUnitScope_Output, 0,
                        m_volume / 100.0f, 0);
  if (AudioUnitInitialize(m_audio_unit) != noErr)
  {
    ERROR_LOG_FMT(AUDIO, "Could not initialize the RemoteIO audio unit");
    return false;
  }

  INFO_LOG_FMT(AUDIO, "CoreAudio: RemoteIO initialized at {:.0f} Hz", format.mSampleRate);
  return true;
}

bool CoreAudioSound::SetRunning(bool running)
{
  if (!m_audio_unit || m_running == running)
    return m_audio_unit != nullptr;
  const OSStatus status = running ? AudioOutputUnitStart(m_audio_unit) :
                                    AudioOutputUnitStop(m_audio_unit);
  if (status == noErr)
    m_running = running;
  return status == noErr;
}

void CoreAudioSound::SetVolume(int volume)
{
  m_volume = std::clamp(volume, 0, 100);
  if (m_audio_unit)
    AudioUnitSetParameter(m_audio_unit, kHALOutputParam_Volume, kAudioUnitScope_Output, 0,
                          m_volume / 100.0f, 0);
}
