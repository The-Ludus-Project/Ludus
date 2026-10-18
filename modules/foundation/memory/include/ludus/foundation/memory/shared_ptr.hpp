#pragma once

/// @file
/// @brief Atomic shared ownership and weak observation of one domain-allocated object.
///
/// `TryMakeShared<T>` allocates one combined controller/object block through an
/// `AllocationDomain` and writes `SharedPtr<T>`. Copies of that owner share the
/// object; `WeakPtr<T>` observes it without keeping it alive and can promote
/// back to a strong owner only while it is live. The final strong release
/// destroys the exact object on the releasing thread; the final weak release
/// (including the strong lifetime's implicit hold) frees the block through the
/// original domain. The contract is specified in
/// docs/architecture/smart-pointers.md ("Shared object and controller",
/// "Controller state and reclamation", delivery slice 3).
///
/// Thanks to the ISO C++ working draft's shared owner
/// (https://eel.is/c++draft/util.smartptr.shared) and weak observer
/// (https://eel.is/c++draft/util.smartptr.weak.obs) contracts, whose ownership
/// and atomic weak-promotion semantics this narrows: there is no raw-pointer
/// adoption, aliasing, array ownership or shared-from-this, and construction
/// reports failure as an explicit status.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/detail/shared_controller.hpp>
#include <ludus/foundation/memory/owned_object.hpp>

#include <new>
#include <type_traits>

namespace ludus::foundation
{
template <typename ElementType>
class SharedPtr;
template <typename ElementType>
class WeakPtr;

namespace detail
{
// Factory access to SharedPtr's private adopting constructor; no public
// operation can attach a controller to an arbitrary pointer.
struct SharedOwnerAccess final
{
    template <typename ElementType>
    [[nodiscard]] static SharedPtr<ElementType> AdoptCreated(ElementType* object,
                                                             SharedController& inoutController) noexcept;
};

// An owner of SourceType converts to an owner of DestinationType when the view
// pointer converts implicitly: an accessible, unambiguous upcast (including a
// virtual base) or an added cv-qualification.
template <typename SourceType, typename DestinationType>
concept SharedViewConversion =
    std::is_convertible_v<SourceType*, DestinationType*> && std::is_same_v<SourceType, DestinationType> == false;

// Weak-to-weak conversion never adjusts a possibly expired view: only cv
// qualification may change. Derived-to-Base weak views are made from a live
// SharedPtr instead.
template <typename SourceType, typename DestinationType>
concept WeakViewConversion = SharedViewConversion<SourceType, DestinationType> &&
                             std::is_same_v<std::remove_cv_t<SourceType>, std::remove_cv_t<DestinationType>>;
} // namespace detail

/// @brief Atomic shared owner of one object created by `TryMakeShared`.
///
/// Holds two words: the typed view and the controller of the original
/// allocation. Copies share ownership; the final release destroys the exact
/// created object (never through the view's type) and keeps the controller
/// alive for remaining `WeakPtr` observers. An upcast changes only the view.
///
/// There is no constructor or `Reset` that adopts a raw pointer: the only
/// source of ownership is `TryMakeShared` (or `WeakPtr::Lock` of an object it
/// created). Never create a second owner from `Get()` or `this`.
///
/// @tparam ElementType Owned object type, optionally cv-qualified. Arrays,
///         references, functions and `void` are rejected. It may be incomplete
///         where an owner is declared, copied or destroyed: destruction runs
///         through the creator's type-erased thunk.
///
/// @note Thread safety: distinct owner values that share one object may be
///       copied, moved and destroyed concurrently. One owner variable is not
///       synchronized: concurrent reads and writes of the same `SharedPtr`
///       value need external synchronization. Ownership does not synchronize
///       access to the object; prefer immutable `SharedPtr<const T>` snapshots.
/// @note The final release runs the destructor synchronously on whichever
///       thread releases last. Objects that need thread-affine or deferred
///       retirement (windows, GPU, audio) do not belong in a `SharedPtr`.
template <typename ElementType>
class SharedPtr final
{
    static_assert(std::is_object_v<ElementType>,
                  "SharedPtr owns objects; references, functions and void cannot be shared owners");
    static_assert(std::is_array_v<ElementType> == false,
                  "SharedPtr does not own arrays; use a Ludus container for sequences");

