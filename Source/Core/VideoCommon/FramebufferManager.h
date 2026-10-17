// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/EnumFormatter.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoEvents.h"

class NativeVertexFormat;
class PointerWrap;

enum class EFBReinterpretType
{
  RGB8ToRGB565 = 0,
  RGB8ToRGBA6 = 1,
  RGBA6ToRGB8 = 2,
  RGBA6ToRGB565 = 3,
  RGB565ToRGB8 = 4,
  RGB565ToRGBA6 = 5
};
constexpr u32 NUM_EFB_REINTERPRET_TYPES = 6;
template <>
struct fmt::formatter<EFBReinterpretType> : EnumFormatter<EFBReinterpretType::RGB565ToRGBA6>
{
  static constexpr array_type names = {"RGB8 to RGB565", "RGB8 to RGBA6",  "RGBA6 to RGB8",
                                       "RGB6 to RGB565", "RGB565 to RGB8", "RGB565 to RGBA6"};
  constexpr formatter() : EnumFormatter(names) {}
};

inline bool AddressRangesOverlap(u32 aLower, u32 aUpper, u32 bLower, u32 bUpper)
{
  return !((aLower >= bUpper) || (bLower >= aUpper));
}

class FramebufferManager final
{
public:
  FramebufferManager();
  ~FramebufferManager();

  // Does not require the framebuffer to be created. Slower than direct queries.
  static AbstractTextureFormat GetEFBColorFormat();
  static AbstractTextureFormat GetEFBDepthFormat();
  static AbstractTextureFormat GetEFBDepthCopyFormat();
  static TextureConfig GetEFBColorTextureConfig(u32 width, u32 height);
  static TextureConfig GetEFBDepthTextureConfig(u32 width, u32 height);

  // Accessors.
  AbstractTexture* GetEFBColorTexture() const { return m_efb_color_texture.get(); }
  AbstractTexture* GetEFBDepthTexture() const { return m_efb_depth_texture.get(); }
  AbstractTexture* GetEFBCoverageTexture() const { return m_efb_coverage_texture.get(); }
  AbstractFramebuffer* GetEFBFramebuffer() const { return m_efb_framebuffer.get(); }
  bool HasEFBCoverage() const { return m_efb_coverage_texture != nullptr; }
  u32 GetEFBWidth() const { return m_efb_color_texture->GetWidth(); }
  u32 GetEFBHeight() const { return m_efb_color_texture->GetHeight(); }
  u32 GetEFBLayers() const { return m_efb_color_texture->GetLayers(); }
  u32 GetEFBSamples() const { return m_efb_color_texture->GetSamples(); }
  bool IsEFBMultisampled() const { return m_efb_color_texture->IsMultisampled(); }
  bool IsEFBStereo() const { return m_efb_color_texture->GetLayers() > 1; }
  FramebufferState GetEFBFramebufferState(bool with_coverage = false) const;
  FramebufferState GetEFBCoverageFramebufferState() const;

  // True when the EFB render pass reads a fragment density map (Vulkan VR foveation).
  // Latched on first EFB creation: cached GX pipelines are built against this render
  // pass layout, so it must not flip mid-session (setting changes need a restart).
  // The backend consults this when creating the EFB framebuffer to attach the map.
  bool IsEFBFoveated() const { return m_efb_foveated.value_or(false); }

  // EFB coordinate conversion functions
  // Use this to convert a whole native EFB rect to backbuffer coordinates
  MathUtil::Rectangle<int> ConvertEFBRectangle(const MathUtil::Rectangle<int>& rc) const;

  unsigned int GetEFBScale() const;

  // Use this to upscale native EFB coordinates to IDEAL internal resolution
  int EFBToScaledX(int x) const;
  int EFBToScaledY(int y) const;

  // Floating point versions of the above - only use them if really necessary
  float EFBToScaledXf(float x) const;
  float EFBToScaledYf(float y) const;

  // First-time setup.
  bool Initialize(int efb_scale);

  // Recreate EFB framebuffers, call when the EFB size (IR) changes.
  void RecreateEFBFramebuffer(int efb_scale);

