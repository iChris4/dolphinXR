// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VertexShaderGen.h"

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/ConstantManager.h"
#include "VideoCommon/LightingShaderGen.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

static constexpr u32 VERTEX_SHADER_CODE_VERSION = 22;

VertexShaderUid GetVertexShaderUid()
{
  ASSERT(bpmem.genMode.numtexgens == xfmem.numTexGen.numTexGens);
  ASSERT(bpmem.genMode.numcolchans == xfmem.numChan.numColorChans);

  VertexShaderUid out;
  vertex_shader_uid_data* const uid_data = out.GetUidData();
  uid_data->code_version = VERTEX_SHADER_CODE_VERSION;
  uid_data->numTexGens = xfmem.numTexGen.numTexGens;
  uid_data->components = VertexLoaderManager::g_current_components;
  uid_data->numColorChans = xfmem.numChan.numColorChans;

  GetLightingShaderUid(uid_data->lighting);

  // transform texcoords
  for (u32 i = 0; i < uid_data->numTexGens; ++i)
  {
    auto& texinfo = uid_data->texMtxInfo[i];

    texinfo.sourcerow = xfmem.texMtxInfo[i].sourcerow;
    texinfo.texgentype = xfmem.texMtxInfo[i].texgentype;
    texinfo.inputform = xfmem.texMtxInfo[i].inputform;

    // first transformation
    switch (texinfo.texgentype)
    {
    case TexGenType::EmbossMap:  // calculate tex coords into bump map
      if ((uid_data->components & (VB_HAS_TANGENT | VB_HAS_BINORMAL)) != 0)
      {
        // transform the light dir into tangent space
        texinfo.embosslightshift = xfmem.texMtxInfo[i].embosslightshift;
        texinfo.embosssourceshift = xfmem.texMtxInfo[i].embosssourceshift;
      }
      else
      {
        texinfo.embosssourceshift = xfmem.texMtxInfo[i].embosssourceshift;
      }
      break;
    case TexGenType::Color0:
    case TexGenType::Color1:
      break;
    case TexGenType::Regular:
    default:
      uid_data->texMtxInfo_n_projection |= static_cast<u32>(xfmem.texMtxInfo[i].projection.Value())
                                           << i;
      break;
    }

    uid_data->dualTexTrans_enabled = xfmem.dualTexTrans.enabled;
    // CHECKME: does this only work for regular tex gen types?
    if (uid_data->dualTexTrans_enabled && texinfo.texgentype == TexGenType::Regular)
    {
      auto& postInfo = uid_data->postMtxInfo[i];
      postInfo.index = xfmem.postMtxInfo[i].index;
      postInfo.normalize = xfmem.postMtxInfo[i].normalize;
    }
  }

  return out;
}

static void WritePrimitiveExpand(APIType api_type, const ShaderHostConfig& host_config,
                                 const vertex_shader_uid_data* uid_data, ShaderCode& out)
{
  if (uid_data->vs_expand == VSExpand::None)
    return;

  out.Write("InputData dolphin_primitive_expand_data(int index_offset)\n");
  out.Write("{{\n");
  if (api_type == APIType::D3D)
  {
    // D3D doesn't include the base vertex in SV_VertexID
    // See comment in UberShaderVertex for details
    out.Write("\tuint vertex_id = (gl_VertexID >> 2) + base_vertex;\n");
  }
  else
  {
    out.Write("\tuint vertex_id = uint(gl_VertexID) >> 2u;\n");
  }
  out.Write("\treturn input_buffer[vertex_id + index_offset];\n");
  out.Write("}}\n\n");
}

static void WriteTransformMatrices(APIType api_type, const ShaderHostConfig& host_config,
                                   const vertex_shader_uid_data* uid_data, ShaderCode& out)
{
  out.Write("mat3x4 dolphin_position_matrix()\n");
  out.Write("{{\n");
  out.Write("\tmat3x4 result;\n");
  if ((uid_data->components & VB_HAS_POSMTXIDX) != 0)
  {
    if (uid_data->vs_expand != VSExpand::None)
    {
      out.Write("\tInputData i = dolphin_primitive_expand_data(0);\n");
      out.Write("\tuvec4 posmtx = unpack_ubyte4(i.posmtx);\n");
    }
    // Vertex format has a per-vertex matrix
    out.Write("\tint posidx = int(posmtx.r);\n"
              "\tresult[0] = " I_TRANSFORMMATRICES "[posidx];\n"
              "\tresult[1] = " I_TRANSFORMMATRICES "[posidx + 1];\n"
              "\tresult[2] = " I_TRANSFORMMATRICES "[posidx + 2];\n");
  }
  else
  {
    // One shared matrix
    out.Write("\tresult[0] = " I_POSNORMALMATRIX "[0];\n"
              "\tresult[1] = " I_POSNORMALMATRIX "[1];\n"
              "\tresult[2] = " I_POSNORMALMATRIX "[2];\n");
  }
  out.Write("\treturn result;\n");
  out.Write("}}\n\n");

  // The scale of the transform matrix is used to control the size of the emboss map effect, by
  // changing the scale of the transformed binormals (which only get used by emboss map texgens).
  // By normalising the first transformed normal (which is used by lighting calculations and needs
  // to be unit length), the same transform matrix can do double duty, scaling for emboss mapping,
  // and not scaling for lighting.
  out.Write("mat3 dolphin_normal_matrix()\n");
  out.Write("{{\n");
  out.Write("\tmat3 result;\n");
  if ((uid_data->components & VB_HAS_POSMTXIDX) != 0)
  {
    if (uid_data->vs_expand != VSExpand::None)
    {
      out.Write("\tInputData i = dolphin_primitive_expand_data(0);\n");
      out.Write("\tuvec4 posmtx = unpack_ubyte4(i.posmtx);\n");
    }
    // Vertex format has a per-vertex matrix
    out.Write("\tint posidx = int(posmtx.r);\n");
    out.Write("\tint normidx = posidx & 31;\n"
              "\tresult[0] = " I_NORMALMATRICES "[normidx].xyz;\n"
              "\tresult[1] = " I_NORMALMATRICES "[normidx + 1].xyz;\n"
              "\tresult[2] = " I_NORMALMATRICES "[normidx + 2].xyz;\n");
  }
  else
  {
    // One shared matrix
    out.Write("\tresult[0] = " I_POSNORMALMATRIX "[3].xyz;\n"
              "\tresult[1] = " I_POSNORMALMATRIX "[4].xyz;\n"
              "\tresult[2] = " I_POSNORMALMATRIX "[5].xyz;\n");
  }
  out.Write("\treturn result;\n");
  out.Write("}}\n\n");
}

