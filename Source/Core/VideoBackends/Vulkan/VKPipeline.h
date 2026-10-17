// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <utility>

#include "VideoBackends/Vulkan/VulkanLoader.h"
#include "VideoCommon/AbstractPipeline.h"

namespace Vulkan
{
class VKPipeline final : public AbstractPipeline
{
public:
  explicit VKPipeline(const AbstractPipelineConfig& config, VkPipeline pipeline,
                      VkPipeline plain_pipeline, VkPipelineLayout pipeline_layout,
                      AbstractPipelineUsage usage);
  ~VKPipeline() override;

  // A pipeline built for a foveated EFB render pass also has a twin for the plain one
  // (EFBFoveationScheduler); density_map selects between them.
  VkPipeline GetVkPipeline(bool density_map = true) const
  {
    return density_map || m_plain_pipeline == VK_NULL_HANDLE ? m_pipeline : m_plain_pipeline;
  }

  // Relinquishes ownership of the handles so the caller can hand them to deferred destruction;
  // the destructor then destroys VK_NULL_HANDLE, which is a no-op.
  VkPipeline Release() { return std::exchange(m_pipeline, VK_NULL_HANDLE); }
  VkPipeline ReleasePlain() { return std::exchange(m_plain_pipeline, VK_NULL_HANDLE); }
  VkPipelineLayout GetVkPipelineLayout() const { return m_pipeline_layout; }
  AbstractPipelineUsage GetUsage() const { return m_usage; }
  static std::unique_ptr<VKPipeline> Create(const AbstractPipelineConfig& config);

private:
  VkPipeline m_pipeline;
  VkPipeline m_plain_pipeline;
  VkPipelineLayout m_pipeline_layout;
  AbstractPipelineUsage m_usage;
};

}  // namespace Vulkan