  // Recompile shaders, use when MSAA mode changes.
  void RecompileShaders();

  // This is virtual, because D3D has both normalized and integer framebuffers.
  void BindEFBFramebuffer();
  void BindEFBFramebufferWithCoverage();
  void BindEFBCoverageFramebuffer();
  void ClearEFBForOpenXR();
  // Queue a ClearEFBForOpenXR for the next game frame's first draw (before_frame_event).
  // Called at the XFB-copy frame boundary (video thread). Deferring means a mid-frame
  // VI present can never wipe a half-drawn frame, and multi-copy bursts keep the EFB
  // intact until every copy of the finished frame has been read out.
  void RequestVRClearEFB() { m_vr_efb_clear_pending = true; }

  // Resolve color/depth textures to a non-msaa texture, and return it.
  AbstractTexture* ResolveEFBColorTexture(const MathUtil::Rectangle<int>& region);
  AbstractTexture* ResolveEFBCoverageTexture(const MathUtil::Rectangle<int>& region);
  AbstractTexture* ResolveEFBDepthTexture(const MathUtil::Rectangle<int>& region,
                                          bool force_r32f = false);

  // Reinterpret pixel format of EFB color texture.
  // Assumes no render pass is currently in progress.
  // Swaps EFB framebuffers, so re-bind afterwards.
  bool ReinterpretPixelData(EFBReinterpretType convtype);
  PixelFormat GetPrevPixelFormat() const { return m_prev_efb_format; }
  void StorePixelFormat(PixelFormat new_format) { m_prev_efb_format = new_format; }

  // Clears the EFB using shaders.
  void ClearEFB(const MathUtil::Rectangle<int>& rc, bool clear_color, bool clear_alpha,
                bool clear_z, u32 color, u32 z, PixelFormat pixel_format);

  AbstractPipeline* GetClearPipeline(bool clear_color, bool clear_alpha, bool clear_z) const;

  // Reads a framebuffer value back from the GPU. This may block if the cache is not current,
  // except in a VR session with GFX_VR_NONBLOCKING_EFB_ACCESS, where a read whose readback is
  // still in flight answers from the last completed one (see PeekEFBCacheNonBlocking).
  u32 PeekEFBColor(u32 x, u32 y);
  float PeekEFBDepth(u32 x, u32 y);
  void SetEFBCacheTileSize(u32 size);
  void InvalidatePeekCache(bool forced = true);
  void RefreshPeekCache();
  void FlagPeekCacheAsOutOfDate();
  void EndOfFrame();

  // Writes a value to the framebuffer. This will never block, and writes will be batched.
  void PokeEFBColor(u32 x, u32 y, u32 color);
  void PokeEFBDepth(u32 x, u32 y, float depth);
  void FlushEFBPokes();

  // Save state load/save.
  void DoState(PointerWrap& p);

protected:
  struct EFBPokeVertex
  {
    float position[4];
    u32 color;
  };
  static_assert(std::is_standard_layout<EFBPokeVertex>::value, "EFBPokeVertex is standard-layout");

  struct EFBCacheTile
  {
    bool present;
    u8 frame_access_mask;
  };

  // EFB cache - for CPU EFB access
  // Tiles are ordered left-to-right, then top-to-bottom
  struct EFBCacheData
  {
    std::unique_ptr<AbstractTexture> texture;
    std::unique_ptr<AbstractFramebuffer> framebuffer;
    std::unique_ptr<AbstractStagingTexture> readback_texture;
    std::unique_ptr<AbstractPipeline> copy_pipeline;
    std::vector<EFBCacheTile> tiles;
    bool out_of_date;
    bool has_active_tiles;
    bool needs_refresh;
    bool needs_flush;