    template <typename OtherElementType>
    friend class SharedPtr;
    template <typename OtherElementType>
    friend class WeakPtr;
    /// @brief Lets `TryMakeShared` adopt the hold of a newly created controller; not caller API.
    friend struct detail::SharedOwnerAccess;

    // Adopts one strong hold that the caller already acquired for `inoutController`.
    SharedPtr(detail::SharedController& inoutController, ElementType* object) noexcept
        : mObjectOrNull(object), mControllerOrNull(&inoutController)
    {
    }

public:
    /// @brief Constructs an empty owner.
    constexpr SharedPtr() noexcept = default;

    /// @brief Constructs an empty owner.
    constexpr SharedPtr(nullptr_t) noexcept {}

    /// @brief Shares `other`'s object, adding one strong owner.
    /// @param other Owner to copy; may be empty.
    SharedPtr(const SharedPtr& other) noexcept
        : mObjectOrNull(other.mObjectOrNull), mControllerOrNull(other.mControllerOrNull)
    {
        retainStrong(mControllerOrNull);
    }

    /// @brief Shares `other`'s object through an upcast or cv-qualified view.
    ///
    /// The view adjustment is computed while `other` keeps the object alive;
    /// destruction remains that of the created type.
    /// @param other Owner to copy; may be empty.
    template <typename OtherElementType>
        requires detail::SharedViewConversion<OtherElementType, ElementType>
    SharedPtr(const SharedPtr<OtherElementType>& other) noexcept
        : mObjectOrNull(other.mObjectOrNull), mControllerOrNull(other.mControllerOrNull)
    {
        retainStrong(mControllerOrNull);
    }

    /// @brief Takes `other`'s ownership without reference traffic, leaving `other` empty.
    SharedPtr(SharedPtr&& other) noexcept
        : mObjectOrNull(other.mObjectOrNull), mControllerOrNull(other.mControllerOrNull)
    {
        other.mObjectOrNull = nullptr;
        other.mControllerOrNull = nullptr;
    }

    /// @brief Takes `other`'s ownership through an upcast or cv-qualified view, leaving `other` empty.
    template <typename OtherElementType>
        requires detail::SharedViewConversion<OtherElementType, ElementType>
    SharedPtr(SharedPtr<OtherElementType>&& other) noexcept
        : mObjectOrNull(other.mObjectOrNull), mControllerOrNull(other.mControllerOrNull)
    {
        other.mObjectOrNull = nullptr;
        other.mControllerOrNull = nullptr;
    }

    /// @brief Releases this owner's hold, if any.
    ~SharedPtr()
    {
        Reset();
    }

    /// @brief Shares `other`'s object, then releases the previous one.
    ///
    /// Self-assignment leaves the owner unchanged. Otherwise the incoming hold
    /// is acquired before the previous hold is released, so assigning from a
    /// value owned by the previous object is safe.
    SharedPtr& operator=(const SharedPtr& other) noexcept
    {
        if (this != &other)
        {
            retainStrong(other.mControllerOrNull);
            replace(other.mObjectOrNull, other.mControllerOrNull);
        }
        return *this;
    }

    /// @brief Shares `other`'s object through a converted view, then releases the previous one.
    template <typename OtherElementType>
        requires detail::SharedViewConversion<OtherElementType, ElementType>
    SharedPtr& operator=(const SharedPtr<OtherElementType>& other) noexcept
    {
        retainStrong(other.mControllerOrNull);
        replace(other.mObjectOrNull, other.mControllerOrNull);
        return *this;
    }

