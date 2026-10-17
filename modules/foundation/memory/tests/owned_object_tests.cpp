// Domain object ownership coverage for TryCreateOwned and DomainDeleter.
// Contract and delivery slice: docs/architecture/smart-pointers.md ("Fallible
// construction and domain lifetime", slice 2 "Domain object ownership").
#include <ludus/foundation/base/core.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/detail/object_layout.hpp>
#include <ludus/foundation/memory/owned_object.hpp>

#include <type_traits>

#include <catch2/catch_test_macros.hpp>

namespace
{
using ludus::foundation::AllocationDomain;
using ludus::foundation::DomainDeleter;
using ludus::foundation::GetSystemAllocationDomain;
using ludus::foundation::int32;
using ludus::foundation::Move;
using ludus::foundation::OwnershipStatus;
using ludus::foundation::StaticArray;
using ludus::foundation::TryCreateOwned;
using ludus::foundation::UniquePtr;
using ludus::foundation::usize;
namespace detail = ludus::foundation::detail;
using detail::ObjectLayoutCheck;

template <typename ElementType>
using DomainOwner = UniquePtr<ElementType, DomainDeleter<ElementType>>;

constexpr usize MAX_LIVE_ALLOCATIONS = 8;
constexpr usize NEVER_FAIL = 0;

struct LiveAllocation
{
    void* StorageOrNull = nullptr;
    usize Bytes = 0;
    usize Alignment = 0;
};

// Failure-injecting domain state. Every successful allocation is recorded so a
// free can be matched against the exact pointer, size and alignment it came
// from; a free that matches nothing is counted as a mismatch.
struct DomainRecord
{
    StaticArray<LiveAllocation, MAX_LIVE_ALLOCATIONS> Live{};
    usize AttemptCount = 0;
    usize AllocationCount = 0;
    usize FreeCount = 0;
    usize LiveCount = 0;
    usize MismatchedFreeCount = 0;
    usize FailAtAttempt = NEVER_FAIL;
    bool ShouldFailEveryAttempt = false;
    LiveAllocation LastAllocation{};
    LiveAllocation LastFree{};
};

void* AllocateRecorded(void* context, usize bytes, usize alignment) noexcept
{
    DomainRecord& record = *static_cast<DomainRecord*>(context);
    ++record.AttemptCount;
    if (record.ShouldFailEveryAttempt == true || record.AttemptCount == record.FailAtAttempt)
    {
        return nullptr;
    }
    for (usize index = 0; index < MAX_LIVE_ALLOCATIONS; ++index)
    {
        LiveAllocation& slot = record.Live[index];
        if (slot.StorageOrNull == nullptr)
        {
            void* storageOrNull = GetSystemAllocationDomain().TryAllocate(bytes, alignment);
            if (storageOrNull == nullptr)
            {
                return nullptr;
            }
            slot = LiveAllocation{ .StorageOrNull = storageOrNull, .Bytes = bytes, .Alignment = alignment };
            record.LastAllocation = slot;
            ++record.AllocationCount;
            ++record.LiveCount;
            return storageOrNull;
        }
    }
    return nullptr;
}

// The parameter order is fixed by AllocationDomain::FreeFunction.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void FreeRecorded(void* context, void* pointer, usize bytes, usize alignment) noexcept
{
    DomainRecord& record = *static_cast<DomainRecord*>(context);
    ++record.FreeCount;
    record.LastFree = LiveAllocation{ .StorageOrNull = pointer, .Bytes = bytes, .Alignment = alignment };
    for (usize index = 0; index < MAX_LIVE_ALLOCATIONS; ++index)
    {
        LiveAllocation& slot = record.Live[index];
        if (slot.StorageOrNull == pointer)
        {
            if (slot.Bytes != bytes || slot.Alignment != alignment)
            {
                ++record.MismatchedFreeCount;
            }
            GetSystemAllocationDomain().Free(pointer, slot.Bytes, slot.Alignment);
            slot = LiveAllocation{};
            --record.LiveCount;
            return;
        }
    }
    ++record.MismatchedFreeCount;
}

// Owns the record and the immutable domain bound to it. Declare it before any
// owner it allocates for, so the domain outlives those owners.
struct RecordingDomain
{
    DomainRecord Record{};
    AllocationDomain Domain{&Record, AllocateRecorded, FreeRecorded};
};

struct Tracked
{
    Tracked(int32 value, int32& inoutDestructionCount) noexcept : Value(value), DestructionCount(&inoutDestructionCount)
    {
    }
    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
    ~Tracked()
    {
        ++*DestructionCount;
    }

