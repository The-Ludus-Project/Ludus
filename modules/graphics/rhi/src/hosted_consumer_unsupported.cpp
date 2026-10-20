#include "internal/hosted_consumer.h"

// Targets without a hosted surface (browser builds) import nothing.
namespace ludus::graphics::rhi::internal
{
bool IsHostedImportAvailable([[maybe_unused]] Backend api) noexcept
{
    return false;
}

HostedConsumerRefusal ImportHostedGeneration([[maybe_unused]] const NativeDeviceIdentity& device,
                                             platform::HostedSurfaceImages& inoutImages,
                                             [[maybe_unused]] HostedImport& outImport) noexcept
{
    for (platform::HostedImageHandle& handle : inoutImages.Handles)
    {
        handle.Reset();
    }
    return HostedConsumerRefusal::UnsupportedHandle;
}

void DestroyHostedImport([[maybe_unused]] const NativeDeviceIdentity& device,
                         [[maybe_unused]] HostedImport& inoutImport) noexcept
{
}

bool EnqueueHostedQueueMarker([[maybe_unused]] const NativeDeviceIdentity& device,
                              [[maybe_unused]] HostedQueueMarker& outMarker) noexcept
{
    return false;
}

bool IsHostedQueueMarkerComplete([[maybe_unused]] const NativeDeviceIdentity& device,
                                 [[maybe_unused]] const HostedQueueMarker& marker) noexcept
{
    return true;
}

void WaitHostedQueueMarker([[maybe_unused]] const NativeDeviceIdentity& device,
                           [[maybe_unused]] const HostedQueueMarker& marker) noexcept
{
}

void DestroyHostedQueueMarker([[maybe_unused]] const NativeDeviceIdentity& device,
                              HostedQueueMarker& inoutMarker) noexcept
{
    inoutMarker = {};
}

HostedConsumerStatus RecordHostedOwnershipTransfer([[maybe_unused]] const NativeDeviceIdentity& device,
                                                   [[maybe_unused]] foundation::uint64 image,
                                                   [[maybe_unused]] NativeVulkanCommandBuffer commandBuffer,
                                                   [[maybe_unused]] HostedOwnershipTransfer transfer) noexcept
{
    return HostedConsumerStatus::Unavailable;
}
} // namespace ludus::graphics::rhi::internal
