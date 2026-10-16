// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/VRVideoRecorder.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <utility>

#ifdef ANDROID
#include <algorithm>
#include <ctime>
#include <deque>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <android/native_window.h>
#include <fmt/format.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>

#include "AudioCommon/Mixer.h"
#include "AudioCommon/SoundStream.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/System.h"
#include "jni/AndroidCommon/AndroidCommon.h"
#endif

#include "VideoCommon/VR/OpenXRManager.h"

namespace VR
{
#ifdef ANDROID
namespace
{
// A 16:9 crop across the left eye's full width, scaled to 1080p, at the game's frame rate.
constexpr u32 VIDEO_WIDTH = 1920;
constexpr u32 VIDEO_HEIGHT = 1080;
constexpr int VIDEO_BIT_RATE = 16'000'000;
constexpr int VIDEO_FRAME_RATE = 60;
constexpr int AUDIO_BIT_RATE = 192'000;
// MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface: frames come from the input surface.
constexpr int COLOR_FORMAT_SURFACE = 0x7F000789;
constexpr int AAC_PROFILE_LC = 2;
// Game audio queued for the encoder at most; older audio is dropped if it falls behind.
constexpr u32 MAX_QUEUED_AUDIO_SECONDS = 2;
// Encoded samples kept while the MP4 waits for both tracks' formats.
constexpr std::size_t MAX_PENDING_SAMPLES = 1200;
// How long the encoder may take to drain once the input ended.
constexpr u64 FINISH_TIMEOUT_US = 3'000'000;

// The input surface and video end-of-stream calls are Android 8 (API 26) additions, above
// Dolphin's minimum, so they are looked up at run time.
struct MediaCodecApi26
{
  media_status_t (*create_input_surface)(AMediaCodec*, ANativeWindow**) = nullptr;
  media_status_t (*signal_end_of_input_stream)(AMediaCodec*) = nullptr;
};

const MediaCodecApi26* GetMediaCodecApi26()
{
  static const MediaCodecApi26 api = [] {
    MediaCodecApi26 result;
    if (void* library = dlopen("libmediandk.so", RTLD_NOW))
    {
      result.create_input_surface = reinterpret_cast<decltype(result.create_input_surface)>(
          dlsym(library, "AMediaCodec_createInputSurface"));
      result.signal_end_of_input_stream =
          reinterpret_cast<decltype(result.signal_end_of_input_stream)>(
              dlsym(library, "AMediaCodec_signalEndOfInputStream"));
    }
    return result;
  }();
  return api.create_input_surface && api.signal_end_of_input_stream ? &api : nullptr;
}

u64 MonotonicUs()
{
  // The encoder's input surface stamps frames with CLOCK_MONOTONIC; audio uses the same clock.
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<u64>(ts.tv_sec) * 1'000'000 + static_cast<u64>(ts.tv_nsec) / 1000;
}

MathUtil::Rectangle<int> CropToVideo(const MathUtil::Rectangle<int>& eye)
{
  int width = eye.GetWidth();
  int height = width * static_cast<int>(VIDEO_HEIGHT) / static_cast<int>(VIDEO_WIDTH);
  if (height > eye.GetHeight())
  {
    height = eye.GetHeight();
    width = height * static_cast<int>(VIDEO_WIDTH) / static_cast<int>(VIDEO_HEIGHT);
  }
  const int left = eye.left + (eye.GetWidth() - width) / 2;
  const int top = eye.top + (eye.GetHeight() - height) / 2;
  return {left, top, left + width, top + height};
}
}  // namespace
#endif

struct VideoRecorder::Impl
{
  mutable std::mutex mutex;
  std::atomic<State> state{State::Idle};
  std::optional<std::string> finished_message;
  std::thread thread;

#ifdef ANDROID
  u64 start_us = 0;
  std::string display_name;
  // Fallback output path when MediaStore is unavailable (Android 9 and older).
  std::string file_path;
  bool media_store = false;
  int fd = -1;
  AMediaCodec* video = nullptr;
  AMediaCodec* audio = nullptr;
  AMediaMuxer* muxer = nullptr;
  ANativeWindow* window = nullptr;

  // Video thread (under mutex): the swapchain the encoder's input surface is attached to.
  IOpenXRSwapchain* capture_swapchain = nullptr;
  std::string capture_error;
  // Set once nothing presents into the input surface anymore: the video stream can end.
  std::atomic<bool> video_input_done{false};

