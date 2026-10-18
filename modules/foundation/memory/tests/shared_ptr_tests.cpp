// Shared and weak ownership coverage for SharedPtr, WeakPtr and TryMakeShared.
// Contract and delivery slice: docs/architecture/smart-pointers.md ("Shared
// object and controller", "Controller state and reclamation", slice 3 "Shared
// and weak consumer"). Concurrent promotion races are slice 4. Forbidden raw
// adoption and unsafe conversions are also rejected by real compilations in
// shared_ptr_compile_failures.cpp (ludus_pointer_diagnostics).
#include <ludus/foundation/base/core.h>

#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/detail/shared_controller.hpp>
#include <ludus/foundation/memory/owned_object.hpp>
#include <ludus/foundation/memory/shared_ptr.hpp>

#include "internal/shared_count.hpp"
#include "recording_domain.hpp"

#include <atomic>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

namespace
{
using ludus::foundation::GetSystemAllocationDomain;
using ludus::foundation::int32;
using ludus::foundation::int64;
using ludus::foundation::Move;
using ludus::foundation::OwnershipStatus;
using ludus::foundation::SharedPtr;
using ludus::foundation::TryMakeShared;
using ludus::foundation::uint8;
using ludus::foundation::usize;
using ludus::foundation::WeakPtr;
namespace detail = ludus::foundation::detail;
using detail::CountChange;
using ludus_memory_tests::NEVER_FAIL;
using ludus_memory_tests::RecordingDomain;

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

// Destruction order record for the inheritance cases. The bases deliberately
// have non-virtual destructors: the exact creator thunk, not the view type,
// selects the destructor.
struct DestructionLog
{
    int32 DerivedCount = 0;
    int32 LeftCount = 0;
    int32 RightCount = 0;
    int32 RootCount = 0;
};

struct LeftBase
{
    explicit LeftBase(DestructionLog& inoutLog) noexcept : LeftLog(&inoutLog) {}
    LeftBase(const LeftBase&) = delete;
    LeftBase& operator=(const LeftBase&) = delete;
    ~LeftBase()
    {
        ++LeftLog->LeftCount;
    }

    DestructionLog* LeftLog;
    int64 LeftValue = 1;
};

struct RightBase
{
    explicit RightBase(DestructionLog& inoutLog) noexcept : RightLog(&inoutLog) {}
    RightBase(const RightBase&) = delete;
    RightBase& operator=(const RightBase&) = delete;
    ~RightBase()
    {
        ++RightLog->RightCount;
    }

    DestructionLog* RightLog;
    int64 RightValue = 2;
};

// RightBase sits at a nonzero offset, so a RightBase view adjusts the address.
struct MultiplyDerived final : LeftBase, RightBase
{
    explicit MultiplyDerived(DestructionLog& inoutLog) noexcept : LeftBase(inoutLog), RightBase(inoutLog) {}
    MultiplyDerived(const MultiplyDerived&) = delete;
    MultiplyDerived& operator=(const MultiplyDerived&) = delete;
    ~MultiplyDerived()
    {
        ++LeftLog->DerivedCount;
    }

    int64 DerivedValue = 3;
};

struct VirtualRoot
{
    explicit VirtualRoot(DestructionLog& inoutLog) noexcept : RootLog(&inoutLog) {}
    VirtualRoot(const VirtualRoot&) = delete;
    VirtualRoot& operator=(const VirtualRoot&) = delete;
    ~VirtualRoot()
    {
        ++RootLog->RootCount;
    }

