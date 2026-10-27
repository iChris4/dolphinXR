// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/RuntimeElementMatcher.h"

namespace RuntimeElementMatcher
{
bool HasActiveGroups(const ShaderHunter::RuntimeElementSignature& signature)
{
  return signature.valid &&
         (signature.use_projection || signature.use_projection_type || signature.use_layer ||
          signature.use_viewport || signature.use_scissor || signature.use_render_state ||
          signature.use_load);
}

bool Matches(const ShaderHunter::RuntimeElementSignature& pattern,
             const ShaderHunter::RuntimeElementSignature& candidate)
{
  if (!pattern.valid || !candidate.valid)
    return false;

  // Coarse projection filter: match only the projection kind (perspective vs orthographic),
  // ignoring the exact FOV/bounds values. Robust against value drift and the compact OpenXR
  // projection scale, where exact comparison of the detailed values fails.
  if (pattern.use_projection_type)
  {
    if (pattern.perspective != candidate.perspective)
      return false;
  }

  if (pattern.use_projection)
  {
    if (pattern.perspective != candidate.perspective)
      return false;

    if (pattern.perspective)
    {
      if (pattern.perspective_hfov_x100 != candidate.perspective_hfov_x100 ||
          pattern.perspective_vfov_x100 != candidate.perspective_vfov_x100 ||
          pattern.perspective_near_x1000 != candidate.perspective_near_x1000 ||
          pattern.perspective_far_x100 != candidate.perspective_far_x100)
      {
        return false;
      }
    }
    else
    {
      if (pattern.ortho_left_x100 != candidate.ortho_left_x100 ||
          pattern.ortho_right_x100 != candidate.ortho_right_x100 ||
          pattern.ortho_top_x100 != candidate.ortho_top_x100 ||
          pattern.ortho_bottom_x100 != candidate.ortho_bottom_x100)
      {
        return false;
      }
    }
  }

  if (pattern.use_layer)
  {
    if (candidate.perspective || pattern.perspective)
      return false;
    if (pattern.ortho_layer != candidate.ortho_layer)
      return false;
  }

  if (pattern.use_load)
  {
    if (candidate.projection_load < pattern.load_min ||
        (pattern.load_max >= 0 && candidate.projection_load > pattern.load_max))
    {
      return false;
    }
  }

  if (pattern.use_viewport)
  {
    if (pattern.viewport_x != candidate.viewport_x ||
        pattern.viewport_y != candidate.viewport_y ||
        pattern.viewport_width != candidate.viewport_width ||
        pattern.viewport_height != candidate.viewport_height)
    {
      return false;
    }
  }

  if (pattern.use_scissor)
  {
    if (pattern.scissor_left != candidate.scissor_left ||
        pattern.scissor_top != candidate.scissor_top ||
        pattern.scissor_right != candidate.scissor_right ||
        pattern.scissor_bottom != candidate.scissor_bottom)
    {
      return false;
    }
  }

  if (pattern.use_render_state)
  {
    // The alpha test's two reference values are its low 16 bits.
    const u32 alpha_test_mask = pattern.any_alpha_ref ? 0xFFFF0000u : 0xFFFFFFFFu;
    if ((pattern.alpha_test_hex & alpha_test_mask) !=
            (candidate.alpha_test_hex & alpha_test_mask) ||
        pattern.ztest != candidate.ztest || pattern.zupdate != candidate.zupdate ||
        pattern.zfunc != candidate.zfunc ||
        pattern.blend_color_update != candidate.blend_color_update ||
        (!pattern.any_alpha_update && pattern.blend_alpha_update != candidate.blend_alpha_update))
    {
      return false;
    }
  }

  if (pattern.efb_copy >= 0 && pattern.efb_copy != candidate.efb_copy)
    return false;

  return true;
}
}  // namespace RuntimeElementMatcher
