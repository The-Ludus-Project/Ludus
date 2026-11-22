#include "internal/surface_formats_vulkan.h"

#include "internal/storage.h"

namespace ludus::graphics::rhi::backend
{
VkResult QueryVulkanSurfaceFormat(PFN_vkGetPhysicalDeviceSurfaceFormatsKHR query,
                                  VkPhysicalDevice physical,
                                  VkSurfaceKHR surface,
                                  VkSurfaceFormatKHR& outFormat) noexcept
{
    LUDUS_ASSERT(query != nullptr);
    // Thanks to Khronos Group, Vulkan Specification, vkGetPhysicalDeviceSurfaceFormatsKHR:
    // https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPhysicalDeviceSurfaceFormatsKHR.html
    // Count/list queries can race. Retry VK_INCOMPLETE instead of selecting from a truncated list.
    internal::StorageArray<VkSurfaceFormatKHR> formats;
    for (foundation::uint32 attempt = 0; attempt < MAX_SURFACE_FORMAT_QUERY_ATTEMPTS; ++attempt)
    {
        foundation::uint32 count = 0;
        const VkResult countResult = query(physical, surface, &count, nullptr);
        if (countResult != VK_SUCCESS)
        {
            return countResult;
        }
        if (count == 0)
        {
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
        if (formats.TryResize(count) == false)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        const VkResult listResult = query(physical, surface, &count, formats.GetData());
        if (listResult == VK_INCOMPLETE)
        {
            continue;
        }
        if (listResult != VK_SUCCESS)
        {
            return listResult;
        }
        VkSurfaceFormatKHR selected{};
        for (foundation::uint32 i = 0; i < count; ++i)
        {
            const VkSurfaceFormatKHR candidate = formats[i];
            const VkFormat format = candidate.format;
            if (candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                (format == VK_FORMAT_UNDEFINED || format == VK_FORMAT_B8G8R8A8_UNORM ||
                 format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB ||
                 format == VK_FORMAT_R8G8B8A8_SRGB))
            {
                selected = candidate;
                if (format == VK_FORMAT_UNDEFINED || format == VK_FORMAT_B8G8R8A8_UNORM)
                {
                    selected.format = VK_FORMAT_B8G8R8A8_UNORM;
                    break;
                }
            }
        }
        if (selected.format == VK_FORMAT_UNDEFINED)
        {
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
        outFormat = selected;
        return VK_SUCCESS;
    }
    return VK_INCOMPLETE;
}
} // namespace ludus::graphics::rhi::backend
