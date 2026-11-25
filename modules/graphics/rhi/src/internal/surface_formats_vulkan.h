#pragma once

#include <ludus/foundation/base/types.h>

#include <volk.h>

namespace ludus::graphics::rhi::backend
{
/// Bound repeated count/list races without imposing a limit on advertised formats.
inline constexpr foundation::uint32 MAX_SURFACE_FORMAT_QUERY_ATTEMPTS = 3;

/// Query the complete format list using temporary, host-budgeted RHI storage.
/// Retains the existing preference for BGRA8 UNORM, then the last compatible
/// RGBA8/BGRA8 UNORM or sRGB entry. A legacy undefined entry selects BGRA8 UNORM.
/// @param query Nonnull Vulkan entry point; injected by driver-independent tests.
/// @param physical Adapter associated with surface; native handle validity belongs to query.
/// @param surface Surface supported by physical.
/// @param outFormat Written only on VK_SUCCESS; preserved on every failure.
/// @return VK_SUCCESS, the driver error, VK_ERROR_FORMAT_NOT_SUPPORTED for an
/// empty/incompatible list, VK_ERROR_OUT_OF_HOST_MEMORY for storage rejection,
/// or VK_INCOMPLETE when the list never stabilizes within the retry bound.
/// @note Device owner thread, between frames. Temporary storage is released before return.
[[nodiscard]] VkResult QueryVulkanSurfaceFormat(PFN_vkGetPhysicalDeviceSurfaceFormatsKHR query,
                                                VkPhysicalDevice physical,
                                                VkSurfaceKHR surface,
                                                VkSurfaceFormatKHR& outFormat) noexcept;
} // namespace ludus::graphics::rhi::backend