    /// @brief Takes `other`'s ownership, then releases the previous one.
    ///
    /// Self-move leaves the owner unchanged. Otherwise both source fields are
    /// staged and `other` emptied before the previous hold is released, so
    /// `head = Move(head->Next)` is valid.
    SharedPtr& operator=(SharedPtr&& other) noexcept
    {
        if (this != &other)
        {
            ElementType* stagedObject = other.mObjectOrNull;
            detail::SharedController* stagedController = other.mControllerOrNull;
            other.mObjectOrNull = nullptr;
            other.mControllerOrNull = nullptr;
            replace(stagedObject, stagedController);
        }
        return *this;
    }

    /// @brief Takes `other`'s ownership through a converted view, then releases the previous one.
    template <typename OtherElementType>
        requires detail::SharedViewConversion<OtherElementType, ElementType>
    SharedPtr& operator=(SharedPtr<OtherElementType>&& other) noexcept
    {
        ElementType* stagedObject = other.mObjectOrNull;
        detail::SharedController* stagedController = other.mControllerOrNull;
        other.mObjectOrNull = nullptr;
        other.mControllerOrNull = nullptr;
        replace(stagedObject, stagedController);
        return *this;
    }

    /// @brief Releases this owner's hold, leaving it empty.
    SharedPtr& operator=(nullptr_t) noexcept
    {
        Reset();
        return *this;
    }

    /// @brief Releases this owner's hold, leaving it empty.
    ///
    /// The owner is detached before the release, so a destructor run by this
    /// call observes an empty owner. It must not access this owner otherwise.
    void Reset() noexcept
    {
        replace(nullptr, nullptr);
    }

    /// @brief Exchanges view and controller with `other` without reference traffic.
    void Swap(SharedPtr& inoutOther) noexcept
    {
        ElementType* stagedObject = mObjectOrNull;
        detail::SharedController* stagedController = mControllerOrNull;
        mObjectOrNull = inoutOther.mObjectOrNull;
        mControllerOrNull = inoutOther.mControllerOrNull;
        inoutOther.mObjectOrNull = stagedObject;
        inoutOther.mControllerOrNull = stagedController;
    }

    /// @brief Borrows the typed view without affecting ownership.
    /// @return The shared object, or `nullptr` for an empty owner. Valid only
    ///         while some strong owner keeps the object alive.
    [[nodiscard]] LUDUS_INLINE ElementType* Get() const noexcept
    {
        return mObjectOrNull;
    }

    /// @brief Accesses the shared object.
    /// @pre The owner is not empty. Development builds assert this.
    [[nodiscard]] LUDUS_INLINE ElementType& operator*() const noexcept
    {
        LUDUS_ASSERT(mObjectOrNull != nullptr);
        return *mObjectOrNull;
    }

    /// @brief Accesses a member of the shared object.
    /// @pre The owner is not empty. Development builds assert this.
    [[nodiscard]] LUDUS_INLINE ElementType* operator->() const noexcept
    {
        LUDUS_ASSERT(mObjectOrNull != nullptr);
        return mObjectOrNull;
    }

    /// @brief Reports whether the owner holds an object.
    [[nodiscard]] LUDUS_INLINE explicit operator bool() const noexcept
    {
        return mObjectOrNull != nullptr;
    }

    /// @brief Reports whether `owner` is empty (also provides `!=`, and either operand order).
    [[nodiscard]] friend LUDUS_INLINE bool operator==(const SharedPtr& owner, nullptr_t) noexcept
    {
        return owner.mObjectOrNull == nullptr;
    }

private:
    static void retainStrong(detail::SharedController* controllerOrNull) noexcept
    {
        if (controllerOrNull != nullptr)
        {
            detail::RetainSharedStrong(*controllerOrNull);
        }
    }

