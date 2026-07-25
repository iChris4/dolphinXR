// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/WindowSystemInfo.h"
#include "VideoBackends/Vulkan/Constants.h"
#include "VideoCommon/VideoConfig.h"

namespace Vulkan
{
class VulkanContext
{
public:
  struct PhysicalDeviceInfo
  {
    PhysicalDeviceInfo(const PhysicalDeviceInfo&) = default;
    explicit PhysicalDeviceInfo(VkPhysicalDevice device);
    VkPhysicalDeviceFeatures features() const;

    char deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    u8 pipelineCacheUUID[VK_UUID_SIZE];
    u32 apiVersion;
    u32 driverVersion;
    u32 vendorID;
    u32 deviceID;
    VkDeviceSize minUniformBufferOffsetAlignment;
    VkDeviceSize bufferImageGranularity;
    u32 maxTexelBufferElements;
    u32 maxImageDimension2D;
    u32 maxFragmentOutputAttachments;
    u32 maxFragmentDualSrcAttachments;
    VkSampleCountFlags framebufferColorSampleCounts;
    VkSampleCountFlags framebufferDepthSampleCounts;
    float pointSizeRange[2];
    float maxSamplerAnisotropy;
    u32 subgroupSize = 1;
    VkDriverId driverID = static_cast<VkDriverId>(0);
    bool dualSrcBlend;
    bool independentBlend;
    bool geometryShader;
    bool samplerAnisotropy;
    bool logicOp;
    bool fragmentStoresAndAtomics;
    bool sampleRateShading;
    bool largePoints;
    bool shaderStorageImageMultisample;
    bool shaderTessellationAndGeometryPointSize;
    bool occlusionQueryPrecise;
    bool shaderClipDistance;
    bool depthClamp;
    bool textureCompressionBC;
    bool shaderSubgroupOperations = false;
    bool multiview = false;
    bool timelineSemaphore = false;
    u32 maxMultiviewViewCount = 0;
    u32 maxMultiviewInstanceIndex = 0;
    // VK_EXT_fragment_density_map (VR foveation). nonSubsampled is required to foveate
    // render passes whose attachments are ordinary images (the EFB).
    bool fragmentDensityMap = false;
    bool fragmentDensityMapNonSubsampled = false;
    VkExtent2D minFragmentDensityTexelSize{1, 1};
    VkExtent2D maxFragmentDensityTexelSize{1, 1};
  };

  VulkanContext(VkInstance instance, VkPhysicalDevice physical_device);
  ~VulkanContext();

  // Determines if the Vulkan validation layer is available on the system.
  static bool CheckValidationLayerAvailablility();

  // Helper method to create a Vulkan instance.
  // extra_instance_extensions: additional extensions to enable (e.g. from OpenXR).
  static VkInstance CreateVulkanInstance(
      WindowSystemType wstype, bool enable_debug_utils, bool enable_validation_layer,
      u32* out_vk_api_version,
      const std::vector<std::string>& extra_instance_extensions = {}, u32 max_api_version = 0);

  // Returns a list of Vulkan-compatible GPUs.
  using GPUList = std::vector<VkPhysicalDevice>;
  static GPUList EnumerateGPUs(VkInstance instance);

  // Populates backend/video config.
  // These are public so that the backend info can be populated without creating a context.
  static void PopulateBackendInfo(BackendInfo* backend_info);
  static void PopulateBackendInfoAdapters(BackendInfo* backend_info, const GPUList& gpu_list);
  static void PopulateBackendInfoFeatures(BackendInfo* backend_info, VkPhysicalDevice gpu,
                                          const PhysicalDeviceInfo& info);
  static void PopulateBackendInfoMultisampleModes(BackendInfo* backend_info, VkPhysicalDevice gpu,
                                                  const PhysicalDeviceInfo& info);

  // Creates a Vulkan device context.
  // This assumes that PopulateBackendInfo and PopulateBackendInfoAdapters has already
  // been called for the specified VideoConfig.
  // extra_device_extensions: additional device extensions to enable (e.g. from OpenXR).
  static std::unique_ptr<VulkanContext> Create(
      VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface, bool enable_debug_utils,
      bool enable_validation_layer, u32 api_version,
      const std::vector<std::string>& extra_device_extensions = {});

  // Enable/disable debug message runtime.
  bool EnableDebugUtils();
  void DisableDebugUtils();

  // Global state accessors
  VkInstance GetVulkanInstance() const { return m_instance; }
  VkPhysicalDevice GetPhysicalDevice() const { return m_physical_device; }
  VkDevice GetDevice() const { return m_device; }
  VkQueue GetGraphicsQueue() const { return m_graphics_queue; }
  u32 GetGraphicsQueueFamilyIndex() const { return m_graphics_queue_family_index; }
  VkQueue GetPresentQueue() const { return m_present_queue; }
  u32 GetPresentQueueFamilyIndex() const { return m_present_queue_family_index; }
  const VkQueueFamilyProperties& GetGraphicsQueueProperties() const
  {
    return m_graphics_queue_properties;
  }
  const PhysicalDeviceInfo& GetDeviceInfo() const { return m_device_info; }
  // Support bits
  bool SupportsAnisotropicFiltering() const { return m_device_info.samplerAnisotropy; }
  bool SupportsPreciseOcclusionQueries() const { return m_device_info.occlusionQueryPrecise; }
  u32 GetShaderSubgroupSize() const { return m_device_info.subgroupSize; }
  bool SupportsShaderSubgroupOperations() const { return m_device_info.shaderSubgroupOperations; }
  bool SupportsMultiview() const { return m_multiview_enabled; }
  u32 GetMaxMultiviewViewCount() const { return m_device_info.maxMultiviewViewCount; }
  // True when the timelineSemaphore feature was enabled at device creation (done when the
  // OpenXR runtime lists VK_KHR_timeline_semaphore as a required device extension).
  bool SupportsTimelineSemaphores() const { return m_timeline_semaphore_enabled; }
  // True when VK_EXT_fragment_density_map was enabled at device creation (VR foveation).
  bool SupportsFragmentDensityMap() const { return m_fragment_density_map_enabled; }
  // True when fragmentDensityMapNonSubsampledImages is enabled: FDM render passes may
  // target ordinary (non-subsampled) images such as the EFB.
  bool SupportsNonSubsampledFragmentDensityMap() const { return m_fdm_non_subsampled_enabled; }