  // Game audio from the mixer, interleaved stereo.
  std::mutex audio_mutex;
  std::deque<s16> audio_queue;
  u32 audio_rate = 0;
  // Monotonic time of the first mixed frame, and frames since then (encoded or dropped).
  u64 audio_base_us = 0;
  u64 audio_frames = 0;

  bool OpenMedia(const MediaCodecApi26& api, u32 sample_rate, std::string* error);
  void ReleaseMedia(bool keep);
  void SetMixerCapture(bool enable);
  void PushAudio(const s16* samples, std::size_t num_frames, u32 sample_rate);
  void EncodeLoop();
  void DetachCapture();
#endif
};

VideoRecorder::VideoRecorder() : m_impl(std::make_unique<Impl>())
{
}

VideoRecorder::~VideoRecorder()
{
  StopForShutdown();
}

VideoRecorder& VideoRecorder::Get()
{
  static VideoRecorder s_recorder;
  return s_recorder;
}

bool VideoRecorder::IsSupported()
{
#ifdef ANDROID
  return GetMediaCodecApi26() != nullptr;
#else
  return false;
#endif
}

VideoRecorder::State VideoRecorder::GetState() const
{
  return m_impl->state.load(std::memory_order_acquire);
}

std::optional<std::string> VideoRecorder::TakeFinishedMessage()
{
  std::lock_guard lock(m_impl->mutex);
  return std::exchange(m_impl->finished_message, std::nullopt);
}

#ifdef ANDROID
std::string VideoRecorder::Start(const std::string& game_id)
{
  Impl& impl = *m_impl;
  std::lock_guard lock(impl.mutex);
  if (impl.state != State::Idle)
    return "Already recording";
  if (impl.thread.joinable())
    impl.thread.join();

  const MediaCodecApi26* api = GetMediaCodecApi26();
  if (!api)
    return "Recording needs Android 8 or newer";

  u32 sample_rate = 0;
  if (SoundStream* stream = Core::System::GetInstance().GetSoundStream();
      stream && stream->GetMixer())
  {
    sample_rate = stream->GetMixer()->GetSampleRate();
  }

  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  impl.display_name =
      fmt::format("{}_{:04}-{:02}-{:02}_{:02}-{:02}-{:02}.mp4", game_id, local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
  impl.fd = CreateVideoRecordingFile(impl.display_name);
  impl.media_store = impl.fd >= 0;
  impl.file_path.clear();
  if (!impl.media_store)
  {
    impl.file_path = File::GetUserPath(D_DUMPFRAMES_IDX) + impl.display_name;
    File::CreateFullPath(impl.file_path);
    impl.fd = open(impl.file_path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (impl.fd < 0)
      return "Could not create the video file";
  }

  std::string error;
  if (!impl.OpenMedia(*api, sample_rate, &error))
  {
    impl.ReleaseMedia(false);
    return error;
  }

  {
    std::lock_guard audio_lock(impl.audio_mutex);
    impl.audio_queue.clear();
    impl.audio_base_us = 0;
    impl.audio_frames = 0;
  }
  impl.capture_swapchain = nullptr;
  impl.capture_error.clear();
  impl.video_input_done = false;
  impl.start_us = MonotonicUs();
  impl.state = State::Recording;
  impl.thread = std::thread([&impl] { impl.EncodeLoop(); });
  if (impl.audio)
    impl.SetMixerCapture(true);

  INFO_LOG_FMT(VIDEO, "Video recording: started {} ({}x{}, audio {} Hz).", impl.display_name,
               VIDEO_WIDTH, VIDEO_HEIGHT, impl.audio_rate);
  return impl.media_store ? fmt::format("Recording to Movies/DolphinXR/{}", impl.display_name) :
                            fmt::format("Recording to Dump/Frames/{}", impl.display_name);
}

void VideoRecorder::RequestStop()
{
  Impl& impl = *m_impl;
  std::lock_guard lock(impl.mutex);
  if (impl.state != State::Recording)
    return;
  impl.state = State::Stopping;
  // Nothing attached yet: there is nothing for the video thread to detach.
  if (!impl.capture_swapchain)
    impl.video_input_done = true;
}

double VideoRecorder::GetElapsedSeconds() const
{
  if (GetState() == State::Idle)
    return 0.0;
  return static_cast<double>(MonotonicUs() - m_impl->start_us) / 1'000'000.0;
}

void VideoRecorder::OnEyeRendered(IOpenXRSwapchain& swapchain, AbstractTexture* eye_texture,
                                  const MathUtil::Rectangle<int>& eye_rect)
{
  Impl& impl = *m_impl;
  if (GetState() == State::Idle)
    return;

  std::lock_guard lock(impl.mutex);
  if (impl.state == State::Recording)
  {
    if (!impl.capture_swapchain)
    {
      if (!swapchain.StartVideoCapture(impl.window, VIDEO_WIDTH, VIDEO_HEIGHT))
      {
        impl.capture_error = "Recording failed: the left eye can only be captured with Vulkan";
        impl.state = State::Stopping;
        impl.video_input_done = true;
        return;
      }
      impl.capture_swapchain = &swapchain;
    }
    if (swapchain.CaptureVideoFrame(eye_texture, CropToVideo(eye_rect)))
      return;
    impl.capture_error = "Recording stopped: the encoder no longer accepts frames";
    impl.state = State::Stopping;
  }

  if (impl.state == State::Stopping)
    impl.DetachCapture();
}

void VideoRecorder::StopForShutdown()
{
  Impl& impl = *m_impl;
  {
    std::lock_guard lock(impl.mutex);
    if (impl.state != State::Idle)
    {
      impl.state = State::Stopping;
      impl.DetachCapture();
    }
  }
  if (impl.thread.joinable())
    impl.thread.join();
}

void VideoRecorder::Impl::DetachCapture()
{
  if (capture_swapchain)
  {
    capture_swapchain->StopVideoCapture();
    capture_swapchain = nullptr;
  }
  video_input_done = true;
}

bool VideoRecorder::Impl::OpenMedia(const MediaCodecApi26& api, u32 sample_rate,
                                    std::string* error)
{
  video = AMediaCodec_createEncoderByType("video/avc");
  if (!video)
  {
    *error = "Recording failed: no H.264 encoder";
    return false;
  }
  AMediaFormat* format = AMediaFormat_new();
  AMediaFormat_setString(format, "mime", "video/avc");
  AMediaFormat_setInt32(format, "width", static_cast<int>(VIDEO_WIDTH));
  AMediaFormat_setInt32(format, "height", static_cast<int>(VIDEO_HEIGHT));
  AMediaFormat_setInt32(format, "bitrate", VIDEO_BIT_RATE);
  AMediaFormat_setInt32(format, "frame-rate", VIDEO_FRAME_RATE);
  AMediaFormat_setInt32(format, "i-frame-interval", 1);
  AMediaFormat_setInt32(format, "color-format", COLOR_FORMAT_SURFACE);
  const media_status_t status =
      AMediaCodec_configure(video, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
  AMediaFormat_delete(format);
  if (status != AMEDIA_OK || api.create_input_surface(video, &window) != AMEDIA_OK ||
      AMediaCodec_start(video) != AMEDIA_OK)
  {
    *error = "Recording failed: could not start the H.264 encoder";
    return false;
  }

  // Game audio when an audio backend runs; the video is still recorded without it.
  audio_rate = 0;
  if (sample_rate != 0)
  {
    audio = AMediaCodec_createEncoderByType("audio/mp4a-latm");
    if (audio)
    {
      format = AMediaFormat_new();
      AMediaFormat_setString(format, "mime", "audio/mp4a-latm");
      AMediaFormat_setInt32(format, "sample-rate", static_cast<int>(sample_rate));
      AMediaFormat_setInt32(format, "channel-count", 2);
      AMediaFormat_setInt32(format, "bitrate", AUDIO_BIT_RATE);
      AMediaFormat_setInt32(format, "aac-profile", AAC_PROFILE_LC);
      AMediaFormat_setInt32(format, "max-input-size", 16384);
      const bool started = AMediaCodec_configure(audio, format, nullptr, nullptr,
                                                 AMEDIACODEC_CONFIGURE_FLAG_ENCODE) == AMEDIA_OK &&
                           AMediaCodec_start(audio) == AMEDIA_OK;
      AMediaFormat_delete(format);
      if (started)
      {
        audio_rate = sample_rate;
      }
      else
      {
        WARN_LOG_FMT(VIDEO, "Video recording: no AAC encoder for {} Hz, recording without audio.",
                     sample_rate);
        AMediaCodec_delete(audio);
        audio = nullptr;
      }
    }
  }

  muxer = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
  if (!muxer)
  {
    *error = "Recording failed: could not create the MP4 file";
    return false;
  }
  return true;
}

void VideoRecorder::Impl::ReleaseMedia(bool keep)
{
  if (muxer)
    AMediaMuxer_delete(muxer);
  muxer = nullptr;
  for (AMediaCodec** codec : {&video, &audio})
  {
    if (*codec)
    {
      AMediaCodec_stop(*codec);
      AMediaCodec_delete(*codec);
    }
    *codec = nullptr;
  }
  if (window)
    ANativeWindow_release(window);
  window = nullptr;
  if (fd >= 0)
    close(fd);
  fd = -1;
  if (media_store)
    FinishVideoRecordingFile(keep);
  else if (!keep && !file_path.empty())
    File::Delete(file_path);
  media_store = false;
}

void VideoRecorder::Impl::SetMixerCapture(bool enable)
{
  SoundStream* stream = Core::System::GetInstance().GetSoundStream();
  if (!stream || !stream->GetMixer())
    return;
  if (enable)
  {
    stream->GetMixer()->SetCaptureCallback(
        [this](const s16* samples, std::size_t num_frames, u32 sample_rate) {
          PushAudio(samples, num_frames, sample_rate);
        });
  }
  else
  {
    stream->GetMixer()->SetCaptureCallback({});
  }
}

void VideoRecorder::Impl::PushAudio(const s16* samples, std::size_t num_frames, u32 sample_rate)
{
  if (video_input_done.load(std::memory_order_acquire) || sample_rate != audio_rate)
    return;

  std::lock_guard lock(audio_mutex);
  if (audio_base_us == 0)
    audio_base_us = MonotonicUs();
  audio_queue.insert(audio_queue.end(), samples, samples + num_frames * 2);
  const std::size_t max_samples = std::size_t{audio_rate} * MAX_QUEUED_AUDIO_SECONDS * 2;
  if (audio_queue.size() > max_samples)
  {
    // A gap in the audio rather than a shift: the timeline still advances over dropped frames.
    const std::size_t drop = (audio_queue.size() - max_samples) & ~std::size_t{1};
    audio_queue.erase(audio_queue.begin(), audio_queue.begin() + drop);
    audio_frames += drop / 2;
  }
}

void VideoRecorder::Impl::EncodeLoop()
{
  Common::SetCurrentThreadName("VR video encoder");
  const MediaCodecApi26& api = *GetMediaCodecApi26();

  struct PendingSample
  {
    bool is_video;
    std::vector<u8> data;
    AMediaCodecBufferInfo info;
  };
  std::vector<PendingSample> pending;
  int video_track = -1;
  int audio_track = -1;
  bool muxer_started = false;
  bool wrote_video = false;
  bool video_eos = false;
  bool audio_eos = audio == nullptr;
  bool video_eos_signaled = false;
  bool audio_eos_queued = false;
  ssize_t audio_input = -1;
  u64 finish_deadline_us = 0;

  const auto write = [&](bool is_video, const u8* data, const AMediaCodecBufferInfo& info) {
    if (muxer_started)
    {
      AMediaMuxer_writeSampleData(muxer, is_video ? video_track : audio_track, data, &info);
      wrote_video |= is_video;
    }
    else if (pending.size() < MAX_PENDING_SAMPLES)
    {
      pending.push_back({is_video, std::vector<u8>(data, data + info.offset + info.size), info});
    }
  };

  const auto drain = [&](AMediaCodec* codec, bool is_video, int64_t timeout_us) {
    while (true)
    {
      AMediaCodecBufferInfo info;
      const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, timeout_us);
      timeout_us = 0;
      if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
        return;
      if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
      {
        int& track = is_video ? video_track : audio_track;
        if (track < 0 && !muxer_started)
        {
          AMediaFormat* format = AMediaCodec_getOutputFormat(codec);
          track = static_cast<int>(AMediaMuxer_addTrack(muxer, format));
          AMediaFormat_delete(format);
        }
        if (!muxer_started && video_track >= 0 && (audio_track >= 0 || !audio) &&
            AMediaMuxer_start(muxer) == AMEDIA_OK)
        {
          muxer_started = true;
          for (const PendingSample& sample : pending)
            write(sample.is_video, sample.data.data(), sample.info);
          pending.clear();
        }
        continue;
      }
      if (index < 0)
        continue;

      std::size_t size = 0;
      const u8* data = AMediaCodec_getOutputBuffer(codec, static_cast<std::size_t>(index), &size);
      const bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
      if (data && info.size > 0 && (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0)
      {
        // The file's timeline starts at the Record press.
        info.presentationTimeUs -= static_cast<int64_t>(start_us);
        info.flags &= ~AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
        if (info.presentationTimeUs >= 0)
          write(is_video, data, info);
      }
      AMediaCodec_releaseOutputBuffer(codec, static_cast<std::size_t>(index), false);
      if (eos)
      {
        (is_video ? video_eos : audio_eos) = true;
        return;
      }
    }
  };

  const auto feed_audio = [&] {
    while (audio && !audio_eos_queued)
    {
      if (audio_input < 0)
        audio_input = AMediaCodec_dequeueInputBuffer(audio, 0);
      if (audio_input < 0)
        return;
      std::size_t capacity = 0;
      u8* buffer = AMediaCodec_getInputBuffer(audio, static_cast<std::size_t>(audio_input),
                                              &capacity);
      std::unique_lock lock(audio_mutex);
      const std::size_t frames = std::min(audio_queue.size() / 2, capacity / 4);
      const u64 pts = audio_base_us + audio_frames * 1'000'000 / audio_rate;
      if (frames == 0)
      {
        lock.unlock();
        if (!video_input_done.load(std::memory_order_acquire))
          return;
        AMediaCodec_queueInputBuffer(audio, static_cast<std::size_t>(audio_input), 0, 0, pts,
                                     AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
        audio_eos_queued = true;
        audio_input = -1;
        return;
      }
      std::copy_n(audio_queue.begin(), frames * 2, reinterpret_cast<s16*>(buffer));
      audio_queue.erase(audio_queue.begin(), audio_queue.begin() + frames * 2);
      audio_frames += frames;
      lock.unlock();
      AMediaCodec_queueInputBuffer(audio, static_cast<std::size_t>(audio_input), 0, frames * 4,
                                   pts, 0);
      audio_input = -1;
    }
  };

  while (!(video_eos && audio_eos))
  {
    feed_audio();
    if (video_input_done.load(std::memory_order_acquire) && !video_eos_signaled)
    {
      api.signal_end_of_input_stream(video);
      video_eos_signaled = true;
      finish_deadline_us = MonotonicUs() + FINISH_TIMEOUT_US;
    }
    drain(video, true, 10'000);
    if (audio)
      drain(audio, false, 0);
    if (finish_deadline_us != 0 && MonotonicUs() > finish_deadline_us)
    {
      WARN_LOG_FMT(VIDEO, "Video recording: the encoder did not finish in time.");
      break;
    }
  }

  SetMixerCapture(false);
  const bool kept = muxer_started && wrote_video && AMediaMuxer_stop(muxer) == AMEDIA_OK;
  const bool to_media_store = media_store;
  ReleaseMedia(kept);

  std::string message;
  if (kept)
  {
    message = to_media_store ? fmt::format("Saved to Movies/DolphinXR/{}", display_name) :
                               fmt::format("Saved to Dump/Frames/{}", display_name);
  }
  INFO_LOG_FMT(VIDEO, "Video recording: finished {} (kept={}).", display_name, kept);

  std::lock_guard lock(mutex);
  if (!kept)
    message = capture_error.empty() ? "Recording failed: no video was encoded" : capture_error;
  finished_message = std::move(message);
  state = State::Idle;
}

#else

std::string VideoRecorder::Start(const std::string&)
{
  return "Recording is only available on Android";
}

void VideoRecorder::RequestStop()
{
}

double VideoRecorder::GetElapsedSeconds() const
{
  return 0.0;
}

void VideoRecorder::OnEyeRendered(IOpenXRSwapchain&, AbstractTexture*,
                                  const MathUtil::Rectangle<int>&)
{
}

void VideoRecorder::StopForShutdown()
{
  if (m_impl->thread.joinable())
    m_impl->thread.join();
}

#endif
}  // namespace VR