    int32 Value;
    int32* DestructionCount;
};

// A move-only argument that records whether it has been consumed.
struct Payload
{
    explicit Payload(int32 value) noexcept : Value(value) {}
    Payload(Payload&& other) noexcept : Value(other.Value)
    {
        other.IsMovedFrom = true;
    }
    Payload(const Payload&) = delete;
    Payload& operator=(const Payload&) = delete;
    Payload& operator=(Payload&&) = delete;
    ~Payload() = default;

    int32 Value;
    bool IsMovedFrom = false;
};

struct PayloadOwner
{
    explicit PayloadOwner(Payload&& payload) noexcept : Stored(Move(payload)) {}

    Payload Stored;
};

constexpr usize OVER_ALIGNMENT = 128;

struct alignas(OVER_ALIGNMENT) OverAligned
{
    int32 Value = 0;
};

struct Base
{
    virtual ~Base() = default;
    int32 BaseValue = 0;
};

struct Derived final : Base
{
    int32 DerivedValue = 0;
};

struct ThrowingConstructor
{
    ThrowingConstructor() noexcept(false) {}
};

struct ThrowingDestructor
{
    // Deliberately potentially-throwing so trait tests can reject it.
    // NOLINTNEXTLINE(bugprone-unsafe-to-allow-exceptions)
    ~ThrowingDestructor() noexcept(false) {}
};

template <typename OwnerType>
concept CanRelease = requires(OwnerType& owner) { owner.Release(); };

constexpr usize ISIZE_MAX_BYTES = static_cast<usize>(-1) >> 1;

template <typename ElementType, typename... ArgumentTypes>
concept CanCreateOwned =
    requires(const AllocationDomain& domain, DomainOwner<ElementType>& inoutOwner, ArgumentTypes&&... arguments) {
        TryCreateOwned<ElementType>(domain, inoutOwner, static_cast<ArgumentTypes&&>(arguments)...);
    };
} // namespace

// An exact-domain owner is two pointer words: the domain and the object.
static_assert(sizeof(DomainDeleter<Tracked>) == sizeof(void*));
static_assert(sizeof(DomainOwner<Tracked>) == 2 * sizeof(void*));
static_assert(sizeof(DomainOwner<OverAligned>) == 2 * sizeof(void*));

// Factory constraints: nothrow construction and destruction, exact non-cv
// non-array object types only.
static_assert(CanCreateOwned<Tracked, int32, int32&>);
static_assert(CanCreateOwned<PayloadOwner, Payload&&>);
static_assert(CanCreateOwned<ThrowingConstructor> == false);
static_assert(CanCreateOwned<ThrowingDestructor> == false);
static_assert(CanCreateOwned<const OverAligned> == false);
static_assert(CanCreateOwned<PayloadOwner, Payload&> == false);

// Deleter conversions: adding const through the explicit adapter only.
static_assert(std::is_nothrow_constructible_v<DomainDeleter<const Tracked>, DomainDeleter<Tracked>&&>);
static_assert(std::is_convertible_v<DomainDeleter<Tracked>&&, DomainDeleter<const Tracked>> == false);
static_assert(std::is_constructible_v<DomainDeleter<Tracked>, DomainDeleter<const Tracked>&&> == false);
static_assert(std::is_constructible_v<DomainDeleter<Base>, DomainDeleter<Derived>&&> == false);

// Owner conversions follow the deleter: explicit const addition, no Derived-to-Base,
// even with a virtual destructor, and no bare Release of domain storage.
static_assert(std::is_nothrow_constructible_v<DomainOwner<const Tracked>, DomainOwner<Tracked>&&>);
static_assert(std::is_convertible_v<DomainOwner<Tracked>&&, DomainOwner<const Tracked>> == false);
static_assert(std::is_constructible_v<DomainOwner<Base>, DomainOwner<Derived>&&> == false);
static_assert(std::is_assignable_v<DomainOwner<Base>&, DomainOwner<Derived>&&> == false);
static_assert(CanRelease<DomainOwner<Tracked>> == false);

