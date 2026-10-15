// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <compare>
#include <map>
#include <memory>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/TextureConfig.h"

class AbstractPipeline;
class AbstractShader;
class AbstractTexture;
class NativeVertexFormat;
struct ImDrawData;
struct ImTextureData;

namespace VideoCommon
{
// Renders Dear ImGui draw data through AbstractGfx, so it works on every video backend.
// One instance per ImGui context: it owns that context's textures (the font atlas).
// Pipelines are created lazily per render-target description.
class ImGuiRenderer
{
public:
  struct TargetDesc
  {
    AbstractTextureFormat format = AbstractTextureFormat::Undefined;
    // Write premultiplied alpha (alpha = src + dst * (1 - src)) for targets that are
    // composited later, like OpenXR quad layers. The desktop OSD keeps the backbuffer's alpha.
    bool write_alpha = false;
    // Duplicate the UI to both layers of a stereo target (AbstractGfx::UseGeometryShaderForUI).
    bool use_geometry_shader = false;

    auto operator<=>(const TargetDesc&) const = default;
  };

  ImGuiRenderer();
  ~ImGuiRenderer();

  ImGuiRenderer(const ImGuiRenderer&) = delete;
  ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;

  bool Initialize();

  // Drops the cached pipelines, e.g. when the stereo mode changes.
  void InvalidatePipelines();

  // Creates, updates and destroys the textures the draw data asks for (ImGui 1.92
  // ImGuiBackendFlags_RendererHasTextures protocol). Call after ImGui::Render().
  void UpdateTextures(ImDrawData* draw_data);

  // Draws into the currently bound framebuffer, which must be `width` x `height` and match
  // `target`. Leaves the scissor covering the whole target.
  void RenderDrawData(ImDrawData* draw_data, u32 width, u32 height, const TargetDesc& target);

private:
  const AbstractPipeline* GetPipeline(const TargetDesc& target);
  void UpdateTexture(ImTextureData* tex);

  std::unique_ptr<NativeVertexFormat> m_vertex_format;
  std::unique_ptr<AbstractShader> m_vertex_shader;
  std::unique_ptr<AbstractShader> m_pixel_shader;
  std::unique_ptr<AbstractShader> m_pixel_shader_linear;
  std::unique_ptr<AbstractShader> m_geometry_shader;
  std::map<TargetDesc, std::unique_ptr<AbstractPipeline>> m_pipelines;
  std::vector<std::unique_ptr<AbstractTexture>> m_textures;
};
}  // namespace VideoCommon
