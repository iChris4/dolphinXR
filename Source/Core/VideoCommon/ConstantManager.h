// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "Common/CommonTypes.h"

// all constant buffer attributes must be 16 bytes aligned, so this are the only allowed components:
using float4 = std::array<float, 4>;
using uint4 = std::array<u32, 4>;
using int4 = std::array<s32, 4>;

enum class DstBlendFactor : u32;
enum class SrcBlendFactor : u32;
enum class ZTexOp : u32;
enum class LogicOp : u32;

struct alignas(16) PixelShaderConstants
{
  std::array<int4, 4> colors;
  std::array<int4, 4> kcolors;
  int4 alpha;
  std::array<uint4, 8> texdims;
  std::array<int4, 2> zbias;
  std::array<int4, 2> indtexscale;
  std::array<int4, 6> indtexmtx;
  int4 fogcolor;
  int4 fogi;
  float4 fogf;
  std::array<float4, 3> fogrange;
  float4 zslope;
  std::array<float, 2> efbscale;  // .xy

  // Constants from here onwards are only used in ubershaders, other than pack2.
  u32 genmode;                  // .z
  u32 alphaTest;                // .w
  u32 fogParam3;                // .x
  u32 fogRangeBase;             // .y
  u32 dstalpha;                 // .z
  ZTexOp ztex_op;               // .w
  u32 late_ztest;               // .x (bool)
  u32 rgba6_format;             // .y (bool)
  u32 dither;                   // .z (bool)
  u32 bounding_box;             // .w (bool)
  std::array<uint4, 16> pack1;  // .xy - combiners, .z - tevind, .w - iref
  std::array<uint4, 8> pack2;   // .x - tevorder, .y - tevksel, .z/.w - SamplerState tm0/tm1
  std::array<int4, 32> konst;   // .rgba
  // The following are used in ubershaders when using shader_framebuffer_fetch blending
  u32 blend_enable;
  SrcBlendFactor blend_src_factor;
  SrcBlendFactor blend_src_factor_alpha;
  DstBlendFactor blend_dst_factor;
  DstBlendFactor blend_dst_factor_alpha;
  u32 blend_subtract;
  u32 blend_subtract_alpha;
  // For shader_framebuffer_fetch logic ops:
  u32 logic_op_enable;  // bool
  LogicOp logic_op_mode;
  // For custom shaders...
  u32 time_ms;
  // Dedicated VR passthrough coverage written by game draws. 1.0 marks pixels as covered;
  // Passthrough element overrides replace it with their configured opacity.
  float vr_passthrough_alpha;
  // Exact virtual-screen depth export: the host viewport depth range exactly as programmed by
  // BPFunctions::SetScissorAndViewport (near, far - near) in backend window-z convention. The
  // PS reproduces the fixed-function viewport transform: depth = near + vr_depth * range.
  float vr_screen_depth_near;
  float vr_screen_depth_range;
};

struct alignas(16) VertexShaderConstants
{
  u32 components;           // .x
  u32 xfmem_dualTexInfo;    // .y
  u32 xfmem_numColorChans;  // .z
  u32 missing_color_hex;    // .w, used for change detection but not directly by shaders
  float4 missing_color_value;

  std::array<float4, 6> posnormalmatrix;
  std::array<float4, 4> projection;
  std::array<int4, 4> materials;
  struct Light
  {
    int4 color;
    float4 cosatt;
    float4 distatt;
    float4 pos;
    float4 dir;
  };
  std::array<Light, 8> lights;
  std::array<float4, 24> texmatrices;
  std::array<float4, 64> transformmatrices;
  std::array<float4, 32> normalmatrices;
  std::array<float4, 64> posttransformmatrices;
  float4 pixelcentercorrection;
  std::array<float, 2> viewport;  // .xy
  std::array<float, 2> pad2;      // .zw

  // .x - texMtxInfo, .y - postMtxInfo, [0..1].z = color, [0..1].w = alpha
  std::array<uint4, 8> xfmem_pack1;

  float4 cached_normal;
  float4 cached_tangent;
  float4 cached_binormal;
  // For UberShader vertex loader
  u32 vertex_stride;
  std::array<u32, 3> vertex_offset_normals;
  u32 vertex_offset_position;
  u32 vertex_offset_posmtx;
  std::array<u32, 2> vertex_offset_colors;
  std::array<u32, 8> vertex_offset_texcoords;
};

enum class VSExpand : u32
{
  None = 0,
  Point,
  Line,
};

struct alignas(16) GeometryShaderConstants
{
  float4 stereoparams;
  float4 lineptparams;
  int4 texoffset;
  VSExpand vs_expand;  // Used by VS point/line expansion in ubershaders
  u32 pad[3];
  // Per-eye HMD projection rows for OpenXR VR (ceye_proj in shaders).
  // [0],[1] = left eye rows 0,1; [2],[3] = right eye rows 0,1.
  // Row 0 computes x_clip = dot(row, viewPos); row 1 computes y_clip.
  // Head rotation and eye position are baked into these rows.
  std::array<float4, 4> eye_projection{};
  // Per-eye z-axis row for depth/w recomputation (cvr_eye_z in shaders).
  // z_eye = dot(eye_z_row[eye], viewPos) gives the eye-space depth with
  // head rotation and eye position baked in.
  std::array<float4, 2> eye_z_row{};
  // OpenXR GS depth params:
  //   x,y = game's projection depth encoding {P[2][2], P[2][3]}
  //   z,w = VS-equivalent depth remap {depth_scale, depth_offset}
  // Used to recompute and remap:
  //   z_clip_raw = P[2][2] * z_eye + P[2][3]
  //   z_clip = w * f.pos.w - z * z_clip_raw   (plus API clip-control conversion in shader)
  float4 depth_params{};
  // Virtual screen params for placing 2D (ortho) content in VR:
  //   x = half-width in game units,  y = half-height in game units
  //   z = distance in game units,    w = ortho draw layer index (for depth separation)
  float4 vr_screen{};
  // Unrotated per-eye projection rows for head-locked VR content (cvr_head_proj in shaders).
  // Same layout as eye_projection but WITHOUT head rotation baked in.
  // Used for HUD-like elements that follow head movements.
  std::array<float4, 4> head_projection{};
  // Head-locked virtual screen tuning params (cvr_head_params in shaders).
  // x = curvature amount (0 = flat)
  // yzw = reserved.
  float4 head_locked_params{};
  // Copy of the VS viewport-sign and pixel-center correction for GS-built VR positions.
  float4 pixel_center_correction{};
  // Pane->frame NDC remap for sub-screen 3D viewports routed to the virtual screen
  // (cvr_pane_remap in shaders): ndc_frame = ndc_pane * xy + zw.
  // Identity {1,1,0,0} for every draw whose NDC already spans the full screen.
  float4 vr_pane_remap{1.0f, 1.0f, 0.0f, 0.0f};
  // HudPlane clip rectangle in frame NDC (cvr_pane_clip in shaders): {left, right, bottom, top}.
  // The game's scissor cannot clip a plane seen in perspective, so the GS turns it into clip
  // distances. Wide open for every other draw.
  float4 vr_pane_clip{-1.0e6f, 1.0e6f, -1.0e6f, 1.0e6f};
};
