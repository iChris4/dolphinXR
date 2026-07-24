// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VertexManagerBase.h"

#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <string>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Contains.h"
#include "Common/EnumMap.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/SmallVector.h"

#include "Core/DolphinAnalytics.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/DataReader.h"
#include "VideoCommon/ElementsGroupManager.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/GraphicsModSystem/Runtime/CustomShaderCache.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModActionData.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModManager.h"
#include "VideoCommon/IndexGenerator.h"
#include "VideoCommon/MetroidElementClassifier.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/PerfQueryBase.h"
#include "VideoCommon/PixelShaderGen.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VR/OpenXRManager.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VR/VRFrameRegion.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/CullingCodeFinder.h"
#include "VideoCommon/HideObjectEngine.h"
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/TextureElementManager.h"
#include "VideoCommon/XFStateManager.h"

#include "Common/Hash.h"
#include "Core/ConfigManager.h"

std::unique_ptr<VertexManagerBase> g_vertex_manager;

using OpcodeDecoder::Primitive;

namespace
{
struct MetroidLayerBehavior
{
  bool skip = false;
  ShaderHunter::HandlingType handling = ShaderHunter::HandlingType::Skip;
};

struct MetroidHydraHudSettings
{
  bool enabled = false;
  bool perspective_hud = false;
  float scale = 1.0f;
  float width = 1.0f;
  float height = 1.0f;
  float up = 0.0f;
  float right = 0.0f;
};

ShaderHunter::RuntimeElementSignature BuildRuntimeElementSignature(const XFMemory& xf_memory,
                                                                   const BPMemory& bp_memory)
{
  ShaderHunter::RuntimeElementSignature signature;
  signature.valid = true;
  signature.perspective = xf_memory.projection.type == ProjectionType::Perspective;

  const float* projection = xf_memory.projection.rawProjection.data();
  if (signature.perspective)
  {
    const float hfov = 2.0f * std::atan(1.0f / projection[0]) * 180.0f / 3.14159265f;
    const float vfov = 2.0f * std::atan(1.0f / projection[2]) * 180.0f / 3.14159265f;
    const float far_plane = projection[5] / projection[4];
    const float near_plane = far_plane * projection[4] / (projection[4] - 1.0f);
    signature.perspective_hfov_x100 = static_cast<int>(std::lround(hfov * 100.0f));
    signature.perspective_vfov_x100 = static_cast<int>(std::lround(vfov * 100.0f));
    signature.perspective_near_x1000 = static_cast<int>(std::lround(near_plane * 1000.0f));
    signature.perspective_far_x100 = static_cast<int>(std::lround(far_plane * 100.0f));
  }
  else
  {
    const float left = -(projection[1] + 1.0f) / projection[0];
    const float right = left + 2.0f / projection[0];
    const float bottom = -(projection[3] + 1.0f) / projection[2];
    const float top = bottom + 2.0f / projection[2];
    signature.ortho_left_x100 = static_cast<int>(std::lround(left * 100.0f));
    signature.ortho_right_x100 = static_cast<int>(std::lround(right * 100.0f));
    signature.ortho_top_x100 = static_cast<int>(std::lround(top * 100.0f));
    signature.ortho_bottom_x100 = static_cast<int>(std::lround(bottom * 100.0f));
    // The per-draw ortho layer counter is gone (it only ever advanced under the removed Auto
    // Layer Spread option). Kept at 0 so existing INIs with sig_use_layer=1/sig_layer=0 —
    // i.e. every signature captured with the default settings — keep matching.
    signature.ortho_layer = 0;
  }

  const auto& viewport = xf_memory.viewport;
  signature.viewport_x = static_cast<int>(std::lround(viewport.xOrig));
  signature.viewport_y = static_cast<int>(std::lround(viewport.yOrig));
  signature.viewport_width = static_cast<int>(std::lround(std::abs(viewport.wd)));
  signature.viewport_height = static_cast<int>(std::lround(std::abs(viewport.ht)));
  signature.scissor_left = bp_memory.scissorTL.x;
  signature.scissor_top = bp_memory.scissorTL.y;
  signature.scissor_right = bp_memory.scissorBR.x;
  signature.scissor_bottom = bp_memory.scissorBR.y;
  signature.alpha_test_hex = bp_memory.alpha_test.hex;
  signature.ztest = bp_memory.zmode.test_enable != 0;
  signature.zupdate = bp_memory.zmode.update_enable != 0;
  signature.zfunc = static_cast<int>(bp_memory.zmode.func.Value());
  signature.blend_color_update = bp_memory.blendmode.color_update != 0;
  signature.blend_alpha_update = bp_memory.blendmode.alpha_update != 0;
  return signature;
}

MetroidProjectionMetrics BuildMetroidProjectionMetrics(const XFMemory& xf_memory,
                                                       u32 projection_sequence)
{
  MetroidProjectionMetrics metrics;
  metrics.projection_sequence = projection_sequence;
  metrics.perspective = xf_memory.projection.type == ProjectionType::Perspective;

  const float* projection = xf_memory.projection.rawProjection.data();
  if (metrics.perspective)
  {
    metrics.hfov = 2.0f * std::atan(1.0f / projection[0]) * 180.0f / 3.14159265f;
    metrics.vfov = 2.0f * std::atan(1.0f / projection[2]) * 180.0f / 3.14159265f;
    metrics.zfar = projection[5] / projection[4];
    metrics.znear = metrics.zfar * projection[4] / (projection[4] - 1.0f);
  }
  else
  {
    metrics.left = -(projection[1] + 1.0f) / projection[0];
    metrics.right = metrics.left + 2.0f / projection[0];
    metrics.bottom = -(projection[3] + 1.0f) / projection[2];
    metrics.top = metrics.bottom + 2.0f / projection[2];
    metrics.zfar = projection[5] / projection[4];
    metrics.znear = (1.0f + projection[4] * metrics.zfar) / projection[4];
  }

  return metrics;
}

MetroidElementProfile GetMetroidProfileForGameID(std::string_view game_id)
{
  if (game_id.starts_with("GM8") || game_id.starts_with("D43") || game_id.starts_with("D93"))
    return MetroidElementProfile::Prime1GC;
  if (game_id.starts_with("G2M") || game_id.starts_with("P2M"))
    return MetroidElementProfile::Prime2GC;
  if (game_id.starts_with("R3I"))
    return MetroidElementProfile::Prime1Wii;
  if (game_id.starts_with("R32"))
    return MetroidElementProfile::Prime2Wii;
  if (game_id.starts_with("RM3"))
    return MetroidElementProfile::Prime3;
  if (game_id.starts_with("R3M") || game_id.starts_with("R3O"))
    return MetroidElementProfile::TrilogyAuto;

  return MetroidElementProfile::None;
}

MetroidElementClassifier& GetMetroidElementClassifier()
{
  static MetroidElementClassifier classifier;
  return classifier;
}

constexpr int METROID_HUD_CONTEXT_DEFAULT = 0;
constexpr int METROID_HUD_CONTEXT_COMBAT = 1;
constexpr int METROID_HUD_CONTEXT_MENU = 2;

bool IsMetroidPrime2Profile(MetroidElementProfile profile)
{
  return profile == MetroidElementProfile::Prime2GC ||
         profile == MetroidElementProfile::Prime2Wii;
}

bool IsMetroidPrime1Profile(MetroidElementProfile profile)
{
  return profile == MetroidElementProfile::Prime1GC ||
         profile == MetroidElementProfile::Prime1Wii;
}

MetroidHydraHudSettings GetMetroidHydraHudSettings(MetroidElementProfile profile,
                                                   MetroidElementLayer layer,
                                                   std::string_view game_id)
{
  if (!IsMetroidPrime1Profile(profile))
    return {};

  if (!(game_id.starts_with("GM8") || game_id.starts_with("D93") || game_id.starts_with("R3I")))
    return {};

  switch (layer)
  {
  case MetroidElementLayer::HUD:
  case MetroidElementLayer::UnknownHUD:
  case MetroidElementLayer::MapOrHint:
  case MetroidElementLayer::Map:
    return {.enabled = true,
            .perspective_hud = true,
            .scale = 30.0f,
            .width = 0.79f,
            .height = 0.79f};

  case MetroidElementLayer::XRayHUD:
    // The classifier uses XRayHUD for the compact visor/reticle HUD projection in both X-Ray and
    // Thermal visor modes.  Those draws still contain 3D CMDL reticle pieces (beam/visor boxes),
    // so keep them on the Hydra perspective-HUD path instead of flattening them as a 2D overlay.
    return {.enabled = true,
            .perspective_hud = true,
            .scale = 30.0f,
            .width = 0.79f,
            .height = 0.79f};

  case MetroidElementLayer::DarkVisorHUD:
    return {.enabled = true,
            .perspective_hud = true,
            .scale = 5.0f,
            .width = 0.79f,
            .height = 0.79f};

  case MetroidElementLayer::VisorRadarHint:
    return {.enabled = true,
            .perspective_hud = true,
            .scale = 32.0f,
            .width = 0.79f,
            .height = 0.79f};

  case MetroidElementLayer::RadarDot:
    return {.enabled = true,
            .perspective_hud = true,
            .scale = 36.0f,
            .width = 0.79f,
            .height = 0.79f};

  case MetroidElementLayer::Visor:
    return {.enabled = true, .perspective_hud = true, .scale = 90.0f};

  case MetroidElementLayer::Helmet:
    return {.enabled = true,
            .perspective_hud = true,
            .scale = 100.0f,
            .width = 1.7f,
            .height = 1.7f};

  case MetroidElementLayer::ScanText:
    return {.enabled = true,
            .perspective_hud = true,
            .scale = 32.0f,
            .width = 0.8f,
            .height = 0.8f};

  case MetroidElementLayer::ScanHologram:
    return {.enabled = true,
            .perspective_hud = true,
            .scale = 40.0f,
            .width = 1.2f,
            .height = 1.2f};

  case MetroidElementLayer::ScanReticle:
    return {.enabled = true, .width = 1.0f, .height = 1.0f, .up = 0.14f, .right = 0.01f};

  default:
    return {};
  }
}

bool IsMetroidPerspectiveHudAnchorLayer(MetroidElementLayer layer)
{
  switch (layer)
  {
  case MetroidElementLayer::HUD:
  case MetroidElementLayer::UnknownHUD:
  case MetroidElementLayer::MapOrHint:
  case MetroidElementLayer::Map:
  case MetroidElementLayer::XRayHUD:
  case MetroidElementLayer::DarkVisorHUD:
  case MetroidElementLayer::Helmet:
    return true;

  default:
    return false;
  }
}

bool IsMetroidPrime1CombatContextLayer(MetroidElementLayer layer)
{
  switch (layer)
  {
  case MetroidElementLayer::Gun:
  case MetroidElementLayer::World:
  case MetroidElementLayer::Reticle:
  case MetroidElementLayer::WiiReticle:
  case MetroidElementLayer::XRayWorld:
  case MetroidElementLayer::ThermalGunAndDoor:
    return true;

  default:
    return false;
  }
}

bool IsMetroidPrime1MenuContextLayer(MetroidElementLayer layer)
{
  switch (layer)
  {
  case MetroidElementLayer::Map0:
  case MetroidElementLayer::Map1:
  case MetroidElementLayer::Map2:
  case MetroidElementLayer::Dialog:
  case MetroidElementLayer::MapMap:
  case MetroidElementLayer::MapLegend:
  case MetroidElementLayer::InventorySamus:
  case MetroidElementLayer::InventorySamusOutline:
  case MetroidElementLayer::MapNorth:
    return true;

  default:
    return false;
  }
}

bool IsMetroidPrime1CombatHudAnchorCandidate(MetroidElementLayer layer, float reference_view_z,
                                             int hud_context)
{
  // Normal combat visor draws its 3D HUD body as METROID_SCAN_TEXT instead of one of the
  // explicit HUD anchor layers. Use only the stable body-depth range as a combat-only fallback;
  // this excludes close reticle/minimap pieces (~-5.6), free-aim far draws (~-32), and menu/map
  // ScanText draws that should be anchored by their own UI context.
  return hud_context == METROID_HUD_CONTEXT_COMBAT && layer == MetroidElementLayer::ScanText &&
         std::isfinite(reference_view_z) && reference_view_z >= -22.0f &&
         reference_view_z <= -18.0f;
}

bool IsMetroidPrime1ScannerPreviewMask(const ShaderHunter::RuntimeElementSignature& signature)
{
  return signature.valid && !signature.perspective && signature.ortho_left_x100 == -32000 &&
         signature.ortho_right_x100 == 32000 && signature.ortho_top_x100 == 22400 &&
         signature.ortho_bottom_x100 == -22400 && signature.ortho_layer == 0 &&
         signature.viewport_x == 662 && signature.viewport_y == 566 &&
         signature.viewport_width == 320 && signature.viewport_height == 224 &&
         signature.scissor_left == 342 && signature.scissor_top == 342 &&
         signature.scissor_right == 981 && signature.scissor_bottom == 789 &&
         signature.alpha_test_hex == 0x007f0000 && signature.ztest && signature.zupdate &&
         signature.zfunc == 3 && signature.blend_color_update && signature.blend_alpha_update;
}

MetroidLayerBehavior GetMetroidLayerBehavior(MetroidElementLayer layer)
{
  switch (layer)
  {
  case MetroidElementLayer::EFBCopy:
  case MetroidElementLayer::BlackBars:
  case MetroidElementLayer::ScanVisor:
  case MetroidElementLayer::ScanDarken:
  case MetroidElementLayer::ScanHighlighter:
  case MetroidElementLayer::ScanBox:
  case MetroidElementLayer::ScanCircle:
  case MetroidElementLayer::ScanReticle:
    return {.skip = true};

  case MetroidElementLayer::Helmet:
  case MetroidElementLayer::HUD:
  case MetroidElementLayer::MorphballHUD:
  case MetroidElementLayer::XRayHUD:
  case MetroidElementLayer::DarkVisorHUD:
  case MetroidElementLayer::UnknownHUD:
  case MetroidElementLayer::VisorRadarHint:
  case MetroidElementLayer::RadarDot:
  case MetroidElementLayer::MorphballMapOrHint:
  case MetroidElementLayer::MapOrHint:
  case MetroidElementLayer::MorphballMap:
  case MetroidElementLayer::Map:
  case MetroidElementLayer::Map0:
  case MetroidElementLayer::Map1:
  case MetroidElementLayer::Map2:
  case MetroidElementLayer::Dialog:
  case MetroidElementLayer::MapMap:
  case MetroidElementLayer::MapLegend:
  case MetroidElementLayer::InventorySamus:
  case MetroidElementLayer::InventorySamusOutline:
  case MetroidElementLayer::MapNorth:
  case MetroidElementLayer::ScanText:
  case MetroidElementLayer::ScanHologram:
  case MetroidElementLayer::Visor:
  case MetroidElementLayer::VisorBootup:
  case MetroidElementLayer::UnknownVisor:
    return {.handling = ShaderHunter::HandlingType::HeadLocked};

  default:
    return {};
  }
}

// Row-major 3x3 helpers for the ControllerAnchor rotation path.
std::array<float, 9> Mul3x3(const std::array<float, 9>& a, const std::array<float, 9>& b)
{
  std::array<float, 9> m{};
  for (int r = 0; r < 3; ++r)
  {
    for (int c = 0; c < 3; ++c)
      m[r * 3 + c] = a[r * 3 + 0] * b[0 + c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
  }
  return m;
}

// Model-axis correction from the per-override Euler angles, applied in the controller's
// local frame: yaw about +Y, then pitch about +X, then roll about +Z. Models differ in
// which way they natively point (a blade along +Y, a barrel along -Z, ...); these dial
// the element onto the controller's aim direction.
std::array<float, 9> ControllerAnchorAxisCorrection(float yaw_deg, float pitch_deg,
                                                    float roll_deg)
{
  constexpr float DEG_TO_RAD = 0.01745329252f;
  const float cy = std::cos(yaw_deg * DEG_TO_RAD), sy = std::sin(yaw_deg * DEG_TO_RAD);
  const float cx = std::cos(pitch_deg * DEG_TO_RAD), sx = std::sin(pitch_deg * DEG_TO_RAD);
  const float cz = std::cos(roll_deg * DEG_TO_RAD), sz = std::sin(roll_deg * DEG_TO_RAD);
  const std::array<float, 9> rot_y = {cy, 0.0f, sy, 0.0f, 1.0f, 0.0f, -sy, 0.0f, cy};
  const std::array<float, 9> rot_x = {1.0f, 0.0f, 0.0f, 0.0f, cx, -sx, 0.0f, sx, cx};
  const std::array<float, 9> rot_z = {cz, -sz, 0.0f, sz, cz, 0.0f, 0.0f, 0.0f, 1.0f};
  return Mul3x3(Mul3x3(rot_y, rot_x), rot_z);
}
}  // namespace

// GX primitive -> RenderState primitive, no primitive restart
constexpr Common::EnumMap<PrimitiveType, Primitive::GX_DRAW_POINTS> primitive_from_gx{
    PrimitiveType::Triangles,  // GX_DRAW_QUADS
    PrimitiveType::Triangles,  // GX_DRAW_QUADS_2
    PrimitiveType::Triangles,  // GX_DRAW_TRIANGLES
    PrimitiveType::Triangles,  // GX_DRAW_TRIANGLE_STRIP
    PrimitiveType::Triangles,  // GX_DRAW_TRIANGLE_FAN
    PrimitiveType::Lines,      // GX_DRAW_LINES
    PrimitiveType::Lines,      // GX_DRAW_LINE_STRIP
    PrimitiveType::Points,     // GX_DRAW_POINTS
};

// GX primitive -> RenderState primitive, using primitive restart
constexpr Common::EnumMap<PrimitiveType, Primitive::GX_DRAW_POINTS> primitive_from_gx_pr{
    PrimitiveType::TriangleStrip,  // GX_DRAW_QUADS
    PrimitiveType::TriangleStrip,  // GX_DRAW_QUADS_2
    PrimitiveType::TriangleStrip,  // GX_DRAW_TRIANGLES
    PrimitiveType::TriangleStrip,  // GX_DRAW_TRIANGLE_STRIP
    PrimitiveType::TriangleStrip,  // GX_DRAW_TRIANGLE_FAN
    PrimitiveType::Lines,          // GX_DRAW_LINES
    PrimitiveType::Lines,          // GX_DRAW_LINE_STRIP
    PrimitiveType::Points,         // GX_DRAW_POINTS
};

// Due to the BT.601 standard which the GameCube is based on being a compromise
// between PAL and NTSC, neither standard gets square pixels. They are each off
// by ~9% in opposite directions.
// Just in case any game decides to take this into account, we do both these
// tests with a large amount of slop.

static float CalculateProjectionViewportRatio(const Projection::Raw& projection,
                                              const Viewport& viewport)
{
  const float projection_ar = projection[2] / projection[0];
  const float viewport_ar = viewport.wd / viewport.ht;

  return std::abs(projection_ar / viewport_ar);
}

static bool IsAnamorphicProjection(const Projection::Raw& projection, const Viewport& viewport,
                                   const VideoConfig& config)
{
  // If ratio between our projection and viewport aspect ratios is similar to 16:9 / 4:3
  // we have an anamorphic projection. This value can be overridden by a GameINI.
  // Game cheats that change the aspect ratio to natively unsupported ones
  // won't be automatically recognized here.

  return std::abs(CalculateProjectionViewportRatio(projection, viewport) -
                  config.widescreen_heuristic_widescreen_ratio) <
         config.widescreen_heuristic_aspect_ratio_slop;
}

static bool IsNormalProjection(const Projection::Raw& projection, const Viewport& viewport,
                               const VideoConfig& config)
{
  return std::abs(CalculateProjectionViewportRatio(projection, viewport) -
                  config.widescreen_heuristic_standard_ratio) <
         config.widescreen_heuristic_aspect_ratio_slop;
}

VertexManagerBase::VertexManagerBase()
    : m_cpu_vertex_buffer(MAXVBUFFERSIZE), m_cpu_index_buffer(MAXIBUFFERSIZE)
{
}

VertexManagerBase::~VertexManagerBase() = default;

MetroidElementProfile VertexManagerBase::GetCachedMetroidProfile()
{
  if (!m_metroid_profile_resolved)
  {
    std::string game_id = SConfig::GetInstance().GetGameID();
    if (game_id.empty())
      return MetroidElementProfile::None;  // ID not available yet; resolve on a later draw.
    m_metroid_profile = GetMetroidProfileForGameID(game_id);
    m_metroid_game_id = std::move(game_id);
    m_metroid_profile_resolved = true;
  }
  return m_metroid_profile;
}

bool VertexManagerBase::Initialize()
{
  auto& video_events = GetVideoEvents();

  m_frame_end_event =
      video_events.after_frame_event.Register([this](Core::System&) { OnEndFrame(); });
  m_after_present_event = video_events.after_present_event.Register(
      [this](const PresentInfo& pi) { m_ticks_elapsed = pi.emulated_timestamp; });
  m_index_generator.Init();
  m_custom_shader_cache = std::make_unique<CustomShaderCache>();
  m_cpu_cull.Init();
  return true;
}

u32 VertexManagerBase::GetRemainingSize() const
{
  return static_cast<u32>(m_end_buffer_pointer - m_cur_buffer_pointer);
}

void VertexManagerBase::AddIndices(OpcodeDecoder::Primitive primitive, u32 num_vertices)
{
  m_index_generator.AddIndices(primitive, num_vertices);
}

bool VertexManagerBase::AreAllVerticesCulled(VertexLoaderBase* loader,
                                             OpcodeDecoder::Primitive primitive, const u8* src,
                                             u32 count)
{
  return m_cpu_cull.AreAllVerticesCulled(loader, primitive, src, count);
}

DataReader VertexManagerBase::PrepareForAdditionalData(OpcodeDecoder::Primitive primitive,
                                                       u32 count, u32 stride, bool cullall)
{
  // Flush all EFB pokes. Since the buffer is shared, we can't draw pokes+primitives concurrently.
  g_framebuffer_manager->FlushEFBPokes();

  // The SSE vertex loader can write up to 4 bytes past the end
  u32 const needed_vertex_bytes = count * stride + 4;

  // We can't merge different kinds of primitives, so we have to flush here
  PrimitiveType new_primitive_type = g_backend_info.bSupportsPrimitiveRestart ?
                                         primitive_from_gx_pr[primitive] :
                                         primitive_from_gx[primitive];
  if (m_current_primitive_type != new_primitive_type) [[unlikely]]
  {
    Flush();

    // Have to update the rasterization state for point/line cull modes.
    m_current_primitive_type = new_primitive_type;
    SetRasterizationStateChanged();
  }

  u32 remaining_indices = GetRemainingIndices(primitive);
  u32 remaining_index_generator_indices = m_index_generator.GetRemainingIndices(primitive);

  // Check for size in buffer, if the buffer gets full, call Flush()
  if (!m_is_flushed && (count > remaining_index_generator_indices || count > remaining_indices ||
                        needed_vertex_bytes > GetRemainingSize())) [[unlikely]]
  {
    Flush();
  }

  m_cull_all = cullall;

  // need to alloc new buffer
  if (m_is_flushed) [[unlikely]]
  {
    if (cullall)
    {
      // This buffer isn't getting sent to the GPU. Just allocate it on the cpu.
      m_cur_buffer_pointer = m_base_buffer_pointer = m_cpu_vertex_buffer.data();
      m_end_buffer_pointer = m_base_buffer_pointer + m_cpu_vertex_buffer.size();
      m_index_generator.Start(m_cpu_index_buffer.data());
    }
    else
    {
      ResetBuffer(stride);
    }

    remaining_index_generator_indices = m_index_generator.GetRemainingIndices(primitive);
    remaining_indices = GetRemainingIndices(primitive);
    m_is_flushed = false;
  }

  // Now that we've reset the buffer, there should be enough space. It's possible that we still
  // won't have enough space in a few rare cases, such as vertex shader line/point expansion with a
  // ton of lines in one draw command, in which case we will either need to add support for
  // splitting a single draw command into multiple draws or using bigger indices.
  ASSERT_MSG(VIDEO, count <= remaining_index_generator_indices,
             "VertexManager: Too few remaining index values ({} > {}). "
             "32-bit indices or primitive breaking needed.",
             count, remaining_index_generator_indices);
  ASSERT_MSG(VIDEO, count <= remaining_indices,
             "VertexManager: Buffer not large enough for all indices! ({} > {}) "
             "Increase MAXIBUFFERSIZE or we need primitive breaking after all.",
             count, remaining_indices);
  ASSERT_MSG(VIDEO, needed_vertex_bytes <= GetRemainingSize(),
             "VertexManager: Buffer not large enough for all vertices! ({} > {}) "
             "Increase MAXVBUFFERSIZE or we need primitive breaking after all.",
             needed_vertex_bytes, GetRemainingSize());

  return DataReader(m_cur_buffer_pointer, m_end_buffer_pointer);
}

DataReader VertexManagerBase::DisableCullAll(u32 stride)
{
  if (m_cull_all)
  {
    m_cull_all = false;
    ResetBuffer(stride);
  }
  return DataReader(m_cur_buffer_pointer, m_end_buffer_pointer);
}

void VertexManagerBase::FlushData(u32 count, u32 stride)
{
  m_cur_buffer_pointer += count * stride;
}

u32 VertexManagerBase::GetRemainingIndices(OpcodeDecoder::Primitive primitive) const
{
  const u32 index_len = MAXIBUFFERSIZE - m_index_generator.GetIndexLen();

  if (primitive >= Primitive::GX_DRAW_LINES)
  {
    if (g_Config.UseVSForLinePointExpand())
    {
      if (g_backend_info.bSupportsPrimitiveRestart)
      {
        switch (primitive)
        {
        case Primitive::GX_DRAW_LINES:
          return index_len / 5 * 2;
        case Primitive::GX_DRAW_LINE_STRIP:
          return index_len / 5 + 1;
        case Primitive::GX_DRAW_POINTS:
          return index_len / 5;
        default:
          return 0;
        }
      }
      else
      {
        switch (primitive)
        {
        case Primitive::GX_DRAW_LINES:
          return index_len / 6 * 2;
        case Primitive::GX_DRAW_LINE_STRIP:
          return index_len / 6 + 1;
        case Primitive::GX_DRAW_POINTS:
          return index_len / 6;
        default:
          return 0;
        }
      }
    }
    else
    {
      switch (primitive)
      {
      case Primitive::GX_DRAW_LINES:
        return index_len;
      case Primitive::GX_DRAW_LINE_STRIP:
        return index_len / 2 + 1;
      case Primitive::GX_DRAW_POINTS:
        return index_len;
      default:
        return 0;
      }
    }
  }
  else if (g_backend_info.bSupportsPrimitiveRestart)
  {
    switch (primitive)
    {
    case Primitive::GX_DRAW_QUADS:
    case Primitive::GX_DRAW_QUADS_2:
      return index_len / 5 * 4;
    case Primitive::GX_DRAW_TRIANGLES:
      return index_len / 4 * 3;
    case Primitive::GX_DRAW_TRIANGLE_STRIP:
      return index_len / 1 - 1;
    case Primitive::GX_DRAW_TRIANGLE_FAN:
      return index_len / 6 * 4 + 1;
    default:
      return 0;
    }
  }
  else
  {
    switch (primitive)
    {
    case Primitive::GX_DRAW_QUADS:
    case Primitive::GX_DRAW_QUADS_2:
      return index_len / 6 * 4;
    case Primitive::GX_DRAW_TRIANGLES:
      return index_len;
    case Primitive::GX_DRAW_TRIANGLE_STRIP:
      return index_len / 3 + 2;
    case Primitive::GX_DRAW_TRIANGLE_FAN:
      return index_len / 3 + 2;
    default:
      return 0;
    }
  }
}

auto VertexManagerBase::ResetFlushAspectRatioCount() -> FlushStatistics
{
  const auto result = m_flush_statistics;
  m_flush_statistics = {};
  return result;
}

void VertexManagerBase::ResetBuffer(u32 vertex_stride)
{
  m_base_buffer_pointer = m_cpu_vertex_buffer.data();
  m_cur_buffer_pointer = m_cpu_vertex_buffer.data();
  m_end_buffer_pointer = m_base_buffer_pointer + m_cpu_vertex_buffer.size();
  m_index_generator.Start(m_cpu_index_buffer.data());
}

void VertexManagerBase::CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices,
                                     u32* out_base_vertex, u32* out_base_index)
{
  *out_base_vertex = 0;
  *out_base_index = 0;
}