    // Non-blocking EFB access only (see UseNonBlockingEFBAccess). snapshot is a CPU copy of the
    // last readback that completed, laid out like readback_texture (EFB_WIDTH x EFB_HEIGHT
    // texels, same y orientation); snapshot_tiles flags the tiles it holds data for.
    // populated_tiles flags the tiles copied into readback_texture since its contents were last
    // promoted into the snapshot (PromoteReadback).
    std::vector<u8> snapshot;
    std::vector<u8> snapshot_tiles;
    std::vector<bool> populated_tiles;
    // m_efb_cache_frame when the oldest copy still in flight was recorded (valid while
    // needs_flush is set).
    u64 pending_frame;
    // When each tile of the snapshot last received new data. Bounds how old an answer can be.
    std::vector<u64> snapshot_tile_time_us;
    // A sync point's refresh was skipped while an older readback was in flight: the present tiles
    // hold that older sync point's data (reads of them are reported as stale).
    bool serving_skipped_refresh;
  };

  bool CreateEFBFramebuffer(int efb_scale);
  void DestroyEFBFramebuffer();

  bool CompileConversionPipelines();
  void DestroyConversionPipelines();

  bool CompileReadbackPipelines();
  void DestroyReadbackPipelines();

  bool CreateReadbackFramebuffer();
  void DestroyReadbackFramebuffer();

  bool CompileClearPipelines();
  void DestroyClearPipelines();

  bool CompilePokePipelines();
  void DestroyPokePipelines();

  bool IsUsingTiledEFBCache() const;
  bool IsEFBCacheTilePresent(bool depth, u32 x, u32 y, u32* tile_index) const;
  MathUtil::Rectangle<int> GetEFBCacheTileRect(u32 tile_index) const;
  void PopulateEFBCache(bool depth, u32 tile_index, bool async = false);

  // Non-blocking EFB access (GFX_VR_NONBLOCKING_EFB_ACCESS). Evaluated per call; a change of mode
  // drops the snapshots, so data captured under the other mode is never served.
  bool UseNonBlockingEFBAccess();
  // Completes the readback in flight, if any, and promotes the tiles it copied into the snapshot.
  // Without allow_blocking it only succeeds when the copy has already finished on the GPU.
  // Returns whether readback_texture is now flushed.
  bool FlushReadback(EFBCacheData& data, bool allow_blocking);
  void PromoteReadback(EFBCacheData& data);
  bool CanQueueReadback(EFBCacheData& data);
  bool IsSnapshotUsable(const EFBCacheData& data, u32 tile_index) const;
  static void ClearEFBCacheSnapshot(EFBCacheData& data);
  void PeekEFBCacheNonBlocking(bool depth, u32 x, u32 y, u32 tile_index, bool present,
                               void* out_ptr);
  void PokeEFBCacheNonBlocking(EFBCacheData& data, u32 x, u32 y, u32 tile_index, bool present,
                               const void* in_ptr);
  void RefreshPeekCacheNonBlocking(bool at_sync_point);
  void SkipEFBCacheRefresh(EFBCacheData& data);
  static void ReadSnapshotTexel(const EFBCacheData& data, u32 x, u32 y, void* out_ptr);

  void CreatePokeVertices(std::vector<EFBPokeVertex>* destination_list, u32 x, u32 y, float z,
                          u32 color);

  void DrawPokeVertices(const EFBPokeVertex* vertices, u32 vertex_count,
                        const AbstractPipeline* pipeline);

  std::tuple<u32, u32> CalculateTargetSize(int efb_scale);

  void DoLoadState(PointerWrap& p);
  void DoSaveState(PointerWrap& p);

  float m_efb_scale = 1.0f;
  PixelFormat m_prev_efb_format;

  // See IsEFBFoveated(). Latched by the first CreateEFBFramebuffer of the session.
  std::optional<bool> m_efb_foveated;

  std::unique_ptr<AbstractTexture> m_efb_color_texture;
  std::unique_ptr<AbstractTexture> m_efb_convert_color_texture;
  std::unique_ptr<AbstractTexture> m_efb_depth_texture;
  std::unique_ptr<AbstractTexture> m_efb_coverage_texture;
  std::unique_ptr<AbstractTexture> m_efb_resolve_color_texture;
  std::unique_ptr<AbstractTexture> m_efb_resolve_coverage_texture;
  std::unique_ptr<AbstractTexture> m_efb_depth_resolve_texture;

