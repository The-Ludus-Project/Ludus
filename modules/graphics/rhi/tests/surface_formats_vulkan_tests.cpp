#include "internal/storage.h"
#include "internal/surface_formats_vulkan.h"

#include <ludus/foundation/containers/static_array.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using ludus::foundation::uint32;
using ludus::foundation::usize;
namespace backend = ludus::graphics::rhi::backend;
namespace internal = ludus::graphics::rhi::internal;

constexpr uint32 ADVERTISED_FORMAT_COUNT = 72;
constexpr usize FIXTURE_FORMAT_CAPACITY = 80;
constexpr VkSurfaceFormatKHR UNCHANGED_FORMAT
{
    .format = VK_FORMAT_R32_SFLOAT,
    .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
};

struct SurfaceFormatFixture final
{
    ludus::foundation::StaticArray<VkSurfaceFormatKHR, FIXTURE_FORMAT_CAPACITY> Formats{};
    uint32 Count = ADVERTISED_FORMAT_COUNT;
    uint32 WrittenCount = ADVERTISED_FORMAT_COUNT;
    uint32 CountCalls = 0;
    uint32 ListCalls = 0;
    uint32 IncompleteLists = 0;
    bool ShouldGrow = false;
    VkResult CountResult = VK_SUCCESS;
    VkResult ListResult = VK_SUCCESS;
};
SurfaceFormatFixture gFixture;

void ResetFixture() noexcept
{
    gFixture = {};
    for (VkSurfaceFormatKHR& format : gFixture.Formats)
    {
        format =
        {
            .format = VK_FORMAT_R16G16B16A16_SFLOAT,
            .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        };
    }
    gFixture.Formats[ADVERTISED_FORMAT_COUNT - 1].format = VK_FORMAT_R8G8B8A8_SRGB;
}

VKAPI_ATTR VkResult VKAPI_CALL QueryFormats(VkPhysicalDevice,
                                            VkSurfaceKHR,
                                            uint32* inoutCount,
                                            VkSurfaceFormatKHR* formatsOrNull) noexcept
{
    if (formatsOrNull == nullptr)
    {
        ++gFixture.CountCalls;
        *inoutCount = gFixture.Count;
        return gFixture.CountResult;
    }
    ++gFixture.ListCalls;
    const uint32 capacity = *inoutCount;
    *inoutCount = gFixture.WrittenCount < capacity ? gFixture.WrittenCount : capacity;
    for (uint32 i = 0; i < *inoutCount; ++i)
    {
        formatsOrNull[i] = gFixture.Formats[i];
    }
    if (gFixture.ShouldGrow == true && gFixture.ListCalls == 1)
    {
        ++gFixture.Count;
        ++gFixture.WrittenCount;
    }
    return gFixture.ListCalls <= gFixture.IncompleteLists ? VK_INCOMPLETE : gFixture.ListResult;
}

VkResult QueryFormat(VkSurfaceFormatKHR& outFormat) noexcept
{
    // The injected query ignores native handles; no Vulkan loader or compositor is needed.
    return backend::QueryVulkanSurfaceFormat(QueryFormats, VK_NULL_HANDLE, VK_NULL_HANDLE, outFormat);
}
} // namespace

