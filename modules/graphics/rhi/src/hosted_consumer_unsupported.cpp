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
} // namespace ludus::graphics::rhi::internal
