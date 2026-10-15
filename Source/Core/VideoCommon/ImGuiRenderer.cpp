// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/ImGuiRenderer.h"

#include <algorithm>
#include <cstddef>

#include <imgui.h>

#include "Common/MathUtil.h"
#include "Common/MsgHandler.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/FramebufferShaderGen.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VertexManagerBase.h"

namespace VideoCommon
{
ImGuiRenderer::ImGuiRenderer() = default;
ImGuiRenderer::~ImGuiRenderer() = default;

bool ImGuiRenderer::Initialize()
{
  PortableVertexDeclaration vdecl = {};
  vdecl.position = {ComponentFormat::Float, 2, offsetof(ImDrawVert, pos), true, false};
  vdecl.texcoords[0] = {ComponentFormat::Float, 2, offsetof(ImDrawVert, uv), true, false};
  vdecl.colors[0] = {ComponentFormat::UByte, 4, offsetof(ImDrawVert, col), true, false};
  vdecl.stride = sizeof(ImDrawVert);
  m_vertex_format = g_gfx->CreateNativeVertexFormat(vdecl);
  if (!m_vertex_format)
  {
    PanicAlertFmt("Failed to create ImGui vertex format");
    return false;
  }
  return true;
}

void ImGuiRenderer::InvalidatePipelines()
{
  m_pipelines.clear();
  m_geometry_shader.reset();
}

const AbstractPipeline* ImGuiRenderer::GetPipeline(const TargetDesc& target)
{
  if (target.format == AbstractTextureFormat::Undefined)
    return nullptr;

  if (const auto it = m_pipelines.find(target); it != m_pipelines.end())
    return it->second.get();

  const bool linear_space_output = target.format == AbstractTextureFormat::RGBA16F;
  std::unique_ptr<AbstractShader>& pixel_shader =
      linear_space_output ? m_pixel_shader_linear : m_pixel_shader;
  if (!m_vertex_shader)
  {
    m_vertex_shader = g_gfx->CreateShaderFromSource(
        ShaderStage::Vertex, FramebufferShaderGen::GenerateImGuiVertexShader(), nullptr,
        "ImGui vertex shader");
  }
  if (!pixel_shader)
  {
    pixel_shader = g_gfx->CreateShaderFromSource(
        ShaderStage::Pixel, FramebufferShaderGen::GenerateImGuiPixelShader(linear_space_output),
        nullptr, "ImGui pixel shader");
  }
  if (target.use_geometry_shader && !m_geometry_shader)
  {
    m_geometry_shader = g_gfx->CreateShaderFromSource(
        ShaderStage::Geometry, FramebufferShaderGen::GeneratePassthroughGeometryShader(1, 1),
        nullptr, "ImGui passthrough geometry shader");
  }
  if (!m_vertex_shader || !pixel_shader || (target.use_geometry_shader && !m_geometry_shader))
  {
    PanicAlertFmt("Failed to compile ImGui shaders");
    // Remember the failure so a broken target does not recompile every frame.
    m_pipelines.emplace(target, nullptr);
    return nullptr;
  }

  AbstractPipelineConfig pconfig = {};
  pconfig.vertex_format = m_vertex_format.get();
  pconfig.vertex_shader = m_vertex_shader.get();
  pconfig.geometry_shader = target.use_geometry_shader ? m_geometry_shader.get() : nullptr;
  pconfig.pixel_shader = pixel_shader.get();
  pconfig.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
  pconfig.depth_state = RenderState::GetNoDepthTestingDepthState();
  pconfig.blending_state = RenderState::GetNoBlendingBlendState();
  pconfig.blending_state.blend_enable = true;
  pconfig.blending_state.src_factor = SrcBlendFactor::SrcAlpha;
  pconfig.blending_state.dst_factor = DstBlendFactor::InvSrcAlpha;
  if (target.write_alpha)
  {
    pconfig.blending_state.src_factor_alpha = SrcBlendFactor::One;
    pconfig.blending_state.dst_factor_alpha = DstBlendFactor::InvSrcAlpha;
  }
  else
  {
    pconfig.blending_state.src_factor_alpha = SrcBlendFactor::Zero;
    pconfig.blending_state.dst_factor_alpha = DstBlendFactor::One;
  }
  pconfig.framebuffer_state.color_texture_format = target.format;
  pconfig.framebuffer_state.depth_texture_format = AbstractTextureFormat::Undefined;
  pconfig.framebuffer_state.samples = 1;
  pconfig.framebuffer_state.per_sample_shading = false;
  pconfig.usage = AbstractPipelineUsage::Utility;
  std::unique_ptr<AbstractPipeline> pipeline = g_gfx->CreatePipeline(pconfig);
  if (!pipeline)
    PanicAlertFmt("Failed to create imgui pipeline");

  const AbstractPipeline* result = pipeline.get();
  m_pipelines.emplace(target, std::move(pipeline));
  return result;
}

void ImGuiRenderer::RenderDrawData(ImDrawData* draw_data, u32 width, u32 height,
                                   const TargetDesc& target)
{
  if (!draw_data || width == 0 || height == 0)
    return;
  const AbstractPipeline* pipeline = GetPipeline(target);
  if (!pipeline)
    return;

  g_gfx->SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f,
                     1.0f);

  // Uniform buffer for draws.
  struct ImGuiUbo
  {
    float u_rcp_viewport_size_mul2[2];
    float padding[2];
  };
  ImGuiUbo ubo = {{1.0f / width * 2.0f, 1.0f / height * 2.0f}};

  // Set up common state for drawing.
  g_gfx->SetPipeline(pipeline);
  g_gfx->SetSamplerState(0, RenderState::GetPointSamplerState());
  g_vertex_manager->UploadUtilityUniforms(&ubo, sizeof(ubo));