void VertexManagerBase::DrawCurrentBatch(u32 base_index, u32 num_indices, u32 base_vertex)
{
  // If bounding box is enabled, we need to flush any changes first, then invalidate what we have.
  if (g_bounding_box->IsEnabled() && g_ActiveConfig.bBBoxEnable && g_backend_info.bSupportsBBox)
  {
    g_bounding_box->Flush();
  }

  g_gfx->DrawIndexed(base_index, num_indices, base_vertex);
}

void VertexManagerBase::UploadUniforms()
{
}

void VertexManagerBase::InvalidateConstants()
{
  auto& system = Core::System::GetInstance();
  auto& vertex_shader_manager = system.GetVertexShaderManager();
  auto& geometry_shader_manager = system.GetGeometryShaderManager();
  auto& pixel_shader_manager = system.GetPixelShaderManager();
  vertex_shader_manager.dirty = true;
  geometry_shader_manager.dirty = true;
  pixel_shader_manager.dirty = true;
}

void VertexManagerBase::UploadUtilityUniforms(const void* uniforms, u32 uniforms_size)
{
}

void VertexManagerBase::UploadUtilityVertices(const void* vertices, u32 vertex_stride,
                                              u32 num_vertices, const u16* indices, u32 num_indices,
                                              u32* out_base_vertex, u32* out_base_index)
{
  // The GX vertex list should be flushed before any utility draws occur.
  ASSERT(m_is_flushed);

  // Copy into the buffers usually used for GX drawing.
  ResetBuffer(std::max(vertex_stride, 1u));
  if (vertices)
  {
    const u32 copy_size = vertex_stride * num_vertices;
    ASSERT((m_cur_buffer_pointer + copy_size) <= m_end_buffer_pointer);
    std::memcpy(m_cur_buffer_pointer, vertices, copy_size);
    m_cur_buffer_pointer += copy_size;
  }
  if (indices)
    m_index_generator.AddExternalIndices(indices, num_indices, num_vertices);

  CommitBuffer(num_vertices, vertex_stride, num_indices, out_base_vertex, out_base_index);
}