    // Installs an already-acquired hold (or empty), then releases the previous
    // hold; a destructor run by that release observes the new state.
    void replace(ElementType* objectOrNull, detail::SharedController* controllerOrNull) noexcept
    {
        detail::SharedController* previousControllerOrNull = mControllerOrNull;
        mObjectOrNull = objectOrNull;
        mControllerOrNull = controllerOrNull;
        if (previousControllerOrNull != nullptr)
        {
            detail::ReleaseSharedStrong(*previousControllerOrNull);
        }
    }

    ElementType* mObjectOrNull = nullptr;
    detail::SharedController* mControllerOrNull = nullptr;
};

/// @brief Non-owning observer of an object owned by `SharedPtr`.
///
/// Keeps the controller (and the combined allocation) alive but not the
/// object. `Lock()` obtains a temporary strong owner while the object is live
/// and returns empty forever after the final strong release; a weak value
/// never follows a replacement object. There is deliberately no `Get`,
/// dereference or arrow operator: access the object only through `Lock()`.
///
/// @tparam ElementType Observed object type, optionally cv-qualified. Arrays,
///         references, functions and `void` are rejected.
///
/// @note Thread safety: as for `SharedPtr`, distinct values may be used
///       concurrently; one variable needs external synchronization.
/// @note Retained weak values keep the whole combined block allocated, including
///       the dead inline object storage, until the last weak value is released.
template <typename ElementType>
class WeakPtr final
{
    static_assert(std::is_object_v<ElementType>,
                  "WeakPtr observes objects; references, functions and void cannot be observed");
    static_assert(std::is_array_v<ElementType> == false,
                  "WeakPtr does not observe arrays; use a Ludus container for sequences");

    template <typename OtherElementType>
    friend class WeakPtr;

public:
    /// @brief Constructs an empty, expired observer.
    constexpr WeakPtr() noexcept = default;

    /// @brief Constructs an empty, expired observer.
    constexpr WeakPtr(nullptr_t) noexcept {}

    /// @brief Observes `owner`'s object, possibly through an upcast or cv-qualified view.
    ///
    /// The view adjustment is computed here, while `owner` keeps the object alive.
    /// @param owner Strong owner to observe; an empty owner yields an empty observer.
    template <typename OtherElementType>
        requires std::is_convertible_v<OtherElementType*, ElementType*>
    WeakPtr(const SharedPtr<OtherElementType>& owner) noexcept
        : mObjectOrNull(owner.mObjectOrNull), mControllerOrNull(owner.mControllerOrNull)
    {
        retainWeak(mControllerOrNull);
    }

    /// @brief Observes the same object as `other`.
    WeakPtr(const WeakPtr& other) noexcept
        : mObjectOrNull(other.mObjectOrNull), mControllerOrNull(other.mControllerOrNull)
    {
        retainWeak(mControllerOrNull);
    }

    /// @brief Observes the same object as `other` through a more cv-qualified view.
    ///
    /// Derived-to-Base conversion from a weak value is rejected: the object may
    /// already be destroyed, so the adjustment cannot be computed safely.
    template <typename OtherElementType>
        requires detail::WeakViewConversion<OtherElementType, ElementType>
    WeakPtr(const WeakPtr<OtherElementType>& other) noexcept
        : mObjectOrNull(other.mObjectOrNull), mControllerOrNull(other.mControllerOrNull)
    {
        retainWeak(mControllerOrNull);
    }

    /// @brief Takes `other`'s observation without reference traffic, leaving `other` empty.
    WeakPtr(WeakPtr&& other) noexcept : mObjectOrNull(other.mObjectOrNull), mControllerOrNull(other.mControllerOrNull)
    {
        other.mObjectOrNull = nullptr;
        other.mControllerOrNull = nullptr;
    }

    /// @brief Takes `other`'s observation through a more cv-qualified view, leaving `other` empty.
    template <typename OtherElementType>
        requires detail::WeakViewConversion<OtherElementType, ElementType>
    WeakPtr(WeakPtr<OtherElementType>&& other) noexcept
        : mObjectOrNull(other.mObjectOrNull), mControllerOrNull(other.mControllerOrNull)
    {
        other.mObjectOrNull = nullptr;
        other.mControllerOrNull = nullptr;
    }