  for (int i = 0; i < draw_data->CmdListsCount; i++)
  {
    const ImDrawList* cmdlist = draw_data->CmdLists[i];
    if (cmdlist->VtxBuffer.empty() || cmdlist->IdxBuffer.empty())
      continue;

    u32 base_vertex, base_index;
    g_vertex_manager->UploadUtilityVertices(cmdlist->VtxBuffer.Data, sizeof(ImDrawVert),
                                            cmdlist->VtxBuffer.Size, cmdlist->IdxBuffer.Data,
                                            cmdlist->IdxBuffer.Size, &base_vertex, &base_index);

    for (const ImDrawCmd& cmd : cmdlist->CmdBuffer)
    {
      if (cmd.UserCallback)
      {
        cmd.UserCallback(cmdlist, &cmd);
        continue;
      }

      g_gfx->SetScissorRect(g_gfx->ConvertFramebufferRectangle(
          MathUtil::Rectangle<int>(
              static_cast<int>(cmd.ClipRect.x), static_cast<int>(cmd.ClipRect.y),
              static_cast<int>(cmd.ClipRect.z), static_cast<int>(cmd.ClipRect.w)),
          g_gfx->GetCurrentFramebuffer()));
      g_gfx->SetTexture(0, reinterpret_cast<const AbstractTexture*>(cmd.GetTexID()));
      // ImGuiBackendFlags_RendererHasVtxOffset: large lists split into 16-bit index windows.
      g_gfx->DrawIndexed(base_index + cmd.IdxOffset, cmd.ElemCount, base_vertex + cmd.VtxOffset);
    }
  }

  // Some capture software (such as OBS) hooks SwapBuffers and uses glBlitFramebuffer to copy our
  // back buffer just before swap. Because glBlitFramebuffer honors the scissor test, the capture
  // itself will be clipped to whatever bounds were last set by ImGui, resulting in a rather useless
  // capture whenever any ImGui windows are open. We'll reset the scissor rectangle to the entire
  // viewport here to avoid this problem.
  g_gfx->SetScissorRect(g_gfx->ConvertFramebufferRectangle(
      MathUtil::Rectangle<int>(0, 0, width, height), g_gfx->GetCurrentFramebuffer()));
}

void ImGuiRenderer::UpdateTextures(ImDrawData* draw_data)
{
  if (!draw_data || draw_data->Textures == nullptr)
    return;
  for (ImTextureData* tex : *draw_data->Textures)
  {
    if (tex->Status != ImTextureStatus_OK)
      UpdateTexture(tex);
  }
}

void ImGuiRenderer::UpdateTexture(ImTextureData* tex)
{
  if (tex->Status == ImTextureStatus_WantCreate)
  {
    // Create new font texture.
    IM_ASSERT(tex->TexID == ImTextureID_Invalid);
    IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);

    TextureConfig font_tex_config(tex->Width, tex->Height, 1, 1, 1, AbstractTextureFormat::RGBA8, 0,
                                  AbstractTextureType::Texture_2DArray);
    std::unique_ptr<AbstractTexture> font_tex =
        g_gfx->CreateTexture(font_tex_config, "ImGui font texture");

    if (!font_tex)
    {
      PanicAlertFmt("Failed to create ImGui texture");
      return;
    }

    font_tex->Load(0, tex->Width, tex->Height, tex->Width, tex->Pixels,
                   sizeof(u32) * tex->Width * tex->Height);

    tex->SetTexID(static_cast<ImTextureID>(*font_tex.get()));
    // Keeps the texture alive.
    m_textures.push_back(std::move(font_tex));
    tex->SetStatus(ImTextureStatus_OK);
  }
  else if (tex->Status == ImTextureStatus_WantUpdates)
  {
    AbstractTexture* font_tex = reinterpret_cast<AbstractTexture*>(tex->GetTexID());

    if (!font_tex || tex->TexID == ImTextureID_Invalid)
    {
      PanicAlertFmt("ImGui texture not created before update");
      return;
    }

    for (const ImTextureRect& r : tex->Updates)
    {
      // Rect of texture that will be updated.
      const int x_offset = static_cast<int>(r.x);
      const int y_offset = static_cast<int>(r.y);
      const int width = static_cast<int>(r.w);
      const int height = static_cast<int>(r.h);

      // Create a staging texture to update the font texture with.
      TextureConfig font_tex_config(width, height, 1, 1, 1, AbstractTextureFormat::RGBA8, 0,
                                    AbstractTextureType::Texture_2DArray);
      std::unique_ptr<AbstractStagingTexture> stage =
          g_gfx->CreateStagingTexture(StagingTextureType::Upload, font_tex_config);

      const int src_pitch = width * tex->BytesPerPixel;

      // Write to staging texture.
      for (int y = 0; y < height; y++)
      {
        const MathUtil::Rectangle<int> rect_line = {0, y, width, y + 1};
        stage->WriteTexels(rect_line, tex->GetPixelsAt(x_offset, y_offset + y), src_pitch);
      }

      // Copy to font texture.
      const MathUtil::Rectangle<int> rect_staging = {0, 0, width, height};
      const MathUtil::Rectangle<int> rect_target = {x_offset, y_offset, width + x_offset,
                                                    height + y_offset};

      stage->CopyToTexture(rect_staging, font_tex, rect_target, 0, 0);
    }

    tex->SetStatus(ImTextureStatus_OK);
  }
  else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0)
  {
    AbstractTexture* font_tex = reinterpret_cast<AbstractTexture*>(tex->GetTexID());

    tex->SetTexID(ImTextureID_Invalid);

    std::erase_if(m_textures, [font_tex](auto& element) { return element.get() == font_tex; });

    tex->Status = ImTextureStatus_Destroyed;
  }
}
}  // namespace VideoCommon