u32 VertexManagerBase::GetTexelBufferElementSize(TexelBufferFormat buffer_format)
{
  // R8 - 1, R16 - 2, RGBA8 - 4, R32G32 - 8
  return 1u << static_cast<u32>(buffer_format);
}

bool VertexManagerBase::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                          u32* out_offset)
{
  return false;
}

bool VertexManagerBase::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                          u32* out_offset, const void* palette_data,
                                          u32 palette_size, TexelBufferFormat palette_format,
                                          u32* palette_offset)
{
  return false;
}

BitSet32 VertexManagerBase::UsedTextures() const
{
  BitSet32 usedtextures;
  for (u32 i = 0; i < bpmem.genMode.numtevstages + 1u; ++i)
    if (bpmem.tevorders[i / 2].getEnable(i & 1))
      usedtextures[bpmem.tevorders[i / 2].getTexMap(i & 1)] = true;

  if (bpmem.genMode.numindstages > 0)
    for (unsigned int i = 0; i < bpmem.genMode.numtevstages + 1u; ++i)
      if (bpmem.tevind[i].IsActive() && bpmem.tevind[i].bt < bpmem.genMode.numindstages)
        usedtextures[bpmem.tevindref.getTexMap(bpmem.tevind[i].bt)] = true;

  return usedtextures;
}

void VertexManagerBase::Flush()
{
  if (m_is_flushed)
    return;

  m_is_flushed = true;

  if (m_draw_counter == 0)
  {
    // This is more or less the start of the Frame
    GetVideoEvents().before_frame_event.Trigger();
  }

  if (xfmem.numTexGen.numTexGens != bpmem.genMode.numtexgens ||
      xfmem.numChan.numColorChans != bpmem.genMode.numcolchans)
  {
    ERROR_LOG_FMT(
        VIDEO,
        "Mismatched configuration between XF and BP stages - {}/{} texgens, {}/{} colors. "
        "Skipping draw. Please report on the issue tracker.",
        xfmem.numTexGen.numTexGens, bpmem.genMode.numtexgens.Value(), xfmem.numChan.numColorChans,
        bpmem.genMode.numcolchans.Value());

    // Analytics reporting so we can discover which games have this problem, that way when we
    // eventually simulate the behavior we have test cases for it.
    if (xfmem.numTexGen.numTexGens != bpmem.genMode.numtexgens)
    {
      DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::MismatchedGPUTexGensBetweenXFAndBP);
    }
    if (xfmem.numChan.numColorChans != bpmem.genMode.numcolchans)
    {
      DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::MismatchedGPUColorsBetweenXFAndBP);
    }

    HideObjectEngine::Engine::GetInstance().DiscardPendingCapturedPrefixes();
    return;
  }

#if defined(_DEBUG) || defined(DEBUGFAST)
  PRIM_LOG("frame{}:\n texgen={}, numchan={}, dualtex={}, ztex={}, cole={}, alpe={}, ze={}",
           g_ActiveConfig.iSaveTargetId, xfmem.numTexGen.numTexGens, xfmem.numChan.numColorChans,
           xfmem.dualTexTrans.enabled, bpmem.ztex2.op.Value(), bpmem.blendmode.color_update.Value(),
           bpmem.blendmode.alpha_update.Value(), bpmem.zmode.update_enable.Value());

  for (u32 i = 0; i < xfmem.numChan.numColorChans; ++i)
  {
    LitChannel* ch = &xfmem.color[i];
    PRIM_LOG("colchan{}: matsrc={}, light={:#x}, ambsrc={}, diffunc={}, attfunc={}", i,
             ch->matsource.Value(), ch->GetFullLightMask(), ch->ambsource.Value(),
             ch->diffusefunc.Value(), ch->attnfunc.Value());
    ch = &xfmem.alpha[i];
    PRIM_LOG("alpchan{}: matsrc={}, light={:#x}, ambsrc={}, diffunc={}, attfunc={}", i,
             ch->matsource.Value(), ch->GetFullLightMask(), ch->ambsource.Value(),
             ch->diffusefunc.Value(), ch->attnfunc.Value());
  }

  for (u32 i = 0; i < xfmem.numTexGen.numTexGens; ++i)
  {
    TexMtxInfo tinfo = xfmem.texMtxInfo[i];
    if (tinfo.texgentype != TexGenType::EmbossMap)
      tinfo.hex &= 0x7ff;
    if (tinfo.texgentype != TexGenType::Regular)
      tinfo.projection = TexSize::ST;

    PRIM_LOG("txgen{}: proj={}, input={}, gentype={}, srcrow={}, embsrc={}, emblght={}, "
             "postmtx={}, postnorm={}",
             i, tinfo.projection.Value(), tinfo.inputform.Value(), tinfo.texgentype.Value(),
             tinfo.sourcerow.Value(), tinfo.embosssourceshift.Value(),
             tinfo.embosslightshift.Value(), xfmem.postMtxInfo[i].index.Value(),
             xfmem.postMtxInfo[i].normalize.Value());
  }

  PRIM_LOG("pixel: tev={}, ind={}, texgen={}, dstalpha={}, alphatest={:#x}",
           bpmem.genMode.numtevstages.Value() + 1, bpmem.genMode.numindstages.Value(),
           bpmem.genMode.numtexgens.Value(), bpmem.dstalpha.enable.Value(),
           (bpmem.alpha_test.hex >> 16) & 0xff);