    /// @brief Releases this observer's weak hold, if any.
    ~WeakPtr()
    {
        Reset();
    }

    /// @brief Observes `other`'s object, then releases the previous weak hold.
    ///
    /// Self-assignment leaves the observer unchanged.
    WeakPtr& operator=(const WeakPtr& other) noexcept
    {
        if (this != &other)
        {
            retainWeak(other.mControllerOrNull);
            replace(other.mObjectOrNull, other.mControllerOrNull);
        }
        return *this;
    }

    /// @brief Observes `owner`'s object, then releases the previous weak hold.
    /// @param owner Strong owner to observe; an empty owner empties this observer.
    template <typename OtherElementType>
        requires std::is_convertible_v<OtherElementType*, ElementType*>
    WeakPtr& operator=(const SharedPtr<OtherElementType>& owner) noexcept
    {
        retainWeak(owner.mControllerOrNull);
        replace(owner.mObjectOrNull, owner.mControllerOrNull);
        return *this;
    }

    /// @brief Takes `other`'s observation, then releases the previous weak hold.
    ///
    /// Self-move leaves the observer unchanged.
    WeakPtr& operator=(WeakPtr&& other) noexcept
    {
        if (this != &other)
        {
            ElementType* stagedObject = other.mObjectOrNull;
            detail::SharedController* stagedController = other.mControllerOrNull;
            other.mObjectOrNull = nullptr;
            other.mControllerOrNull = nullptr;
            replace(stagedObject, stagedController);
        }
        return *this;
    }

    /// @brief Releases this observer's weak hold, leaving it empty.
    WeakPtr& operator=(nullptr_t) noexcept
    {
        Reset();
        return *this;
    }

    /// @brief Releases this observer's weak hold, leaving it empty.
    ///
    /// The observer is detached before the release.
    void Reset() noexcept
    {
        replace(nullptr, nullptr);
    }

    /// @brief Exchanges view and controller with `other` without reference traffic.
    void Swap(WeakPtr& inoutOther) noexcept
    {
        ElementType* stagedObject = mObjectOrNull;
        detail::SharedController* stagedController = mControllerOrNull;
        mObjectOrNull = inoutOther.mObjectOrNull;
        mControllerOrNull = inoutOther.mControllerOrNull;
        inoutOther.mObjectOrNull = stagedObject;
        inoutOther.mControllerOrNull = stagedController;
    }

    /// @brief Snapshot of whether the observed object has been released.
    ///
    /// `true` is permanent. `false` can become stale immediately on another
    /// thread: never check and then access; use the owner returned by `Lock()`.
    /// @return `true` for an empty observer or once the final strong release committed.
    [[nodiscard]] bool IsExpired() const noexcept
    {
        return mControllerOrNull == nullptr || detail::IsSharedObjectExpired(*mControllerOrNull);
    }

    /// @brief Obtains a strong owner of the observed object if it is still live.
    ///
    /// Promotion succeeds only while the strong count is nonzero; once the final
    /// strong release commits, every later lock returns empty. The returned view
    /// is the one stored at observation time; no adjustment is recomputed.
    /// @return A new strong owner, or an empty owner if this observer is empty or expired.
    [[nodiscard]] SharedPtr<ElementType> Lock() const noexcept
    {
        if (mControllerOrNull == nullptr || detail::TryPromoteSharedWeak(*mControllerOrNull) == false)
        {
            return SharedPtr<ElementType>();
        }
        return SharedPtr<ElementType>(*mControllerOrNull, mObjectOrNull);
    }

private:
    static void retainWeak(detail::SharedController* controllerOrNull) noexcept
    {
        if (controllerOrNull != nullptr)
        {
            detail::RetainSharedWeak(*controllerOrNull);
        }
    }

