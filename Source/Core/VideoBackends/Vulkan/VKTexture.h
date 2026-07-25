// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "VideoBackends/Vulkan/VulkanLoader.h"
#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"

namespace Vulkan
{
class StagingBuffer;
class Texture2D;

class VKTexture final : public AbstractTexture
{
public:
  // Custom image layouts, mainly used for switching to/from compute
  enum class ComputeImageLayout
  {
    Undefined,
    ReadOnly,
    WriteOnly,
    ReadWrite
  };

  VKTexture() = delete;
  VKTexture(const TextureConfig& tex_config, VmaAllocation alloc, VkImage image,
            std::string_view name, VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED,
            ComputeImageLayout compute_layout = ComputeImageLayout::Undefined);
  ~VKTexture() override;

  static VkFormat GetLinearFormat(VkFormat format);
  static VkFormat GetVkFormatForHostTextureFormat(AbstractTextureFormat format);
  static VkImageAspectFlags GetImageAspectForFormat(AbstractTextureFormat format);
  static VkImageAspectFlags GetImageViewAspectForFormat(AbstractTextureFormat format);

  void CopyRectangleFromTexture(const AbstractTexture* src,
                                const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                u32 src_level, const MathUtil::Rectangle<int>& dst_rect,
                                u32 dst_layer, u32 dst_level) override;
  void ResolveFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& rect,
                          u32 layer, u32 level) override;
  void Load(u32 level, u32 width, u32 height, u32 row_length, const u8* buffer, size_t buffer_size,
            u32 layer) override;
  void FinishedRendering() override;

  VkImage GetImage() const { return m_image; }
  VkImageView GetView() const { return m_view; }
  // Relinquishes the image view without putting it on the deferred-destruction queue.
  // Used for externally owned images whose owner must not destroy the image before its
  // Dolphin-created view has actually been destroyed.
  VkImageView ReleaseView();
  VkImageLayout GetLayout() const { return m_layout; }
  VkFormat GetVkFormat() const
  {
    return m_vk_format_override != VK_FORMAT_UNDEFINED ? m_vk_format_override
                                                       : GetVkFormatForHostTextureFormat(m_config.format);
  }
  bool IsAdopted() const { return m_alloc != VmaAllocation(VK_NULL_HANDLE); }

  static std::unique_ptr<VKTexture> Create(const TextureConfig& tex_config, std::string_view name);
  static std::unique_ptr<VKTexture>
  CreateAdopted(const TextureConfig& tex_config, VkImage image,
                VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED,
                VkFormat vk_format_override = VK_FORMAT_UNDEFINED);

  // Used when the render pass is changing the image layout, or to force it to
  // VK_IMAGE_LAYOUT_UNDEFINED, if the existing contents of the image is
  // irrelevant and will not be loaded.
  void OverrideImageLayout(VkImageLayout new_layout);

  void TransitionToLayout(VkCommandBuffer command_buffer, VkImageLayout new_layout) const;
  void TransitionToLayout(VkCommandBuffer command_buffer, ComputeImageLayout new_layout) const;
  void PrepareForRenderPass(VkCommandBuffer command_buffer) const;

private:
  bool CreateView(VkImageViewType type);

  VmaAllocation m_alloc;
  VkImage m_image;
  VkImageView m_view = VK_NULL_HANDLE;
  VkFormat m_vk_format_override = VK_FORMAT_UNDEFINED;
  mutable VkImageLayout m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  mutable ComputeImageLayout m_compute_layout = ComputeImageLayout::Undefined;
  mutable bool m_written_since_last_layout_change = false;
  std::string m_name;
};

class VKStagingTexture final : public AbstractStagingTexture
{
  struct PrivateTag
  {
  };

public:
  VKStagingTexture() = delete;
  VKStagingTexture(PrivateTag, StagingTextureType type, const TextureConfig& config,
                   std::unique_ptr<StagingBuffer> buffer, VkImage linear_image,
                   VmaAllocation linear_image_alloc);

