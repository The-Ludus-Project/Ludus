// Ownership and compile-constraint coverage for UniquePtr<T, D>. Contract and
// delivery slice: docs/architecture/smart-pointers.md ("Unique owner contract",
// slice 1 "Unique correctness"). Hard-error diagnostics that cannot be observed
// through a trait are checked by tests/build_contract/verify_pointer_diagnostics.py.
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>

#include <type_traits>

#include <catch2/catch_test_macros.hpp>

// An incomplete native type with a specialized, stateless deleter, mirroring
// the Wayland object owners. Its operator() is never instantiated here.
struct NativeObject;

namespace ludus::foundation::core
{
template <>
struct DefaultDeleter<NativeObject>
{
    void operator()(NativeObject* object) const noexcept;
};
} // namespace ludus::foundation::core

namespace
{
using ludus::foundation::float32;
using ludus::foundation::int32;
using ludus::foundation::Move;
using ludus::foundation::UniquePtr;
using ludus::foundation::usize;
namespace core = ludus::foundation::core;

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

struct DeletionLog
{
    int32 InvocationCount = 0;
    int32 MovedFromInvocationCount = 0;
    int32 LastDomainId = 0;
    int32 LastValue = 0;
};

constexpr int32 MOVED_FROM_DOMAIN_ID = -1;

// A stateful deleter standing in for an allocation domain. Moving it marks the
// source so a destruction through a moved-from deleter is observable.
template <typename ElementType>
struct RecordingDeleter
{
    RecordingDeleter() noexcept = default;
    RecordingDeleter(int32 domainId, DeletionLog& inoutLog) noexcept : DomainId(domainId), LogOrNull(&inoutLog) {}

    // Adapter adding const qualification while preserving the domain.
    template <typename OtherElementType>
        requires(std::is_same_v<const OtherElementType, ElementType> &&
                 std::is_same_v<OtherElementType, ElementType> == false)
    explicit RecordingDeleter(RecordingDeleter<OtherElementType>&& other) noexcept
        : DomainId(other.DomainId), LogOrNull(other.LogOrNull)
    {
        other.DomainId = MOVED_FROM_DOMAIN_ID;
    }

    RecordingDeleter(const RecordingDeleter&) = delete;
    RecordingDeleter& operator=(const RecordingDeleter&) = delete;

    RecordingDeleter(RecordingDeleter&& other) noexcept : DomainId(other.DomainId), LogOrNull(other.LogOrNull)
    {
        other.DomainId = MOVED_FROM_DOMAIN_ID;
    }
    RecordingDeleter& operator=(RecordingDeleter&& other) noexcept
    {
        DomainId = other.DomainId;
        LogOrNull = other.LogOrNull;
        other.DomainId = MOVED_FROM_DOMAIN_ID;
        return *this;
    }
    ~RecordingDeleter() = default;

    void operator()(ElementType* object) const noexcept
    {
        if (LogOrNull != nullptr)
        {
            ++LogOrNull->InvocationCount;
            if (DomainId == MOVED_FROM_DOMAIN_ID)
            {
                ++LogOrNull->MovedFromInvocationCount;
            }
            LogOrNull->LastDomainId = DomainId;
            LogOrNull->LastValue = object->Value;
        }
        delete object;
    }

    int32 DomainId = 0;
    DeletionLog* LogOrNull = nullptr;
};

using RecordedOwner = UniquePtr<Tracked, RecordingDeleter<Tracked>>;

RecordedOwner MakeRecorded(int32 value, int32& inoutDestructionCount, int32 domainId, DeletionLog& inoutLog)
{
    Tracked* object = new Tracked(value, inoutDestructionCount);
    return RecordedOwner(object, RecordingDeleter<Tracked>(domainId, inoutLog));
}

UniquePtr<Tracked> MakeTracked(int32 value, int32& inoutDestructionCount)
{
    Tracked* object = new Tracked(value, inoutDestructionCount);
    return UniquePtr<Tracked>(object);
}

struct VirtualBase
{
    VirtualBase() noexcept = default;
    VirtualBase(const VirtualBase&) = delete;
    VirtualBase& operator=(const VirtualBase&) = delete;
    virtual ~VirtualBase() = default;