    DestructionLog* RootLog;
    int64 RootValue = 4;
};

struct VirtualLeft : virtual VirtualRoot
{
    explicit VirtualLeft(DestructionLog& inoutLog) noexcept : VirtualRoot(inoutLog) {}
};

struct VirtualRight : virtual VirtualRoot
{
    explicit VirtualRight(DestructionLog& inoutLog) noexcept : VirtualRoot(inoutLog) {}
};

// A virtual-base view's adjustment is read through the object's own layout data.
struct Diamond final : VirtualLeft, VirtualRight
{
    explicit Diamond(DestructionLog& inoutLog) noexcept
        : VirtualRoot(inoutLog), VirtualLeft(inoutLog), VirtualRight(inoutLog)
    {
    }
    Diamond(const Diamond&) = delete;
    Diamond& operator=(const Diamond&) = delete;
    ~Diamond()
    {
        ++RootLog->DerivedCount;
    }
};

// Observes itself; its destructor releases that weak value while the object is
// being destroyed, and optionally the last strong owner of another object.
struct SelfObserver
{
    explicit SelfObserver(int32& inoutDestructionCount) noexcept : DestructionCount(&inoutDestructionCount) {}
    SelfObserver(const SelfObserver&) = delete;
    SelfObserver& operator=(const SelfObserver&) = delete;
    ~SelfObserver()
    {
        Self.Reset();
        Neighbour.Reset();
        ++*DestructionCount;
    }

    WeakPtr<SelfObserver> Self;
    SharedPtr<Tracked> Neighbour;
    int32* DestructionCount;
};

struct Node
{
    Node(int32 value, int32& inoutDestructionCount) noexcept : Value(value), DestructionCount(&inoutDestructionCount) {}
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    ~Node()
    {
        ++*DestructionCount;
    }

