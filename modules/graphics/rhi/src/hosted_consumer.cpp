#include <ludus/graphics/rhi/hosted_surface.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>

#include "internal/hosted_consumer.h"

#include <new>

// Consumer bookkeeping of the hosted surface: one record per imported
// generation, the newest completed frame not yet acquired, and the frames the
// editor holds. Native objects live in the backend, addressed by generation slot
// and image index. See docs/architecture/editor-gui-systems.md, "The hosted
// surface and frame ownership".
namespace ludus::graphics::rhi
{
namespace
{
using namespace foundation;
constexpr logging::LogCategory LOG_RHI_HOSTED{"RHI"};
constexpr usize MAX_IMPORTED_GENERATIONS = internal::HOSTED_GENERATION_SLOTS;
/// Releases waiting for the queue: at most every image of every imported generation.
constexpr usize MAX_QUEUED_RELEASES = MAX_IMPORTED_GENERATIONS * platform::HOSTED_SURFACE_IMAGE_COUNT;
/// Slot index of a generation that is not imported.
constexpr usize NO_SLOT = MAX_IMPORTED_GENERATIONS;

struct GenerationRecord final
{
    bool IsLive = false;
    uint32 Generation = 0;
    uint32 Width = 0;
    uint32 Height = 0;
    StaticArray<bool, platform::HOSTED_SURFACE_IMAGE_COUNT> IsHeld{};
};

/// A held frame returned after the consumer's queue finishes the sampling submitted before it.
struct QueuedRelease final
{
    bool IsValid = false;
    uint32 Generation = 0;
    uint32 Image = 0;
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
    internal::HostedConsumerBackendOwner Backend;
    StaticArray<GenerationRecord, MAX_IMPORTED_GENERATIONS> Generations{};
    uint32 Latest = 0;
    PendingFrame Pending;
    StaticArray<QueuedRelease, MAX_QUEUED_RELEASES> QueuedReleases{};
    HostedConsumerRefusal Refusal = HostedConsumerRefusal::None;

    ~State() noexcept
    {
        if (Backend.Get() == nullptr)
        {
            return;
        }
        // The imports may still be sampled by work ahead of a queued release; wait for it first.
        for (QueuedRelease& queued : QueuedReleases)
        {
            const usize slot = FindSlot(queued.Generation);
            if (queued.IsValid == true && slot != NO_SLOT)
            {
                internal::CompleteHostedRelease(*Backend, slot, queued.Image);
            }
            queued = {};
        }
        for (usize slot = 0; slot < MAX_IMPORTED_GENERATIONS; ++slot)
        {
            if (Generations[slot].IsLive == true)
            {
                internal::DestroyHostedGeneration(*Backend, slot);
            }
        }
    }

    /// Slot of an imported generation, or NO_SLOT.
    [[nodiscard]] usize FindSlot(uint32 generation) const noexcept
    {
        for (usize slot = 0; slot < MAX_IMPORTED_GENERATIONS; ++slot)
        {
            if (Generations[slot].IsLive == true && Generations[slot].Generation == generation)
            {
                return slot;
            }
        }
        return NO_SLOT;
    }

    void Release(uint32 generation, uint32 image) noexcept
    {
        // A window that is gone needs no release; the frame is simply forgotten.
        static_cast<void>(EndpointOrNull->SendRelease(generation, image));
    }

    [[nodiscard]] bool IsQueued(uint32 generation, uint32 image) const noexcept
    {
        for (const QueuedRelease& queued : QueuedReleases)
        {
            if (queued.IsValid == true && queued.Generation == generation && queued.Image == image)
            {
                return true;
            }
        }
        return false;
    }

    /// Slot of a held frame, or NO_SLOT when `frame` is not held or already queued for release.
    [[nodiscard]] usize FindHeldSlot(const HostedFrame& frame) const noexcept
    {
        if (frame.Image >= platform::HOSTED_SURFACE_IMAGE_COUNT)
        {
            return NO_SLOT;
        }
        const usize slot = FindSlot(frame.Generation);
        if (slot == NO_SLOT || Generations[slot].IsHeld[frame.Image] == false ||
            IsQueued(frame.Generation, frame.Image) == true)
        {
            return NO_SLOT;
        }
        return slot;
    }

    /// Return every queued frame whose release the queue has completed.
    void CompleteQueuedReleases() noexcept
    {
        for (QueuedRelease& queued : QueuedReleases)
        {
            if (queued.IsValid == false)
            {
                continue;
            }
            const usize slot = FindSlot(queued.Generation);
            if (slot != NO_SLOT)
            {
                if (internal::TryCompleteHostedRelease(*Backend, slot, queued.Image) == false)
                {
                    continue;
                }
                Generations[slot].IsHeld[queued.Image] = false;
                Release(queued.Generation, queued.Image);
            }
            queued = {};
        }
    }