    int32 BaseValue = 1;
};

struct SecondBase
{
    SecondBase() noexcept = default;
    SecondBase(const SecondBase&) = delete;
    SecondBase& operator=(const SecondBase&) = delete;
    virtual ~SecondBase() = default;

    int32 SecondValue = 2;
};

struct VirtualDerived final : VirtualBase, SecondBase
{
    explicit VirtualDerived(int32& inoutDestructionCount) noexcept : DestructionCount(&inoutDestructionCount) {}
    VirtualDerived(const VirtualDerived&) = delete;
    VirtualDerived& operator=(const VirtualDerived&) = delete;
    ~VirtualDerived() override
    {
        ++*DestructionCount;
    }

    int32* DestructionCount;
};

struct PlainBase
{
    int32 Value = 0;
};

struct PlainDerived final : PlainBase
{
};

struct PrivatelyDerived final : private VirtualBase
{
};

struct LeftBranch : VirtualBase
{
};

struct RightBranch : VirtualBase
{
};

struct Diamond final : LeftBranch, RightBranch
{
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

    UniquePtr<Node> Child;
    int32 Value;
    int32* DestructionCount;
};

UniquePtr<Node> MakeNode(int32 value, int32& inoutDestructionCount)
{
    Node* node = new Node(value, inoutDestructionCount);
    return UniquePtr<Node>(node);
}

// Records whether its owner had already detached when destruction began.
struct SelfObserved
{
    SelfObserved(const UniquePtr<SelfObserved>& owner, bool& outWasOwnerEmpty) noexcept
        : Owner(&owner), WasOwnerEmpty(&outWasOwnerEmpty)
    {
    }
    SelfObserved(const SelfObserved&) = delete;
    SelfObserved& operator=(const SelfObserved&) = delete;
    ~SelfObserved()
    {
        *WasOwnerEmpty = Owner->Get() == nullptr;
    }

    const UniquePtr<SelfObserved>* Owner;
    bool* WasOwnerEmpty;
};

// A PIMPL owner whose implementation type is incomplete at the member
// declaration and complete where the special members are defined.
class PimplOwner final
{
public:
    PimplOwner(int32 value, int32& inoutDestructionCount);
    PimplOwner(const PimplOwner&) = delete;
    PimplOwner& operator=(const PimplOwner&) = delete;
    PimplOwner(PimplOwner&& other) noexcept;
    PimplOwner& operator=(PimplOwner&& other) noexcept;
    ~PimplOwner();

    [[nodiscard]] int32 GetValue() const noexcept;
    [[nodiscard]] bool HasImplementation() const noexcept;

private:
    struct Implementation;
    UniquePtr<Implementation> mImplementation;
};

static_assert(sizeof(PimplOwner) == sizeof(void*));

struct PimplOwner::Implementation
{
    Implementation(int32 value, int32& inoutDestructionCount) noexcept : Object(value, inoutDestructionCount) {}

    Tracked Object;
};

PimplOwner::PimplOwner(int32 value, int32& inoutDestructionCount)
{
    Implementation* implementation = new Implementation(value, inoutDestructionCount);
    mImplementation.Reset(implementation);
}

PimplOwner::PimplOwner(PimplOwner&& other) noexcept = default;
PimplOwner& PimplOwner::operator=(PimplOwner&& other) noexcept = default;
PimplOwner::~PimplOwner() = default;

int32 PimplOwner::GetValue() const noexcept
{
    return mImplementation->Object.Value;
}

bool PimplOwner::HasImplementation() const noexcept
{
    return static_cast<bool>(mImplementation);
}

// --- Compile-time contract -------------------------------------------------

struct ThrowingDeleter
{
    void operator()(int32* object) const
    {
        delete object;
    }
};

struct MismatchedDeleter
{
    void operator()(float32* object) const noexcept
    {
        delete object;
    }
};

struct ThrowingDestructorDeleter
{
    ThrowingDestructorDeleter() noexcept = default;
    ThrowingDestructorDeleter(const ThrowingDestructorDeleter&) noexcept = default;
    ThrowingDestructorDeleter& operator=(const ThrowingDestructorDeleter&) noexcept = default;
    // Deliberately potentially-throwing so trait tests can reject it.
    // NOLINTNEXTLINE(bugprone-unsafe-to-allow-exceptions)
    ~ThrowingDestructorDeleter() noexcept(false) {}

