#include <ludus/graphics/rhi/hosted_surface.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>

#include "internal/hosted_consumer.h"

#include <new>

// Consumer bookkeeping of the hosted surface: one record per imported
// generation, the newest completed frame not yet acquired, and the frames the
// editor holds. See docs/architecture/editor-gui-systems.md, "The hosted surface
// and frame ownership".
namespace ludus::graphics::rhi
{
namespace
{
using namespace foundation;
constexpr logging::LogCategory LOG_RHI_HOSTED{"RHI"};
/// Generations imported at once: the newest plus older ones whose frames are still held.
/// The producer keeps at most three alive, so four leaves room for a late release.
constexpr usize MAX_IMPORTED_GENERATIONS = 4;

struct GenerationRecord final
{
    bool IsLive = false;
    uint32 Generation = 0;
    uint32 Width = 0;
    uint32 Height = 0;
    internal::HostedImport Import;
    StaticArray<bool, platform::HOSTED_SURFACE_IMAGE_COUNT> IsHeld{};
};

struct PendingFrame final
{
    bool IsValid = false;
    uint32 Generation = 0;
    uint32 Image = 0;
    uint64 Sequence = 0;
};

const char* GetRefusalText(HostedConsumerRefusal refusal) noexcept
{
    switch (refusal)
    {
        case HostedConsumerRefusal::None:
            return "none";
        case HostedConsumerRefusal::AdapterMismatch:
            return "the game renders on another GPU adapter or driver";
        case HostedConsumerRefusal::UnsupportedHandle:
            return "the game shares its images with a handle this editor cannot import";
        case HostedConsumerRefusal::ImportFailed:
            return "the game's images could not be imported";
        case HostedConsumerRefusal::ProtocolViolation:
            return "the game announced a frame it did not share";
    }
    return "unknown";
}
} // namespace

struct HostedSurfaceConsumer::State final
{
    platform::HostedWindowEndpoint* EndpointOrNull = nullptr;
    NativeDeviceIdentity Device{};
    StaticArray<GenerationRecord, MAX_IMPORTED_GENERATIONS> Generations{};
    uint32 Latest = 0;
    PendingFrame Pending;
    HostedConsumerRefusal Refusal = HostedConsumerRefusal::None;

    ~State() noexcept
    {
        for (GenerationRecord& record : Generations)
        {
            if (record.IsLive == true)
            {
                internal::DestroyHostedImport(Device, record.Import);
            }
        }
    }

    [[nodiscard]] GenerationRecord* FindOrNull(uint32 generation) noexcept
    {
        for (GenerationRecord& record : Generations)
        {
            if (record.IsLive == true && record.Generation == generation)
            {
                return &record;
            }
        }
        return nullptr;
    }

    void Release(uint32 generation, uint32 image) noexcept
    {
        // A window that is gone needs no release; the frame is simply forgotten.
        static_cast<void>(EndpointOrNull->SendRelease(generation, image));
    }

    /// Destroy older generations of which nothing is held or pending.
    void RetireIdle() noexcept
    {
        for (GenerationRecord& record : Generations)
        {
            if (record.IsLive == false || record.Generation == Latest ||
                (Pending.IsValid == true && Pending.Generation == record.Generation))
            {
                continue;
            }
            bool isHeld = false;
            for (const bool isImageHeld : record.IsHeld)
            {
                isHeld = isHeld == true || isImageHeld == true;
            }
            if (isHeld == false)
            {
                internal::DestroyHostedImport(Device, record.Import);
                record = {};
            }
        }
    }

    void Refuse(HostedConsumerRefusal refusal) noexcept
    {
        if (Refusal == HostedConsumerRefusal::None)
        {
            LUDUS_LOG_ERROR(LOG_RHI_HOSTED, "Play in the viewport is unavailable: {}", GetRefusalText(refusal));
        }
        Refusal = refusal;
        Pending = {};
    }