// Layout validation uses checked arithmetic before any allocation.
static_assert(detail::CheckObjectLayout(sizeof(OverAligned), alignof(OverAligned)) == ObjectLayoutCheck::Valid);
static_assert(detail::CheckObjectLayout(0, 8) == ObjectLayoutCheck::Malformed);
static_assert(detail::CheckObjectLayout(8, 0) == ObjectLayoutCheck::Malformed);
static_assert(detail::CheckObjectLayout(8, 24) == ObjectLayoutCheck::Malformed);
static_assert(detail::CheckObjectLayout(static_cast<usize>(-1), 16) == ObjectLayoutCheck::TooLarge);
static_assert(detail::CheckObjectLayout(static_cast<usize>(-1), 1) == ObjectLayoutCheck::TooLarge);
static_assert(detail::CheckObjectLayout(ISIZE_MAX_BYTES, 1) == ObjectLayoutCheck::Valid);
static_assert(detail::CheckObjectLayout(ISIZE_MAX_BYTES, 2) == ObjectLayoutCheck::TooLarge);

TEST_CASE("TryCreateOwned allocates the exact layout and frees it through the same domain", "[memory][owned]")
{
    RecordingDomain recording;
    int32 destructionCount = 0;
    {
        DomainOwner<Tracked> owner;
        REQUIRE(TryCreateOwned<Tracked>(recording.Domain, owner, 7, destructionCount) == OwnershipStatus::Ok);
        REQUIRE(owner.Get() != nullptr);
        REQUIRE(owner->Value == 7);
        REQUIRE(owner.GetDeleter().GetDomainOrNull() == &recording.Domain);
        REQUIRE(recording.Record.AllocationCount == 1);
        REQUIRE(recording.Record.LastAllocation.StorageOrNull == static_cast<void*>(owner.Get()));
        REQUIRE(recording.Record.LastAllocation.Bytes == sizeof(Tracked));
        REQUIRE(recording.Record.LastAllocation.Alignment == alignof(Tracked));
        REQUIRE(destructionCount == 0);
    }
    REQUIRE(destructionCount == 1);
    REQUIRE(recording.Record.FreeCount == 1);
    REQUIRE(recording.Record.MismatchedFreeCount == 0);
    REQUIRE(recording.Record.LastFree.Bytes == sizeof(Tracked));
    REQUIRE(recording.Record.LastFree.Alignment == alignof(Tracked));
    REQUIRE(recording.Record.LiveCount == 0);
}

TEST_CASE("Allocation failure preserves the output and does not consume move arguments", "[memory][owned]")
{
    RecordingDomain recording;
    recording.Record.ShouldFailEveryAttempt = true;
    DomainOwner<PayloadOwner> owner;
    Payload payload(42);

    REQUIRE(TryCreateOwned<PayloadOwner>(recording.Domain, owner, Move(payload)) == OwnershipStatus::OutOfMemory);
    REQUIRE(owner.Get() == nullptr);
    REQUIRE(owner.GetDeleter().GetDomainOrNull() == nullptr);
    REQUIRE(payload.IsMovedFrom == false);
    REQUIRE(payload.Value == 42);
    REQUIRE(recording.Record.AttemptCount == 1);
    REQUIRE(recording.Record.AllocationCount == 0);

    // The same argument is still usable once the domain recovers.
    recording.Record.ShouldFailEveryAttempt = false;
    REQUIRE(TryCreateOwned<PayloadOwner>(recording.Domain, owner, Move(payload)) == OwnershipStatus::Ok);
    REQUIRE(payload.IsMovedFrom == true);
    REQUIRE(owner->Stored.Value == 42);
    owner.Reset();
    REQUIRE(recording.Record.LiveCount == 0);
    REQUIRE(recording.Record.MismatchedFreeCount == 0);
}

TEST_CASE("A nonempty output is rejected before allocation and preserved", "[memory][owned]")
{
    RecordingDomain recording;
    int32 destructionCount = 0;
    DomainOwner<Tracked> owner;
    REQUIRE(TryCreateOwned<Tracked>(recording.Domain, owner, 1, destructionCount) == OwnershipStatus::Ok);
    Tracked* original = owner.Get();

    RecordingDomain other;
    REQUIRE(TryCreateOwned<Tracked>(other.Domain, owner, 2, destructionCount) == OwnershipStatus::InvalidState);
    REQUIRE(owner.Get() == original);
    REQUIRE(owner->Value == 1);
    REQUIRE(owner.GetDeleter().GetDomainOrNull() == &recording.Domain);
    REQUIRE(other.Record.AttemptCount == 0);
    REQUIRE(destructionCount == 0);

    owner.Reset();
    REQUIRE(destructionCount == 1);
    REQUIRE(recording.Record.LiveCount == 0);
}