TEST_CASE("Vulkan selects a surface format beyond the former fixed capacity", "[rhi][vulkan][surface-formats]")
{
    ResetFixture();
    const usize hostBytes = internal::CurrentStorageUsage().HostBytes;
    VkSurfaceFormatKHR selected = UNCHANGED_FORMAT;
    REQUIRE(QueryFormat(selected) == VK_SUCCESS);
    CHECK(selected.format == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(selected.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    CHECK(gFixture.ListCalls == 1);
    CHECK(internal::CurrentStorageUsage().HostBytes == hostBytes);
}

TEST_CASE("Vulkan retries a growing surface format list without publishing a prefix", "[rhi][vulkan][surface-formats]")
{
    ResetFixture();
    gFixture.ShouldGrow = true;
    gFixture.IncompleteLists = 1;
    // Only the complete list contains a compatible entry.
    gFixture.Formats[ADVERTISED_FORMAT_COUNT].format = VK_FORMAT_R8G8B8A8_UNORM;
    gFixture.Formats[ADVERTISED_FORMAT_COUNT - 1].format = VK_FORMAT_R16G16B16A16_SFLOAT;
    const usize hostBytes = internal::CurrentStorageUsage().HostBytes;
    VkSurfaceFormatKHR selected = UNCHANGED_FORMAT;
    REQUIRE(QueryFormat(selected) == VK_SUCCESS);
    CHECK(selected.format == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(gFixture.CountCalls == 2);
    CHECK(gFixture.ListCalls == 2);
    CHECK(internal::CurrentStorageUsage().HostBytes == hostBytes);
}

TEST_CASE("Vulkan surface format selection preserves its preference and returned count",
          "[rhi][vulkan][surface-formats]")
{
    ResetFixture();
    VkFormat expected = VK_FORMAT_B8G8R8A8_UNORM;
    SECTION("BGRA8 UNORM takes precedence over later compatible entries")
    {
        gFixture.Formats[0].format = expected;
    }
    SECTION("A legacy undefined entry selects BGRA8 UNORM")
    {
        gFixture.Formats[0].format = VK_FORMAT_UNDEFINED;
    }
    SECTION("A compatible format in another color space is ignored")
    {
        gFixture.Formats[0].format = expected;
        gFixture.Formats[0].colorSpace = VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT;
        expected = VK_FORMAT_R8G8B8A8_SRGB;
    }
    SECTION("A shrinking list visits only the returned entries")
    {
        gFixture.WrittenCount = 1;
        gFixture.Formats[0].format = VK_FORMAT_B8G8R8A8_SRGB;
        expected = VK_FORMAT_B8G8R8A8_SRGB;
    }
    VkSurfaceFormatKHR selected = UNCHANGED_FORMAT;
    REQUIRE(QueryFormat(selected) == VK_SUCCESS);
    CHECK(selected.format == expected);
}

TEST_CASE("Vulkan surface format failures preserve output and release query storage", "[rhi][vulkan][surface-formats]")
{
    ResetFixture();
    VkResult expected = VK_ERROR_FORMAT_NOT_SUPPORTED;
    SECTION("An empty advertised list")
    {
        gFixture.Count = 0;
    }
    SECTION("An empty returned list")
    {
        gFixture.WrittenCount = 0;
    }
    SECTION("No compatible format")
    {
        gFixture.Formats[ADVERTISED_FORMAT_COUNT - 1].format = VK_FORMAT_R16G16B16A16_SFLOAT;
    }
    SECTION("Count query failure")
    {
        expected = VK_ERROR_SURFACE_LOST_KHR;
        gFixture.CountResult = expected;
    }
    SECTION("List query failure")
    {
        expected = VK_ERROR_SURFACE_LOST_KHR;
        gFixture.ListResult = expected;
    }
    SECTION("A driver count larger than the RHI host-memory budget")
    {
        expected = VK_ERROR_OUT_OF_HOST_MEMORY;
        gFixture.Count = UINT32_MAX;
    }
    SECTION("A list that never stabilizes exhausts the bounded retry")
    {
        expected = VK_INCOMPLETE;
        gFixture.IncompleteLists = backend::MAX_SURFACE_FORMAT_QUERY_ATTEMPTS;
    }
    const usize hostBytes = internal::CurrentStorageUsage().HostBytes;
    VkSurfaceFormatKHR selected = UNCHANGED_FORMAT;
    CHECK(QueryFormat(selected) == expected);
    CHECK(selected.format == UNCHANGED_FORMAT.format);
    CHECK(selected.colorSpace == UNCHANGED_FORMAT.colorSpace);
    CHECK(internal::CurrentStorageUsage().HostBytes == hostBytes);
    if (expected == VK_INCOMPLETE)
    {
        CHECK(gFixture.ListCalls == backend::MAX_SURFACE_FORMAT_QUERY_ATTEMPTS);
        CHECK(gFixture.CountCalls == backend::MAX_SURFACE_FORMAT_QUERY_ATTEMPTS);
    }
}
