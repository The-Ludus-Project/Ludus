#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/memory/allocation_statistics.hpp>

#include <atomic>
#include <mutex>

namespace ludus::foundation::memory_internal
{
inline constexpr usize MAX_ACTIVITY_LIVE_SLOTS = 8192;
inline constexpr usize MAX_ACTIVITY_EVENTS = 16384;

enum class ActivityKind : uint8
{
    Allocate,
    Free,
    Failure,
};

enum class ActivityLoss : uint8
{
    None,
    JournalFull,
    TableFull,
    InvalidOwnership,
    Reentry,
};

struct ActivityMetadata
{
    uint64 Ticks;
    uint64 Thread;
};

struct ActivityEvent
{
    ActivityMetadata Metadata;
    uintptr Address;
    usize Bytes;
    usize Alignment;
    usize Lifetime;
    AllocationDomainId Domain;
    AllocationSiteId Site;
    ActivityKind Kind;
};

struct ObservationConfig
{
    usize LiveSlots;
    usize EventLimit;
    AllocationSiteId SiteCount;
};

// Private FoundationMemory/Profiling seam. Only the coordinator attaches a
// route, after joining whole operations on every selected domain.
class Observation final
{
public:
    using ReadMetadata = ActivityMetadata (*)() noexcept;

    Observation(const ObservationConfig& config, ReadMetadata readMetadata) noexcept;

    static void Attach(const AllocationDomain& domain, AllocationDomainId id, Observation& inoutObservation) noexcept;
    static void Detach(const AllocationDomain& domain) noexcept;

    void Record(ActivityKind kind,
                AllocationDomainId domain,
                void* pointerOrNull,
                usize bytes,
                usize alignment,
                AllocationSiteId site = 0) noexcept;
    void MarkReentry() noexcept;

    [[nodiscard]] usize GetEventCount() const noexcept;
    [[nodiscard]] const ActivityEvent& GetEvent(usize index) const noexcept;
    [[nodiscard]] ActivityLoss GetLoss() const noexcept;
    [[nodiscard]] usize GetFirstLostOperation() const noexcept;

private:
    struct LiveEntry
    {
        uintptr Address = 0;
        usize EventIndex = 0;
    };

    [[nodiscard]] usize findSlot(uintptr address) const noexcept;
    [[nodiscard]] usize getHome(uintptr address) const noexcept;
    [[nodiscard]] usize getDistance(usize home, usize index) const noexcept;
    [[nodiscard]] usize getNext(usize index) const noexcept;
    void removeSlot(usize index) noexcept;

    const usize mSlots;
    const usize mEventLimit;
    const AllocationSiteId mSiteCount;
    const ReadMetadata mReadMetadata;
    std::mutex mMutex;
    std::atomic<bool> mHasReentry{false};
    ActivityLoss mLoss = ActivityLoss::None;
    usize mEventCount = 0;
    usize mLiveCount = 0;
    // Memory depends only on Base. These arrays are never resized and their
    // storage is obtained before any route is published.
    LiveEntry mTable[MAX_ACTIVITY_LIVE_SLOTS]{};
    ActivityEvent mJournal[MAX_ACTIVITY_EVENTS];
};

// A recursion guard spans backend calls, accounting and detail publication.
// It owns no route and is never loaded on a counters-only operation.
class ObservationScope final
{
public:
    explicit ObservationScope(Observation& inoutObservation) noexcept;
    ~ObservationScope() noexcept;
    ObservationScope(const ObservationScope&) = delete;
    ObservationScope& operator=(const ObservationScope&) = delete;

    [[nodiscard]] bool IsOutermost() const noexcept;

private:
    bool mIsOutermost;
};

[[nodiscard]] const AllocationDomain& GetCaptureStorageDomain() noexcept;
} // namespace ludus::foundation::memory_internal