TEST_CASE("Over-aligned objects request and free their exact alignment", "[memory][owned]")
{
    RecordingDomain recording;
    DomainOwner<OverAligned> first;
    DomainOwner<OverAligned> second;
    REQUIRE(TryCreateOwned<OverAligned>(recording.Domain, first) == OwnershipStatus::Ok);
    REQUIRE(TryCreateOwned<OverAligned>(recording.Domain, second) == OwnershipStatus::Ok);
    REQUIRE(recording.Record.LastAllocation.Alignment == OVER_ALIGNMENT);
    REQUIRE(recording.Record.LastAllocation.Bytes == sizeof(OverAligned));
    REQUIRE(reinterpret_cast<usize>(first.Get()) % OVER_ALIGNMENT == 0);
    REQUIRE(reinterpret_cast<usize>(second.Get()) % OVER_ALIGNMENT == 0);

    first.Reset();
    REQUIRE(recording.Record.LastFree.Alignment == OVER_ALIGNMENT);
    REQUIRE(recording.Record.LastFree.Bytes == sizeof(OverAligned));
    second.Reset();
    REQUIRE(recording.Record.MismatchedFreeCount == 0);
    REQUIRE(recording.Record.LiveCount == 0);
}

TEST_CASE("Owners free through their original domain after moves and swaps", "[memory][owned]")
{
    RecordingDomain left;
    RecordingDomain right;
    int32 destructionCount = 0;
    DomainOwner<Tracked> leftOwner;
    DomainOwner<Tracked> rightOwner;
    REQUIRE(TryCreateOwned<Tracked>(left.Domain, leftOwner, 1, destructionCount) == OwnershipStatus::Ok);
    REQUIRE(TryCreateOwned<Tracked>(right.Domain, rightOwner, 2, destructionCount) == OwnershipStatus::Ok);

    leftOwner.Swap(rightOwner);
    REQUIRE(leftOwner->Value == 2);
    REQUIRE(leftOwner.GetDeleter().GetDomainOrNull() == &right.Domain);

    DomainOwner<Tracked> moved(Move(leftOwner));
    REQUIRE(leftOwner.Get() == nullptr);
    moved.Reset();
    REQUIRE(right.Record.FreeCount == 1);
    REQUIRE(left.Record.FreeCount == 0);

    rightOwner.Reset();
    REQUIRE(left.Record.FreeCount == 1);
    REQUIRE(destructionCount == 2);
    REQUIRE(left.Record.MismatchedFreeCount == 0);
    REQUIRE(right.Record.MismatchedFreeCount == 0);
}

TEST_CASE("The const adapter keeps the domain and the exact layout", "[memory][owned]")
{
    RecordingDomain recording;
    int32 destructionCount = 0;
    DomainOwner<Tracked> owner;
    REQUIRE(TryCreateOwned<Tracked>(recording.Domain, owner, 3, destructionCount) == OwnershipStatus::Ok);
    Tracked* original = owner.Get();

    DomainOwner<const Tracked> constOwner(Move(owner));
    REQUIRE(owner.Get() == nullptr);
    REQUIRE(constOwner.Get() == original);
    REQUIRE(constOwner.GetDeleter().GetDomainOrNull() == &recording.Domain);

    constOwner.Reset();
    REQUIRE(destructionCount == 1);
    REQUIRE(recording.Record.LastFree.StorageOrNull == static_cast<void*>(original));
    REQUIRE(recording.Record.LastFree.Bytes == sizeof(Tracked));
    REQUIRE(recording.Record.MismatchedFreeCount == 0);
}

namespace
{
// A resource with fallible setup: a nothrow inert constructor plus an explicit
// status operation. It owns two buffers from its domain; its destructor
// releases whatever setup acquired, so destroying a partial candidate rolls
// the setup back.
class StagedResource final
{
public:
    static constexpr usize BUFFER_BYTES = 64;
    static constexpr usize BUFFER_ALIGNMENT = 16;

