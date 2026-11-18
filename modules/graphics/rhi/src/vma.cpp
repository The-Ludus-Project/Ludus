// Thanks to AMD GPUOpen and VMA contributors, "Vulkan Memory Allocator",
// Quick start (project setup and dynamic Vulkan functions):
// https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/quick_start.html
// VMA returns Vulkan statuses and needs no C++ exceptions. Configuration and
// the immutable vendor/hash pin are private to GraphicsRhi. The 3.5.0 pin includes
// GPUOpen's fix for finding free space shared by buffer and optimal-image allocations.
// https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/issues/517
#define VMA_IMPLEMENTATION
#include "internal/vma.h"