    /// Destroy older generations of which nothing is held or pending.
    void RetireIdle() noexcept
    {
        for (usize slot = 0; slot < MAX_IMPORTED_GENERATIONS; ++slot)
        {
            GenerationRecord& record = Generations[slot];
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
                internal::DestroyHostedGeneration(*Backend, slot);
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
        usize freeSlot = NO_SLOT;
        for (usize slot = 0; slot < MAX_IMPORTED_GENERATIONS; ++slot)
        {
            if (Generations[slot].IsLive == false)
            {
                freeSlot = slot;
                break;
            }
        }
        if (freeSlot == NO_SLOT)
        {
            Refuse(HostedConsumerRefusal::ProtocolViolation);
            return;
        }
        const HostedConsumerRefusal refusal = internal::ImportHostedGeneration(*Backend, freeSlot, inoutImages);
        if (refusal != HostedConsumerRefusal::None)
        {
            Refuse(refusal);
            return;
        }
        GenerationRecord& record = Generations[freeSlot];
        record.IsLive = true;
        record.Generation = inoutImages.Generation;
        record.Width = inoutImages.Width;
        record.Height = inoutImages.Height;
        record.IsHeld = {};
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
        const usize slot = FindSlot(generation);
        if (slot == NO_SLOT || generation != Latest)
        {
            // A late frame of a replaced generation: return it unseen.
            Release(generation, image);
            return;
        }
        if (Generations[slot].IsHeld[image] == true ||
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
    internal::HostedConsumerBackendOwner backend;
    const HostedConsumerStatus created = internal::CreateHostedConsumerBackend(device, backend);
    if (created != HostedConsumerStatus::Ok)
    {
        return created;
    }
    State* allocationOrNull = new (std::nothrow) State;
    if (allocationOrNull == nullptr)
    {
        return HostedConsumerStatus::Unavailable;
    }
    mState.Reset(allocationOrNull);
    mState->EndpointOrNull = &inoutEndpoint;
    mState->Backend = foundation::Move(backend);
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
    state.CompleteQueuedReleases();
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
    const PendingFrame pending = state.Pending;
    state.Pending = {};
    const usize slot = state.FindSlot(pending.Generation);
    if (slot == NO_SLOT)
    {
        return HostedConsumerStatus::Empty;
    }
    const GenerationRecord& record = state.Generations[slot];
    HostedFrame frame;
    frame.Generation = pending.Generation;
    frame.Image = pending.Image;
    frame.Sequence = pending.Sequence;
    frame.Width = record.Width;
    frame.Height = record.Height;
    frame.Format = platform::HostedImageFormat::Rgba8Unorm;
    if (internal::AcquireHostedImage(*state.Backend, slot, pending.Image, frame) == false)
    {
        // Never sampled: the game may render into it again.
        state.Release(pending.Generation, pending.Image);
        return HostedConsumerStatus::Unavailable;
    }
    state.Generations[slot].IsHeld[pending.Image] = true;
    outFrame = frame;
    return HostedConsumerStatus::Ok;
}

HostedConsumerStatus HostedSurfaceConsumer::ReleaseFrame(const HostedFrame& frame) noexcept
{
    if (mState.Get() == nullptr)
    {
        return HostedConsumerStatus::InvalidState;
    }
    State& state = *mState;
    const usize slot = state.FindHeldSlot(frame);
    if (slot == NO_SLOT)
    {
        return HostedConsumerStatus::InvalidState;
    }
    state.Generations[slot].IsHeld[frame.Image] = false;
    state.Release(frame.Generation, frame.Image);
    state.RetireIdle();
    return HostedConsumerStatus::Ok;
}

HostedConsumerStatus HostedSurfaceConsumer::ReleaseFrameAfterQueue(const HostedFrame& frame) noexcept
{
    if (mState.Get() == nullptr)
    {
        return HostedConsumerStatus::InvalidState;
    }
    State& state = *mState;
    const usize slot = state.FindHeldSlot(frame);
    if (slot == NO_SLOT)
    {
        return HostedConsumerStatus::InvalidState;
    }
    QueuedRelease* freeOrNull = nullptr;
    for (QueuedRelease& queued : state.QueuedReleases)
    {
        if (queued.IsValid == false)
        {
            freeOrNull = &queued;
            break;
        }
    }
    // Every held image fits: the table covers all images of all imported generations.
    LUDUS_REQUIRE(freeOrNull != nullptr);
    if (internal::EnqueueHostedRelease(*state.Backend, slot, frame.Image) == false)
    {
        return HostedConsumerStatus::Unavailable;
    }
    *freeOrNull = { .IsValid = true, .Generation = frame.Generation, .Image = frame.Image };
    return HostedConsumerStatus::Ok;
}

usize HostedSurfaceConsumer::GetQueuedReleaseCount() const noexcept
{
    if (mState.Get() == nullptr)
    {
        return 0;
    }
    usize count = 0;
    for (const QueuedRelease& queued : mState->QueuedReleases)
    {
        count += queued.IsValid == true ? 1 : 0;
    }
    return count;
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