    SharedPtr<Node> Next;
    int32 Value;
    int32* DestructionCount;
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

struct Opaque;

template <typename ElementType, typename... ArgumentTypes>
concept CanMakeShared = requires(SharedPtr<ElementType>& inoutOwner, ArgumentTypes&&... arguments) {
    TryMakeShared<ElementType>(GetSystemAllocationDomain(), inoutOwner, static_cast<ArgumentTypes&&>(arguments)...);
};

template <typename OwnerType, typename ElementType>
concept CanResetWithPointer = requires(OwnerType& inoutOwner, ElementType* pointer) { inoutOwner.Reset(pointer); };

template <typename ObserverType>
concept CanGet = requires(ObserverType& inoutObserver) { inoutObserver.Get(); };

template <typename ObserverType>
concept CanDereference = requires(ObserverType& inoutObserver) { *inoutObserver; };

template <typename ObserverType>
concept CanArrow = requires(ObserverType& inoutObserver) { inoutObserver.operator->(); };

constexpr usize ISIZE_MAX_BYTES = static_cast<usize>(-1) >> 1;
constexpr usize POINTER_WORD_BYTES = sizeof(void*);

bool IsAlignedTo(const void* pointer, usize alignment) noexcept
{
    return reinterpret_cast<usize>(pointer) % alignment == 0;
}

bool IsInsideAllocation(const void* pointer, usize bytes, const ludus_memory_tests::LiveAllocation& allocation) noexcept
{
    const usize begin = reinterpret_cast<usize>(allocation.StorageOrNull);
    const usize address = reinterpret_cast<usize>(pointer);
    return address >= begin && address + bytes <= begin + allocation.Bytes;
}
} // namespace

// Ownership surface: no raw adoption, compact views, weak values cannot access.
static_assert(sizeof(SharedPtr<int32>) == 2 * POINTER_WORD_BYTES);
static_assert(sizeof(WeakPtr<int32>) == 2 * POINTER_WORD_BYTES);
static_assert(sizeof(SharedPtr<OverAligned>) == 2 * POINTER_WORD_BYTES);
static_assert(std::is_constructible_v<SharedPtr<int32>, int32*> == false);
static_assert(std::is_constructible_v<SharedPtr<int32>, int32*&> == false);
static_assert(CanResetWithPointer<SharedPtr<int32>, int32> == false);
static_assert(std::is_constructible_v<WeakPtr<int32>, int32*> == false);
static_assert(CanGet<WeakPtr<int32>> == false);
static_assert(CanDereference<WeakPtr<int32>> == false);
static_assert(CanArrow<WeakPtr<int32>> == false);
static_assert(std::is_nothrow_copy_constructible_v<SharedPtr<int32>>);
static_assert(std::is_nothrow_move_constructible_v<SharedPtr<int32>>);
static_assert(std::is_nothrow_copy_assignable_v<SharedPtr<int32>>);
static_assert(std::is_nothrow_move_assignable_v<SharedPtr<int32>>);
static_assert(std::is_nothrow_destructible_v<SharedPtr<int32>>);
static_assert(std::is_nothrow_copy_constructible_v<WeakPtr<int32>>);
static_assert(std::is_nothrow_move_assignable_v<WeakPtr<int32>>);
// Type-erased destruction: an owner of an incomplete type can be destroyed.
static_assert(std::is_nothrow_destructible_v<SharedPtr<Opaque>>);
static_assert(std::is_nothrow_destructible_v<WeakPtr<Opaque>>);

// Safe upcasts and cv additions only; no downcasts or cross-type weak adjustment.
static_assert(std::is_convertible_v<SharedPtr<Tracked>, SharedPtr<const Tracked>>);
static_assert(std::is_convertible_v<SharedPtr<const Tracked>, SharedPtr<Tracked>> == false);
static_assert(std::is_convertible_v<SharedPtr<MultiplyDerived>, SharedPtr<RightBase>>);
static_assert(std::is_convertible_v<SharedPtr<Diamond>, SharedPtr<VirtualRoot>>);
static_assert(std::is_constructible_v<SharedPtr<MultiplyDerived>, SharedPtr<RightBase>> == false);
static_assert(std::is_convertible_v<SharedPtr<MultiplyDerived>, WeakPtr<RightBase>>);
static_assert(std::is_convertible_v<SharedPtr<Diamond>, WeakPtr<VirtualRoot>>);
static_assert(std::is_convertible_v<WeakPtr<Tracked>, WeakPtr<const Tracked>>);
static_assert(std::is_constructible_v<WeakPtr<RightBase>, WeakPtr<MultiplyDerived>> == false);
static_assert(std::is_constructible_v<WeakPtr<VirtualRoot>, WeakPtr<Diamond>> == false);
static_assert(std::is_constructible_v<WeakPtr<Tracked>, WeakPtr<const Tracked>> == false);

// Factory constraints mirror TryCreateOwned: exact, nothrow, non-array objects.
static_assert(CanMakeShared<Tracked, int32, int32&>);
static_assert(CanMakeShared<const Tracked, int32, int32&> == false);
static_assert(CanMakeShared<ThrowingConstructor> == false);
static_assert(CanMakeShared<ThrowingDestructor> == false);
static_assert(CanMakeShared<Tracked, int32> == false);

TEST_CASE("TryMakeShared uses one exact combined allocation and destroys once", "[foundation][memory][shared]")
{
    RecordingDomain recording;
    int32 destructionCount = 0;
    {
        SharedPtr<Tracked> owner;
        REQUIRE(TryMakeShared<Tracked>(recording.Domain, owner, 7, destructionCount) == OwnershipStatus::Ok);
        REQUIRE(owner.Get() != nullptr);
        REQUIRE(owner != nullptr);
        REQUIRE(static_cast<bool>(owner) == true);
        REQUIRE(owner->Value == 7);
        REQUIRE((*owner).Value == 7);
        REQUIRE(recording.Record.AttemptCount == 1);
        REQUIRE(recording.Record.AllocationCount == 1);
        REQUIRE(recording.Record.LastAllocation.Alignment >= alignof(Tracked));
        REQUIRE(IsAlignedTo(owner.Get(), alignof(Tracked)) == true);
        REQUIRE(IsInsideAllocation(owner.Get(), sizeof(Tracked), recording.Record.LastAllocation) == true);
        REQUIRE(destructionCount == 0);
    }
    REQUIRE(destructionCount == 1);
    REQUIRE(recording.Record.FreeCount == 1);
    REQUIRE(recording.Record.MismatchedFreeCount == 0);
    REQUIRE(recording.Record.LastFree.StorageOrNull == recording.Record.LastAllocation.StorageOrNull);
    REQUIRE(recording.Record.LastFree.Bytes == recording.Record.LastAllocation.Bytes);
    REQUIRE(recording.Record.LastFree.Alignment == recording.Record.LastAllocation.Alignment);
    REQUIRE(recording.Record.LiveCount == 0);
}

TEST_CASE("TryMakeShared aligns over-aligned objects inside the combined block", "[foundation][memory][shared]")
{
    RecordingDomain recording;
    {
        SharedPtr<OverAligned> owner;
        REQUIRE(TryMakeShared<OverAligned>(recording.Domain, owner) == OwnershipStatus::Ok);
        REQUIRE(recording.Record.LastAllocation.Alignment == OVER_ALIGNMENT);
        REQUIRE(IsAlignedTo(recording.Record.LastAllocation.StorageOrNull, OVER_ALIGNMENT) == true);
        REQUIRE(IsAlignedTo(owner.Get(), OVER_ALIGNMENT) == true);
        REQUIRE(IsInsideAllocation(owner.Get(), sizeof(OverAligned), recording.Record.LastAllocation) == true);
        REQUIRE(owner->Value == 0);
    }
    REQUIRE(recording.Record.MismatchedFreeCount == 0);
    REQUIRE(recording.Record.LiveCount == 0);
}

TEST_CASE("TryMakeShared failures preserve the output and unconsumed arguments", "[foundation][memory][shared]")
{
    RecordingDomain recording;
    int32 destructionCount = 0;

    SECTION("allocation failure")
    {
        recording.Record.ShouldFailEveryAttempt = true;
        SharedPtr<PayloadOwner> owner;
        Payload payload(5);
        REQUIRE(TryMakeShared<PayloadOwner>(recording.Domain, owner, Move(payload)) == OwnershipStatus::OutOfMemory);
        REQUIRE(owner == nullptr);
        REQUIRE(payload.IsMovedFrom == false);
        REQUIRE(payload.Value == 5);
        REQUIRE(recording.Record.AttemptCount == 1);
        REQUIRE(recording.Record.AllocationCount == 0);

        recording.Record.ShouldFailEveryAttempt = false;
        recording.Record.FailAtAttempt = NEVER_FAIL;
        REQUIRE(TryMakeShared<PayloadOwner>(recording.Domain, owner, Move(payload)) == OwnershipStatus::Ok);
        REQUIRE(payload.IsMovedFrom == true);
        REQUIRE(owner->Stored.Value == 5);
    }
    SECTION("nonempty output")
    {
        SharedPtr<Tracked> owner;
        REQUIRE(TryMakeShared<Tracked>(recording.Domain, owner, 1, destructionCount) == OwnershipStatus::Ok);
        Tracked* const original = owner.Get();
        REQUIRE(TryMakeShared<Tracked>(recording.Domain, owner, 2, destructionCount) == OwnershipStatus::InvalidState);
        REQUIRE(owner.Get() == original);
        REQUIRE(owner->Value == 1);
        REQUIRE(recording.Record.AttemptCount == 1);
        REQUIRE(destructionCount == 0);
    }
    SECTION("unrepresentable and malformed layouts fail before allocation")
    {
        detail::SharedAllocation allocation{};
        const detail::DestroySharedObjectFunction destroy = &detail::DestroySharedObject<int32>;
        REQUIRE(detail::TryAllocateSharedController(recording.Domain, ISIZE_MAX_BYTES, 8, destroy, allocation) ==
                OwnershipStatus::TooLarge);
        REQUIRE(
            detail::TryAllocateSharedController(recording.Domain, static_cast<usize>(-1) - 8, 8, destroy, allocation) ==
            OwnershipStatus::TooLarge);
        REQUIRE(detail::TryAllocateSharedController(recording.Domain, ISIZE_MAX_BYTES - 16, 16, destroy, allocation) ==
                OwnershipStatus::TooLarge);
        REQUIRE(detail::TryAllocateSharedController(recording.Domain, 0, 8, destroy, allocation) ==
                OwnershipStatus::InvalidState);
        REQUIRE(detail::TryAllocateSharedController(recording.Domain, 8, 3, destroy, allocation) ==
                OwnershipStatus::InvalidState);
        REQUIRE(detail::TryAllocateSharedController(recording.Domain, 8, 8, nullptr, allocation) ==
                OwnershipStatus::InvalidState);
        REQUIRE(allocation.ControllerOrNull == nullptr);
        REQUIRE(allocation.ObjectStorageOrNull == nullptr);
        REQUIRE(recording.Record.AttemptCount == 0);
    }
    REQUIRE(recording.Record.MismatchedFreeCount == 0);
}

TEST_CASE("TryMakeShared works with the system domain", "[foundation][memory][shared]")
{
    int32 destructionCount = 0;
    SharedPtr<Tracked> owner;
    REQUIRE(TryMakeShared<Tracked>(GetSystemAllocationDomain(), owner, 9, destructionCount) == OwnershipStatus::Ok);
    REQUIRE(owner->Value == 9);
    owner.Reset();
    REQUIRE(owner == nullptr);
    REQUIRE(destructionCount == 1);
}

TEST_CASE("SharedPtr copy, move, self-assignment and swap transfer exact ownership", "[foundation][memory][shared]")
{
    RecordingDomain recording;
    int32 firstDestructionCount = 0;
    int32 secondDestructionCount = 0;
    SharedPtr<Tracked> first;
    SharedPtr<Tracked> second;
    REQUIRE(TryMakeShared<Tracked>(recording.Domain, first, 1, firstDestructionCount) == OwnershipStatus::Ok);
    REQUIRE(TryMakeShared<Tracked>(recording.Domain, second, 2, secondDestructionCount) == OwnershipStatus::Ok);
    Tracked* const firstObject = first.Get();
    Tracked* const secondObject = second.Get();

    SECTION("copy construction and copy assignment share one object")
    {
        SharedPtr<Tracked> copy(first);
        REQUIRE(copy.Get() == firstObject);
        first.Reset();
        REQUIRE(firstDestructionCount == 0);
        // Acquire the incoming hold before releasing the previous object.
        copy = second;
        REQUIRE(firstDestructionCount == 1);
        REQUIRE(copy.Get() == secondObject);
        second.Reset();
        REQUIRE(secondDestructionCount == 0);
        copy = nullptr;
        REQUIRE(secondDestructionCount == 1);
    }
    SECTION("self copy and self move leave the owner unchanged")
    {
        SharedPtr<Tracked>& alias = first;
        first = alias;
        REQUIRE(first.Get() == firstObject);
        first = Move(alias);
        REQUIRE(first.Get() == firstObject);
        REQUIRE(firstDestructionCount == 0);
        first.Reset();
        REQUIRE(firstDestructionCount == 1);
    }
    SECTION("move construction and move assignment add no hold")
    {
        SharedPtr<Tracked> moved(Move(first));
        REQUIRE(first == nullptr);
        REQUIRE(moved.Get() == firstObject);
        moved = Move(second);
        REQUIRE(second == nullptr);
        REQUIRE(firstDestructionCount == 1);
        REQUIRE(moved.Get() == secondObject);
        moved.Reset();
        REQUIRE(secondDestructionCount == 1);
    }
    SECTION("swap exchanges view and controller together")
    {
        first.Swap(second);
        REQUIRE(first.Get() == secondObject);
        REQUIRE(second.Get() == firstObject);
        first.Swap(first);
        REQUIRE(first.Get() == secondObject);
        first.Reset();
        REQUIRE(secondDestructionCount == 1);
        REQUIRE(firstDestructionCount == 0);
        second.Reset();
        REQUIRE(firstDestructionCount == 1);
    }
    first.Reset();
    second.Reset();
    REQUIRE(firstDestructionCount == 1);
    REQUIRE(secondDestructionCount == 1);
    REQUIRE(recording.Record.FreeCount == 2);
    REQUIRE(recording.Record.MismatchedFreeCount == 0);
    REQUIRE(recording.Record.LiveCount == 0);
}

TEST_CASE("Adjusted and virtual base views destroy the exact created object", "[foundation][memory][shared]")
{
    RecordingDomain recording;
    DestructionLog log;

    SECTION("nonzero base offset")
    {
        SharedPtr<RightBase> right;
        WeakPtr<RightBase> weakRight;
        {
            SharedPtr<MultiplyDerived> derived;
            REQUIRE(TryMakeShared<MultiplyDerived>(recording.Domain, derived, log) == OwnershipStatus::Ok);
            RightBase* const expected = static_cast<RightBase*>(derived.Get());
            REQUIRE(static_cast<void*>(expected) != static_cast<void*>(derived.Get()));
            right = derived;
            SharedPtr<LeftBase> left(Move(derived));
            REQUIRE(derived == nullptr);
            REQUIRE(right.Get() == expected);
            REQUIRE(right->RightValue == 2);
            REQUIRE(left->LeftValue == 1);
            weakRight = right;
        }
        // The left view was the other owner; its release must not destroy.
        REQUIRE(log.DerivedCount == 0);
        SharedPtr<RightBase> locked = weakRight.Lock();
        REQUIRE(locked.Get() == right.Get());
        locked.Reset();
        right.Reset();
        REQUIRE(log.DerivedCount == 1);
        REQUIRE(log.LeftCount == 1);
        REQUIRE(log.RightCount == 1);
        REQUIRE(weakRight.IsExpired() == true);
        REQUIRE(weakRight.Lock() == nullptr);
        REQUIRE(recording.Record.LiveCount == 1);
        weakRight.Reset();
    }
    SECTION("virtual base")
    {
        SharedPtr<VirtualRoot> root;
        WeakPtr<VirtualRoot> weakRoot;
        {
            SharedPtr<Diamond> diamond;
            REQUIRE(TryMakeShared<Diamond>(recording.Domain, diamond, log) == OwnershipStatus::Ok);
            VirtualRoot* const expected = static_cast<VirtualRoot*>(diamond.Get());
            // Weak base views adjust while the strong owner protects the object.
            weakRoot = diamond;
            root = Move(diamond);
            REQUIRE(root.Get() == expected);
            REQUIRE(root->RootValue == 4);
        }
        REQUIRE(weakRoot.Lock().Get() == root.Get());
        root.Reset();
        REQUIRE(log.DerivedCount == 1);
        REQUIRE(log.RootCount == 1);
        REQUIRE(weakRoot.IsExpired() == true);
        weakRoot.Reset();
    }
    REQUIRE(recording.Record.FreeCount == 1);
    REQUIRE(recording.Record.MismatchedFreeCount == 0);
    REQUIRE(recording.Record.LiveCount == 0);
}

TEST_CASE("Const views share and destroy the mutable created object", "[foundation][memory][shared]")
{
    RecordingDomain recording;
    int32 destructionCount = 0;
    SharedPtr<Tracked> mutableOwner;
    REQUIRE(TryMakeShared<Tracked>(recording.Domain, mutableOwner, 3, destructionCount) == OwnershipStatus::Ok);
    mutableOwner->Value = 4;
    SharedPtr<const Tracked> copied = mutableOwner;
    WeakPtr<Tracked> weak = mutableOwner;
    WeakPtr<const Tracked> constWeak = weak;
    SharedPtr<const Tracked> moved(Move(mutableOwner));
    REQUIRE(mutableOwner == nullptr);
    REQUIRE(copied.Get() == moved.Get());
    REQUIRE(copied->Value == 4);
    REQUIRE(constWeak.Lock().Get() == moved.Get());
    copied.Reset();
    moved.Reset();
    REQUIRE(destructionCount == 1);
    REQUIRE(constWeak.IsExpired() == true);
    REQUIRE(weak.IsExpired() == true);
    weak.Reset();
    REQUIRE(recording.Record.LiveCount == 1);
    constWeak.Reset();
    REQUIRE(recording.Record.LiveCount == 0);
    REQUIRE(recording.Record.MismatchedFreeCount == 0);
}

TEST_CASE("Weak observers keep only the controller and never revive the object", "[foundation][memory][shared]")
{
    RecordingDomain recording;
    int32 destructionCount = 0;

    SECTION("empty observers are expired")
    {
        WeakPtr<Tracked> empty;
        REQUIRE(empty.IsExpired() == true);
        REQUIRE(empty.Lock() == nullptr);
        const SharedPtr<Tracked> emptyOwner;
        WeakPtr<Tracked> fromEmpty(emptyOwner);
        REQUIRE(fromEmpty.IsExpired() == true);
        REQUIRE(fromEmpty.Lock() == nullptr);
        REQUIRE(recording.Record.AttemptCount == 0);
    }
    SECTION("final strong release destroys; final weak release frees")
    {
        SharedPtr<Tracked> owner;
        REQUIRE(TryMakeShared<Tracked>(recording.Domain, owner, 6, destructionCount) == OwnershipStatus::Ok);
        WeakPtr<Tracked> weak(owner);
        WeakPtr<Tracked> copy(weak);
        REQUIRE(weak.IsExpired() == false);
        {
            SharedPtr<Tracked> locked = weak.Lock();
            REQUIRE(locked.Get() == owner.Get());
            owner.Reset();
            // The promoted owner keeps the object alive.
            REQUIRE(destructionCount == 0);
            REQUIRE(copy.IsExpired() == false);
            REQUIRE(locked->Value == 6);
        }
        REQUIRE(destructionCount == 1);
        REQUIRE(weak.IsExpired() == true);
        REQUIRE(copy.Lock() == nullptr);
        REQUIRE(recording.Record.LiveCount == 1);
        REQUIRE(recording.Record.FreeCount == 0);

        WeakPtr<Tracked> moved(Move(weak));
        REQUIRE(weak.IsExpired() == true);
        moved = copy;
        WeakPtr<Tracked>& alias = moved;
        moved = alias;
        moved = Move(alias);
        REQUIRE(moved.IsExpired() == true);
        copy.Reset();
        REQUIRE(recording.Record.LiveCount == 1);
        moved = nullptr;
        REQUIRE(recording.Record.LiveCount == 0);
        REQUIRE(recording.Record.FreeCount == 1);
        REQUIRE(recording.Record.MismatchedFreeCount == 0);
    }
    SECTION("weak swap and reassignment from a new owner")
    {
        int32 secondDestructionCount = 0;
        SharedPtr<Tracked> first;
        SharedPtr<Tracked> second;
        REQUIRE(TryMakeShared<Tracked>(recording.Domain, first, 1, destructionCount) == OwnershipStatus::Ok);
        REQUIRE(TryMakeShared<Tracked>(recording.Domain, second, 2, secondDestructionCount) == OwnershipStatus::Ok);
        WeakPtr<Tracked> observer(first);
        WeakPtr<Tracked> other(second);
        observer.Swap(other);
        REQUIRE(observer.Lock().Get() == second.Get());
        REQUIRE(other.Lock().Get() == first.Get());
        first.Reset();
        REQUIRE(other.IsExpired() == true);
        // A weak value cannot follow a replacement; reassigning observes the new object.
        other = second;
        REQUIRE(other.Lock().Get() == second.Get());
        REQUIRE(recording.Record.LiveCount == 1);
        second.Reset();
        REQUIRE(recording.Record.LiveCount == 1);
        observer.Reset();
        other.Reset();
        REQUIRE(recording.Record.LiveCount == 0);
        REQUIRE(secondDestructionCount == 1);
    }
}

TEST_CASE("Weak and strong releases inside the object destructor are safe", "[foundation][memory][shared]")
{
    RecordingDomain recording;
    int32 observerDestructionCount = 0;
    int32 neighbourDestructionCount = 0;

    SECTION("the object's own weak value is the last explicit weak hold")
    {
        SharedPtr<SelfObserver> owner;
        REQUIRE(TryMakeShared<SelfObserver>(recording.Domain, owner, observerDestructionCount) == OwnershipStatus::Ok);
        owner->Self = owner;
        REQUIRE(owner->Self.Lock().Get() == owner.Get());
        REQUIRE(TryMakeShared<Tracked>(recording.Domain, owner->Neighbour, 8, neighbourDestructionCount) ==
                OwnershipStatus::Ok);
        owner.Reset();
        REQUIRE(observerDestructionCount == 1);
        REQUIRE(neighbourDestructionCount == 1);
        REQUIRE(recording.Record.FreeCount == 2);
    }
    SECTION("an external observer outlives the self-observing object")
    {
        SharedPtr<SelfObserver> owner;
        REQUIRE(TryMakeShared<SelfObserver>(recording.Domain, owner, observerDestructionCount) == OwnershipStatus::Ok);
        owner->Self = owner;
        WeakPtr<SelfObserver> external(owner);
        owner.Reset();
        REQUIRE(observerDestructionCount == 1);
        REQUIRE(external.IsExpired() == true);
        REQUIRE(recording.Record.LiveCount == 1);
        external.Reset();
    }
    REQUIRE(recording.Record.LiveCount == 0);
    REQUIRE(recording.Record.MismatchedFreeCount == 0);
}

TEST_CASE("Nested release and member-sourced assignment stage before releasing", "[foundation][memory][shared]")
{
    RecordingDomain recording;
    int32 destructionCount = 0;
    SharedPtr<Node> head;
    REQUIRE(TryMakeShared<Node>(recording.Domain, head, 1, destructionCount) == OwnershipStatus::Ok);
    REQUIRE(TryMakeShared<Node>(recording.Domain, head->Next, 2, destructionCount) == OwnershipStatus::Ok);
    REQUIRE(TryMakeShared<Node>(recording.Domain, head->Next->Next, 3, destructionCount) == OwnershipStatus::Ok);

    SECTION("releasing the head releases the whole chain")
    {
        head.Reset();
        REQUIRE(destructionCount == 3);
    }
    SECTION("move from a member of the previous object")
    {
        head = Move(head->Next);
        REQUIRE(destructionCount == 1);
        REQUIRE(head->Value == 2);
        head = Move(head->Next);
        REQUIRE(destructionCount == 2);
        REQUIRE(head->Value == 3);
        head.Reset();
        REQUIRE(destructionCount == 3);
    }
    SECTION("copy from a member of the previous object")
    {
        head = head->Next;
        REQUIRE(destructionCount == 1);
        REQUIRE(head->Value == 2);
        head = head->Next;
        REQUIRE(destructionCount == 2);
        REQUIRE(head->Value == 3);
        REQUIRE(head->Next == nullptr);
        head.Reset();
        REQUIRE(destructionCount == 3);
    }
    REQUIRE(recording.Record.FreeCount == 3);
    REQUIRE(recording.Record.LiveCount == 0);
    REQUIRE(recording.Record.MismatchedFreeCount == 0);
}

TEST_CASE("Reduced-width counts pin at saturation and reject promotion from zero", "[foundation][memory][shared]")
{
    using detail::GetPinnedCount;
    constexpr uint8 PINNED = GetPinnedCount<uint8>();
    static_assert(PINNED == 255);

    SECTION("retain saturates once, then the count is pinned")
    {
        std::atomic<uint8> count{static_cast<uint8>(PINNED - 2)};
        REQUIRE(detail::RetainCount(count) == CountChange::Changed);
        REQUIRE(detail::RetainCount(count) == CountChange::Saturated);
        REQUIRE(detail::RetainCount(count) == CountChange::Pinned);
        REQUIRE(count.load() == PINNED);
        // Releases cannot undo saturation and never reach zero.
        for (usize i = 0; i < 2 * static_cast<usize>(PINNED); ++i)
        {
            REQUIRE(detail::ReleaseCount(count) == CountChange::Pinned);
        }
        REQUIRE(count.load() == PINNED);
        // A pinned strong count still promotes, without changing.
        REQUIRE(detail::TryPromoteCount(count) == CountChange::Pinned);
        REQUIRE(count.load() == PINNED);
    }
    SECTION("promotion saturates like retain")
    {
        std::atomic<uint8> count{static_cast<uint8>(PINNED - 1)};
        REQUIRE(detail::TryPromoteCount(count) == CountChange::Saturated);
        REQUIRE(count.load() == PINNED);
    }
    SECTION("release reaches zero exactly once and zero rejects promotion")
    {
        std::atomic<uint8> count{2};
        REQUIRE(detail::TryPromoteCount(count) == CountChange::Changed);
        REQUIRE(count.load() == 3);
        REQUIRE(detail::ReleaseCount(count) == CountChange::Changed);
        REQUIRE(detail::ReleaseCount(count) == CountChange::Changed);
        REQUIRE(detail::ReleaseCount(count) == CountChange::ReachedZero);
        REQUIRE(count.load() == 0);
        REQUIRE(detail::TryPromoteCount(count) == CountChange::Rejected);
        REQUIRE(detail::TryPromoteCount(count) == CountChange::Rejected);
        REQUIRE(count.load() == 0);
    }
}