    void operator()(int32* object) const noexcept
    {
        delete object;
    }
};

struct NonDefaultDeleter
{
    explicit NonDefaultDeleter(int32 domainId) noexcept : DomainId(domainId) {}

    void operator()(int32* object) const noexcept
    {
        delete object;
    }

    int32 DomainId;
};

struct ThrowingMoveDeleter
{
    ThrowingMoveDeleter() noexcept = default;
    // Deliberately potentially-throwing so trait tests can reject it.
    // NOLINTNEXTLINE(bugprone-unsafe-to-allow-exceptions)
    ThrowingMoveDeleter(ThrowingMoveDeleter&&) noexcept(false) {}
    // Deliberately potentially-throwing so trait tests can reject it.
    // NOLINTNEXTLINE(bugprone-unsafe-to-allow-exceptions)
    ThrowingMoveDeleter& operator=(ThrowingMoveDeleter&&) noexcept(false)
    {
        return *this;
    }
    ~ThrowingMoveDeleter() = default;

    void operator()(int32* object) const noexcept
    {
        delete object;
    }
};

// Accepts any pointer, so it cannot know whether a converted address still
// identifies the allocation it must destroy.
struct GenericDeleter
{
    template <typename ElementType>
    void operator()(ElementType* object) const noexcept
    {
        delete object;
    }
};

template <typename ElementType, typename DeleterType>
concept CanNameOwner = requires { typename core::UniquePtr<ElementType, DeleterType>; };

template <typename OwnerType>
concept CanBareRelease = requires(OwnerType& owner) { owner.Release(); };

template <typename OwnerType>
concept CanSwap = requires(OwnerType& owner) { owner.Swap(owner); };

// Supplied deleters are validated against the actual element pointer.
static_assert(core::Deleter<core::DefaultDeleter<int32>, int32>);
static_assert(core::Deleter<RecordingDeleter<Tracked>, Tracked>);
static_assert(core::Deleter<ThrowingDeleter, int32> == false);
static_assert(core::Deleter<MismatchedDeleter, int32> == false);
static_assert(core::Deleter<ThrowingDestructorDeleter, int32> == false);
static_assert(core::Deleter<RecordingDeleter<Tracked>&, Tracked> == false);
static_assert(core::Deleter<const RecordingDeleter<Tracked>, Tracked> == false);
static_assert(CanNameOwner<int32, ThrowingDeleter> == false);
static_assert(CanNameOwner<int32, MismatchedDeleter> == false);
static_assert(CanNameOwner<int32, ThrowingDestructorDeleter> == false);
static_assert(CanNameOwner<int32[], core::DefaultDeleter<int32[]>> == false);
constexpr usize FIXED_ARRAY_COUNT = 4;
static_assert(CanNameOwner<int32[FIXED_ARRAY_COUNT], core::DefaultDeleter<int32[FIXED_ARRAY_COUNT]>> == false);

// The public alias carries the deleter parameter.
static_assert(std::is_same_v<UniquePtr<int32>, core::UniquePtr<int32, core::DefaultDeleter<int32>>>);
static_assert(
    std::is_same_v<UniquePtr<Tracked, RecordingDeleter<Tracked>>, core::UniquePtr<Tracked, RecordingDeleter<Tracked>>>);

// Layout: an empty deleter occupies no storage.
static_assert(sizeof(UniquePtr<int32>) == sizeof(int32*));
static_assert(sizeof(UniquePtr<NativeObject>) == sizeof(NativeObject*));

// Every destruction, move and swap is nothrow; there is no copy or raw conversion.
static_assert(std::is_nothrow_destructible_v<UniquePtr<int32>>);
static_assert(std::is_nothrow_move_constructible_v<RecordedOwner>);
static_assert(std::is_nothrow_move_assignable_v<RecordedOwner>);
static_assert(std::is_copy_constructible_v<UniquePtr<int32>> == false);
static_assert(std::is_copy_assignable_v<UniquePtr<int32>> == false);
static_assert(std::is_convertible_v<int32*&, UniquePtr<int32>> == false);
static_assert(std::is_constructible_v<UniquePtr<int32>, int32*> == false);
static_assert(std::is_nothrow_constructible_v<UniquePtr<int32>, int32*&>);

// Operations are constrained individually on the deleter operations they perform.
using NonDefaultOwner = UniquePtr<int32, NonDefaultDeleter>;
static_assert(std::is_default_constructible_v<NonDefaultOwner> == false);
static_assert(std::is_constructible_v<NonDefaultOwner, ludus::foundation::nullptr_t> == false);
static_assert(std::is_constructible_v<NonDefaultOwner, int32*&> == false);
static_assert(std::is_nothrow_constructible_v<NonDefaultOwner, int32*&, NonDefaultDeleter>);
static_assert(std::is_nothrow_move_constructible_v<NonDefaultOwner>);
using ThrowingMoveOwner = UniquePtr<int32, ThrowingMoveDeleter>;
static_assert(std::is_nothrow_default_constructible_v<ThrowingMoveOwner>);
static_assert(std::is_move_constructible_v<ThrowingMoveOwner> == false);
static_assert(std::is_move_assignable_v<ThrowingMoveOwner> == false);
static_assert(std::is_constructible_v<ThrowingMoveOwner, int32*&, ThrowingMoveDeleter> == false);
static_assert(CanSwap<ThrowingMoveOwner> == false);
static_assert(CanSwap<RecordedOwner>);

// Bare release is limited to the stateless DefaultDeleter family.
static_assert(CanBareRelease<UniquePtr<Tracked>>);
static_assert(CanBareRelease<UniquePtr<NativeObject>>);
static_assert(CanBareRelease<RecordedOwner> == false);
static_assert(CanBareRelease<UniquePtr<int32, GenericDeleter>> == false);

// Ordinary-delete conversion: accessible, unambiguous, virtual or cv-only.
static_assert(std::is_nothrow_convertible_v<UniquePtr<VirtualDerived>, UniquePtr<VirtualBase>>);
static_assert(std::is_nothrow_convertible_v<UniquePtr<VirtualDerived>, UniquePtr<SecondBase>>);
static_assert(std::is_nothrow_assignable_v<UniquePtr<VirtualBase>&, UniquePtr<VirtualDerived>&&>);
static_assert(std::is_nothrow_convertible_v<UniquePtr<Tracked>, UniquePtr<const Tracked>>);
static_assert(std::is_nothrow_convertible_v<UniquePtr<PlainDerived>, UniquePtr<const PlainDerived>>);
static_assert(std::is_constructible_v<UniquePtr<PlainBase>, UniquePtr<PlainDerived>&&> == false);
static_assert(std::is_assignable_v<UniquePtr<PlainBase>&, UniquePtr<PlainDerived>&&> == false);
static_assert(std::is_constructible_v<UniquePtr<VirtualDerived>, UniquePtr<VirtualBase>&&> == false);
static_assert(std::is_constructible_v<UniquePtr<Tracked>, UniquePtr<const Tracked>&&> == false);
static_assert(std::is_constructible_v<UniquePtr<VirtualBase>, UniquePtr<PrivatelyDerived>&&> == false);
static_assert(std::is_constructible_v<UniquePtr<VirtualBase>, UniquePtr<Diamond>&&> == false);
static_assert(std::is_constructible_v<UniquePtr<VirtualBase>, UniquePtr<VirtualDerived>&> == false);

// Native specialized deleters never convert by type relationship alone.
static_assert(std::is_constructible_v<UniquePtr<const NativeObject>, UniquePtr<NativeObject>&&> == false);

// Custom deleters convert only explicitly, through a protocol-preserving adapter.
using ConstRecordedOwner = UniquePtr<const Tracked, RecordingDeleter<const Tracked>>;
static_assert(std::is_nothrow_constructible_v<ConstRecordedOwner, RecordedOwner&&>);
static_assert(std::is_convertible_v<RecordedOwner, ConstRecordedOwner> == false);
static_assert(std::is_assignable_v<ConstRecordedOwner&, RecordedOwner&&> == false);
static_assert(
    std::is_constructible_v<UniquePtr<VirtualBase, GenericDeleter>, UniquePtr<VirtualDerived, GenericDeleter>&&> ==
    false);
static_assert(std::is_constructible_v<UniquePtr<const int32, GenericDeleter>, UniquePtr<int32, GenericDeleter>&&>);
static_assert(std::is_convertible_v<UniquePtr<int32, GenericDeleter>, UniquePtr<const int32, GenericDeleter>> == false);
static_assert(std::is_constructible_v<UniquePtr<VirtualBase>, UniquePtr<VirtualDerived, GenericDeleter>&&> == false);
} // namespace

