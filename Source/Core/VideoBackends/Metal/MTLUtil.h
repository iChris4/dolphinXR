// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <TargetConditionals.h>

#if TARGET_OS_OSX
#include <AppKit/NSScreen.h>
#endif
#include <Metal/Metal.h>

#include <vector>

#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoConfig.h"

#include "VideoBackends/Metal/MRCHelpers.h"

namespace Metal
{
struct DeviceFeatures
{
  /// Manually copy buffer data to the GPU (instead of letting the GPU read from system memory)
  /// On discrete GPUs, this tends to be faster if the copy is able to operate in parallel with a
  /// previous render.  This is the case unless a game uses features like bbox or texture downloads.
  bool manual_buffer_upload;
  bool subgroup_ops;
};

extern DeviceFeatures g_features;

namespace Util
{
struct Viewport
{
  float x;
  float y;
  float width;
  float height;
  float near_depth;
  float far_depth;
};

/// Gets the list of Metal devices, ordered so the system default device is first
std::vector<MRCOwned<id<MTLDevice>>> GetAdapterList();
void PopulateBackendInfo(BackendInfo* backend_info);
void PopulateBackendInfoAdapters(BackendInfo* backend_info,
                                 const std::vector<MRCOwned<id<MTLDevice>>>& adapters);
void PopulateBackendInfoFeatures(const VideoConfig& config, BackendInfo* backend_info,
                                 id<MTLDevice> device);

AbstractTextureFormat ToAbstract(MTLPixelFormat format);
MTLPixelFormat FromAbstract(AbstractTextureFormat format);
static inline bool HasStencil(AbstractTextureFormat format)
{
  return format == AbstractTextureFormat::D24_S8 || format == AbstractTextureFormat::D32F_S8;
}

// Metal has no multiview. Instanced layered rendering stands in for it: a multiview shader draws
// one instance per view, SPIRV-Cross derives gl_ViewIndex from the instance index and writes it to
// [[render_target_array_index]]. It reads {first view, view count} from this buffer index, in the
// vertex and (when the fragment shader uses gl_ViewIndex) fragment stages.
constexpr u32 VIEW_MASK_BUFFER_INDEX = 24;

// Shaders whose GLSL enables GL_EXT_multiview are translated as multiview shaders;
// force_multiview does so for any vertex shader (a layered variant of a utility shader).
std::optional<std::string> TranslateShaderToMSL(ShaderStage stage, std::string_view source,
                                                VideoCommon::ShaderIncluder* shader_includer,
                                                bool force_multiview = false);

}  // namespace Util
}  // namespace Metal
