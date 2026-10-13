add_library(videovulkan
  CommandBufferManager.cpp
  CommandBufferManager.h
  Constants.h
  ObjectCache.cpp
  ObjectCache.h
  SamplerDescriptorSetCache.h
  ShaderCompiler.cpp
  ShaderCompiler.h
  StagingBuffer.cpp
  StagingBuffer.h
  StateTracker.cpp
  StateTracker.h
  VKBoundingBox.cpp
  VKBoundingBox.h
  VKMain.cpp
  VKPerfQuery.cpp
  VKPerfQuery.h
  VKPipeline.cpp
  VKPipeline.h
  VKGfx.cpp
  VKGfx.h
  VKRecordingQueue.h
  VKRecordingWorker.cpp
  VKRecordingWorker.h
  VKShader.cpp
  VKShader.h
  VKStreamBuffer.cpp
  VKStreamBuffer.h
  VKSwapChain.cpp
  VKSwapChain.h
  VKTexture.cpp
  VKTexture.h
  VKVertexFormat.cpp
  VKVertexFormat.h
  VKVertexManager.cpp
  VKVertexManager.h
  VulkanContext.cpp
  VulkanContext.h
  VulkanLoader.cpp
  VulkanLoader.h
)

target_link_libraries(videovulkan
PUBLIC
  common
  videocommon

PRIVATE
  xxhash::xxhash
)

if (ANDROID AND _M_ARM_64)
  target_link_libraries(videovulkan
  PRIVATE
      adrenotools
  )
endif()

# Only include the Vulkan headers when building the Vulkan backend
target_include_directories(videovulkan
PRIVATE
  ${CMAKE_SOURCE_DIR}/Externals/Vulkan-Headers/include
  ${CMAKE_SOURCE_DIR}/Externals/VulkanMemoryAllocator/include
  ${CMAKE_SOURCE_DIR}/Externals/libadrenotools/include
)

if(ENABLE_VR)
  target_sources(videovulkan PRIVATE
    VulkanOpenXR.cpp
    VulkanOpenXR.h
    ${CMAKE_SOURCE_DIR}/Source/Core/VideoCommon/VR/OpenXRUtilitySession.cpp
    ${CMAKE_SOURCE_DIR}/Externals/imgui/imgui/backends/imgui_impl_vulkan.cpp
  )
  target_compile_definitions(videovulkan PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
  target_link_libraries(videovulkan PRIVATE imgui openxr_loader)
  target_include_directories(videovulkan PRIVATE
    "${CMAKE_SOURCE_DIR}/Externals/OpenXR/include"
    "${CMAKE_SOURCE_DIR}/Externals/imgui/imgui/backends"
  )
endif()

if(MSVC)
  # Add precompiled header
  target_link_libraries(videovulkan PRIVATE use_pch)
endif()