    StagedResource(const AllocationDomain& domain, int32& inoutDestructionCount) noexcept
        : mDomain(&domain), mDestructionCount(&inoutDestructionCount)
    {
    }
    StagedResource(const StagedResource&) = delete;
    StagedResource& operator=(const StagedResource&) = delete;
    ~StagedResource()
    {
        mDomain->Free(mSecondOrNull, BUFFER_BYTES, BUFFER_ALIGNMENT);
        mDomain->Free(mFirstOrNull, BUFFER_BYTES, BUFFER_ALIGNMENT);
        ++*mDestructionCount;
    }

    [[nodiscard]] bool TryInitialize() noexcept
    {
        mFirstOrNull = mDomain->TryAllocate(BUFFER_BYTES, BUFFER_ALIGNMENT);
        if (mFirstOrNull == nullptr)
        {
            return false;
        }
        mSecondOrNull = mDomain->TryAllocate(BUFFER_BYTES, BUFFER_ALIGNMENT);
        return mSecondOrNull != nullptr;
    }

    [[nodiscard]] bool IsComplete() const noexcept
    {
        return mFirstOrNull != nullptr && mSecondOrNull != nullptr;
    }

private:
    const AllocationDomain* mDomain;
    int32* mDestructionCount;
    void* mFirstOrNull = nullptr;
    void* mSecondOrNull = nullptr;
};

// The factory pattern from the contract: own a private candidate, run setup,
// roll back on failure and publish only a complete object.
[[nodiscard]] OwnershipStatus TryCreateStagedResource(const AllocationDomain& domain,
                                                      int32& inoutDestructionCount,
                                                      DomainOwner<StagedResource>& inoutOwner) noexcept
{
    if (inoutOwner.Get() != nullptr)
    {
        return OwnershipStatus::InvalidState;
    }
    DomainOwner<StagedResource> candidate;
    const OwnershipStatus status = TryCreateOwned<StagedResource>(domain, candidate, domain, inoutDestructionCount);
    if (status != OwnershipStatus::Ok)
    {
        return status;
    }
    if (candidate->TryInitialize() == false)
    {
        return OwnershipStatus::OutOfMemory;
    }
    inoutOwner = Move(candidate);
    return OwnershipStatus::Ok;
}
} // namespace

TEST_CASE("Failed setup rolls back a partially initialized candidate", "[memory][owned]")
{
    RecordingDomain recording;
    int32 destructionCount = 0;
    DomainOwner<StagedResource> owner;

    // Attempt 1 is the object, 2 the first buffer, 3 the second buffer.
    recording.Record.FailAtAttempt = 3;
    REQUIRE(TryCreateStagedResource(recording.Domain, destructionCount, owner) == OwnershipStatus::OutOfMemory);
    REQUIRE(owner.Get() == nullptr);
    REQUIRE(destructionCount == 1);
    REQUIRE(recording.Record.AllocationCount == 2);
    REQUIRE(recording.Record.FreeCount == 2);
    REQUIRE(recording.Record.MismatchedFreeCount == 0);
    REQUIRE(recording.Record.LiveCount == 0);

    // Failing the object allocation itself never constructs a candidate.
    recording.Record = DomainRecord{};
    recording.Record.FailAtAttempt = 1;
    destructionCount = 0;
    REQUIRE(TryCreateStagedResource(recording.Domain, destructionCount, owner) == OwnershipStatus::OutOfMemory);
    REQUIRE(owner.Get() == nullptr);
    REQUIRE(destructionCount == 0);
    REQUIRE(recording.Record.AllocationCount == 0);

    // Complete setup publishes the object; destruction releases everything.
    recording.Record.FailAtAttempt = NEVER_FAIL;
    REQUIRE(TryCreateStagedResource(recording.Domain, destructionCount, owner) == OwnershipStatus::Ok);
    REQUIRE(owner->IsComplete() == true);
    REQUIRE(recording.Record.LiveCount == 3);
    owner.Reset();
    REQUIRE(destructionCount == 1);
    REQUIRE(recording.Record.LiveCount == 0);
    REQUIRE(recording.Record.MismatchedFreeCount == 0);
}

TEST_CASE("The system domain creates and destroys owned objects", "[memory][owned]")
{
    int32 destructionCount = 0;
    DomainOwner<Tracked> owner;
    REQUIRE(TryCreateOwned<Tracked>(GetSystemAllocationDomain(), owner, 9, destructionCount) == OwnershipStatus::Ok);
    REQUIRE(owner.GetDeleter().GetDomainOrNull() == &GetSystemAllocationDomain());
    owner.Reset();
    REQUIRE(destructionCount == 1);
}