    void ImportGeneration(platform::HostedSurfaceImages& inoutImages) noexcept
    {
        RetireIdle();
        GenerationRecord* freeOrNull = nullptr;
        for (GenerationRecord& record : Generations)
        {
            if (record.IsLive == false)
            {
                freeOrNull = &record;
                break;
            }
        }
        if (freeOrNull == nullptr)
        {
            Refuse(HostedConsumerRefusal::ProtocolViolation);
            return;
        }
        internal::HostedImport imported;
        const HostedConsumerRefusal refusal = internal::ImportHostedGeneration(Device, inoutImages, imported);
        if (refusal != HostedConsumerRefusal::None)
        {
            Refuse(refusal);
            return;
        }
        freeOrNull->IsLive = true;
        freeOrNull->Generation = inoutImages.Generation;
        freeOrNull->Width = inoutImages.Width;
        freeOrNull->Height = inoutImages.Height;
        freeOrNull->Import = imported;
        freeOrNull->IsHeld = {};
        Latest = inoutImages.Generation;
        // A frame of the previous generation that was never acquired is no longer worth showing.
        if (Pending.IsValid == true)
        {
            Release(Pending.Generation, Pending.Image);
            Pending = {};
        }
    }

    void RecordPresent(uint32 generation, uint32 image, uint64 sequence) noexcept
    {
        GenerationRecord* recordOrNull = FindOrNull(generation);
        if (recordOrNull == nullptr || generation != Latest)
        {
            // A late frame of a replaced generation: return it unseen.
            Release(generation, image);
            return;
        }
        if (recordOrNull->IsHeld[image] == true ||
            (Pending.IsValid == true && Pending.Generation == generation && Pending.Image == image))
        {
            Refuse(HostedConsumerRefusal::ProtocolViolation);
            return;
        }
        if (Pending.IsValid == true)
        {
            // Superseded before it was acquired, so it was never sampled.
            Release(Pending.Generation, Pending.Image);
        }
        Pending = { .IsValid = true, .Generation = generation, .Image = image, .Sequence = sequence };
    }
};

HostedSurfaceConsumer::HostedSurfaceConsumer() noexcept = default;

HostedSurfaceConsumer::~HostedSurfaceConsumer() noexcept
{
    Stop();
}

HostedConsumerStatus HostedSurfaceConsumer::Start(platform::HostedWindowEndpoint& inoutEndpoint,
                                                  const NativeDeviceIdentity& device) noexcept
{
    if (mState.Get() != nullptr)
    {
        return HostedConsumerStatus::InvalidState;
    }
    if (device.Device.IsNull() == true || internal::IsHostedImportAvailable(device.Api) == false)
    {
        return internal::IsHostedImportAvailable(Backend::Metal) == false &&
                       internal::IsHostedImportAvailable(Backend::Vulkan) == false
                   ? HostedConsumerStatus::Unavailable
                   : HostedConsumerStatus::InvalidArgument;
    }
    if (device.Api == Backend::Vulkan && (device.Instance.IsNull() == true || device.PhysicalDevice.IsNull() == true))
    {
        return HostedConsumerStatus::InvalidArgument;
    }
    State* allocationOrNull = new (std::nothrow) State;
    if (allocationOrNull == nullptr)
    {
        return HostedConsumerStatus::Unavailable;
    }
    mState.Reset(allocationOrNull);
    mState->EndpointOrNull = &inoutEndpoint;
    mState->Device = device;
    return HostedConsumerStatus::Ok;
}

void HostedSurfaceConsumer::Stop() noexcept
{
    mState.Reset();
}

HostedConsumerStatus HostedSurfaceConsumer::Pump() noexcept
{
    if (mState.Get() == nullptr)
    {
        return HostedConsumerStatus::InvalidState;
    }
    State& state = *mState;
    platform::HostedSurfaceEvent event;
    while (state.EndpointOrNull->TryTakeSurfaceEvent(event) == true)
    {
        if (state.Refusal != HostedConsumerRefusal::None)
        {
            // Refused for the session: drop images (their handles close) and frames.
            event = {};
            continue;
        }
        if (event.Kind == platform::HostedSurfaceEventKind::Images)
        {
            state.ImportGeneration(event.Images);
        }
        else
        {
            state.RecordPresent(event.Generation, event.Image, event.Sequence);
        }
        event = {};
    }
    state.RetireIdle();
    if (state.Refusal != HostedConsumerRefusal::None)
    {
        return HostedConsumerStatus::Refused;
    }
    return state.EndpointOrNull->GetState() == platform::HostedConnectionState::Connected
               ? HostedConsumerStatus::Ok
               : HostedConsumerStatus::NotConnected;
}

bool HostedSurfaceConsumer::HasFrame() const noexcept
{
    return mState.Get() != nullptr && mState->Pending.IsValid == true;
}

HostedConsumerStatus HostedSurfaceConsumer::AcquireFrame(HostedFrame& outFrame) noexcept
{
    if (mState.Get() == nullptr)
    {
        return HostedConsumerStatus::InvalidState;
    }
    State& state = *mState;
    if (state.Refusal != HostedConsumerRefusal::None)
    {
        return HostedConsumerStatus::Refused;
    }
    if (state.Pending.IsValid == false)
    {
        return HostedConsumerStatus::Empty;
    }
    GenerationRecord* recordOrNull = state.FindOrNull(state.Pending.Generation);
    if (recordOrNull == nullptr)
    {
        state.Pending = {};
        return HostedConsumerStatus::Empty;
    }
    const uint32 image = state.Pending.Image;
    recordOrNull->IsHeld[image] = true;
    HostedFrame frame;
    frame.Generation = state.Pending.Generation;
    frame.Image = image;
    frame.Sequence = state.Pending.Sequence;
    frame.Width = recordOrNull->Width;
    frame.Height = recordOrNull->Height;
    frame.Format = platform::HostedImageFormat::Rgba8Unorm;
    if (state.Device.Api == Backend::Metal)
    {
        frame.MetalTexture = NativeMetalTexture(recordOrNull->Import.MetalTextures[image]);
    }
    else
    {
        frame.VulkanImage = NativeVulkanImage(recordOrNull->Import.VulkanImages[image]);
        frame.VulkanLayout = HOSTED_VULKAN_SHADER_READ_LAYOUT;
        frame.VulkanSourceQueueFamily = HOSTED_VULKAN_QUEUE_FAMILY_EXTERNAL;
    }
    state.Pending = {};
    outFrame = frame;
    return HostedConsumerStatus::Ok;
}

HostedConsumerStatus HostedSurfaceConsumer::ReleaseFrame(const HostedFrame& frame) noexcept
{
    if (mState.Get() == nullptr || frame.Image >= platform::HOSTED_SURFACE_IMAGE_COUNT)
    {
        return HostedConsumerStatus::InvalidState;
    }
    State& state = *mState;
    GenerationRecord* recordOrNull = state.FindOrNull(frame.Generation);
    if (recordOrNull == nullptr || recordOrNull->IsHeld[frame.Image] == false)
    {
        return HostedConsumerStatus::InvalidState;
    }
    recordOrNull->IsHeld[frame.Image] = false;
    state.Release(frame.Generation, frame.Image);
    state.RetireIdle();
    return HostedConsumerStatus::Ok;
}

HostedConsumerRefusal HostedSurfaceConsumer::GetRefusal() const noexcept
{
    return mState.Get() == nullptr ? HostedConsumerRefusal::None : mState->Refusal;
}

usize HostedSurfaceConsumer::GetImportedGenerationCount() const noexcept
{
    if (mState.Get() == nullptr)
    {
        return 0;
    }
    usize count = 0;
    for (const GenerationRecord& record : mState->Generations)
    {
        count += record.IsLive == true ? 1 : 0;
    }
    return count;
}
} // namespace ludus::graphics::rhi
