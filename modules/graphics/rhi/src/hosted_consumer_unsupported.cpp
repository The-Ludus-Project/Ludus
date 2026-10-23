#include "internal/hosted_consumer.h"

// Targets without a hosted surface (browser builds) import nothing; no backend is ever created.
namespace ludus::graphics::rhi::internal
{
struct HostedConsumerBackend final
{
};

void HostedConsumerBackendDeleter::operator()(HostedConsumerBackend* backendOrNull) const noexcept
{
    delete backendOrNull;
}

bool IsHostedImportAvailable([[maybe_unused]] Backend api) noexcept
{
    return false;
}

bool TryReadHostedConsumerAdapter([[maybe_unused]] const NativeDeviceIdentity& device,
                                  [[maybe_unused]] platform::HostedAdapterIdentity& outAdapter) noexcept
{
    // Nothing is imported, so there is no adapter to announce.
    return false;
}

HostedConsumerStatus CreateHostedConsumerBackend([[maybe_unused]] const NativeDeviceIdentity& device,
                                                 [[maybe_unused]] HostedConsumerBackendOwner& outBackend) noexcept
{
    return HostedConsumerStatus::Unavailable;
}

HostedConsumerRefusal ImportHostedGeneration([[maybe_unused]] HostedConsumerBackend& inoutBackend,
                                             [[maybe_unused]] foundation::usize slot,
                                             platform::HostedSurfaceImages& inoutImages) noexcept
{
    for (platform::HostedImageHandle& handle : inoutImages.Handles)
    {
        handle.Reset();
    }
    return HostedConsumerRefusal::UnsupportedHandle;
}

void DestroyHostedGeneration([[maybe_unused]] HostedConsumerBackend& inoutBackend,
                             [[maybe_unused]] foundation::usize slot) noexcept
{
}

bool AcquireHostedImage([[maybe_unused]] HostedConsumerBackend& inoutBackend,
                        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): shared backend signature
                        [[maybe_unused]] foundation::usize slot,
                        [[maybe_unused]] foundation::uint32 image,
                        [[maybe_unused]] HostedFrame& inoutFrame) noexcept
{
    return false;
}

bool EnqueueHostedRelease([[maybe_unused]] HostedConsumerBackend& inoutBackend,
                          // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): shared backend signature
                          [[maybe_unused]] foundation::usize slot,
                          [[maybe_unused]] foundation::uint32 image) noexcept
{
    return false;
}

bool TryCompleteHostedRelease([[maybe_unused]] HostedConsumerBackend& inoutBackend,
                              // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): shared backend signature
                              [[maybe_unused]] foundation::usize slot,
                              [[maybe_unused]] foundation::uint32 image) noexcept
{
    return true;
}

void CompleteHostedRelease([[maybe_unused]] HostedConsumerBackend& inoutBackend,
                           // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): shared backend signature
                           [[maybe_unused]] foundation::usize slot,
                           [[maybe_unused]] foundation::uint32 image) noexcept
{
}
} // namespace ludus::graphics::rhi::internal