  std::unique_ptr<AbstractFramebuffer> m_efb_framebuffer;
  std::unique_ptr<AbstractFramebuffer> m_efb_convert_framebuffer;
  std::unique_ptr<AbstractFramebuffer> m_efb_framebuffer_with_coverage;
  std::unique_ptr<AbstractFramebuffer> m_efb_convert_framebuffer_with_coverage;
  std::unique_ptr<AbstractFramebuffer> m_efb_coverage_framebuffer;
  std::unique_ptr<AbstractFramebuffer> m_efb_color_resolve_framebuffer;
  std::unique_ptr<AbstractFramebuffer> m_efb_depth_resolve_framebuffer;
  std::unique_ptr<AbstractPipeline> m_efb_color_resolve_pipeline;
  std::unique_ptr<AbstractPipeline> m_efb_depth_resolve_pipeline;

  // Pipeline for restoring the contents of the EFB from a save state
  std::unique_ptr<AbstractPipeline> m_efb_restore_pipeline;
  std::unique_ptr<AbstractPipeline> m_efb_coverage_restore_pipeline;

  // Format conversion shaders
  std::array<std::unique_ptr<AbstractPipeline>, 6> m_format_conversion_pipelines;

  // EFB cache - for CPU EFB access (EFB peeks/pokes), not for EFB copies

  // Width and height of a tile in pixels at 1x IR. 0 indicates non-tiled, in which case a single
  // tile is used for the entire EFB.
  // Note that as EFB peeks and pokes are a CPU feature, they always operate at 1x IR.
  u32 m_efb_cache_tile_size = 0;
  // Number of tiles that make up a row in m_efb_color_cache.tiles / m_efb_depth_cache.tiles.
  u32 m_efb_cache_tile_row_stride = 1;
  EFBCacheData m_efb_color_cache = {};
  EFBCacheData m_efb_depth_cache = {};

  // Latched UseNonBlockingEFBAccess() result: the snapshot bookkeeping only runs while it is set.
  bool m_efb_nonblocking = false;
  // Counts EndOfFrame() calls; ages the readback in flight (see CanQueueReadback).
  u64 m_efb_cache_frame = 0;
  // Set by InvalidatePeekCache(false), which only the BP sync points (draw done, PE tokens) call,
  // right before RefreshPeekCache(). Tells those refreshes apart from the FIFO-idle one.
  bool m_peek_cache_sync_point = false;

  // EFB clear pipelines
  // Indexed by [color_write_enabled][alpha_write_enabled][depth_write_enabled]
  std::array<std::array<std::array<std::unique_ptr<AbstractPipeline>, 2>, 2>, 2> m_clear_pipelines;

  // EFB poke drawing setup
  std::unique_ptr<NativeVertexFormat> m_poke_vertex_format;
  std::unique_ptr<AbstractPipeline> m_color_poke_pipeline;
  std::unique_ptr<AbstractPipeline> m_coverage_poke_pipeline;
  std::unique_ptr<AbstractPipeline> m_depth_poke_pipeline;
  std::vector<EFBPokeVertex> m_color_poke_vertices;
  std::vector<EFBPokeVertex> m_depth_poke_vertices;

  void FlushPendingVRClearEFB();
  bool WillRunVRClearEFB() const;
  bool m_vr_efb_clear_pending = false;

  struct DeferredEFBClear
  {
    MathUtil::Rectangle<int> rc;
    bool color_enable;
    bool alpha_enable;
    bool z_enable;
    u32 color;
    u32 z;
    PixelFormat pixel_format;
  };
  void ClearEFBNow(const DeferredEFBClear& clear);
  // Runs the game clears ClearEFB held back behind a pending VR clear. Called before anything
  // reads, pokes, reinterprets or saves the EFB.
  void FlushDeferredEFBClears();
  // Game clears issued while the VR clear is pending: it overwrites the whole EFB at the next
  // draw, so they only run if the EFB is used before that draw.
  std::vector<DeferredEFBClear> m_deferred_efb_clears;

  Common::EventHook m_end_of_frame_event;
  Common::EventHook m_before_frame_event;
};

extern std::unique_ptr<FramebufferManager> g_framebuffer_manager;