static void WriteTexCoordTransforms(APIType api_type, const ShaderHostConfig& host_config,
                                    const vertex_shader_uid_data* uid_data, ShaderCode& out)
{
  for (u32 i = 0; i < uid_data->numTexGens; ++i)
  {
    auto& texinfo = uid_data->texMtxInfo[i];
    out.Write("vec3 dolphin_transform_texcoord{}(vec4 coord)\n", i);
    out.Write("{{\n");
    if (texinfo.texgentype != TexGenType::Regular)
    {
      out.Write("\treturn vec3(coord.xyz);\n");
    }
    else
    {
      out.Write("\tvec3 result;\n");
      if ((uid_data->components & (VB_HAS_TEXMTXIDX0 << i)) != 0)
      {
        out.Write("\tint tmp = int(rawtex{}.z);\n", i);
        if (static_cast<TexSize>((uid_data->texMtxInfo_n_projection >> i) & 1) == TexSize::STQ)
        {
          out.Write("\tresult = vec3(dot(coord, " I_TRANSFORMMATRICES
                    "[tmp]), dot(coord, " I_TRANSFORMMATRICES
                    "[tmp+1]), dot(coord, " I_TRANSFORMMATRICES "[tmp+2]));\n");
        }
        else
        {
          out.Write("\tresult = vec3(dot(coord, " I_TRANSFORMMATRICES
                    "[tmp]), dot(coord, " I_TRANSFORMMATRICES "[tmp+1]), 1);\n");
        }
      }
      else
      {
        if (static_cast<TexSize>((uid_data->texMtxInfo_n_projection >> i) & 1) == TexSize::STQ)
        {
          out.Write("\tresult = vec3(dot(coord, " I_TEXMATRICES "[{}]), dot(coord, " I_TEXMATRICES
                    "[{}]), dot(coord, " I_TEXMATRICES "[{}]));\n",
                    3 * i, 3 * i + 1, 3 * i + 2);
        }
        else
        {
          out.Write("\tresult = vec3(dot(coord, " I_TEXMATRICES "[{}]), dot(coord, " I_TEXMATRICES
                    "[{}]), 1);\n",
                    3 * i, 3 * i + 1);
        }
      }
      // CHECKME: does this only work for regular tex gen types?
      if (uid_data->dualTexTrans_enabled)
      {
        auto& postInfo = uid_data->postMtxInfo[i];

        out.Write("\tvec4 P0 = " I_POSTTRANSFORMMATRICES "[{}];\n"
                  "\tvec4 P1 = " I_POSTTRANSFORMMATRICES "[{}];\n"
                  "\tvec4 P2 = " I_POSTTRANSFORMMATRICES "[{}];\n",
                  postInfo.index & 0x3f, (postInfo.index + 1) & 0x3f, (postInfo.index + 2) & 0x3f);

        if (postInfo.normalize)
          out.Write("\tresult = normalize(result);\n");

        // multiply by postmatrix
        out.Write("\tresult = vec3(dot(P0.xyz, result) + P0.w, dot(P1.xyz, result) + "
                  "P1.w, dot(P2.xyz, result) + P2.w);\n");
      }

      // When q is 0, the GameCube appears to have a special case
      // This can be seen in devkitPro's neheGX Lesson08 example for Wii
      // Makes differences in Rogue Squadron 3 (Hoth sky) and The Last Story (shadow culling)
      // TODO: check if this only affects XF_TEXGEN_REGULAR
      out.Write("\tif(result.z == 0.0f)\n"
                "\t\tresult.xy = clamp(result.xy / 2.0f, vec2(-1.0f,-1.0f), vec2(1.0f,1.0f));\n");
      out.Write("\treturn result;\n");
    }
    out.Write("}}\n\n");
  }
}

static void WriteVertexStructs(APIType api_type, const ShaderHostConfig& host_config,
                               const vertex_shader_uid_data* uid_data, ShaderCode& out)
{
  out.Write("struct DolphinVertexInput\n");
  out.Write("{{\n");
  out.Write("\tvec4 color_0;\n");
  out.Write("\tvec4 color_1;\n");
  out.Write("\tvec4 position;\n");
  out.Write("\tvec3 normal;\n");
  out.Write("\tvec3 binormal;\n");
  out.Write("\tvec3 tangent;\n");
  for (u32 i = 0; i < 8; i++)
  {
    out.Write("\tvec4 texture_coord_{};\n", i);
  }
  out.Write("}};\n\n");

  out.Write("struct DolphinVertexOutput\n");
  out.Write("{{\n");
  out.Write("\tvec4 color_0;\n");
  out.Write("\tvec4 color_1;\n");
  out.Write("\tvec4 position;\n");
  out.Write("\tvec3 normal;\n");
  for (u32 i = 0; i < 8; i++)
  {
    out.Write("\tvec3 texture_coord_{};\n", i);
  }
  out.Write("}};\n\n");
}

static void WriteVertexDefines(APIType, const ShaderHostConfig&,
                               const vertex_shader_uid_data* uid_data, ShaderCode& out)
{
  if ((uid_data->components & VB_HAS_COL0) != 0)
  {
    out.Write("#define HAS_COLOR_0 1\n");
  }
  else
  {
    out.Write("#define HAS_COLOR_0 0\n");
  }

  if ((uid_data->components & VB_HAS_COL1) != 0)
  {
    out.Write("#define HAS_COLOR_1 1\n");
  }
  else
  {
    out.Write("#define HAS_COLOR_1 0\n");
  }

  if ((uid_data->components & VB_HAS_NORMAL) != 0)
  {
    out.Write("#define HAS_NORMAL 1\n");
  }
  else
  {
    out.Write("#define HAS_NORMAL 0\n");
  }

  if ((uid_data->components & VB_HAS_BINORMAL) != 0)
  {
    out.Write("#define HAS_BINORMAL 1\n");
  }
  else
  {
    out.Write("#define HAS_BINORMAL 0\n");
  }

  if ((uid_data->components & VB_HAS_TANGENT) != 0)
  {
    out.Write("#define HAS_TANGENT 1\n");
  }
  else
  {
    out.Write("#define HAS_TANGENT 0\n");
  }

  for (u32 i = 0; i < uid_data->numTexGens; i++)
  {
    if ((uid_data->components & (VB_HAS_UV0 << i)) != 0)
    {
      out.Write("#define HAS_TEXTURE_COORD_{} 1\n", i);
    }
    else
    {
      out.Write("#define HAS_TEXTURE_COORD_{} 0\n", i);
    }
  }

  for (u32 i = uid_data->numTexGens; i < 8; i++)
  {
    out.Write("#define HAS_TEXTURE_COORD_{} 0\n", i);
  }
}

static void WriteEmulatedVertexBodyHeader(APIType api_type, const ShaderHostConfig& host_config,
                                          const vertex_shader_uid_data* uid_data, ShaderCode& out)
{
  constexpr std::string_view emulated_fragment_definition =
      "void dolphin_process_emulated_vertex(in DolphinVertexInput vertex_input, out "
      "DolphinVertexOutput vertex_output)";
  out.Write("{}\n", emulated_fragment_definition);
  out.Write("{{\n");

  WriteVertexBody(api_type, host_config, uid_data, out);

  out.Write("}}\n");
}

bool UseVRMultiviewVSProjection(APIType api_type, const ShaderHostConfig& host_config)
{
  return host_config.vr_stereo && host_config.vk_multiview &&
         (api_type == APIType::OpenGL || api_type == APIType::Vulkan);
}