#endif

  // Track some stats used elsewhere by the anamorphic widescreen heuristic.
  auto& system = Core::System::GetInstance();
  if (!system.IsWii())
  {
    const bool is_perspective = xfmem.projection.type == ProjectionType::Perspective;

    auto& counts =
        is_perspective ? m_flush_statistics.perspective : m_flush_statistics.orthographic;

    const auto& projection = xfmem.projection.rawProjection;
    // TODO: Potentially the viewport size could be used as weight for the flush count average.
    // This way a small minimap would have less effect than a fullscreen projection.
    const auto& viewport = xfmem.viewport;

    // FYI: This average is based on flushes.
    // It doesn't look at vertex counts like the heuristic does.
    counts.average_ratio.Push(CalculateProjectionViewportRatio(projection, viewport));

    if (IsAnamorphicProjection(projection, viewport, g_ActiveConfig))
    {
      ++counts.anamorphic_flush_count;
      counts.anamorphic_vertex_count += m_index_generator.GetIndexLen();
    }
    else if (IsNormalProjection(projection, viewport, g_ActiveConfig))
    {
      ++counts.normal_flush_count;
      counts.normal_vertex_count += m_index_generator.GetIndexLen();
    }
    else
    {
      ++counts.other_flush_count;
      counts.other_vertex_count += m_index_generator.GetIndexLen();
    }
  }

  auto& pixel_shader_manager = system.GetPixelShaderManager();
  auto& geometry_shader_manager = system.GetGeometryShaderManager();
  auto& vertex_shader_manager = system.GetVertexShaderManager();
  auto& xf_state_manager = system.GetXFStateManager();
  bool committed_hide_object_capture = false;

  // VR passthrough: coverage defaults to fully opaque each draw; a matching
  // Passthrough override below replaces it with the element's opacity for this draw.
  pixel_shader_manager.SetVRPassthroughAlpha(1.0f);

  if (g_ActiveConfig.bGraphicMods)
  {
    const double seconds_elapsed =
        static_cast<double>(m_ticks_elapsed) / system.GetSystemTimers().GetTicksPerSecond();
    pixel_shader_manager.constants.time_ms = seconds_elapsed * 1000;
  }

  CalculateNormals(VertexLoaderManager::GetCurrentVertexFormat());
  // Calculate ZSlope for zfreeze
  const auto used_textures = UsedTextures();
  std::vector<std::string> texture_names;
  Common::SmallVector<u32, 8> texture_units;
  std::array<SamplerState, 8> samplers;
  if (!m_cull_all)
  {
    if (!g_ActiveConfig.bGraphicMods)
    {
      for (const u32 i : used_textures)
      {
        const auto cache_entry = g_texture_cache->Load(i);
        if (!cache_entry)
          continue;
        const float custom_tex_scale = cache_entry->GetWidth() / float(cache_entry->native_width);
        samplers[i] = TextureCacheBase::GetSamplerState(
            i, custom_tex_scale, cache_entry->is_custom_tex, cache_entry->has_arbitrary_mips);
      }
    }
    else
    {
      for (const u32 i : used_textures)
      {
        const auto cache_entry = g_texture_cache->Load(i);
        if (cache_entry)
        {
          if (!Common::Contains(texture_names, cache_entry->texture_info_name))
          {
            texture_names.push_back(cache_entry->texture_info_name);
            texture_units.push_back(i);
          }

          const float custom_tex_scale = cache_entry->GetWidth() / float(cache_entry->native_width);
          samplers[i] = TextureCacheBase::GetSamplerState(
              i, custom_tex_scale, cache_entry->is_custom_tex, cache_entry->has_arbitrary_mips);
        }
      }
    }
  }
  vertex_shader_manager.SetConstants(texture_names, xf_state_manager);
  if (!bpmem.genMode.zfreeze)
  {
    // Must be done after VertexShaderManager::SetConstants()
    CalculateZSlope(VertexLoaderManager::GetCurrentVertexFormat());
  }
  else if (m_zslope.dirty && !m_cull_all)  // or apply any dirty ZSlopes
  {
    pixel_shader_manager.SetZSlope(m_zslope.dfdx, m_zslope.dfdy, m_zslope.f0);
    m_zslope.dirty = false;
  }

  if (!m_cull_all)
  {
    CustomPixelShaderContents custom_pixel_shader_contents;
    std::optional<CustomPixelShader> custom_pixel_shader;
    std::vector<std::string> custom_pixel_texture_names;
    std::span<u8> custom_pixel_shader_uniforms;
    bool skip = false;
    for (size_t i = 0; i < texture_names.size(); i++)
    {
      GraphicsModActionData::DrawStarted draw_started{texture_units, &skip, &custom_pixel_shader,
                                                      &custom_pixel_shader_uniforms};
      for (const auto& action : g_graphics_mod_manager->GetDrawStartedActions(texture_names[i]))
      {
        action->OnDrawStarted(&draw_started);
        if (custom_pixel_shader)
        {
          custom_pixel_shader_contents.shaders.push_back(*custom_pixel_shader);
          custom_pixel_texture_names.push_back(texture_names[i]);
        }
        custom_pixel_shader = std::nullopt;
      }
    }

    // Now the vertices can be flushed to the GPU. Everything following the CommitBuffer() call
    // must be careful to not upload any utility vertices, as the binding will be lost otherwise.
    const u32 num_indices = m_index_generator.GetIndexLen();
    if (num_indices == 0)
    {
      HideObjectEngine::Engine::GetInstance().DiscardPendingCapturedPrefixes();
      return;
    }

    // Record actual perspective draws, rather than viewport state changes, so the dominant
    // composed-scene extent can drive the final VR XFB presentation resample. This never
    // changes the EFB viewport seen by the game or by EFB effect readbacks.
    if (g_ActiveConfig.stereo_mode == StereoMode::OpenXR && g_ActiveConfig.vr_remove_bars &&
        g_ActiveConfig.vr_frame_size_from_xfb &&
        xfmem.projection.type == ProjectionType::Perspective)
    {
      const int scissor_width =
          static_cast<int>(bpmem.scissorBR.x) - static_cast<int>(bpmem.scissorTL.x) + 1;
      const int scissor_height =
          static_cast<int>(bpmem.scissorBR.y) - static_cast<int>(bpmem.scissorTL.y) + 1;
      const int viewport_width =
          static_cast<int>(std::lround(2.0f * std::fabs(xfmem.viewport.wd)));
      const int viewport_height =
          static_cast<int>(std::lround(2.0f * std::fabs(xfmem.viewport.ht)));
      const bool scissor_describes_scene = scissor_width >= viewport_width * 9 / 10 &&
                                            scissor_height >= viewport_height / 2;
      if (scissor_describes_scene)
      {
        VR::ObserveVRPerspectiveViewport(xfmem.viewport, bpmem.scissorOffset.x << 1,
                                         bpmem.scissorOffset.y << 1);
      }
    }

    // Texture loading can cause palettes to be applied (-> uniforms -> draws).
    // Palette application does not use vertices, only a full-screen quad, so this is okay.
    // Same with GPU texture decoding, which uses compute shaders.
    g_texture_cache->BindTextures(used_textures, samplers);

    if (!skip)
    {
      UpdatePipelineConfig();
      UpdatePipelineObject();
      bool shader_hunter_force_pink = false;
      bool manual_screen_pane = false;
      auto screen_pane_depth = ElementsGroupManager::ScreenPaneDepthMode::Game;
      u64 screen_pane_group = 0;
      if (m_current_pipeline_object)
      {
        // Shader Hunter: register shader hashes and check for skip.
        // Also check persistent overrides (always active, even when hunting is disabled).
        bool hunter_skip = false;
        bool elements_skip = false;
        bool texmgr_skip = false;
        auto& hunter = ShaderHunter::GetInstance();
        auto& elements = ElementsGroupManager::GetInstance();
        auto& texmgr = TextureElementManager::GetInstance();
        const bool hunter_enabled = hunter.IsEnabled();
        const bool hunter_debug_logging = hunter.IsDebugLogging();
        const bool hunter_has_overrides = hunter.HasOverrides();
        const bool elements_popup_open = elements.IsPopupOpen();
        const bool elements_has_overrides = elements.HasOverrides();
        const bool metroid_profile_active =
            GetCachedMetroidProfile() != MetroidElementProfile::None;
        const bool elements_runtime_active =
            elements_popup_open || elements_has_overrides || metroid_profile_active;
        const bool hunter_needs_families = hunter.NeedsShaderFamilySignatures();
        const bool hunter_needs_textures = hunter.NeedsTextureHashes();
        const bool texmgr_has_overrides = texmgr.HasOverrides();
        const bool texmgr_hunter_active = texmgr.IsHunterActive();
        const bool texmgr_active = texmgr_has_overrides || texmgr_hunter_active;
        // In flat 2D "cinema" mode the game renders mono exactly like a normal flat game. The
        // Shader/Elements/Textures overrides (and the hunter) only skip or reproject draws for
        // stereo VR, so running them here would corrupt the flat image and waste work — skip the
        // whole override/hunter pass when the panel path is active.
        const bool vr_flat_mode = g_ActiveConfig.vr_flat_screen;
        if (!vr_flat_mode && (hunter_enabled || hunter_has_overrides || hunter_debug_logging ||
                              elements_runtime_active || texmgr_active))
        {
          const auto& vs = m_current_pipeline_config.vs_uid;
          const auto& ps = m_current_pipeline_config.ps_uid;
          const auto& gs = m_current_pipeline_config.gs_uid;
          const u64 vs_hash =
              Common::ComputeCRC32(vs.GetUidDataRaw(), static_cast<u32>(vs.GetUidDataSize()));
          const u64 ps_hash =
              Common::ComputeCRC32(ps.GetUidDataRaw(), static_cast<u32>(ps.GetUidDataSize()));
          const u64 gs_hash =
              Common::ComputeCRC32(gs.GetUidDataRaw(), static_cast<u32>(gs.GetUidDataSize()));

          std::array<u64, 8> tex_hashes{};
          std::array<std::string, 8> tex_names{};
          const bool needs_texture_hashes =
              hunter_enabled || hunter_needs_textures || elements_runtime_active || texmgr_active;
          const bool needs_texture_names =
              hunter_enabled || elements_popup_open || texmgr_hunter_active;
          if (needs_texture_hashes || needs_texture_names)
          {
            for (u32 i = 0; i < 8; i++)
            {
              if (needs_texture_hashes)
                tex_hashes[i] = g_texture_cache->GetBoundTextureHash(i);
              if (needs_texture_names)
                tex_names[i] = g_texture_cache->GetBoundTextureName(i);
            }
          }

          if (hunter_enabled || hunter_needs_textures)
            hunter.SetCurrentDrawTextures(tex_hashes, tex_names);

          if (texmgr_hunter_active)
            texmgr.CaptureDrawTextures(tex_hashes, tex_names);

          u64 vs_family = 0;
          u64 ps_family = 0;
          u64 gs_family = 0;
          if (hunter_enabled || hunter_needs_families || elements_runtime_active)
          {
            vs_family = hunter.RegisterShader(ShaderHunter::ShaderType::Vertex, vs_hash,
                                              vs.GetUidDataRaw(), vs.GetUidDataSize());
            ps_family = hunter.RegisterShader(ShaderHunter::ShaderType::Pixel, ps_hash,
                                              ps.GetUidDataRaw(), ps.GetUidDataSize());
            gs_family = hunter.RegisterShader(ShaderHunter::ShaderType::Geometry, gs_hash,
                                              gs.GetUidDataRaw(), gs.GetUidDataSize());
            hunter.SetCurrentDrawShaderFamilies(vs_family, ps_family, gs_family);
          }

          ShaderHunter::RuntimeElementSignature draw_signature{};
          if (hunter_enabled || elements_runtime_active)
          {
            draw_signature = BuildRuntimeElementSignature(xfmem, bpmem);
            if (hunter_enabled)
              hunter.SetCurrentDrawSignature(draw_signature);
          }

          if (hunter_enabled)
          {
            hunter.RegisterDrawCombination(vs_hash, ps_hash, gs_hash);
            hunter_skip = hunter.ShouldSkipDraw(vs_hash, ps_hash, gs_hash);
            shader_hunter_force_pink = hunter.ShouldHighlightSelectedDraw();
          }

          std::optional<ElementsGroupManager::DrawRecord> element_draw;
          MetroidElementLayer metroid_layer = MetroidElementLayer::Unknown;
          MetroidElementProfile metroid_profile = MetroidElementProfile::None;
          MetroidProjectionMetrics metroid_metrics{};
          bool uses_mp2_dark_copy = false;
          bool uses_mp2_dark_highlight_copy = false;
          if (elements_runtime_active)
          {
            element_draw.emplace(ElementsGroupManager::DrawRecord{
                .draw_index = -1,
                .draw_sequence = m_draw_counter + 1,
                .vs_hash = vs_hash,
                .ps_hash = ps_hash,
                .gs_hash = gs_hash,
                .vs_family = vs_family,
                .ps_family = ps_family,
                .gs_family = gs_family,
                .signature = draw_signature,
                .textures = tex_hashes,
                .texture_names = tex_names});

            if (metroid_profile_active)
            {
              metroid_profile = GetCachedMetroidProfile();
              metroid_metrics = BuildMetroidProjectionMetrics(xfmem, m_draw_counter);
              metroid_layer =
                  GetMetroidElementClassifier().Classify(metroid_profile, metroid_metrics);
              if (IsMetroidPrime1Profile(metroid_profile) &&
                  IsMetroidPrime1ScannerPreviewMask(draw_signature))
              {
                metroid_layer = MetroidElementLayer::ScanDarken;
              }
              element_draw->profile_id = metroid_profile;
              element_draw->profile_layer = metroid_layer;
              element_draw->profile_layer_name =
                  std::string(MetroidElementLayerToDisplayName(metroid_layer));

              if (IsMetroidPrime1Profile(metroid_profile))
              {
                if (IsMetroidPrime1MenuContextLayer(metroid_layer))
                {
                  m_metroid_prime1_menu_context_seen = true;
                  m_metroid_prime1_combat_context_seen = false;
                }
                else if (!m_metroid_prime1_menu_context_seen &&
                         IsMetroidPrime1CombatContextLayer(metroid_layer))
                {
                  m_metroid_prime1_combat_context_seen = true;
                }
              }

              // Trilogy resolves to TrilogyAuto (never a Prime2 profile), but its MP2 content
              // still copies the 408x286 dark-visor highlight source. That shape is
              // MP2-specific, so scan for it under TrilogyAuto too. The 640x448 full-view dark
              // copy stays Prime2-profile-only — under Trilogy that shape collides with MP1
              // thermal sources.
              const bool trilogy_auto_profile =
                  metroid_profile == MetroidElementProfile::TrilogyAuto;
              if (IsMetroidPrime2Profile(metroid_profile) || trilogy_auto_profile)
              {
                for (const u32 i : used_textures)
                {
                  if (!trilogy_auto_profile &&
                      g_texture_cache->IsBoundMetroidPrime2DarkTexture(i))
                    uses_mp2_dark_copy = true;
                  if (g_texture_cache->IsBoundMetroidPrime2DarkHighlightTexture(i))
                    uses_mp2_dark_highlight_copy = true;
                  if (uses_mp2_dark_copy && uses_mp2_dark_highlight_copy)
                    break;
                }
              }
            }

            HideObjectEngine::Engine::GetInstance().CommitCapturedPrefixesForDraw(
                element_draw->draw_sequence);
            committed_hide_object_capture = true;

            const auto preview_action = elements.RegisterDraw(*element_draw);
            elements_skip = preview_action == ElementsGroupManager::PreviewAction::Skip;
            if (preview_action == ElementsGroupManager::PreviewAction::Pink)
              shader_hunter_force_pink = true;
          }

          // Register flag shaders (must be before skip/handling checks)
          if (hunter_has_overrides)
            hunter.RegisterFlags(vs_hash, ps_hash, gs_hash);
          if (elements_runtime_active)
            elements.RegisterFlagsForDraw(*element_draw);
          if (texmgr_has_overrides)
            texmgr.RegisterFlagsForTextures(tex_hashes);

          if (!hunter_skip && !elements_skip && elements_has_overrides)
            elements_skip = elements.ShouldSkipByOverride(*element_draw);

          if (!hunter_skip && !elements_skip && metroid_profile_active)
            elements_skip = GetMetroidLayerBehavior(metroid_layer).skip;

          if (!hunter_skip && !elements_skip && hunter_has_overrides)
            hunter_skip = hunter.ShouldSkipByOverride(vs_hash, ps_hash, gs_hash);

          // Texture Element Override skip: match purely on bound texture hash (fallback).
          if (!hunter_skip && !elements_skip && texmgr_has_overrides)
            texmgr_skip = texmgr.ShouldSkipByTexture(tex_hashes);

          // Live preview while the Texture Hunter browser is open: skip or pink-highlight draws
          // binding a checked texture (Skip/Pink chosen in the browser's Preview mode toggle).
          if (texmgr_hunter_active && texmgr.HasPreviewMatch(tex_hashes))
          {
            if (texmgr.IsPreviewPink())
              shader_hunter_force_pink = true;
            else
              texmgr_skip = true;
          }

          // Check for screen/fullscreen handling overrides (VR stereo mode override)
          if (!hunter_skip && !elements_skip && !texmgr_skip)
          {
            auto handling = ShaderHunter::HandlingType::Skip;
            bool preserve_stereo_efb = false;
            float element_depth = -1.0f;
            float units_per_meter = -1.0f;
            float passthrough_opacity = 0.0f;
            ElementsGroupManager::CameraAnchorParams anchor_params;
            ElementsGroupManager::ControllerAnchorParams controller_anchor_params;
            const MetroidHydraHudSettings metroid_hydra_hud =
                metroid_profile_active ?
                    GetMetroidHydraHudSettings(metroid_profile, metroid_layer, m_metroid_game_id) :
                    MetroidHydraHudSettings{};

            if (elements_has_overrides)
              handling = elements.GetOverrideHandling(*element_draw, &preserve_stereo_efb);
            if (handling != ShaderHunter::HandlingType::Skip)
            {
              if (handling == ShaderHunter::HandlingType::ScreenPane)
              {
                screen_pane_depth =
                    elements.GetOverrideScreenPaneDepth(*element_draw, &screen_pane_group);
              }
              else if (handling == ShaderHunter::HandlingType::Screen ||
                  handling == ShaderHunter::HandlingType::HeadLocked)
              {
                element_depth = elements.GetOverrideElementDepth(*element_draw);
              }
              else if (handling == ShaderHunter::HandlingType::UnitsPerMeter)
              {
                units_per_meter = elements.GetOverrideUnitsPerMeter(*element_draw);
              }
              else if (handling == ShaderHunter::HandlingType::Passthrough)
              {
                passthrough_opacity = elements.GetOverridePassthroughOpacity(*element_draw);
              }
              else if (handling == ShaderHunter::HandlingType::CameraAnchor)
              {
                elements.GetOverrideCameraAnchor(*element_draw, &anchor_params);
              }
              else if (handling == ShaderHunter::HandlingType::ControllerAnchor)
              {
                elements.GetOverrideControllerAnchor(*element_draw, &controller_anchor_params);
              }
            }
            else if (hunter_has_overrides)
            {
              handling = hunter.GetOverrideHandling(vs_hash, ps_hash, gs_hash);
              if (handling == ShaderHunter::HandlingType::Screen ||
                  handling == ShaderHunter::HandlingType::HeadLocked)
              {
                element_depth = hunter.GetOverrideElementDepth(vs_hash, ps_hash, gs_hash);
              }
              else if (handling == ShaderHunter::HandlingType::UnitsPerMeter)
              {
                units_per_meter = hunter.GetOverrideUnitsPerMeter(vs_hash, ps_hash, gs_hash);
              }
              else if (handling == ShaderHunter::HandlingType::Passthrough)
              {
                passthrough_opacity =
                    hunter.GetOverridePassthroughOpacity(vs_hash, ps_hash, gs_hash);
              }
              else if (handling == ShaderHunter::HandlingType::CameraAnchor)
              {
                hunter.GetOverrideCameraAnchor(vs_hash, ps_hash, gs_hash, &anchor_params);
              }
              else if (handling == ShaderHunter::HandlingType::ControllerAnchor)
              {
                hunter.GetOverrideControllerAnchor(vs_hash, ps_hash, gs_hash,
                                                   &controller_anchor_params);
              }
            }
            // Texture Element Override (fallback): match purely on bound texture hash, applied
            // only when neither Elements nor Shader overrides produced a handling for this draw.
            if (handling == ShaderHunter::HandlingType::Skip && texmgr_has_overrides)
            {
              handling = texmgr.GetHandlingForTextures(tex_hashes, &element_depth,
                                                       &units_per_meter, &passthrough_opacity,
                                                       &anchor_params, &controller_anchor_params);
            }
            if (handling == ShaderHunter::HandlingType::Skip && metroid_profile_active)
              handling = GetMetroidLayerBehavior(metroid_layer).handling;
            if (handling == ShaderHunter::HandlingType::Skip && uses_mp2_dark_copy)
            {
              handling = ShaderHunter::HandlingType::Fullscreen;
            }
            if (handling == ShaderHunter::HandlingType::Skip && uses_mp2_dark_highlight_copy &&
                !metroid_metrics.perspective)
            {
              handling = ShaderHunter::HandlingType::Fullscreen;
            }

            if (handling == ShaderHunter::HandlingType::Fullscreen && preserve_stereo_efb)
            {
              for (const u32 i : used_textures)
                g_texture_cache->ApplyVRPreserveStereoEFBFix(i);
            }

            if (handling == ShaderHunter::HandlingType::Screen)
            {
              geometry_shader_manager.vr_stereo_override = -1.0f;
              if (element_depth >= 0.0f)
                geometry_shader_manager.vr_element_depth_override = element_depth;
            }
            else if (handling == ShaderHunter::HandlingType::ScreenPane)
            {
              const VR::VRFrameRegion frame = VR::GetVRFrameRegion();
              manual_screen_pane =
                  g_ActiveConfig.stereo_mode == StereoMode::OpenXR &&
                  frame.valid && xfmem.projection.type == ProjectionType::Perspective;
              if (manual_screen_pane)
              {
                switch (screen_pane_depth)
                {
                case ElementsGroupManager::ScreenPaneDepthMode::VR:
                  geometry_shader_manager.vr_stereo_override =
                      GeometryShaderManager::VR_STEREO_SCREEN_PANE_3D_VR_DEPTH;
                  break;
                case ElementsGroupManager::ScreenPaneDepthMode::Flat:
                  // Use the normal world-fixed 2D screen route. The pane remap below keeps the
                  // original viewport position, while the per-draw flag forces zero visual Z.
                  geometry_shader_manager.vr_stereo_override = -1.0f;
                  geometry_shader_manager.vr_flat_screen_pane_override = true;
                  break;
                case ElementsGroupManager::ScreenPaneDepthMode::Game:
                default:
                  geometry_shader_manager.vr_stereo_override =
                      GeometryShaderManager::VR_STEREO_SCREEN_PANE_3D;
                  break;
                }
                geometry_shader_manager.vr_pane_screen_override = true;
                geometry_shader_manager.vr_pane_group_override =
                    screen_pane_depth == ElementsGroupManager::ScreenPaneDepthMode::VR ?
                        screen_pane_group :
                        0;
              }
            }
            else if (handling == ShaderHunter::HandlingType::Fullscreen)
            {
              geometry_shader_manager.vr_stereo_override = 0.0f;
            }
            else if (handling == ShaderHunter::HandlingType::FullscreenMono)
            {
              geometry_shader_manager.vr_stereo_override = 0.0f;
            }
            else if (handling == ShaderHunter::HandlingType::HeadLocked)
            {
              const bool perspective_metroid_hud =
                  metroid_hydra_hud.perspective_hud && metroid_metrics.perspective;
              geometry_shader_manager.vr_stereo_override = perspective_metroid_hud ? -3.0f : -2.0f;
              if (metroid_hydra_hud.enabled)
              {
                const auto& pnm = vertex_shader_manager.constants.posnormalmatrix;
                const float metroid_hud_ref_z = pnm[2][3];
                int metroid_hud_context = METROID_HUD_CONTEXT_DEFAULT;
                if (IsMetroidPrime1Profile(metroid_profile))
                {
                  if (m_metroid_prime1_menu_context_seen)
                    metroid_hud_context = METROID_HUD_CONTEXT_MENU;
                  else if (m_metroid_prime1_combat_context_seen)
                    metroid_hud_context = METROID_HUD_CONTEXT_COMBAT;
                }
                // Radar/minimap layers self-centre on their own origin depth in the -3 path
                // (consumed and reset by GeometryShaderManager::SetConstants).
                const std::string_view layer_name = MetroidElementLayerToININame(metroid_layer);
                geometry_shader_manager.vr_metroid_hud_self_center =
                    layer_name.find("RADAR") != std::string_view::npos;
                geometry_shader_manager.vr_metroid_hud_anchor_candidate =
                    perspective_metroid_hud &&
                    (IsMetroidPerspectiveHudAnchorLayer(metroid_layer) ||
                     IsMetroidPrime1CombatHudAnchorCandidate(metroid_layer, metroid_hud_ref_z,
                                                             metroid_hud_context));
                geometry_shader_manager.vr_metroid_hud_reference_context = metroid_hud_context;
              }
              if (element_depth >= 0.0f)
                geometry_shader_manager.vr_element_depth_override = element_depth;
              if (metroid_hydra_hud.enabled)
              {
                if (metroid_hydra_hud.scale > 0.0f)
                {
                  geometry_shader_manager.vr_units_per_meter_override =
                      g_ActiveConfig.vr_units_per_meter * metroid_hydra_hud.scale;
                }
                geometry_shader_manager.vr_headlocked_projection_scale_x =
                    metroid_hydra_hud.width;
                geometry_shader_manager.vr_headlocked_projection_scale_y =
                    metroid_hydra_hud.height;
                geometry_shader_manager.vr_headlocked_projection_offset_x =
                    metroid_hydra_hud.right;
                geometry_shader_manager.vr_headlocked_projection_offset_y = metroid_hydra_hud.up;
              }
            }
            else if (handling == ShaderHunter::HandlingType::UnitsPerMeter)
            {
              if (units_per_meter > 0.0f)
                geometry_shader_manager.vr_units_per_meter_override = units_per_meter;
            }
            else if (handling == ShaderHunter::HandlingType::Passthrough)
            {
              // PS-side only: the draw keeps its normal projection; its pixels write the
              // override's opacity into the coverage target, becoming a camera window.
              pixel_shader_manager.SetVRPassthroughAlpha(
                  std::clamp(passthrough_opacity, 0.0f, 1.0f));
            }
            else if (handling == ShaderHunter::HandlingType::CameraAnchor)
            {
              // The element's object-space origin in game view space (game units): the
              // translation column of the current position matrix. Captured as this frame's
              // pending anchor; OpenXRManager latches it at frame end so all draws of the
              // NEXT frame render from it (never mid-frame). The draw itself renders
              // normally unless the override hides it. Gating capture (not application) on
              // the toggle means disabling glides the camera back to the default position.
              if (g_ActiveConfig.stereo_mode == StereoMode::OpenXR && !vr_flat_mode &&
                  g_ActiveConfig.vr_enable_camera_anchor && VR::g_openxr &&
                  VR::g_openxr->IsSessionRunning())
              {
                const auto& pnm = vertex_shader_manager.constants.posnormalmatrix;
                // Convert the meter offsets with the scale actually in effect (which the
                // anchor itself may be driving), so they stay the requested real-world size
                // while an anchor scale glides in.
                const float upm = VR::g_openxr->GetEffectiveUnitsPerMeter();
                // Offset is {right, up, forward} in meters; view space is +X right, +Y up,
                // -Z forward (same convention as the Camera Forward/Height settings).
                const float ax = pnm[0][3] + anchor_params.offset[0] * upm;
                const float ay = pnm[1][3] + anchor_params.offset[1] * upm;
                const float az = pnm[2][3] - anchor_params.offset[2] * upm;
                // Rig rotation (columns = rig axes in view space). The element's object axes
                // in view space are the columns of the matrix's 3x3 block; the yaw offset
                // corrects models whose forward axis is not object +Z. Scale is removed by
                // the orthonormalization in CommitCameraAnchorFrame.
                constexpr float DEG_TO_RAD = 0.01745329252f;
                std::array<float, 9> anchor_rot = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                                   0.0f, 0.0f, 0.0f, 1.0f};
                if (anchor_params.rotation == ShaderHunter::AnchorRotationMode::Full)
                {
                  const float yaw = anchor_params.yaw_offset_deg * DEG_TO_RAD;
                  const float cy = std::cos(yaw);
                  const float sy = std::sin(yaw);
                  // A = M_element * RotY(yaw): the correction spins the rig about the
                  // element's own up axis before mapping into view space.
                  for (int r = 0; r < 3; ++r)
                  {
                    anchor_rot[r * 3 + 0] = pnm[r][0] * cy - pnm[r][2] * sy;
                    anchor_rot[r * 3 + 1] = pnm[r][1];
                    anchor_rot[r * 3 + 2] = pnm[r][0] * sy + pnm[r][2] * cy;
                  }
                }
                else if (anchor_params.rotation == ShaderHunter::AnchorRotationMode::YawOnly)
                {
                  // Heading of the element's object +Z axis in the view's horizontal plane;
                  // pitch/roll are dropped so the horizon stays level (comfort).
                  const float vx = pnm[0][2];
                  const float vz = pnm[2][2];
                  float yaw = (vx * vx + vz * vz > 1e-8f) ? std::atan2(vx, vz) : 0.0f;
                  yaw += anchor_params.yaw_offset_deg * DEG_TO_RAD;
                  const float cy = std::cos(yaw);
                  const float sy = std::sin(yaw);
                  anchor_rot = {cy, 0.0f, sy, 0.0f, 1.0f, 0.0f, -sy, 0.0f, cy};
                }
                VR::g_openxr->SetPendingCameraAnchor(ax, ay, az, anchor_rot,
                                                     anchor_params.units_per_meter);
                if (hunter_debug_logging)
                {
                  INFO_LOG_FMT(VIDEO,
                               "VR_ANCHOR: draw#{} pos=({:.2f},{:.2f},{:.2f}) offset_m=({:.2f},"
                               "{:.2f},{:.2f}) rot={} yaw_off={:.0f} upm={:.2f}(eff {:.2f}) "
                               "hide={}",
                               m_draw_counter, pnm[0][3], pnm[1][3], pnm[2][3],
                               anchor_params.offset[0], anchor_params.offset[1],
                               anchor_params.offset[2],
                               static_cast<int>(anchor_params.rotation),
                               anchor_params.yaw_offset_deg, anchor_params.units_per_meter, upm,
                               anchor_params.hide);
                }
                if (anchor_params.hide)
                  elements_skip = true;
              }
            }
            else if (handling == ShaderHunter::HandlingType::ControllerAnchor)
            {
              // The element is repositioned to a VR controller: the position matrix
              // translation is replaced with the controller's aim pose mapped into game
              // view space (same space/units as the projection's eye offsets), plus the
              // override's meter offsets. With rotation follow the 3x3 (and normal rows)
              // are rebuilt from the controller orientation too; otherwise orientation
              // stays game-driven. Scale is always preserved. SetPosNormalChanged makes
              // the next flush reload the real matrix from XF memory so the write lives
              // for exactly one draw. Draws with per-vertex position-matrix indices
              // (posmtx) ignore posnormalmatrix entirely — the log flags those so a
              // non-moving element is diagnosable immediately.
              if (g_ActiveConfig.stereo_mode == StereoMode::OpenXR && !vr_flat_mode &&
                  g_ActiveConfig.vr_enable_controller_anchor && VR::g_openxr &&
                  VR::g_openxr->IsSessionRunning())
              {
                // Same scale the camera is using this frame (a Camera Anchor may be driving
                // it), or the hands would land at the wrong distance in the anchored view.
                const float upm = VR::g_openxr->GetEffectiveUnitsPerMeter();
                std::array<float, 3> hand_pos{};
                std::array<float, 9> hand_rot{};
                if (VR::g_openxr->GetControllerAnchorViewPose(controller_anchor_params.hand, upm,
                                                              &hand_pos, &hand_rot))
                {
                  auto& pnm = vertex_shader_manager.constants.posnormalmatrix;
                  const float old_x = pnm[0][3], old_y = pnm[1][3], old_z = pnm[2][3];
                  const float off_x = controller_anchor_params.offset[0] * upm;
                  const float off_y = controller_anchor_params.offset[1] * upm;
                  const float off_z = -controller_anchor_params.offset[2] * upm;
                  float dx = off_x, dy = off_y, dz = off_z;
                  if (controller_anchor_params.rotation)
                  {
                    // Offsets ride the controller frame so the element stays rigidly
                    // attached (a view-space offset would drift around the grip as the
                    // hand turns).
                    dx = hand_rot[0] * off_x + hand_rot[1] * off_y + hand_rot[2] * off_z;
                    dy = hand_rot[3] * off_x + hand_rot[4] * off_y + hand_rot[5] * off_z;
                    dz = hand_rot[6] * off_x + hand_rot[7] * off_y + hand_rot[8] * off_z;
                  }
                  pnm[0][3] = hand_pos[0] + dx;
                  pnm[1][3] = hand_pos[1] + dy;
                  pnm[2][3] = hand_pos[2] + dz;
                  if (controller_anchor_params.rotation)
                  {
                    // 3x3 = (A*C)*X: controller orientation in view space times the
                    // model-axis correction. The game matrix's per-axis scale (column
                    // norms) is preserved so the model keeps its size; the normal rows
                    // get the pure rotation (the correct inverse-transpose for a rigid
                    // transform, and stale game normals would light the element wrong
                    // as it turns).
                    const std::array<float, 9> m = Mul3x3(
                        hand_rot, ControllerAnchorAxisCorrection(controller_anchor_params.yaw_deg,
                                                                 controller_anchor_params.pitch_deg,
                                                                 controller_anchor_params.roll_deg));
                    for (int c = 0; c < 3; ++c)
                    {
                      const float len =
                          std::sqrt(pnm[0][c] * pnm[0][c] + pnm[1][c] * pnm[1][c] +
                                    pnm[2][c] * pnm[2][c]);
                      const float s = len > 1e-6f ? len : 1.0f;
                      pnm[0][c] = m[0 + c] * s;
                      pnm[1][c] = m[3 + c] * s;
                      pnm[2][c] = m[6 + c] * s;
                      pnm[3][c] = m[0 + c];
                      pnm[4][c] = m[3 + c];
                      pnm[5][c] = m[6 + c];
                    }
                  }
                  vertex_shader_manager.dirty = true;
                  xf_state_manager.SetPosNormalChanged();
                  if (hunter_debug_logging)
                  {
                    const NativeVertexFormat* fmt = VertexLoaderManager::GetCurrentVertexFormat();
                    const bool posmtx = fmt && fmt->GetVertexDeclaration().posmtx.enable;
                    INFO_LOG_FMT(VIDEO,
                                 "VR_HANDANCHOR: draw#{} hand={} posmtx={} rot={} "
                                 "old=({:.2f},{:.2f},{:.2f}) new=({:.2f},{:.2f},{:.2f})",
                                 m_draw_counter, controller_anchor_params.hand == 0 ? "L" : "R",
                                 posmtx ? 1 : 0, controller_anchor_params.rotation ? 1 : 0, old_x,
                                 old_y, old_z, pnm[0][3], pnm[1][3], pnm[2][3]);
                  }
                }
                else if (hunter_debug_logging)
                {
                  INFO_LOG_FMT(VIDEO, "VR_HANDANCHOR: draw#{} hand={} pose unavailable",
                               m_draw_counter, controller_anchor_params.hand == 0 ? "L" : "R");
                }
              }
            }
            else
            {
              // No override matched — log for debugging if relevant
              hunter.DebugLogUnmatched(vs_hash, ps_hash, gs_hash);
            }
          }

          // ClearEFB is an independent flag, checked regardless of handling type.
          // This lets an element be e.g. Skip+ClearEFB or Screen+ClearEFB.
          if (elements_has_overrides)
            elements.CheckClearEFBForDraw(*element_draw);

          // VR Draw Debug Logging: log every draw call's projection, viewport, scissor, and
          // shader hashes so we can identify how specific visual elements (e.g. cinematic bars)
          // are drawn.
          if (hunter.IsDebugLogging()) [[unlikely]]
          {
            const auto& proj = xfmem.projection;
            const auto& vp = xfmem.viewport;
            const auto& scTL = bpmem.scissorTL;
            const auto& scBR = bpmem.scissorBR;
            const u32 nidx = m_index_generator.GetIndexLen();
            const bool z_test = bpmem.zmode.test_enable != 0;
            const bool color_update = bpmem.blendmode.color_update != 0;
            const bool alpha_update = bpmem.blendmode.alpha_update != 0;
            const bool z_update = bpmem.zmode.update_enable != 0;
            if (proj.type == ProjectionType::Orthographic)
            {
              const float* p = proj.rawProjection.data();
              const float left = -(p[1] + 1) / p[0];
              const float right = left + 2 / p[0];
              const float bottom = -(p[3] + 1) / p[2];
              const float top = bottom + 2 / p[2];
              const float zfar = p[5] / p[4];
              const float znear = (1 + p[4] * zfar) / p[4];
              INFO_LOG_FMT(VIDEO,
                           "VR_DRAW #{}: ORTHO l={:.1f} r={:.1f} t={:.1f} b={:.1f} n={:.1f} "
                           "f={:.1f} | VP({:.0f},{:.0f} {:.0f}x{:.0f}) | SC({},{} {},{})"
                           " | VS={:08x} PS={:08x} GS={:08x} | idx={} col={} alpha={} zt={} "
                           "z={} zf={} | skip={}",
                           m_draw_counter, left, right, top, bottom, znear, zfar, vp.xOrig,
                           vp.yOrig, vp.wd, vp.ht, scTL.x, scTL.y, scBR.x, scBR.y, vs_hash,
                           ps_hash, gs_hash, nidx, color_update, alpha_update, z_test, z_update,
                           bpmem.zmode.func, hunter_skip);
            }
            else
            {
              const float* p = proj.rawProjection.data();
              const float hfov = 2 * std::atan(1.0f / p[0]) * 180.0f / 3.14159265f;
              const float vfov = 2 * std::atan(1.0f / p[2]) * 180.0f / 3.14159265f;
              const float f = p[5] / p[4];
              const float n = f * p[4] / (p[4] - 1);
              INFO_LOG_FMT(VIDEO,
                           "VR_DRAW #{}: PERSP hfov={:.2f} vfov={:.2f} n={:.2f} f={:.2f}"
                           " | VP({:.0f},{:.0f} {:.0f}x{:.0f}) | SC({},{} {},{})"
                           " | VS={:08x} PS={:08x} GS={:08x} | idx={} col={} alpha={} zt={} "
                           "z={} zf={} | skip={}",
                           m_draw_counter, hfov, vfov, n, f, vp.xOrig, vp.yOrig, vp.wd, vp.ht,
                           scTL.x, scTL.y, scBR.x, scBR.y, vs_hash, ps_hash, gs_hash, nidx,
                           color_update, alpha_update, z_test, z_update, bpmem.zmode.func,
                           hunter_skip);
            }
          }
        }

        // Auto-detect EFB-copy effects: bloom, motion blur, and post-processing draw as ortho
        // quads sampling textures the game just copied out of the EFB, so the virtual-screen
        // path would capture them and smear a whole-scene effect across the 2D screen. When
        // such a draw has no explicit override, render it natively into both eye layers
        // instead (same as a manual Fullscreen override). Downscaled copies are effect
        // buffers by construction; full-resolution copies qualify only when blended over the
        // scene, so opaque frozen-frame quads (pause-menu backgrounds) stay on the screen.
        if (g_ActiveConfig.stereo_mode == StereoMode::OpenXR && !vr_flat_mode &&
            g_ActiveConfig.vr_virtual_screen && g_ActiveConfig.vr_auto_native_efb_effects &&
            !hunter_skip && !elements_skip && !texmgr_skip &&
            std::isnan(geometry_shader_manager.vr_stereo_override) &&
            xfmem.projection.type != ProjectionType::Perspective)
        {
          const bool blend_active = bpmem.blendmode.blend_enable || bpmem.blendmode.subtract;
          for (const u32 i : used_textures)
          {
            u32 copy_width = 0;
            u32 copy_height = 0;
            if (!g_texture_cache->IsBoundEfbCopy(i, &copy_width, &copy_height))
              continue;
            const bool downscaled =
                copy_width * 2 <= EFB_WIDTH && copy_height * 2 <= EFB_HEIGHT;
            if (downscaled || blend_active)
            {
              geometry_shader_manager.vr_stereo_override = 0.0f;
              break;
            }
          }
        }

        // Exact virtual-screen depth: swap in the depth-exporting PS variant for draws that land
        // on the VR virtual screen (Screen/HeadLocked overrides and natural ortho draws). The PS
        // then writes the game's flat-screen depth from the flat-interpolated VS capture,
        // restoring GX's deterministic equal-depth semantics (fixes menu z-fighting). This runs
        // AFTER the hunter hashes were computed, so signature hashes stay stable regardless of
        // the toggle. The -3.0 Metroid perspective-HUD path keeps its own depth handling.
        if (g_ActiveConfig.stereo_mode == StereoMode::OpenXR &&
            g_ActiveConfig.vr_exact_screen_depth)
        {
          const float stereo_ovr = geometry_shader_manager.vr_stereo_override;
          const bool forced_screen =
              !std::isnan(stereo_ovr) && (stereo_ovr == -1.0f || stereo_ovr == -2.0f);
          const bool natural_ortho = std::isnan(stereo_ovr) &&
                                     xfmem.projection.type != ProjectionType::Perspective &&
                                     g_ActiveConfig.vr_virtual_screen;
          // Sub-screen 3D panes auto-routed to the virtual screen (see GeometryShaderManager)
          // land there too and need the same exact-depth PS export.
          const bool auto_pane =
              std::isnan(stereo_ovr) && g_ActiveConfig.vr_panes_on_screen &&
              g_ActiveConfig.vr_virtual_screen &&
              xfmem.projection.type == ProjectionType::Perspective &&
              VR::ClassifyVRViewport(xfmem.viewport, bpmem.scissorOffset.x << 1,
                                     bpmem.scissorOffset.y << 1) ==
                  VR::VRViewportClass::HudElement;
          if (forced_screen || natural_ortho || auto_pane)
          {
            auto* const ps_uid_data = m_current_pipeline_config.ps_uid.GetUidData();
            auto* const uber_ps_uid_data = m_current_uber_pipeline_config.ps_uid.GetUidData();
            if (!ps_uid_data->vr_screen_exact_depth || !uber_ps_uid_data->vr_screen_exact_depth)
            {
              ps_uid_data->vr_screen_exact_depth = 1;
              uber_ps_uid_data->vr_screen_exact_depth = 1;
              m_pipeline_config_changed = true;
              UpdatePipelineObject();
            }
          }
        }

        if (!hunter_skip && !elements_skip && !texmgr_skip)
        {
          if (shader_hunter_force_pink && !m_pink_pixel_shader)
          {
            // Simple PS that outputs solid magenta, like 3DMigoto's hunting mode.
            // No #version or #defines — the backend prepends its own SHADER_HEADER
            // (with #version 450, type aliases, etc.) before SPIRV compilation.
            // Outputs both ocol0 and ocol1 (dual-source blending index 1) so shaders
            // using SRC1_ALPHA blend modes still render visibly.
            constexpr std::string_view pink_source =
                "FRAGMENT_OUTPUT_LOCATION_INDEXED(0, 0) out float4 ocol0;\n"
                "FRAGMENT_OUTPUT_LOCATION_INDEXED(0, 1) out float4 ocol1;\n"
                "void main() {\n"
                "  ocol0 = float4(1.0, 0.0, 1.0, 1.0);\n"
                "  ocol1 = float4(0.0, 0.0, 0.0, 1.0);\n"
                "}\n";
            m_pink_pixel_shader = g_gfx->CreateShaderFromSource(
                ShaderStage::Pixel, pink_source, nullptr, "Pink Highlight");
            if (!m_pink_pixel_shader)
              WARN_LOG_FMT(VIDEO, "Failed to compile pink highlight pixel shader");
          }
          m_force_pink_ps = shader_hunter_force_pink;

          const AbstractPipeline* pipeline_object = m_current_pipeline_object;
          if (!custom_pixel_shader_contents.shaders.empty())
          {
            const AbstractPipeline* custom_pipeline = nullptr;
            if (const auto async_pipeline =
                    GetCustomPipeline(custom_pixel_shader_contents, m_current_pipeline_config,
                                      m_current_uber_pipeline_config, m_current_pipeline_object))
            {
              custom_pipeline = async_pipeline;
            }

            if (custom_pipeline)
            {
              pipeline_object = custom_pipeline;
            }
          }
          const bool pane_raster_overridden =
              manual_screen_pane &&
              BPFunctions::SetVRPaneScreenViewport(g_framebuffer_manager.get());
          RenderDrawCall(pixel_shader_manager, geometry_shader_manager,
                         custom_pixel_shader_contents, custom_pixel_shader_uniforms,
                         m_current_primitive_type, pipeline_object);
          if (pane_raster_overridden)
          {
            BPFunctions::SetScissorAndViewport(g_framebuffer_manager.get(), bpmem.scissorTL,
                                               bpmem.scissorBR, bpmem.scissorOffset,
                                               xfmem.viewport);
          }
          m_force_pink_ps = false;
        }
      }
    }

    if (!committed_hide_object_capture)
      HideObjectEngine::Engine::GetInstance().DiscardPendingCapturedPrefixes();

    // Even if we skip the draw, emulated state should still be impacted
    OnDraw();

    // The EFB cache is now potentially stale.
    g_framebuffer_manager->FlagPeekCacheAsOutOfDate();
  }
  else
  {
    HideObjectEngine::Engine::GetInstance().DiscardPendingCapturedPrefixes();
  }

  if (xfmem.numTexGen.numTexGens != bpmem.genMode.numtexgens)
  {
    ERROR_LOG_FMT(VIDEO,
                  "xf.numtexgens ({}) does not match bp.numtexgens ({}). Error in command stream.",
                  xfmem.numTexGen.numTexGens, bpmem.genMode.numtexgens.Value());
  }
}