  ~VKStagingTexture() override;

  void CopyFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& src_rect,
                       u32 src_layer, u32 src_level,
                       const MathUtil::Rectangle<int>& dst_rect) override;
  void CopyToTexture(const MathUtil::Rectangle<int>& src_rect, AbstractTexture* dst,
                     const MathUtil::Rectangle<int>& dst_rect, u32 dst_layer,
                     u32 dst_level) override;

  bool Map() override;
  void Unmap() override;
  void Flush() override;

  static std::unique_ptr<VKStagingTexture> Create(StagingTextureType type,
                                                  const TextureConfig& config);

  static std::pair<VkImage, VmaAllocation> CreateLinearImage(StagingTextureType type,
                                                             const TextureConfig& config);

private:
  void CopyFromTextureToLinearImage(const VKTexture* src_tex,
                                    const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                    u32 src_level, const MathUtil::Rectangle<int>& dst_rect);

  std::unique_ptr<StagingBuffer> m_staging_buffer;
  VkImage m_linear_image = VK_NULL_HANDLE;
  VmaAllocation m_linear_image_alloc = VK_NULL_HANDLE;
  u64 m_flush_fence_counter = 0;
};

class VKFramebuffer final : public AbstractFramebuffer
{
public:
  VKFramebuffer(VKTexture* color_attachment, VKTexture* depth_attachment,
                std::vector<AbstractTexture*> additional_color_attachments, u32 width, u32 height,
                u32 layers, u32 samples, VkFramebuffer fb, VkRenderPass load_render_pass,
                VkRenderPass discard_render_pass, VkRenderPass clear_render_pass,
                bool has_fragment_density_map = false, bool multiview = false);
  ~VKFramebuffer() override;

  VkFramebuffer GetFB() const { return m_fb; }
  // See VKTexture::ReleaseView(). The caller becomes responsible for destroying the handle.
  VkFramebuffer ReleaseHandle();
  VkRect2D GetRect() const { return VkRect2D{{0, 0}, {m_width, m_height}}; }

  VkRenderPass GetLoadRenderPass() const { return m_load_render_pass; }
  VkRenderPass GetDiscardRenderPass() const { return m_discard_render_pass; }
  VkRenderPass GetClearRenderPass() const { return m_clear_render_pass; }

  // True when the render passes use VK_KHR_multiview (VR stereo EFB). The VkFramebuffer
  // was then created with layers = 1 — per-view replication comes from the view mask, and
  // vkCmdClearAttachments requires VkClearRect layerCount == 1 inside such a pass.
  bool IsMultiview() const { return m_multiview; }

  void Unbind();
  void TransitionForRender();
  void PrepareForRenderPass();

  void SetAndClear(const VkRect2D& rect, const VkClearValue& color_value,
                   const VkClearValue& depth_value);
  std::size_t GetNumberOfAdditonalAttachments() const
  {
    return m_additional_color_attachments.size();
  }

  // fragment_density_map_view: optional VK_EXT_fragment_density_map attachment (VR
  // foveation). The view must outlive the framebuffer; render passes get an FDM entry.
  static std::unique_ptr<VKFramebuffer>
  Create(VKTexture* color_attachments, VKTexture* depth_attachment,
         std::vector<AbstractTexture*> additional_color_attachments,
         VkImageView fragment_density_map_view = VK_NULL_HANDLE);
  static std::unique_ptr<VKFramebuffer>
  CreateMultiview(VKTexture* color_attachment, VKTexture* depth_attachment,
                  std::vector<AbstractTexture*> additional_color_attachments,
                  VkImageView fragment_density_map_view = VK_NULL_HANDLE);

protected:
  VkFramebuffer m_fb;
  VkRenderPass m_load_render_pass;
  VkRenderPass m_discard_render_pass;
  VkRenderPass m_clear_render_pass;
  bool m_multiview = false;
};

}  // namespace Vulkan
