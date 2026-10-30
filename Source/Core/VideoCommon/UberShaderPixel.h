// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include "Common/CommonTypes.h"
#include "VideoCommon/ShaderGenCommon.h"

enum class APIType;

namespace UberShader
{
#pragma pack(1)
struct pixel_ubershader_uid_data
{
  u32 num_texgens : 4;
  u32 early_depth : 1;
  u32 per_pixel_depth : 1;
  u32 uint_output : 1;
  u32 no_dual_src : 1;
  VRPassthroughCoverageShaderMode vr_coverage_mode : 2;
  // Exact virtual-screen depth: export the game's flat-screen depth from the flat-interpolated
  // vr_depth varying (see PixelShaderGen). Set per draw by VertexManagerBase, never by
  // GetPixelShaderUid.
  u32 vr_screen_exact_depth : 1;
  // Draw-ID pass (VR::AgentCapture): write the draw's index (cvr_draw_id) in place of the colour
  // and drop the fragments whose source-alpha blending would show nothing. Set per draw through
  // ShaderCache::GetDrawIdUberPipelineForUid, never by GetPixelShaderUid or the enumeration.
  u32 vr_draw_id : 1;
  // Bump when PS code generation logic changes to invalidate stale shader cache entries.
  u32 code_version : 4;

  u32 NumValues() const { return sizeof(pixel_ubershader_uid_data); }
};
#pragma pack()

using PixelShaderUid = ShaderUid<pixel_ubershader_uid_data>;

PixelShaderUid GetPixelShaderUid();

ShaderCode GenPixelShader(APIType api_type, const ShaderHostConfig& host_config,
                          const pixel_ubershader_uid_data* uid_data);

void EnumeratePixelShaderUids(const std::function<void(const PixelShaderUid&)>& callback);
void ClearUnusedPixelShaderUidBits(APIType api_type, const ShaderHostConfig& host_config,
                                   PixelShaderUid* uid);
}  // namespace UberShader

template <>
struct fmt::formatter<UberShader::pixel_ubershader_uid_data>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const UberShader::pixel_ubershader_uid_data& uid, FormatContext& ctx) const
  {
    return fmt::format_to(
        ctx.out(), "Pixel UberShader ver {} for {} texgens{}{}{}{}", uid.code_version,
        uid.num_texgens,
        uid.early_depth ? ", early-depth" : "", uid.per_pixel_depth ? ", per-pixel depth" : "",
        uid.uint_output ? ", uint output" : "", uid.no_dual_src ? ", no dual-source blending" : "");
  }
};