// Multiview VR path: replace the projected o.pos with a per-eye HMD projection selected
// by gl_ViewIndex (0 = left, 1 = right). This was previously done in the GS; doing it in
// the VS lets us drop the GS stage entirely for triangles. Mirrors the branches in
// GeometryShaderGen.cpp — perspective VR, head-locked perspective HUD, head-locked VR,
// orthographic VR. Emits `bool vr_pos_replaced`, which is true when the block replaced
// o.pos (and therefore already applied the depth-range remap via cvr_depth.z/w) — the
// caller must then skip its trailing depth-range/pixel-center fixups and neutralize the
// console-convention clip distances.
void GenerateVRMultiviewVSProjection(ShaderCode& out, APIType api_type,
                                     const ShaderHostConfig& host_config,
                                     std::string_view view_space_pos)
{
  out.Write("\tbool vr_pos_replaced = false;\n");
  // HudPlane clip rectangle as user clip distances (left, right, bottom, top); see cvr_pane_clip.
  out.Write("\tfloat4 vr_pane_clip_dist = float4(1.0, 1.0, 1.0, 1.0);\n");
  out.Write("\t{{\n");
  out.Write("\tuint eye = gl_ViewIndex;\n");
  out.Write("\tbool right_eye = eye != 0u;\n");

  // World-fixed perspective pane. Scale X/Y/Z together by each vertex's perspective-W ratio.
  // Flat Screen Pane draws use the separate world-fixed 2D screen branch below.
  out.Write("\tif (" I_STEREOPARAMS ".w > 1.5f)\n\t{{\n");
  out.Write("\t\tfloat pane_game_w = (abs(o.pos.w) > 1.0e-6) ? o.pos.w : 1.0e-6;\n");
  // The multiview VS sees console-convention depth before the trailing viewport remap. Convert it
  // now for the selectable original-game-depth path (GS mirror above).
  out.Write("\t\tfloat pane_console_ndc_z = clamp(o.pos.z / pane_game_w, -1.0, 1.0);\n");
  out.Write("\t\tfloat pane_game_ndc_z = clamp(" I_PIXELCENTERCORRECTION
            ".w - pane_console_ndc_z * " I_PIXELCENTERCORRECTION ".z, -1.0, 1.0);\n");
  out.Write("\t\tfloat pane_reference_w = max(" I_HEAD_PARAMS ".w, 1.0e-4);\n");
  // HudPlane (head_locked_params.w < 0): no per-vertex depth, the whole draw on one plane.
  out.Write("\t\tfloat pane_depth_scale = (" I_HEAD_PARAMS ".w < 0.0) ? 1.0 : "
            "max(pane_game_w / pane_reference_w, 0.001);\n");
  out.Write("\t\tfloat ndc_x = o.pos.x / pane_game_w;\n");
  out.Write("\t\tfloat ndc_y = o.pos.y / pane_game_w;\n");
  out.Write("\t\tndc_x = ndc_x * " I_VR_PANE_REMAP ".x + " I_VR_PANE_REMAP ".z;\n");
  out.Write("\t\tndc_y = ndc_y * " I_VR_PANE_REMAP ".y + " I_VR_PANE_REMAP ".w;\n");
  // Exact on the flat HudPlane: frame NDC is affine in the plane position, so linear in clip space.
  out.Write("\t\tvr_pane_clip_dist = float4(ndc_x - " I_VR_PANE_CLIP ".x, " I_VR_PANE_CLIP
            ".y - ndc_x, ndc_y - " I_VR_PANE_CLIP ".z, " I_VR_PANE_CLIP ".w - ndc_y);\n");
  out.Write("\t\tfloat4 panePos = float4(\n");
  out.Write("\t\t\tndc_x * " I_VR_SCREEN ".x * pane_depth_scale,\n");
  out.Write("\t\t\tndc_y * " I_VR_SCREEN ".y * pane_depth_scale,\n");
  out.Write("\t\t\t-" I_VR_SCREEN ".z * pane_depth_scale,\n");
  out.Write("\t\t\t1.0);\n");
  out.Write("\t\tfloat4 row0 = right_eye ? " I_EYE_PROJ "[2] : " I_EYE_PROJ "[0];\n");
  out.Write("\t\tfloat4 row1 = right_eye ? " I_EYE_PROJ "[3] : " I_EYE_PROJ "[1];\n");
  out.Write("\t\tfloat4 zrow = right_eye ? " I_VR_EYE_Z "[1] : " I_VR_EYE_Z "[0];\n");
  out.Write("\t\tfloat z_eye = dot(zrow, panePos);\n");
  out.Write("\t\tfloat clip_x = dot(row0, panePos);\n");
  out.Write("\t\tfloat clip_y = dot(row1, panePos);\n");
  out.Write("\t\tfloat clip_w = -z_eye;\n");
  out.Write("\t\tif (" I_STEREOPARAMS ".w > 2.5f && " I_STEREOPARAMS
            ".w < 3.5f)\n");
  out.Write("\t\t{{\n");
  out.Write("\t\t\tfloat clip_z = " I_VR_DEPTH ".x * z_eye + " I_VR_DEPTH ".y;\n");
  out.Write("\t\t\to.pos = float4(clip_x, clip_y, clip_z, clip_w);\n");
  out.Write("\t\t\to.pos.z = o.pos.w * " I_VR_DEPTH ".w - o.pos.z * " I_VR_DEPTH ".z;\n");
  out.Write("\t\t\to.vr_depth = (abs(o.pos.w) > 1e-6) ? o.pos.z / o.pos.w : 0.0;\n");
  out.Write("\t\t}}\n");
  out.Write("\t\telse\n");
  out.Write("\t\t{{\n");
  out.Write("\t\t\to.pos = float4(clip_x, clip_y, pane_game_ndc_z * clip_w, clip_w);\n");
  out.Write("\t\t\to.vr_depth = clamp(pane_game_ndc_z, 0.0, 1.0);\n");
  out.Write("\t\t}}\n");
  if (!host_config.backend_clip_control)
    out.Write("\t\to.pos.z = o.pos.z * 2.0 - o.pos.w;\n");
  out.Write("\t\to.pos.xy *= sign(" I_VR_PIXELCENTER ".xy * float2(1.0, -1.0));\n");
  out.Write("\t\to.pos.xy = o.pos.xy - o.pos.w * " I_VR_PIXELCENTER ".xy;\n");
  out.Write("\t\tvr_pos_replaced = true;\n");
  out.Write("\t}}\n");

  out.Write("\telse if (" I_STEREOPARAMS ".w > 0.5f)\n\t{{\n");
  out.Write("\t\tfloat4 row0 = right_eye ? " I_EYE_PROJ "[2] : " I_EYE_PROJ "[0];\n");
  out.Write("\t\tfloat4 row1 = right_eye ? " I_EYE_PROJ "[3] : " I_EYE_PROJ "[1];\n");
  out.Write("\t\tfloat4 zrow = right_eye ? " I_VR_EYE_Z "[1] : " I_VR_EYE_Z "[0];\n");
  out.Write("\t\tfloat4 vp = {};\n", view_space_pos);
  out.Write("\t\tvp.x *= " I_STEREOPARAMS ".x;\n");
  // Skybox (Detect Skybox): cstereo.z is the world-position weight (1 = normal, 0 = skybox).
  // The per-eye position offset (IPD + head translation) is baked into the projection rows'
  // .w component and applies only because vp.w == 1; zeroing it renders rotation-only so the
  // skybox sits at infinity. Mirrors the GS perspective path.
  out.Write("\t\tvp.w *= " I_STEREOPARAMS ".z;\n");
  out.Write("\t\tfloat z_eye = dot(zrow, vp);\n");
  out.Write("\t\tfloat clip_x = dot(row0, vp);\n");
  out.Write("\t\tfloat clip_y = dot(row1, vp);\n");
  out.Write("\t\tfloat clip_w = -z_eye;\n");
  out.Write("\t\tfloat clip_z = " I_VR_DEPTH ".x * z_eye + " I_VR_DEPTH ".y;\n");
  out.Write("\t\to.pos = float4(clip_x, clip_y, clip_z, clip_w);\n");
  out.Write("\t\to.pos.z = o.pos.w * " I_VR_DEPTH ".w - o.pos.z * " I_VR_DEPTH ".z;\n");
  // The trailing vr_depth capture is skipped for VR-replaced positions, so keep the
  // varying defined here (perspective draws never use the exact-depth PS export).
  out.Write("\t\to.vr_depth = (abs(o.pos.w) > 1e-6) ? o.pos.z / o.pos.w : 0.0;\n");
  if (!host_config.backend_clip_control)
    out.Write("\t\to.pos.z = o.pos.z * 2.0 - o.pos.w;\n");
  out.Write("\t\to.pos.xy *= sign(" I_VR_PIXELCENTER ".xy * float2(1.0, -1.0));\n");
  out.Write("\t\to.pos.xy = o.pos.xy - o.pos.w * " I_VR_PIXELCENTER ".xy;\n");
  out.Write("\t\tvr_pos_replaced = true;\n");
  out.Write("\t}}\n");

  // Head-locked perspective HUD: keep the game's original perspective GUI/model geometry,
  // then place it in head-locked HMD space.
  out.Write("\telse if (" I_STEREOPARAMS ".w < -2.5f)\n\t{{\n");
  // Capture the game's NDC depth and re-attach it below — see GeometryShaderGen for the full
  // rationale (Prime's minimap depth-tests against the world via its viewport depth slice).
  // Unlike the GS path (which sees f.pos AFTER the VS's trailing depth-range remap), o.pos
  // here is still in console convention (-1..0) — the trailing remap is suppressed for VR
  // draws via vr_pos_replaced. Apply the same remap to the captured NDC ourselves
  // (z' = w*cpc.w - z*cpc.z  =>  ndc' = cpc.w - ndc*cpc.z) so the re-attached depth matches
  // what the game (and its viewport depth slices) expect in the backend convention.
  out.Write("\t\tfloat hud_game_w = (abs(o.pos.w) > 1.0e-6) ? o.pos.w : 1.0e-6;\n");
  out.Write("\t\tfloat hud_console_ndc_z = clamp(o.pos.z / hud_game_w, -1.0, 1.0);\n");
  out.Write("\t\tfloat hud_game_ndc_z = clamp(" I_PIXELCENTERCORRECTION
            ".w - hud_console_ndc_z * " I_PIXELCENTERCORRECTION ".z, -1.0, 1.0);\n");
  out.Write("\t\tfloat4 hudPos = float4(\n");
  out.Write("\t\t\t{}.x * " I_HEAD_PARAMS ".y + " I_VR_SCREEN ".x,\n", view_space_pos);
  out.Write("\t\t\t{}.y * " I_HEAD_PARAMS ".z + " I_VR_SCREEN ".y,\n", view_space_pos);
  out.Write("\t\t\t{}.z * " I_HEAD_PARAMS ".w + " I_VR_SCREEN ".z,\n", view_space_pos);
  out.Write("\t\t\t1.0);\n");
  // Near-plane clamp so depth-outlier HUD pieces (radar/minimap blips) don't vanish at small HUD
  // distances — see GeometryShaderGen.
  out.Write("\t\thudPos.z = min(hudPos.z, -0.1 * max(" I_VR_DEPTH ".w, 0.001));\n");
  out.Write("\t\tfloat4 row0 = right_eye ? " I_HEAD_PROJ "[2] : " I_HEAD_PROJ "[0];\n");
  out.Write("\t\tfloat4 row1 = right_eye ? " I_HEAD_PROJ "[3] : " I_HEAD_PROJ "[1];\n");
  out.Write("\t\to.pos.x = dot(row0, hudPos);\n");
  out.Write("\t\to.pos.y = dot(row1, hudPos);\n");
  out.Write("\t\to.pos.w = max(-hudPos.z, 0.001);\n");
  // Game NDC depth re-attached (hud_game_ndc_z is already remapped to the backend convention).
  out.Write("\t\to.pos.z = hud_game_ndc_z * o.pos.w;\n");
  out.Write("\t\to.vr_depth = clamp(hud_game_ndc_z, 0.0, 1.0);\n");
  if (!host_config.backend_clip_control)
    out.Write("\t\to.pos.z = o.pos.z * 2.0 - o.pos.w;\n");
  out.Write("\t\to.pos.xy *= sign(" I_VR_PIXELCENTER ".xy * float2(1.0, -1.0));\n");
  out.Write("\t\to.pos.xy = o.pos.xy - o.pos.w * " I_VR_PIXELCENTER ".xy;\n");
  out.Write("\t\tvr_pos_replaced = true;\n");
  out.Write("\t}}\n");

  // Head-locked VR: 2D content on a virtual screen that follows head movements.
  out.Write("\telse if (" I_STEREOPARAMS ".w < -1.5f)\n\t{{\n");
  if (api_type == APIType::Vulkan)
  {
    // Vulkan only guards the divide (w ~ 0 gives NaN/Inf and vanishing geometry);
    // do NOT clamp x/y — that squashes elements extending past the screen bounds.
    out.Write("\t\tfloat safe_w = (abs(o.pos.w) > 1.0e-5) ? o.pos.w : "
              "((o.pos.w < 0.0) ? -1.0e-5 : 1.0e-5);\n");
    out.Write("\t\tfloat ndc_x = o.pos.x / safe_w;\n");
    out.Write("\t\tfloat ndc_y = o.pos.y / safe_w;\n");
    out.Write("\t\tfloat ndc_z = o.pos.z / safe_w;\n");
  }
  else
  {
    out.Write("\t\tfloat ndc_x = o.pos.x / o.pos.w;\n");
    out.Write("\t\tfloat ndc_y = o.pos.y / o.pos.w;\n");
    out.Write("\t\tfloat ndc_z = o.pos.z / o.pos.w;\n");
  }
  out.Write("\t\tfloat ndc_z_clamped = clamp(ndc_z, -1.0, 1.0);\n");
  out.Write("\t\tfloat4 screenPos = float4(\n");
  out.Write("\t\t\tndc_x * " I_VR_SCREEN ".x,\n");
  out.Write("\t\t\tndc_y * " I_VR_SCREEN ".y,\n");
  out.Write("\t\t\t-" I_VR_SCREEN ".z + ndc_z_clamped * " I_VR_DEPTH ".z,\n");
  out.Write("\t\t\t1.0);\n");
  out.Write("\t\tfloat curve = max(" I_HEAD_PARAMS ".x, 0.0);\n");
  out.Write("\t\tfloat horizontal = 0.5 * (ndc_x * ndc_x);\n");
  out.Write("\t\tfloat curve_push = curve * horizontal * " I_VR_SCREEN ".z * 0.25;\n");
  out.Write("\t\tcurve_push = min(curve_push, " I_VR_SCREEN ".z * 0.8);\n");
  out.Write("\t\tscreenPos.z += curve_push;\n");
  out.Write("\t\tfloat4 row0 = right_eye ? " I_HEAD_PROJ "[2] : " I_HEAD_PROJ "[0];\n");
  out.Write("\t\tfloat4 row1 = right_eye ? " I_HEAD_PROJ "[3] : " I_HEAD_PROJ "[1];\n");
  out.Write("\t\to.pos.x = dot(row0, screenPos);\n");
  out.Write("\t\to.pos.y = dot(row1, screenPos);\n");
  out.Write("\t\to.pos.w = max(-screenPos.z, 0.001);\n");
  // Park the geometry mid-range; the real depth comes from the PS export (Exact Screen Depth),
  // with the tiny game-z term left as the legacy fallback ordering.
  out.Write("\t\to.pos.z = o.pos.w * (0.5 + ndc_z_clamped * 0.0001);\n");
  // Exact game depth for the PS depth export: remap the console-convention NDC (-1..0) to the
  // backend 0..1 convention, mirroring the trailing capture this branch suppresses.
  out.Write("\t\to.vr_depth = clamp(" I_PIXELCENTERCORRECTION ".w - ndc_z_clamped * "
            I_PIXELCENTERCORRECTION ".z, 0.0, 1.0);\n");
  if (!host_config.backend_clip_control)
    out.Write("\t\to.pos.z = o.pos.z * 2.0 - o.pos.w;\n");
  out.Write("\t\to.pos.xy *= sign(" I_VR_PIXELCENTER ".xy * float2(1.0, -1.0));\n");
  out.Write("\t\to.pos.xy = o.pos.xy - o.pos.w * " I_VR_PIXELCENTER ".xy;\n");
  out.Write("\t\tvr_pos_replaced = true;\n");
  out.Write("\t}}\n");

  // Orthographic VR: place 2D content (HUD/menus/FMV) on a virtual screen plane.
  out.Write("\telse if (" I_STEREOPARAMS ".w < -0.5f)\n\t{{\n");
  if (api_type == APIType::Vulkan)
  {
    // Vulkan only guards the divide (w ~ 0 gives NaN/Inf and vanishing geometry);
    // do NOT clamp x/y — that squashes elements extending past the screen bounds.
    out.Write("\t\tfloat safe_w = (abs(o.pos.w) > 1.0e-5) ? o.pos.w : "
              "((o.pos.w < 0.0) ? -1.0e-5 : 1.0e-5);\n");
    out.Write("\t\tfloat ndc_x = o.pos.x / safe_w;\n");
    out.Write("\t\tfloat ndc_y = o.pos.y / safe_w;\n");
    out.Write("\t\tfloat ndc_z = o.pos.z / safe_w;\n");
  }
  else
  {
    out.Write("\t\tfloat ndc_x = o.pos.x / o.pos.w;\n");
    out.Write("\t\tfloat ndc_y = o.pos.y / o.pos.w;\n");
    out.Write("\t\tfloat ndc_z = o.pos.z / o.pos.w;\n");
  }
  // Pane->frame remap: sub-screen 3D viewports (MKDD character select boxes) have NDC
  // spanning only their pane; map it to the pane's place on the full screen. Identity
  // {1,1,0,0} for normal fullscreen draws. Mirrors the GS Screen branch.
  out.Write("\t\tndc_x = ndc_x * " I_VR_PANE_REMAP ".x + " I_VR_PANE_REMAP ".z;\n");
  out.Write("\t\tndc_y = ndc_y * " I_VR_PANE_REMAP ".y + " I_VR_PANE_REMAP ".w;\n");
  out.Write("\t\tfloat ndc_z_clamped = clamp(ndc_z, -1.0, 1.0);\n");
  out.Write("\t\tfloat4 screenPos = float4(\n");
  out.Write("\t\t\tndc_x * " I_VR_SCREEN ".x,\n");
  out.Write("\t\t\tndc_y * " I_VR_SCREEN ".y,\n");
  out.Write("\t\t\t-" I_VR_SCREEN ".z + ndc_z_clamped * " I_VR_DEPTH ".z,\n");
  out.Write("\t\t\t1.0);\n");
  out.Write("\t\tfloat4 row0 = right_eye ? " I_EYE_PROJ "[2] : " I_EYE_PROJ "[0];\n");
  out.Write("\t\tfloat4 row1 = right_eye ? " I_EYE_PROJ "[3] : " I_EYE_PROJ "[1];\n");
  out.Write("\t\tfloat4 zrow = right_eye ? " I_VR_EYE_Z "[1] : " I_VR_EYE_Z "[0];\n");
  out.Write("\t\tfloat z_eye = dot(zrow, screenPos);\n");
  out.Write("\t\tfloat clip_x = dot(row0, screenPos);\n");
  out.Write("\t\tfloat clip_y = dot(row1, screenPos);\n");
  out.Write("\t\tfloat clip_w = -z_eye;\n");
  out.Write("\t\to.pos = float4(clip_x, clip_y, 0.0, clip_w);\n");
  // Park the geometry mid-range and spread elements by their own game-space ortho z
  // (cvr_depth.y = Element Depth). With Exact Screen Depth on (the default) the PS overwrites
  // this with the game's original depth value; this remains as the legacy fallback.
  out.Write("\t\to.pos.z = o.pos.w * (0.5 + ndc_z_clamped * " I_VR_DEPTH ".y);\n");
  // Exact game depth for the PS depth export: remap the console-convention NDC (-1..0) to the
  // backend 0..1 convention, mirroring the trailing capture this branch suppresses.
  out.Write("\t\to.vr_depth = clamp(" I_PIXELCENTERCORRECTION ".w - ndc_z_clamped * "
            I_PIXELCENTERCORRECTION ".z, 0.0, 1.0);\n");
  if (!host_config.backend_clip_control)
    out.Write("\t\to.pos.z = o.pos.z * 2.0 - o.pos.w;\n");
  out.Write("\t\to.pos.xy *= sign(" I_VR_PIXELCENTER ".xy * float2(1.0, -1.0));\n");
  out.Write("\t\to.pos.xy = o.pos.xy - o.pos.w * " I_VR_PIXELCENTER ".xy;\n");
  out.Write("\t\tvr_pos_replaced = true;\n");
  out.Write("\t}}\n");
  out.Write("\t}}\n");
}