TEST_CASE("UniquePtr empty owners never invoke their deleter", "[pointer]")
{
    DeletionLog log;
    Tracked* noneOrNull = nullptr;
    {
        RecordedOwner empty(noneOrNull, RecordingDeleter<Tracked>(4, log));
        CHECK(empty.Get() == nullptr);
        CHECK(static_cast<bool>(empty) == false);
        CHECK(empty.GetDeleter().DomainId == 4);
        empty.Reset();
    }
    CHECK(log.InvocationCount == 0);

    UniquePtr<int32> defaulted;
    UniquePtr<int32> fromNull = nullptr;
    CHECK(defaulted.Get() == nullptr);
    CHECK(fromNull.Get() == nullptr);
}

TEST_CASE("UniquePtr adoption nulls the source and destroys once", "[pointer]")
{
    int32 destroyed = 0;
    Tracked* object = new Tracked(7, destroyed);
    Tracked* const original = object;
    {
        UniquePtr<Tracked> owner(object);
        CHECK(object == nullptr);
        CHECK(owner.Get() == original);
        CHECK((*owner).Value == 7);
        CHECK(owner->Value == 7);
        CHECK(destroyed == 0);
    }
    CHECK(destroyed == 1);
}

TEST_CASE("UniquePtr move construction transfers the deleter", "[pointer]")
{
    int32 destroyed = 0;
    DeletionLog log;
    {
        RecordedOwner source = MakeRecorded(10, destroyed, 3, log);
        RecordedOwner destination(Move(source));
        CHECK(source.Get() == nullptr);
        CHECK(destination->Value == 10);
        CHECK(destination.GetDeleter().DomainId == 3);
        CHECK(source.GetDeleter().DomainId == MOVED_FROM_DOMAIN_ID);
    }
    CHECK(destroyed == 1);
    CHECK(log.InvocationCount == 1);
    CHECK(log.LastDomainId == 3);
    CHECK(log.MovedFromInvocationCount == 0);
}