void VertexManagerBase::DoState(PointerWrap& p)
{
  if (p.IsReadMode())
  {
    // Flush old vertex data before loading state.
    Flush();
  }

  p.Do(m_zslope);
  p.Do(VertexLoaderManager::normal_cache);
  p.Do(VertexLoaderManager::tangent_cache);
  p.Do(VertexLoaderManager::binormal_cache);
}

void VertexManagerBase::CalculateZSlope(NativeVertexFormat* format)
{
  float out[12];
  float viewOffset[2] = {xfmem.viewport.xOrig - bpmem.scissorOffset.x * 2,
                         xfmem.viewport.yOrig - bpmem.scissorOffset.y * 2};

  if (m_current_primitive_type != PrimitiveType::Triangles &&
      m_current_primitive_type != PrimitiveType::TriangleStrip)
  {
    return;
  }

  // Global matrix ID.
  u32 mtxIdx = g_main_cp_state.matrix_index_a.PosNormalMtxIdx;
  const PortableVertexDeclaration vert_decl = format->GetVertexDeclaration();

  // Make sure the buffer contains at least 3 vertices.
  if ((m_cur_buffer_pointer - m_base_buffer_pointer) < (vert_decl.stride * 3))
    return;

  // Lookup vertices of the last rendered triangle and software-transform them
  // This allows us to determine the depth slope, which will be used if z-freeze
  // is enabled in the following flush.
  auto& system = Core::System::GetInstance();
  auto& vertex_shader_manager = system.GetVertexShaderManager();
  for (unsigned int i = 0; i < 3; ++i)
  {
    // If this vertex format has per-vertex position matrix IDs, look it up.
    if (vert_decl.posmtx.enable)
      mtxIdx = VertexLoaderManager::position_matrix_index_cache[2 - i];

    if (vert_decl.position.components == 2)
      VertexLoaderManager::position_cache[2 - i][2] = 0;

    vertex_shader_manager.TransformToClipSpace(&VertexLoaderManager::position_cache[2 - i][0],
                                               &out[i * 4], mtxIdx);

    // Transform to Screenspace
    float inv_w = 1.0f / out[3 + i * 4];

    out[0 + i * 4] = out[0 + i * 4] * inv_w * xfmem.viewport.wd + viewOffset[0];
    out[1 + i * 4] = out[1 + i * 4] * inv_w * xfmem.viewport.ht + viewOffset[1];
    out[2 + i * 4] = out[2 + i * 4] * inv_w * xfmem.viewport.zRange + xfmem.viewport.farZ;
  }

  float dx31 = out[8] - out[0];
  float dx12 = out[0] - out[4];
  float dy12 = out[1] - out[5];
  float dy31 = out[9] - out[1];

  float DF31 = out[10] - out[2];
  float DF21 = out[6] - out[2];
  float a = DF31 * -dy12 - DF21 * dy31;
  float b = dx31 * DF21 + dx12 * DF31;
  float c = -dx12 * dy31 - dx31 * -dy12;

  // Sometimes we process de-generate triangles. Stop any divide by zeros
  if (c == 0)
    return;

  m_zslope.dfdx = -a / c;
  m_zslope.dfdy = -b / c;
  m_zslope.f0 = out[2] - (out[0] * m_zslope.dfdx + out[1] * m_zslope.dfdy);
  m_zslope.dirty = true;
}