ShaderCode GenerateVertexShaderCode(APIType api_type, const ShaderHostConfig& host_config,
                                    const vertex_shader_uid_data* uid_data,
                                    CustomVertexContents custom_contents)
{
  ShaderCode out;

  const bool per_pixel_lighting = g_ActiveConfig.bEnablePixelLighting;
  const bool msaa = host_config.msaa;
  const bool ssaa = host_config.ssaa;
  const bool vertex_rounding = host_config.vertex_rounding;

  ShaderCode input_extract;

  // VK_KHR_multiview path: the VS reads gl_ViewIndex (0/1) to select the per-eye
  // HMD projection. The extension must be declared at the top of the GLSL source.
  if (UseVRMultiviewVSProjection(api_type, host_config))
    out.Write("#extension GL_EXT_multiview : require\n");

  out.Write("{}", s_lighting_struct);

  // uniforms
  out.Write("UBO_BINDING(std140, 2) uniform VSBlock {{\n");

  out.Write("{}", s_shader_uniforms);
  out.Write("}};\n");

  if (!custom_contents.uniforms.empty())
  {
    out.Write("UBO_BINDING(std140, 3) uniform CustomShaderBlock {{\n");
    out.Write("{}", custom_contents.uniforms);
    out.Write("}} custom_uniforms;\n");
  }

  // The GSBlock UBO is normally only consumed by the GS (and by VS when expanding
  // line/point primitives). Under the multiview VR path the VS performs per-eye
  // projection itself, so it must read the same uniforms.
  const bool needs_gs_block = uid_data->vs_expand != VSExpand::None ||
                              (host_config.vr_stereo && host_config.vk_multiview &&
                               (api_type == APIType::OpenGL || api_type == APIType::Vulkan));
  if (needs_gs_block)
  {
    out.Write("UBO_BINDING(std140, 4) uniform GSBlock {{\n");
    out.Write("{}", s_geometry_shader_uniforms);
    out.Write("}};\n");

    if (api_type == APIType::D3D && uid_data->vs_expand != VSExpand::None)
    {
      // D3D doesn't include the base vertex in SV_VertexID
      out.Write("UBO_BINDING(std140, 5) uniform DX_Constants {{\n"
                "  uint base_vertex;\n"
                "}};\n\n");
    }
  }

  out.Write("struct VS_OUTPUT {{\n");
  GenerateVSOutputMembers(out, api_type, uid_data->numTexGens, host_config, "",
                          ShaderStage::Vertex);
  out.Write("}};\n\n");

  WriteIsNanHeader(out, api_type);
  GenerateLightingShaderHeader(out, uid_data->lighting);

  if (uid_data->vs_expand == VSExpand::None)
  {
    out.Write("ATTRIBUTE_LOCATION({:s}) in float4 rawpos;\n", ShaderAttrib::Position);
    if ((uid_data->components & VB_HAS_POSMTXIDX) != 0)
      out.Write("ATTRIBUTE_LOCATION({:s}) in uint4 posmtx;\n", ShaderAttrib::PositionMatrix);
    if ((uid_data->components & VB_HAS_NORMAL) != 0)
      out.Write("ATTRIBUTE_LOCATION({:s}) in float3 rawnormal;\n", ShaderAttrib::Normal);
    if ((uid_data->components & VB_HAS_TANGENT) != 0)
      out.Write("ATTRIBUTE_LOCATION({:s}) in float3 rawtangent;\n", ShaderAttrib::Tangent);
    if ((uid_data->components & VB_HAS_BINORMAL) != 0)
      out.Write("ATTRIBUTE_LOCATION({:s}) in float3 rawbinormal;\n", ShaderAttrib::Binormal);

    if ((uid_data->components & VB_HAS_COL0) != 0)
      out.Write("ATTRIBUTE_LOCATION({:s}) in float4 rawcolor0;\n", ShaderAttrib::Color0);
    if ((uid_data->components & VB_HAS_COL1) != 0)
      out.Write("ATTRIBUTE_LOCATION({:s}) in float4 rawcolor1;\n", ShaderAttrib::Color1);

    for (u32 i = 0; i < 8; ++i)
    {
      const u32 has_texmtx = (uid_data->components & (VB_HAS_TEXMTXIDX0 << i));

      if ((uid_data->components & (VB_HAS_UV0 << i)) != 0 || has_texmtx != 0)
      {
        out.Write("ATTRIBUTE_LOCATION({:s}) in float{} rawtex{};\n", ShaderAttrib::TexCoord0 + i,
                  has_texmtx != 0 ? 3 : 2, i);
      }
    }
  }
  else
  {
    // Can't use float3, etc because we want 4-byte alignment
    out.Write(
        "uint4 unpack_ubyte4(uint value) {{\n"
        "  return uint4(value & 0xffu, (value >> 8) & 0xffu, (value >> 16) & 0xffu, value >> 24);\n"
        "}}\n\n"
        "struct InputData {{\n");
    if (uid_data->components & VB_HAS_POSMTXIDX)
    {
      out.Write("  uint posmtx;\n");
      // Note: posmtx is handled in the matrix transform functions and
      // doesn't need to be added to 'input_extract'
    }
    if (uid_data->position_has_3_elems)
    {
      out.Write("  float pos0;\n"
                "  float pos1;\n"
                "  float pos2;\n");
      input_extract.Write("float4 rawpos = float4(i.pos0, i.pos1, i.pos2, 1.0f);\n");
    }
    else
    {
      out.Write("  float pos0;\n"
                "  float pos1;\n");
      input_extract.Write("float4 rawpos = float4(i.pos0, i.pos1, 0.0f, 1.0f);\n");
    }
    std::array<std::string_view, 3> names = {"normal", "binormal", "tangent"};
    for (int i = 0; i < 3; i++)
    {
      if (uid_data->components & (VB_HAS_NORMAL << i))
      {
        out.Write("  float {0}0;\n"
                  "  float {0}1;\n"
                  "  float {0}2;\n",
                  names[i]);
        input_extract.Write("float3 raw{0} = float3(i.{0}0, i.{0}1, i.{0}2);\n", names[i]);
      }
    }
    for (int i = 0; i < 2; i++)
    {
      if (uid_data->components & (VB_HAS_COL0 << i))
      {
        out.Write("  uint color{};\n", i);
        input_extract.Write("float4 rawcolor{0} = float4(unpack_ubyte4(i.color{0})) / 255.0f;\n",
                            i);
      }
    }
    for (int i = 0; i < 8; i++)
    {
      if (uid_data->components & (VB_HAS_UV0 << i))
      {
        u32 ncomponents = (uid_data->texcoord_elem_count >> (2 * i)) & 3;
        if (ncomponents < 2)
        {
          out.Write("  float tex{};\n", i);
          input_extract.Write("float3 rawtex{0} = float3(i.tex{0}, 0.0f, 0.0f);\n", i);
        }
        else if (ncomponents == 2)
        {
          out.Write("  float tex{0}_0;\n"
                    "  float tex{0}_1;\n",
                    i);
          input_extract.Write("float3 rawtex{0} = float3(i.tex{0}_0, i.tex{0}_1, 0.0f);\n", i);
        }
        else
        {
          out.Write("  float tex{0}_0;\n"
                    "  float tex{0}_1;\n"
                    "  float tex{0}_2;\n",
                    i);
          input_extract.Write("float3 rawtex{0} = float3(i.tex{0}_0, i.tex{0}_1, i.tex{0}_2);\n",
                              i);
        }
      }
    }
    out.Write("}};\n\n"
              "SSBO_BINDING(1) readonly restrict buffer InputBuffer {{\n"
              "  InputData input_buffer[];\n"
              "}};\n\n");
  }

  if (host_config.backend_geometry_shaders)
  {
    out.Write("VARYING_LOCATION(0) out VertexData {{\n");
    GenerateVSOutputMembers(out, api_type, uid_data->numTexGens, host_config,
                            GetInterpolationQualifier(msaa, ssaa, true, false),
                            ShaderStage::Vertex, true);
    out.Write("}} vs;\n");
  }
  else
  {
    // Let's set up attributes
    u32 counter = 0;
    out.Write("VARYING_LOCATION({}) {} out float4 colors_0;\n", counter++,
              GetInterpolationQualifier(msaa, ssaa));
    out.Write("VARYING_LOCATION({}) {} out float4 colors_1;\n", counter++,
              GetInterpolationQualifier(msaa, ssaa));
    for (u32 i = 0; i < uid_data->numTexGens; ++i)
    {
      out.Write("VARYING_LOCATION({}) {} out float3 tex{};\n", counter++,
                GetInterpolationQualifier(msaa, ssaa), i);
    }
    if (!host_config.fast_depth_calc)
    {
      out.Write("VARYING_LOCATION({}) {} out float4 clipPos;\n", counter++,
                GetInterpolationQualifier(msaa, ssaa));
    }
    if (per_pixel_lighting)
    {
      out.Write("VARYING_LOCATION({}) {} out float3 Normal;\n", counter++,
                GetInterpolationQualifier(msaa, ssaa));
      out.Write("VARYING_LOCATION({}) {} out float3 WorldPos;\n", counter++,
                GetInterpolationQualifier(msaa, ssaa));
    }
    if (host_config.vr_stereo)
    {
      // Game's backend-convention NDC depth for exact virtual-screen depth export.
      out.Write("VARYING_LOCATION({}) flat out float vr_depth;\n", counter++);
    }
  }

  // Note: this is done after to ensure above global variables are accessible
  WritePrimitiveExpand(api_type, host_config, uid_data, out);
  WriteTransformMatrices(api_type, host_config, uid_data, out);
  WriteTexCoordTransforms(api_type, host_config, uid_data, out);
  WriteVertexDefines(api_type, host_config, uid_data, out);
  WriteVertexStructs(api_type, host_config, uid_data, out);
  WriteEmulatedVertexBodyHeader(api_type, host_config, uid_data, out);

  if (custom_contents.shader.empty())
  {
    out.Write("void process_vertex(in DolphinVertexInput vertex_input, out DolphinVertexOutput "
              "vertex_output)\n");
    out.Write("{{\n");

    out.Write("\tdolphin_process_emulated_vertex(vertex_input, vertex_output);\n");

    out.Write("}}\n");
  }
  else
  {
    out.Write("{}\n", custom_contents.shader);
  }

  out.Write("void main()\n{{\n");

  if (uid_data->vs_expand != VSExpand::None)
  {
    out.Write("InputData i = dolphin_primitive_expand_data(0);\n"
              "{}",
              input_extract.GetBuffer());
  }

  out.Write("VS_OUTPUT o;\n");

  // xfmem.numColorChans controls the number of color channels available to TEV, but we still need
  // to generate all channels here, as it can be used in texgen. Cel-damage is an example of this.
  out.Write("float4 vertex_color_0, vertex_color_1;\n");

  // To use color 1, the vertex descriptor must have color 0 and 1.
  // If color 1 is present but not color 0, it is used for lighting channel 0.
  const bool use_color_1 =
      (uid_data->components & (VB_HAS_COL0 | VB_HAS_COL1)) == (VB_HAS_COL0 | VB_HAS_COL1);
  for (u32 color = 0; color < NUM_XF_COLOR_CHANNELS; color++)
  {
    if ((color == 0 || use_color_1) && (uid_data->components & (VB_HAS_COL0 << color)) != 0)
    {
      // Use color0 for channel 0, and color1 for channel 1 if both colors 0 and 1 are present.
      out.Write("vertex_color_{0} = rawcolor{0};\n", color);
    }
    else if (color == 0 && (uid_data->components & VB_HAS_COL1) != 0)
    {
      // Use color1 for channel 0 if color0 is not present.
      out.Write("vertex_color_{} = rawcolor1;\n", color);
    }
    else
    {
      out.Write("vertex_color_{0} = missing_color_value;\n", color);
    }
  }

  out.Write("\tDolphinVertexInput vertex_input;\n");
  out.Write("\tvertex_input.color_0 = vertex_color_0;\n");
  out.Write("\tvertex_input.color_1 = vertex_color_1;\n");
  out.Write("\tvertex_input.position = rawpos;\n");

  if ((uid_data->components & VB_HAS_NORMAL) != 0)
  {
    out.Write("\tvertex_input.normal = rawnormal;\n");
  }
  else
  {
    out.Write("\tvertex_input.normal = " I_CACHED_NORMAL ".xyz;\n");
  }

  if ((uid_data->components & VB_HAS_BINORMAL) != 0)
  {
    out.Write("\tvertex_input.binormal = rawbinormal;\n");
  }
  else
  {
    out.Write("\tvertex_input.binormal = " I_CACHED_BINORMAL ".xyz;\n");
  }

  if ((uid_data->components & VB_HAS_TANGENT) != 0)
  {
    out.Write("\tvertex_input.tangent = rawtangent;\n");
  }
  else
  {
    out.Write("\tvertex_input.tangent = " I_CACHED_TANGENT ".xyz;\n");
  }

  for (u32 i = 0; i < uid_data->numTexGens; ++i)
  {
    auto& texinfo = uid_data->texMtxInfo[i];

    out.Write("\t{{\n");
    out.Write("\t\tvec4 coord = vec4(0.0, 0.0, 1.0, 1.0);\n");
    switch (texinfo.sourcerow)
    {
    case SourceRow::Geom:
      out.Write("\t\tcoord.xyz = rawpos.xyz;\n");
      break;
    case SourceRow::Normal:
      if ((uid_data->components & VB_HAS_NORMAL) != 0)
      {
        out.Write("\t\tcoord.xyz = rawnormal.xyz;\n");
      }
      break;
    case SourceRow::Colors:
      ASSERT(texinfo.texgentype == TexGenType::Color0 || texinfo.texgentype == TexGenType::Color1);
      break;
    case SourceRow::BinormalT:
      if ((uid_data->components & VB_HAS_TANGENT) != 0)
      {
        out.Write("\t\tcoord.xyz = rawtangent.xyz;\n");
      }
      break;
    case SourceRow::BinormalB:
      if ((uid_data->components & VB_HAS_BINORMAL) != 0)
      {
        out.Write("\t\tcoord.xyz = rawbinormal.xyz;\n");
      }
      break;
    default:
      ASSERT(texinfo.sourcerow >= SourceRow::Tex0 && texinfo.sourcerow <= SourceRow::Tex7);
      u32 texnum = static_cast<u32>(texinfo.sourcerow) - static_cast<u32>(SourceRow::Tex0);
      if ((uid_data->components & (VB_HAS_UV0 << (texnum))) != 0)
      {
        out.Write("\t\tcoord = vec4(rawtex{}.x, rawtex{}.y, 1.0, 1.0);\n", texnum, texnum);
      }
      break;
    }
    // Input form of AB11 sets z element to 1.0

    if (texinfo.inputform == TexInputForm::AB11)
      out.Write("\t\tcoord.z = 1.0;\n");

    // Convert NaNs to 1 - needed to fix eyelids in Shadow the Hedgehog during cutscenes
    // See https://bugs.dolphin-emu.org/issues/11458
    out.Write("\t\t// Convert NaN to 1\n");
    out.Write("\t\tif (dolphin_isnan(coord.x)) coord.x = 1.0;\n");
    out.Write("\t\tif (dolphin_isnan(coord.y)) coord.y = 1.0;\n");
    out.Write("\t\tif (dolphin_isnan(coord.z)) coord.z = 1.0;\n");

    out.Write("\t\tvertex_input.texture_coord_{0} = coord;\n", i);
    out.Write("\t}}\n");
  }

  // Initialize other texture coordinates that are unused
  for (u32 i = uid_data->numTexGens; i < 8; i++)
  {
    out.Write("\tvertex_input.texture_coord_{0} = vec4(0, 0, 0, 0);\n", i);
  }

  out.Write("\tDolphinVertexOutput vertex_output;\n");
  out.Write("\tprocess_vertex(vertex_input, vertex_output);\n");

  out.Write("\to.pos = vec4(dot(" I_PROJECTION "[0], vertex_output.position), dot(" I_PROJECTION
            "[1], vertex_output.position), dot(" I_PROJECTION
            "[2], vertex_output.position), dot(" I_PROJECTION "[3], vertex_output.position));\n");
  for (u32 i = 0; i < uid_data->numTexGens; ++i)
  {
    out.Write("\to.tex{0} = vertex_output.texture_coord_{0};\n", i);
  }

  out.Write("\to.colors_0 = vertex_output.color_0;\n");
  out.Write("\to.colors_1 = vertex_output.color_1;\n");
  if (per_pixel_lighting)
  {
    out.Write("\to.Normal = vertex_output.normal;\n");

    // TODO: Rename, this is actually in Viewspace...
    out.Write("\to.WorldPos = vertex_output.position.xyz;\n");
  }

  if (host_config.vr_stereo)
  {
    // Pass the view-space position (before projection) to the GS so it can apply
    // the correct per-eye asymmetric HMD projection for each eye independently.
    out.Write("\to.viewPos = vertex_output.position;\n");
  }

  if (uid_data->vs_expand == VSExpand::Line)
  {
    out.Write("bool is_bottom = (gl_VertexID & 2) != 0;\n");
    out.Write("// Line expansion\n"
              "int id_offset = 0;\n"
              "if (is_bottom) {{\n"
              "  id_offset -= 1;\n"
              "}} else {{\n"
              "  id_offset += 1;\n"
              "}}\n"
              "InputData other = dolphin_primitive_expand_data(id_offset);\n");
    if (uid_data->position_has_3_elems)
      out.Write("float4 other_pos = float4(other.pos0, other.pos1, other.pos2, 1.0f);\n");
    else
      out.Write("float4 other_pos = float4(other.pos0, other.pos1, 0.0f, 1.0f);\n");
    if (uid_data->components & VB_HAS_POSMTXIDX)
    {
      out.Write("uint other_posidx = other.posmtx & 0xff;\n"
                "float4 other_p0 = " I_TRANSFORMMATRICES "[other_posidx];\n"
                "float4 other_p1 = " I_TRANSFORMMATRICES "[other_posidx + 1];\n"
                "float4 other_p2 = " I_TRANSFORMMATRICES "[other_posidx + 2];\n"
                "other_pos = float4(dot(other_p0, other_pos), dot(other_p1, other_pos), "
                "dot(other_p2, other_pos), 1.0f);\n");
    }
    else
    {
      out.Write("other_pos = vec4(other_pos * dolphin_position_matrix(), 1.0);\n");
    }

    // Variable needed by GenerateVSLineExpansion
    out.Write("bool is_right = (gl_VertexID & 1) != 0;\n");
    GenerateVSLineExpansion(out, "", uid_data->numTexGens);
  }
  else if (uid_data->vs_expand == VSExpand::Point)
  {
    // Variables needed by GenerateVSPointExpansion
    out.Write("bool is_bottom = (gl_VertexID & 2) != 0;\n");
    out.Write("bool is_right = (gl_VertexID & 1) != 0;\n");
    out.Write("// Point expansion\n");
    GenerateVSPointExpansion(out, "", uid_data->numTexGens);
  }

  // Multiview VR path: the VS replaces the projected o.pos with per-eye HMD clip
  // coordinates (see GenerateVRMultiviewVSProjection). The emitted vr_pos_replaced
  // flag suppresses the trailing depth-range/pixel-center fixups below to avoid
  // double-remapping.
  if (UseVRMultiviewVSProjection(api_type, host_config))
    GenerateVRMultiviewVSProjection(out, api_type, host_config, "vertex_output.position");

  // clipPos/w needs to be done in pixel shader, not here
  if (!host_config.fast_depth_calc)
    out.Write("o.clipPos = o.pos;\n");

  // If we can disable the incorrect depth clipping planes using depth clamping, then we can do
  // our own depth clipping and calculate the depth range before the perspective divide if
  // necessary.
  if (host_config.backend_depth_clamp)
  {
    // Since we're adjusting z for the depth range before the perspective divide, we have to do our
    // own clipping. We want to clip so that -w <= z <= 0, which matches the console -1..0 range.
    // We adjust our depth value for clipping purposes to match the perspective projection in the
    // software backend, which is a hack to fix Sonic Adventure and Unleashed games.
    out.Write("float clipDepth = o.pos.z * (1.0 - 1e-7);\n"
              "float clipDist0 = clipDepth + o.pos.w;\n"  // Near: z < -w
              "float clipDist1 = -clipDepth;\n");         // Far: z > 0

    // Under multiview VR the VR projection already produced per-eye clip-space
    // values that don't follow the console depth convention; mirror the GS Vulkan
    // VR path and just disable user clipping (clipDist = 1.0) when the VR block
    // replaced o.pos. This is what the working GS path does on Vulkan.
    if (UseVRMultiviewVSProjection(api_type, host_config))
    {
      out.Write("if (vr_pos_replaced) {{\n"
                "  clipDist0 = 1.0;\n"
                "  clipDist1 = 1.0;\n"
                "}}\n");
    }

    // NOTE: under multiview the GS is gone for triangles — the VS itself writes
    // gl_ClipDistance below, so we don't propagate via the o.clipDist members.
    if (host_config.backend_geometry_shaders && !host_config.vk_multiview)
    {
      out.Write("o.clipDist0 = clipDist0;\n"
                "o.clipDist1 = clipDist1;\n");
    }
  }
  else
  {
    // Same depth adjustment for Sonic. Without depth clamping, it unfortunately
    // affects non-clipping uses of depth too.
    out.Write("o.pos.z = o.pos.z * (1.0 - 1e-7);\n");
  }

  // Skip the trailing depth-range / pixel-center fixups when the VR multiview block
  // already replaced o.pos with per-eye HMD coordinates — those branches re-applied
  // the same fixups using the VR uniforms (cvr_depth, cvr_pixelcenter) and reapplying
  // here would double-transform the position.
  const bool guard_with_vr = UseVRMultiviewVSProjection(api_type, host_config);
  if (guard_with_vr)
    out.Write("if (!vr_pos_replaced) {{\n");

  // Write the true depth value. If the game uses depth textures, then the pixel shader will
  // override it with the correct values if not then early z culling will improve speed.
  // There are two different ways to do this, when the depth range is oversized, we process
  // the depth range in the vertex shader, if not we let the host driver handle it.
  //
  // Adjust z for the depth range. We're using an equation which incorporates a depth inversion,
  // so we can map the console -1..0 range to the 0..1 range used in the depth buffer.
  // We have to handle the depth range in the vertex shader instead of after the perspective
  // divide, because some games will use a depth range larger than what is allowed by the
  // graphics API. These large depth ranges will still be clipped to the 0..1 range, so these
  // games effectively add a depth bias to the values written to the depth buffer.
  out.Write("o.pos.z = o.pos.w * " I_PIXELCENTERCORRECTION ".w - "
            "o.pos.z * " I_PIXELCENTERCORRECTION ".z;\n");

  if (host_config.vr_stereo)
  {
    // Capture the game's exact backend-convention NDC depth (0..1) before the optional -1..1
    // clip-control expansion below. For ortho draws o.pos.w == 1, so the divide is bit-exact —
    // this is the basis of deterministic virtual-screen depth export (vr_screen_exact_depth).
    // Under multiview this sits in the !vr_pos_replaced guard; the VR branches assign their own.
    out.Write("o.vr_depth = (abs(o.pos.w) > 1e-6) ? o.pos.z / o.pos.w : 0.0;\n");
  }

  if (!host_config.backend_clip_control)
  {
    // If the graphics API doesn't support a depth range of 0..1, then we need to map z to
    // the -1..1 range. Unfortunately we have to use a subtraction, which is a lossy floating-point
    // operation that can introduce a round-trip error.
    out.Write("o.pos.z = o.pos.z * 2.0 - o.pos.w;\n");
  }

  // Correct for negative viewports by mirroring all vertices. We need to negate the height here,
  // since the viewport height is already negated by the render backend.
  out.Write("o.pos.xy *= sign(" I_PIXELCENTERCORRECTION ".xy * float2(1.0, -1.0));\n");

  // The console GPU places the pixel center at 7/12 in screen space unless
  // antialiasing is enabled, while D3D and OpenGL place it at 0.5. This results
  // in some primitives being placed one pixel too far to the bottom-right,
  // which in turn can be critical if it happens for clear quads.
  // Hence, we compensate for this pixel center difference so that primitives
  // get rasterized correctly.
  out.Write("o.pos.xy = o.pos.xy - o.pos.w * " I_PIXELCENTERCORRECTION ".xy;\n");

  if (guard_with_vr)
    out.Write("}}\n");

  if (vertex_rounding && !host_config.vr_stereo)
  {
    // By now our position is in clip space
    // however, higher resolutions than the Wii outputs
    // cause an additional pixel offset
    // due to a higher pixel density
    // we need to correct this by converting our
    // clip-space position into the Wii's screen-space
    // acquire the right pixel and then convert it back
    out.Write("if (o.pos.w == 1.0f)\n"
              "{{\n"

              "\tfloat ss_pixel_x = ((o.pos.x + 1.0f) * (" I_VIEWPORT_SIZE ".x * 0.5f));\n"
              "\tfloat ss_pixel_y = ((o.pos.y + 1.0f) * (" I_VIEWPORT_SIZE ".y * 0.5f));\n"

              "\tss_pixel_x = round(ss_pixel_x);\n"
              "\tss_pixel_y = round(ss_pixel_y);\n"

              "\to.pos.x = ((ss_pixel_x / (" I_VIEWPORT_SIZE ".x * 0.5f)) - 1.0f);\n"
              "\to.pos.y = ((ss_pixel_y / (" I_VIEWPORT_SIZE ".y * 0.5f)) - 1.0f);\n"
              "}}\n");
  }

  if (host_config.backend_geometry_shaders)
  {
    AssignVSOutputMembers(out, "vs", "o", uid_data->numTexGens, host_config);
  }
  else
  {
    // TODO: Pass interface blocks between shader stages even if geometry shaders
    // are not supported, however that will require at least OpenGL 3.2 support.
    for (u32 i = 0; i < uid_data->numTexGens; ++i)
      out.Write("tex{}.xyz = o.tex{};\n", i, i);
    if (!host_config.fast_depth_calc)
      out.Write("clipPos = o.clipPos;\n");
    if (per_pixel_lighting)
    {
      out.Write("Normal = o.Normal;\n"
                "WorldPos = o.WorldPos;\n");
    }
    out.Write("colors_0 = o.colors_0;\n"
              "colors_1 = o.colors_1;\n");
    if (host_config.vr_stereo)
      out.Write("vr_depth = o.vr_depth;\n");
  }

  if (host_config.backend_depth_clamp)
  {
    out.Write("gl_ClipDistance[0] = clipDist0;\n"
              "gl_ClipDistance[1] = clipDist1;\n");
    if (UseVRMultiviewVSProjection(api_type, host_config))
    {
      out.Write("gl_ClipDistance[2] = vr_pane_clip_dist.x;\n"
                "gl_ClipDistance[3] = vr_pane_clip_dist.y;\n"
                "gl_ClipDistance[4] = vr_pane_clip_dist.z;\n"
                "gl_ClipDistance[5] = vr_pane_clip_dist.w;\n");
    }
  }

  // Vulkan NDC space has Y pointing down (right-handed NDC space).
  if (api_type == APIType::Vulkan)
    out.Write("gl_Position = float4(o.pos.x, -o.pos.y, o.pos.z, o.pos.w);\n");
  else
    out.Write("gl_Position = o.pos;\n");
  out.Write("}}\n");

  return out;
}