TEST_CASE("UniquePtr occupied move assignment destroys with the old deleter", "[pointer]")
{
    int32 destroyed = 0;
    DeletionLog log;
    {
        RecordedOwner destination = MakeRecorded(10, destroyed, 1, log);
        RecordedOwner source = MakeRecorded(20, destroyed, 2, log);
        destination = Move(source);
        CHECK(destroyed == 1);
        CHECK(log.LastDomainId == 1);
        CHECK(log.LastValue == 10);
        CHECK(source.Get() == nullptr);
        CHECK(destination->Value == 20);
        CHECK(destination.GetDeleter().DomainId == 2);
    }
    CHECK(destroyed == 2);
    CHECK(log.InvocationCount == 2);
    CHECK(log.LastDomainId == 2);
    CHECK(log.LastValue == 20);
    CHECK(log.MovedFromInvocationCount == 0);
}

TEST_CASE("UniquePtr default-deleter move assignment does not leak", "[pointer]")
{
    int32 destroyed = 0;
    UniquePtr<Tracked> destination = MakeTracked(1, destroyed);
    destination = MakeTracked(2, destroyed);
    CHECK(destroyed == 1);
    CHECK(destination->Value == 2);
    destination = nullptr;
    CHECK(destroyed == 2);
    CHECK(destination.Get() == nullptr);
}