void VertexManagerBase::CalculateNormals(NativeVertexFormat* format)
{
  const PortableVertexDeclaration vert_decl = format->GetVertexDeclaration();

  // Only update the binormal/tangent vertex shader constants if the vertex format lacks binormals
  // (VertexLoaderManager::binormal_cache gets updated by the vertex loader when binormals are
  // present, though)
  if (vert_decl.normals[1].enable)
    return;

  VertexLoaderManager::tangent_cache[3] = 0;
  VertexLoaderManager::binormal_cache[3] = 0;

  auto& system = Core::System::GetInstance();
  auto& vertex_shader_manager = system.GetVertexShaderManager();
  if (vertex_shader_manager.constants.cached_tangent != VertexLoaderManager::tangent_cache)
  {
    vertex_shader_manager.constants.cached_tangent = VertexLoaderManager::tangent_cache;
    vertex_shader_manager.dirty = true;
  }
  if (vertex_shader_manager.constants.cached_binormal != VertexLoaderManager::binormal_cache)
  {
    vertex_shader_manager.constants.cached_binormal = VertexLoaderManager::binormal_cache;
    vertex_shader_manager.dirty = true;
  }

  if (vert_decl.normals[0].enable)
    return;

  VertexLoaderManager::normal_cache[3] = 0;
  if (vertex_shader_manager.constants.cached_normal != VertexLoaderManager::normal_cache)
  {
    vertex_shader_manager.constants.cached_normal = VertexLoaderManager::normal_cache;
    vertex_shader_manager.dirty = true;
  }
}