  // Dumps VK_EXT_device_fault diagnostics after VK_ERROR_DEVICE_LOST when the driver supports it.
  void LogDeviceFaultInfo() const;

  // VK_EXT_device_address_binding_report hook: the driver reports every GPU virtual-address
  // bind/unbind through the debug messenger so a device fault can be correlated to the resource
  // that owned the faulting page. Called from the messenger callback on arbitrary threads.
  void RecordAddressBinding(u64 base, u64 size, u32 binding_type, u32 flags, int object_type,
                            u64 object_handle);

  // Lightweight CPU-side perf counters for diagnosing VR frame cost. Accumulated by the
  // backend, dumped and reset periodically by CommandBufferManager on present.
  struct PerfCounters
  {
    std::atomic<u64> submit_us{0};
    std::atomic<u64> fence_wait_us{0};
    std::atomic<u64> xr_swapchain_us{0};
    std::atomic<u64> uniform_us{0};
    std::atomic<u64> vertex_commit_us{0};
    std::atomic<u64> draw_us{0};
    std::atomic<u32> draw_count{0};
    std::atomic<u32> submit_count{0};
    std::atomic<u32> pipelines_created{0};
  };
  PerfCounters& GetPerfCounters() { return m_perf_counters; }

  // Helpers for getting constants
  VkDeviceSize GetUniformBufferAlignment() const
  {
    return m_device_info.minUniformBufferOffsetAlignment;
  }
  VkDeviceSize GetBufferImageGranularity() const { return m_device_info.bufferImageGranularity; }
  float GetMaxSamplerAnisotropy() const { return m_device_info.maxSamplerAnisotropy; }

  // Returns true if the specified extension is supported and enabled.
  bool SupportsDeviceExtension(const char* name) const;

  // Returns true if exclusive fullscreen is supported for the given surface.
  bool SupportsExclusiveFullscreen(const WindowSystemInfo& wsi, VkSurfaceKHR surface);

  VmaAllocator GetMemoryAllocator() const { return m_allocator; }

#ifdef WIN32
  // Returns the platform-specific exclusive fullscreen structure.
  VkSurfaceFullScreenExclusiveWin32InfoEXT
  GetPlatformExclusiveFullscreenInfo(const WindowSystemInfo& wsi);
#endif

private:
  static bool SelectInstanceExtensions(
      std::vector<const char*>* extension_list, WindowSystemType wstype, bool enable_debug_utils,
      bool validation_layer_enabled,
      const std::vector<std::string>& extra_extensions = {});
  bool SelectDeviceExtensions(bool enable_surface);
  void WarnMissingDeviceFeatures();
  bool CreateDevice(VkSurfaceKHR surface, bool enable_validation_layer);
  void InitDriverDetails();
  bool CreateAllocator(u32 vk_api_version);

  VkInstance m_instance = VK_NULL_HANDLE;
  VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
  VkDevice m_device = VK_NULL_HANDLE;
  VmaAllocator m_allocator = VK_NULL_HANDLE;

  VkQueue m_graphics_queue = VK_NULL_HANDLE;
  u32 m_graphics_queue_family_index = 0;
  VkQueue m_present_queue = VK_NULL_HANDLE;
  u32 m_present_queue_family_index = 0;
  VkQueueFamilyProperties m_graphics_queue_properties = {};

  VkDebugUtilsMessengerEXT m_debug_utils_messenger = VK_NULL_HANDLE;

  PhysicalDeviceInfo m_device_info;

  std::vector<std::string> m_device_extensions;
  std::vector<std::string> m_extra_device_extensions;

  bool m_multiview_enabled = false;
  bool m_timeline_semaphore_enabled = false;
  bool m_fragment_density_map_enabled = false;
  bool m_fdm_non_subsampled_enabled = false;
  bool m_device_fault_enabled = false;

  // VK_EXT_device_address_binding_report diagnostics (paired with VK_EXT_device_fault). A dedicated
  // debug messenger records recent GPU-VA bind/unbind events into a ring buffer; on device loss the
  // events whose range covers the fault address are dumped to identify the resource.
  bool EnableAddressBindingReport();
  void DisableAddressBindingReport();
  void LogAddressBindingsForFault(u64 fault_address) const;

  struct AddressBindingEvent
  {
    u64 base = 0;
    u64 size = 0;
    u64 seq = 0;
    u64 object_handle = 0;
    u32 binding_type = 0;
    u32 flags = 0;
    int object_type = 0;
  };
  VkDebugUtilsMessengerEXT m_address_binding_messenger = VK_NULL_HANDLE;
  bool m_address_binding_report_enabled = false;
  mutable std::mutex m_address_binding_mutex;
  std::vector<AddressBindingEvent> m_address_binding_events;
  size_t m_address_binding_write = 0;
  std::atomic<u64> m_address_binding_seq{0};

  PerfCounters m_perf_counters;
};

extern std::unique_ptr<VulkanContext> g_vulkan_context;

}  // namespace Vulkan