    // Installs an already-acquired weak hold (or empty), then releases the previous one.
    void replace(ElementType* objectOrNull, detail::SharedController* controllerOrNull) noexcept
    {
        detail::SharedController* previousControllerOrNull = mControllerOrNull;
        mObjectOrNull = objectOrNull;
        mControllerOrNull = controllerOrNull;
        if (previousControllerOrNull != nullptr)
        {
            detail::ReleaseSharedWeak(*previousControllerOrNull);
        }
    }

    // Possibly dangling once expired; read only after a successful promotion.
    ElementType* mObjectOrNull = nullptr;
    detail::SharedController* mControllerOrNull = nullptr;
};

template <typename ElementType>
SharedPtr<ElementType> detail::SharedOwnerAccess::AdoptCreated(ElementType* object,
                                                               SharedController& inoutController) noexcept
{
    return SharedPtr<ElementType>(inoutController, object);
}

/// @brief Allocates one object with its shared controller through `domain` and writes its owner.
///
/// The sequence is: reject a nonempty output, validate the combined
/// controller/object layout with checked `usize` arithmetic, request one
/// allocation aligned for both the controller and `ElementType` (including
/// over-aligned types), and only then forward `arguments` to the constructor.
/// Allocation failure therefore never consumes move arguments. The object is
/// destroyed by its exact type when the last strong owner releases it, and the
/// block is freed through `domain` with its original layout when the last weak
/// hold releases it.
///
/// Construction must not fail. A type with fallible setup uses a nothrow inert
/// constructor and an explicit status operation, completes setup through a
/// private candidate, and publishes only a complete object.
///
/// @tparam ElementType Exact, non-array, non-cv object type, nothrow constructible
///         from `ArgumentTypes` and nothrow destructible. Convert the owner to a
///         `const` or base view afterwards if needed.
/// @param domain Allocation domain for the combined block. Borrowed; it, its
///        callback context and this code must outlive every strong owner and
///        weak observer of the result.
/// @param inoutOwner Output owner. Must be empty on entry. Written only on `Ok`;
///        every other status leaves it unchanged.
/// @param arguments Constructor arguments, forwarded only after allocation succeeds.
/// @return `Ok` with `inoutOwner` holding the object; `InvalidState` if
///         `inoutOwner` was not empty; `TooLarge` if the combined layout cannot be
///         requested; `OutOfMemory` if `domain` returned no storage.
/// @note Thread affinity: thread safe as far as `domain`'s callbacks are; the
///       output owner itself is not synchronized.
template <typename ElementType, typename... ArgumentTypes>
[[nodiscard]] OwnershipStatus
TryMakeShared(const AllocationDomain& domain, SharedPtr<ElementType>& inoutOwner, ArgumentTypes&&... arguments) noexcept
    requires(std::is_object_v<ElementType> && std::is_array_v<ElementType> == false &&
             std::is_same_v<ElementType, std::remove_cv_t<ElementType>> &&
             std::is_nothrow_constructible_v<ElementType, ArgumentTypes&&...> &&
             std::is_nothrow_destructible_v<ElementType>)
{
    if (inoutOwner.Get() != nullptr)
    {
        return OwnershipStatus::InvalidState;
    }
    detail::SharedAllocation allocation{};
    const OwnershipStatus status = detail::TryAllocateSharedController(domain,
                                                                       sizeof(ElementType),
                                                                       alignof(ElementType),
                                                                       &detail::DestroySharedObject<ElementType>,
                                                                       allocation);
    if (status != OwnershipStatus::Ok)
    {
        return status;
    }
    ElementType* object = ::new (allocation.ObjectStorageOrNull) ElementType(Forward<ArgumentTypes>(arguments)...);
    inoutOwner = detail::SharedOwnerAccess::AdoptCreated(object, *allocation.ControllerOrNull);
    return OwnershipStatus::Ok;
}
} // namespace ludus::foundation