void VertexManagerBase::UpdatePipelineConfig()
{
  NativeVertexFormat* vertex_format = VertexLoaderManager::GetCurrentVertexFormat();
  if (vertex_format != m_current_pipeline_config.vertex_format)
  {
    m_current_pipeline_config.vertex_format = vertex_format;
    m_current_uber_pipeline_config.vertex_format =
        VertexLoaderManager::GetUberVertexFormat(vertex_format->GetVertexDeclaration());
    m_pipeline_config_changed = true;
  }

  VertexShaderUid vs_uid = GetVertexShaderUid();
  if (vs_uid != m_current_pipeline_config.vs_uid)
  {
    m_current_pipeline_config.vs_uid = vs_uid;
    m_current_uber_pipeline_config.vs_uid = UberShader::GetVertexShaderUid();
    m_pipeline_config_changed = true;
  }

  PixelShaderUid ps_uid = GetPixelShaderUid();
  if (ps_uid != m_current_pipeline_config.ps_uid)
  {
    m_current_pipeline_config.ps_uid = ps_uid;
    m_current_uber_pipeline_config.ps_uid = UberShader::GetPixelShaderUid();
    m_pipeline_config_changed = true;
  }

  GeometryShaderUid gs_uid = GetGeometryShaderUid(GetCurrentPrimitiveType());
  if (gs_uid != m_current_pipeline_config.gs_uid)
  {
    m_current_pipeline_config.gs_uid = gs_uid;
    m_current_uber_pipeline_config.gs_uid = gs_uid;
    m_pipeline_config_changed = true;
  }

  if (m_rasterization_state_changed)
  {
    m_rasterization_state_changed = false;

    RasterizationState new_rs = {};
    new_rs.Generate(bpmem, m_current_primitive_type);
    if (new_rs != m_current_pipeline_config.rasterization_state)
    {
      m_current_pipeline_config.rasterization_state = new_rs;
      m_current_uber_pipeline_config.rasterization_state = new_rs;
      m_pipeline_config_changed = true;
    }
  }

  if (m_depth_state_changed)
  {
    m_depth_state_changed = false;

    DepthState new_ds = {};
    new_ds.Generate(bpmem);
    if (new_ds != m_current_pipeline_config.depth_state)
    {
      m_current_pipeline_config.depth_state = new_ds;
      m_current_uber_pipeline_config.depth_state = new_ds;
      m_pipeline_config_changed = true;
    }
  }

  if (m_blending_state_changed)
  {
    m_blending_state_changed = false;

    BlendingState new_bs = {};
    new_bs.Generate(bpmem);
    if (new_bs != m_current_pipeline_config.blending_state)
    {
      m_current_pipeline_config.blending_state = new_bs;
      m_current_uber_pipeline_config.blending_state = new_bs;
      m_pipeline_config_changed = true;
    }
  }
}

void VertexManagerBase::UpdatePipelineObject()
{
  if (!m_pipeline_config_changed)
    return;

  m_current_pipeline_object = nullptr;
  m_current_pipeline_is_uber = false;
  m_pipeline_config_changed = false;

  switch (g_ActiveConfig.iShaderCompilationMode)
  {
  case ShaderCompilationMode::Synchronous:
  {
    // Ubershaders disabled? Block and compile the specialized shader.
    m_current_pipeline_object = g_shader_cache->GetPipelineForUid(m_current_pipeline_config);
  }
  break;

  case ShaderCompilationMode::SynchronousUberShaders:
  {
    // Exclusive ubershader mode, always use ubershaders.
    m_current_pipeline_is_uber = true;
    m_current_pipeline_object =
        g_shader_cache->GetUberPipelineForUid(m_current_uber_pipeline_config);
  }
  break;

  case ShaderCompilationMode::AsynchronousUberShaders:
  case ShaderCompilationMode::AsynchronousSkipRendering:
  {
    // Can we background compile shaders? If so, get the pipeline asynchronously.
    auto res = g_shader_cache->GetPipelineForUidAsync(m_current_pipeline_config);
    if (res)
    {
      // Specialized shaders are ready, prefer these.
      m_current_pipeline_object = *res;
      return;
    }

    if (g_ActiveConfig.iShaderCompilationMode == ShaderCompilationMode::AsynchronousUberShaders)
    {
      // Specialized shaders not ready, use the ubershaders.
      m_current_pipeline_is_uber = true;
      m_current_pipeline_object =
          g_shader_cache->GetUberPipelineForUid(m_current_uber_pipeline_config);
    }
    else
    {
      // Ensure we try again next draw. Otherwise, if no registers change between frames, the
      // object will never be drawn, even when the shader is ready.
      m_pipeline_config_changed = true;
    }
  }
  break;
  }
}

void VertexManagerBase::OnConfigChange()
{
  // Reload index generator function tables in case VS expand config changed
  m_index_generator.Init();
}

