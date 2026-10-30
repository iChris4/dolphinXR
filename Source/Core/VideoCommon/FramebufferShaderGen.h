// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include "Common/CommonTypes.h"

enum class EFBReinterpretType;
enum class TextureFormat;

namespace FramebufferShaderGen
{
std::string GenerateScreenQuadVertexShader();
std::string GeneratePassthroughGeometryShader(u32 num_tex, u32 num_colors);
std::string GenerateTextureCopyVertexShader();
std::string GenerateTextureCopyPixelShader();
std::string GenerateResolveColorPixelShader(u32 samples);
std::string GenerateResolveDepthPixelShader(u32 samples);
// Copies the first sample of a multisampled texture: for values that must not be averaged.
std::string GenerateFirstSamplePixelShader();
std::string GenerateClearVertexShader();
std::string GenerateEFBPokeVertexShader();
std::string GenerateColorPixelShader();
std::string GenerateFormatConversionShader(EFBReinterpretType convtype, u32 samples,
                                           bool multiview = false);
std::string GenerateTextureReinterpretShader(TextureFormat from_format, TextureFormat to_format);
std::string GenerateEFBRestorePixelShader();
std::string GenerateEFBCoverageRestorePixelShader(bool multiview);
std::string GenerateImGuiVertexShader();
std::string GenerateImGuiPixelShader(bool linear_space_output = false);

}  // namespace FramebufferShaderGen