void WriteVertexBody(APIType api_type, const ShaderHostConfig& host_config,
                     const vertex_shader_uid_data* uid_data, ShaderCode& out)
{
  out.Write(
      "\tvertex_output.position = vec4(vertex_input.position * dolphin_position_matrix(), 1.0);\n");

  out.Write("\tvertex_output.normal = normalize(vertex_input.normal * dolphin_normal_matrix());\n");

  for (u32 chan = 0; chan < NUM_XF_COLOR_CHANNELS; chan++)
  {
    out.Write(
        "\tvec4 vertex_lighting_{0} = dolphin_calculate_lighting_chn{0}(vertex_input.color_{0}, "
        "vertex_output.position.xyz, vertex_output.normal);\n",
        chan);
    out.Write("\tvertex_output.color_{0} = vertex_lighting_{0};\n", chan);
  }

  if (host_config.per_pixel_lighting)
  {
    // When per-pixel lighting is enabled, the vertex colors are passed through
    // unmodified so we can evaluate the lighting in the pixel shader.
    out.Write("\tvertex_output.color_0 = vertex_input.color_0;\n");
    out.Write("\tvertex_output.color_1 = vertex_input.color_1;\n");
  }
  else
  {
    // The number of colors available to TEV is determined by numColorChans.
    // We have to provide the fields to match the interface, so set to zero if it's not enabled.
    if (uid_data->numColorChans == 0)
      out.Write("\tvertex_output.color_0 = vec4(0.0, 0.0, 0.0, 0.0);\n");
    if (uid_data->numColorChans <= 1)
      out.Write("\tvertex_output.color_1 = vec4(0.0, 0.0, 0.0, 0.0);\n");
  }

  for (u32 i = 0; i < uid_data->numTexGens; ++i)
  {
    auto& texinfo = uid_data->texMtxInfo[i];

    switch (texinfo.texgentype)
    {
    case TexGenType::EmbossMap:  // calculate tex coords into bump map

      out.Write("\t{{\n");
      // transform the light dir into tangent space
      out.Write("\t\tvec3 ldir = normalize(" LIGHT_POS ".xyz - vertex_output.position.xyz);\n",
                LIGHT_POS_PARAMS(texinfo.embosslightshift));

      out.Write("\t\tvec3 tangent = vertex_input.tangent * dolphin_normal_matrix();\n");
      out.Write("\t\tvec3 binormal = vertex_input.binormal * dolphin_normal_matrix();\n");
      out.Write("\t\tvertex_output.texture_coord_{}.xyz = vertex_output.texture_coord_{}.xyz + "
                "vec3(dot(ldir, tangent), "
                "dot(ldir, binormal), 0.0);\n",
                i, texinfo.embosssourceshift);
      out.Write("\t}}\n");
      break;
    case TexGenType::Color0:
      out.Write("\tvertex_output.texture_coord_{}.xyz = vec3(vertex_lighting_0.x, "
                "vertex_lighting_0.y, 1);\n",
                i);
      break;
    case TexGenType::Color1:
      out.Write("\tvertex_output.texture_coord_{}.xyz = vec3(vertex_lighting_1.x, "
                "vertex_lighting_1.y, 1);\n",
                i);
      break;
    case TexGenType::Regular:
      out.Write("\tvertex_output.texture_coord_{0} = "
                "dolphin_transform_texcoord{0}(vertex_input.texture_coord_{0});\n",
                i);
      break;
    };
  }

  // Fill out output that is unused
  for (u32 i = uid_data->numTexGens; i < 8; i++)
  {
    out.Write("\tvertex_output.texture_coord_{0} = vec3(0, 0, 0);\n", i);
  }
}