void VertexManagerBase::OnDraw()
{
  m_draw_counter++;

  // If the last efb copy was too close to the one before it, don't forget about it until the next
  // efb copy happens (which might not be for a long time)
  u32 diff = m_draw_counter - m_last_efb_copy_draw_counter;
  if (m_unflushed_efb_copy && diff > MINIMUM_DRAW_CALLS_PER_COMMAND_BUFFER_FOR_READBACK)
  {
    g_gfx->Flush();
    m_unflushed_efb_copy = false;
    m_last_efb_copy_draw_counter = m_draw_counter;
  }

  // If we didn't have any CPU access last frame, do nothing.
  if (m_scheduled_command_buffer_kicks.empty() || !m_allow_background_execution)
    return;

  // Check if this draw is scheduled to kick a command buffer.
  // The draw counters will always be sorted so a binary search is possible here.
  if (std::ranges::binary_search(m_scheduled_command_buffer_kicks, m_draw_counter))
  {
    // Kick a command buffer on the background thread.
    g_gfx->Flush();
    m_unflushed_efb_copy = false;
    m_last_efb_copy_draw_counter = m_draw_counter;
  }
}

void VertexManagerBase::OnCPUEFBAccess()
{
  // Check this isn't another access without any draws in between.
  if (!m_cpu_accesses_this_frame.empty() && m_cpu_accesses_this_frame.back() == m_draw_counter)
    return;

  // Store the current draw counter for scheduling in OnEndFrame.
  m_cpu_accesses_this_frame.emplace_back(m_draw_counter);
}

void VertexManagerBase::OnEFBCopyToRAM()
{
  // If we're not deferring, try to preempt it next frame.
  if (!g_ActiveConfig.bDeferEFBCopies)
  {
    OnCPUEFBAccess();
    return;
  }

  // Otherwise, only execute if we have at least 10 objects between us and the last copy.
  const u32 diff = m_draw_counter - m_last_efb_copy_draw_counter;
  m_last_efb_copy_draw_counter = m_draw_counter;
  if (diff < MINIMUM_DRAW_CALLS_PER_COMMAND_BUFFER_FOR_READBACK)
  {
    m_unflushed_efb_copy = true;
    return;
  }

  m_unflushed_efb_copy = false;
  g_gfx->Flush();
}

void VertexManagerBase::OnEndFrame()
{
  auto& hunter = ShaderHunter::GetInstance();
  // Lazy-load shader overrides and hide object codes when game ID becomes available or changes
  const std::string game_id = SConfig::GetInstance().GetGameID();
  if (!game_id.empty())
  {
    hunter.LoadOverridesIfNeeded(game_id);
    ElementsGroupManager::GetInstance().LoadOverridesIfNeeded(game_id);
    TextureElementManager::GetInstance().LoadOverridesIfNeeded(game_id);
    HideObjectEngine::Engine::GetInstance().LoadCodesIfNeeded(game_id);
  }
  hunter.OnFrameEnd();
  CullingCodeFinder::GetInstance().OnFrameEnd();
  ElementsGroupManager::GetInstance().OnFrameEnd();
  Core::System::GetInstance().GetGeometryShaderManager().OnEndFrame();
  if (VR::g_openxr)
    VR::g_openxr->CommitCameraAnchorFrame();
  TextureElementManager::GetInstance().OnFrameEnd();
  HideObjectEngine::Engine::GetInstance().OnFrameEnd();
  GetMetroidElementClassifier().ResetFrame();
  m_metroid_prime1_combat_context_seen = false;
  m_metroid_prime1_menu_context_seen = false;
  m_draw_counter = 0;
  m_last_efb_copy_draw_counter = 0;
  m_scheduled_command_buffer_kicks.clear();

  // If we have no CPU access at all, leave everything in the one command buffer for maximum
  // parallelism between CPU/GPU, at the cost of slightly higher latency.
  if (m_cpu_accesses_this_frame.empty())
    return;

  // In order to reduce CPU readback latency, we want to kick a command buffer roughly halfway
  // between the draw counters that invoked the readback, or every 250 draws, whichever is
  // smaller.
  if (g_ActiveConfig.iCommandBufferExecuteInterval > 0)
  {
    u32 last_draw_counter = 0;
    u32 interval = static_cast<u32>(g_ActiveConfig.iCommandBufferExecuteInterval);
    for (u32 draw_counter : m_cpu_accesses_this_frame)
    {
      // We don't want to waste executing command buffers for only a few draws, so set a minimum.
      // Leave last_draw_counter as-is, so we get the correct number of draws between submissions.
      u32 draw_count = draw_counter - last_draw_counter;
      if (draw_count < MINIMUM_DRAW_CALLS_PER_COMMAND_BUFFER_FOR_READBACK)
        continue;

      if (draw_count <= interval)
      {
        u32 mid_point = draw_count / 2;
        m_scheduled_command_buffer_kicks.emplace_back(last_draw_counter + mid_point);
      }
      else
      {
        u32 counter = interval;
        while (counter < draw_count)
        {
          m_scheduled_command_buffer_kicks.emplace_back(last_draw_counter + counter);
          counter += interval;
        }
      }

      last_draw_counter = draw_counter;
    }
  }

  m_cpu_accesses_this_frame.clear();

  // We invalidate the pipeline object at the start of the frame.
  // This is for the rare case where only a single pipeline configuration is used,
  // and hybrid ubershaders have compiled the specialized shader, but without any
  // state changes the specialized shader will not take over.
  InvalidatePipelineObject();
}

void VertexManagerBase::NotifyCustomShaderCacheOfHostChange(const ShaderHostConfig& host_config)
{
  m_custom_shader_cache->SetHostConfig(host_config);
  m_custom_shader_cache->Reload();
}

void VertexManagerBase::RenderDrawCall(
    PixelShaderManager& pixel_shader_manager, GeometryShaderManager& geometry_shader_manager,
    const CustomPixelShaderContents& custom_pixel_shader_contents,
    std::span<u8> custom_pixel_shader_uniforms, PrimitiveType primitive_type,
    const AbstractPipeline* current_pipeline)
{
  // Now we can upload uniforms, as nothing else will override them.
  geometry_shader_manager.SetConstants(primitive_type);
  pixel_shader_manager.SetConstants();
  if (!custom_pixel_shader_uniforms.empty() &&
      pixel_shader_manager.custom_constants.data() != custom_pixel_shader_uniforms.data())
  {
    pixel_shader_manager.custom_constants_dirty = true;
  }
  pixel_shader_manager.custom_constants = custom_pixel_shader_uniforms;
  UploadUniforms();

  u32 base_vertex, base_index;
  CommitBuffer(m_index_generator.GetNumVerts(),
               VertexLoaderManager::GetCurrentVertexFormat()->GetVertexStride(),
               m_index_generator.GetIndexLen(), &base_vertex, &base_index);

  if (g_backend_info.api_type != APIType::D3D && g_ActiveConfig.UseVSForLinePointExpand() &&
      (primitive_type == PrimitiveType::Points || primitive_type == PrimitiveType::Lines))
  {
    // VS point/line expansion puts the vertex id at gl_VertexID << 2
    // That means the base vertex has to be adjusted to match
    // (The shader adds this after shifting right on D3D, so no need to do this)
    base_vertex <<= 2;
  }

  VRPassthroughCoverageShaderMode coverage_mode =
      m_current_pipeline_is_uber ?
          m_current_uber_pipeline_config.ps_uid.GetUidData()->vr_coverage_mode :
          m_current_pipeline_config.ps_uid.GetUidData()->vr_coverage_mode;
  if (!custom_pixel_shader_contents.shaders.empty() &&
      g_framebuffer_manager->HasEFBCoverage())
  {
    coverage_mode = VRPassthroughCoverageShaderMode::Prepass;
  }

  if (coverage_mode == VRPassthroughCoverageShaderMode::Prepass)
  {
    const AbstractPipeline* const coverage_pipeline =
        m_current_pipeline_is_uber ?
            g_shader_cache->GetCoverageUberPipelineForUid(m_current_uber_pipeline_config) :
            g_shader_cache->GetCoveragePipelineForUid(m_current_pipeline_config);
    if (coverage_pipeline)
    {
      g_framebuffer_manager->BindEFBCoverageFramebuffer();
      g_gfx->SetPipeline(coverage_pipeline);
      DrawCurrentBatch(base_index, m_index_generator.GetIndexLen(), base_vertex);
    }
  }

  // All EFB draws share the with-coverage framebuffer whenever the coverage attachment
  // exists — non-MRT pipelines are built against the same attachment layout with a
  // write-masked attachment 1 (see GetGXPipelineConfig), so the render pass never has
  // to restart between draws (a per-switch GMEM store/load on tiler GPUs).
  if (g_framebuffer_manager->HasEFBCoverage())
    g_framebuffer_manager->BindEFBFramebufferWithCoverage();
  else
    g_framebuffer_manager->BindEFBFramebuffer();
  g_gfx->SetPipeline(current_pipeline);

  // Shader Hunter pink highlight: override PS after pipeline is set (like 3DMigoto).
  if (m_force_pink_ps && m_pink_pixel_shader)
    g_gfx->SetForcePixelShader(m_pink_pixel_shader.get());

  if (PerfQueryBase::ShouldEmulate())
    g_perf_query->EnableQuery(bpmem.zcontrol.early_ztest ? PQG_ZCOMP_ZCOMPLOC : PQG_ZCOMP);

  DrawCurrentBatch(base_index, m_index_generator.GetIndexLen(), base_vertex);

  // Track the total emulated state draws
  INCSTAT(g_stats.this_frame.num_draw_calls);

  if (PerfQueryBase::ShouldEmulate())
    g_perf_query->DisableQuery(bpmem.zcontrol.early_ztest ? PQG_ZCOMP_ZCOMPLOC : PQG_ZCOMP);
}

const AbstractPipeline* VertexManagerBase::GetCustomPipeline(
    const CustomPixelShaderContents& custom_pixel_shader_contents,
    const VideoCommon::GXPipelineUid& current_pipeline_config,
    const VideoCommon::GXUberPipelineUid& current_uber_pipeline_config,
    const AbstractPipeline* current_pipeline) const
{
  if (current_pipeline)
  {
    if (!custom_pixel_shader_contents.shaders.empty())
    {
      VideoCommon::GXPipelineUid custom_pipeline_config = current_pipeline_config;
      VideoCommon::GXUberPipelineUid custom_uber_pipeline_config =
          current_uber_pipeline_config;
      AbstractPipelineConfig custom_base_config = current_pipeline->m_config;
      if (g_framebuffer_manager->HasEFBCoverage())
      {
        custom_pipeline_config.ps_uid.GetUidData()->vr_coverage_mode =
            VRPassthroughCoverageShaderMode::Prepass;
        custom_uber_pipeline_config.ps_uid.GetUidData()->vr_coverage_mode =
            VRPassthroughCoverageShaderMode::Prepass;
        // Match the shared with-coverage render pass (attachment 1 write-masked; the
        // coverage itself comes from the separate prepass) so custom-shader draws don't
        // force a render pass restart either.
        custom_base_config.framebuffer_state = g_framebuffer_manager->GetEFBFramebufferState(true);
        custom_base_config.additional_blending_state = RenderState::GetNoColorWriteBlendState();
        custom_base_config.use_independent_blending = true;
      }

      CustomShaderInstance custom_shaders;
      custom_shaders.pixel_contents = custom_pixel_shader_contents;
      switch (g_ActiveConfig.iShaderCompilationMode)
      {
      case ShaderCompilationMode::Synchronous:
      case ShaderCompilationMode::AsynchronousSkipRendering:
      {
        if (auto pipeline = m_custom_shader_cache->GetPipelineAsync(
                custom_pipeline_config, custom_shaders, custom_base_config))
        {
          return *pipeline;
        }
      }
      break;
      case ShaderCompilationMode::SynchronousUberShaders:
      {
        // Custom pixel shader injection is only supported by specialized pixel shader generation.
        // Avoid custom ubershader fallback in this mode so behavior is consistent across backends.
        if (!custom_pixel_shader_contents.shaders.empty())
        {
          if (auto pipeline = m_custom_shader_cache->GetPipelineAsync(
                  custom_pipeline_config, custom_shaders, custom_base_config))
          {
            return *pipeline;
          }
        }
        else
        {
          // D3D has issues compiling large custom ubershaders, use specialized shaders instead.
          if (g_backend_info.api_type == APIType::D3D)
          {
            if (auto pipeline = m_custom_shader_cache->GetPipelineAsync(
                    custom_pipeline_config, custom_shaders, custom_base_config))
            {
              return *pipeline;
            }
          }
          else
          {
            if (auto pipeline = m_custom_shader_cache->GetPipelineAsync(
                    custom_uber_pipeline_config, custom_shaders, custom_base_config))
            {
              return *pipeline;
            }
          }
        }
      }
      break;
      case ShaderCompilationMode::AsynchronousUberShaders:
      {
        if (auto pipeline = m_custom_shader_cache->GetPipelineAsync(
                custom_pipeline_config, custom_shaders, custom_base_config))
        {
          return *pipeline;
        }
        // Custom pixel shader injection is only supported by specialized shaders.
        else if (custom_pixel_shader_contents.shaders.empty())
        {
          if (auto uber_pipeline = m_custom_shader_cache->GetPipelineAsync(
                  custom_uber_pipeline_config, custom_shaders, custom_base_config))
          {
            return *uber_pipeline;
          }
        }
      }
      break;
      };
    }
  }

  return nullptr;
}