TEST_CASE("UniquePtr self-move and self-reset leave the owner unchanged", "[pointer]")
{
    int32 destroyed = 0;
    DeletionLog log;
    RecordedOwner owner = MakeRecorded(5, destroyed, 6, log);
    Tracked* const original = owner.Get();

    RecordedOwner& alias = owner;
    owner = Move(alias);
    CHECK(owner.Get() == original);
    CHECK(owner.GetDeleter().DomainId == 6);

    owner.Reset(owner.Get());
    CHECK(owner.Get() == original);

    owner.Swap(alias);
    CHECK(owner.Get() == original);
    CHECK(owner.GetDeleter().DomainId == 6);
    CHECK(destroyed == 0);
    CHECK(log.InvocationCount == 0);

    owner.Reset();
    CHECK(destroyed == 1);
    CHECK(log.LastDomainId == 6);
}

TEST_CASE("UniquePtr Reset destroys the old object and adopts the new one", "[pointer]")
{
    int32 destroyed = 0;
    UniquePtr<Tracked> owner = MakeTracked(1, destroyed);
    Tracked* replacement = new Tracked(2, destroyed);
    owner.Reset(replacement);
    CHECK(destroyed == 1);
    CHECK(owner.Get() == replacement);
    owner.Reset();
    CHECK(destroyed == 2);
    owner.Reset();
    CHECK(destroyed == 2);
}

TEST_CASE("UniquePtr detaches before invoking the deleter", "[pointer]")
{
    bool wasOwnerEmpty = false;

    {
        UniquePtr<SelfObserved> owner;
        SelfObserved* object = new SelfObserved(owner, wasOwnerEmpty);
        owner.Reset(object);
    }
    CHECK(wasOwnerEmpty == true);

    wasOwnerEmpty = false;
    UniquePtr<SelfObserved> owner;
    SelfObserved* object = new SelfObserved(owner, wasOwnerEmpty);
    owner.Reset(object);
    owner.Reset();
    CHECK(wasOwnerEmpty == true);

    wasOwnerEmpty = false;
    object = new SelfObserved(owner, wasOwnerEmpty);
    owner.Reset(object);
    owner = UniquePtr<SelfObserved>();
    CHECK(wasOwnerEmpty == true);
}

TEST_CASE("UniquePtr move assignment can extract a member of the old object", "[pointer]")
{
    int32 destroyed = 0;
    UniquePtr<Node> root = MakeNode(1, destroyed);
    root->Child = MakeNode(2, destroyed);
    root->Child->Child = MakeNode(3, destroyed);

    root = Move(root->Child);
    CHECK(destroyed == 1);
    REQUIRE(root.Get() != nullptr);
    CHECK(root->Value == 2);
    REQUIRE(root->Child.Get() != nullptr);
    CHECK(root->Child->Value == 3);

    root = Move(root->Child);
    CHECK(destroyed == 2);
    CHECK(root->Value == 3);
    CHECK(root->Child.Get() == nullptr);

    root.Reset();
    CHECK(destroyed == 3);
}

