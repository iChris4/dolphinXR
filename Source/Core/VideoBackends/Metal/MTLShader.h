// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <Metal/Metal.h>

#include <mutex>
#include <string>

#include "VideoBackends/Metal/MRCHelpers.h"

#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"

namespace Metal
{
class Shader : public AbstractShader
{
public:
  // glsl: the source the MSL was translated from (vertex shaders only, empty otherwise), kept so a
  // multiview variant can be translated on demand.
  explicit Shader(ShaderStage stage, std::string msl, MRCOwned<id<MTLFunction>> shader,
                  std::string glsl = {});
  ~Shader();

  id<MTLFunction> GetShader() const { return m_shader; }
  const std::string& GetMSL() const { return m_msl; }
  BinaryData GetBinary() const override;

  // Whether the MSL is a multiview shader (it reads the view mask buffer).
  bool IsMultiview() const { return m_multiview; }
  // The function for a pipeline that targets a multiview (layered) framebuffer: this shader if it
  // is a multiview shader already, otherwise a multiview variant translated from the GLSL on first
  // use, so every draw of the pipeline reaches both eye layers. nil if there is no GLSL or the
  // variant failed to compile.
  id<MTLFunction> GetMultiviewShader() const;

private:
  std::string m_msl;
  MRCOwned<id<MTLFunction>> m_shader;
  std::string m_glsl;
  bool m_multiview = false;

  mutable std::mutex m_multiview_mutex;
  mutable bool m_multiview_variant_tried = false;
  mutable MRCOwned<id<MTLFunction>> m_multiview_variant;
};
}  // namespace Metal
