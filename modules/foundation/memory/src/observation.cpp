#include "internal/observation.hpp"

// Thanks to the Perfetto heapprofd team for order-aware allocation tracking.
// See docs/architecture/memory-profiling-gems-review.md#modern-primary-source-cross-check.
// Ludus sequences detail publication and pointer lifetimes under one mutex
// with a fixed journal that is detached at a domain-wide safe point.
namespace ludus::foundation::memory_internal
{
namespace
{
constinit thread_local bool isInsideObservation = false;
}

Observation::Observation(const ObservationConfig& config, ReadMetadata readMetadata) noexcept
    : mSlots(config.LiveSlots), mEventLimit(config.EventLimit), mSiteCount(config.SiteCount),
      mReadMetadata(readMetadata)
{
}

void Observation::Attach(const AllocationDomain& domain, AllocationDomainId id, Observation& inoutObservation) noexcept
{
    domain.mObservationDomain = id;
    domain.mObservationOrNull = &inoutObservation;
}

void Observation::Detach(const AllocationDomain& domain) noexcept
{
    domain.mObservationOrNull = nullptr;
}

usize Observation::getHome(uintptr address) const noexcept
{
    // Mix away the predictable alignment bits before taking a bounded index.
    constexpr usize ALIGNMENT_SHIFT = 4;
    constexpr usize MIX_SHIFT = 11;
    return ((address >> ALIGNMENT_SHIFT) ^ (address >> MIX_SHIFT)) % mSlots;
}

usize Observation::getNext(usize index) const noexcept
{
    return index + 1 == mSlots ? 0 : index + 1;
}

usize Observation::getDistance(usize home, usize index) const noexcept
{
    return index >= home ? index - home : mSlots - home + index;
}

usize Observation::findSlot(uintptr address) const noexcept
{
    usize index = getHome(address);
    while (mTable[index].Address != 0 && mTable[index].Address != address)
    {
        index = getNext(index);
    }
    return index;
}

void Observation::removeSlot(usize index) noexcept
{
    usize hole = index;
    for (usize candidate = getNext(hole); mTable[candidate].Address != 0; candidate = getNext(candidate))
    {
        const usize home = getHome(mTable[candidate].Address);
        if (getDistance(home, hole) < getDistance(home, candidate))
        {
            mTable[hole] = mTable[candidate];
            hole = candidate;
        }
    }
    mTable[hole] = {};
    --mLiveCount;
}

void Observation::Record(ActivityKind kind,
                         AllocationDomainId domain,
                         void* pointerOrNull,
                         usize bytes,
                         usize alignment,
                         AllocationSiteId site) noexcept
{
    const ActivityMetadata metadata = mReadMetadata();
    const std::lock_guard<std::mutex> lock(mMutex);
    if (mLoss != ActivityLoss::None || mHasReentry.load(std::memory_order_relaxed) == true)
    {
        return;
    }
    if (mEventCount == mEventLimit)
    {
        // The first lost operation lives outside the full journal.
        mLoss = ActivityLoss::JournalFull;
        return;
    }
    const uintptr address = reinterpret_cast<uintptr>(pointerOrNull);
    usize lifetime = 0;
    if (kind == ActivityKind::Allocate)
    {
        const usize index = findSlot(address);
        if (mTable[index].Address != 0)
        {
            mLoss = ActivityLoss::InvalidOwnership;
            return;
        }
        if (mLiveCount == mSlots / 2)
        {
            mLoss = ActivityLoss::TableFull;
            return;
        }
        lifetime = mEventCount + 1;
        mTable[index] = {address, mEventCount};
        ++mLiveCount;
    }
    if (kind == ActivityKind::Free)
    {
        if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
        {
            mLoss = ActivityLoss::InvalidOwnership;
            return;
        }
        const usize index = findSlot(address);
        if (mTable[index].Address != 0)
        {
            const usize originIndex = mTable[index].EventIndex;
            const ActivityEvent& origin = mJournal[originIndex];
            if (origin.Domain != domain || origin.Bytes != bytes || origin.Alignment != alignment)
            {
                mLoss = ActivityLoss::InvalidOwnership;
                return;
            }
            lifetime = origin.Lifetime;
            site = origin.Site;
            removeSlot(index);
        }
    }
    mJournal[mEventCount] =
        {metadata, address, bytes, alignment, lifetime, domain, site <= mSiteCount ? site : 0, kind};
    ++mEventCount;
}

void Observation::MarkReentry() noexcept
{
    mHasReentry.store(true, std::memory_order_relaxed);
}

usize Observation::GetEventCount() const noexcept
{
    return mEventCount;
}

const ActivityEvent& Observation::GetEvent(usize index) const noexcept
{
    return mJournal[index];
}

ActivityLoss Observation::GetLoss() const noexcept
{
    return mHasReentry.load(std::memory_order_relaxed) == true ? ActivityLoss::Reentry : mLoss;
}

usize Observation::GetFirstLostOperation() const noexcept
{
    const ActivityLoss loss = GetLoss();
    return loss == ActivityLoss::None || loss == ActivityLoss::Reentry ? 0 : mEventCount + 1;
}

ObservationScope::ObservationScope(Observation& inoutObservation) noexcept : mIsOutermost(isInsideObservation == false)
{
    if (mIsOutermost == true)
    {
        isInsideObservation = true;
    }
    else
    {
        inoutObservation.MarkReentry();
    }
}

ObservationScope::~ObservationScope() noexcept
{
    if (mIsOutermost == true)
    {
        isInsideObservation = false;
    }
}

bool ObservationScope::IsOutermost() const noexcept
{
    return mIsOutermost;
}
} // namespace ludus::foundation::memory_internal