TEST_CASE("UniquePtr swap exchanges stateful deleters with their objects", "[pointer]")
{
    int32 destroyed = 0;
    DeletionLog log;
    RecordedOwner first = MakeRecorded(10, destroyed, 1, log);
    RecordedOwner second = MakeRecorded(20, destroyed, 2, log);

    first.Swap(second);
    CHECK(first->Value == 20);
    CHECK(first.GetDeleter().DomainId == 2);
    CHECK(second->Value == 10);
    CHECK(second.GetDeleter().DomainId == 1);

    first.Reset();
    CHECK(log.LastDomainId == 2);
    CHECK(log.LastValue == 20);
    second.Reset();
    CHECK(log.LastDomainId == 1);
    CHECK(log.LastValue == 10);
    CHECK(destroyed == 2);
    CHECK(log.MovedFromInvocationCount == 0);
}

TEST_CASE("UniquePtr converts ordinary-delete owners to a virtual base", "[pointer]")
{
    int32 destroyed = 0;

    VirtualDerived* object = new VirtualDerived(destroyed);
    UniquePtr<VirtualDerived> derived(object);
    UniquePtr<VirtualBase> base = Move(derived);
    CHECK(derived.Get() == nullptr);
    CHECK(base->BaseValue == 1);
    base.Reset();
    CHECK(destroyed == 1);

    // A non-primary base adjusts the address; destruction still reaches the
    // complete object through the virtual destructor.
    object = new VirtualDerived(destroyed);
    SecondBase* const adjusted = object;
    UniquePtr<VirtualDerived> second(object);
    UniquePtr<SecondBase> secondBase(Move(second));
    CHECK(secondBase.Get() == adjusted);
    CHECK(secondBase->SecondValue == 2);
    secondBase.Reset();
    CHECK(destroyed == 2);

    // Converting assignment into an occupied owner destroys the old object.
    VirtualBase* baseObject = new VirtualDerived(destroyed);
    UniquePtr<VirtualBase> occupied(baseObject);
    object = new VirtualDerived(destroyed);
    UniquePtr<VirtualDerived> replacement(object);
    occupied = Move(replacement);
    CHECK(destroyed == 3);
    CHECK(replacement.Get() == nullptr);
    occupied = nullptr;
    CHECK(destroyed == 4);
}

TEST_CASE("UniquePtr adds const through ordinary and adapted deleters", "[pointer]")
{
    int32 destroyed = 0;
    DeletionLog log;
    {
        UniquePtr<const Tracked> viewed = MakeTracked(1, destroyed);
        CHECK(viewed->Value == 1);
    }
    CHECK(destroyed == 1);
    {
        RecordedOwner source = MakeRecorded(2, destroyed, 9, log);
        ConstRecordedOwner viewed(Move(source));
        CHECK(source.Get() == nullptr);
        CHECK(viewed.GetDeleter().DomainId == 9);
    }
    CHECK(destroyed == 2);
    CHECK(log.LastDomainId == 9);
    CHECK(log.MovedFromInvocationCount == 0);
}

TEST_CASE("UniquePtr Release hands default-deleter ownership to the caller", "[pointer]")
{
    int32 destroyed = 0;
    UniquePtr<Tracked> owner = MakeTracked(3, destroyed);
    Tracked* released = owner.Release();
    CHECK(owner.Get() == nullptr);
    CHECK(destroyed == 0);
    delete released;
    CHECK(destroyed == 1);
}

TEST_CASE("UniquePtr supports PIMPL owners of incomplete types", "[pointer]")
{
    int32 destroyed = 0;
    {
        PimplOwner first(1, destroyed);
        PimplOwner second(2, destroyed);
        CHECK(first.GetValue() == 1);
        first = Move(second);
        CHECK(destroyed == 1);
        CHECK(first.GetValue() == 2);
        CHECK(second.HasImplementation() == false);
        PimplOwner third(Move(first));
        CHECK(third.GetValue() == 2);
        CHECK(first.HasImplementation() == false);
    }
    CHECK(destroyed == 2);
}
