#pragma once

/// @file
/// @brief Unique single-object ownership with an explicit destruction protocol.
///
/// `UniquePtr<T, D>` owns one `T*` together with its deleter `D`; both form the
/// ownership state and always move, swap and destroy as a pair. The ownership
/// contract, conversion rules and delivery plan are specified in
/// docs/architecture/smart-pointers.md.
///
/// This header is opt-in: it is never included by `core.h`, a foundational
/// header or the precompiled header.

#include <ludus/foundation/base/core.h>

#include <type_traits>

namespace ludus::foundation::core
{
/// @brief Ordinary `delete` destruction protocol for one complete, non-array object.
///
/// The primary template declares `OrdinaryDeleteTag`, which is what
/// permits implicit `UniquePtr<Derived>` to `UniquePtr<Base>` conversion.
/// Native specializations (for example the Wayland object deleters) supply their
/// own `operator()` for incomplete external types and deliberately omit the
/// marker: a type relationship never implies that two native protocols agree.
///
/// @tparam ElementType Owned object type. Must be complete, and must not be an
///         array type, wherever `operator()` is instantiated.
template <typename ElementType>
struct DefaultDeleter
{
    /// @brief Identifies the audited ordinary-`delete` protocol of the primary template.
    using OrdinaryDeleteTag = void;

    /// @brief Destroys `pointerOrNull` with ordinary `delete`.
    /// @param pointerOrNull Object created by ordinary non-array `new` for exactly this
    ///        protocol, or `nullptr`. `UniquePtr` never passes `nullptr`.
    /// @note Instantiating this operator for an incomplete type is a compile error;
    ///       define a PIMPL owner's destructor where its implementation is complete.
    void operator()(ElementType* pointerOrNull) const noexcept
    {
        static_assert(std::is_array_v<ElementType> == false,
                      "DefaultDeleter does not own arrays; use a Ludus container for sequences");
        static_assert(
            requires { sizeof(ElementType); },
            "DefaultDeleter requires a complete type: define the owner's destructor (and any move "
            "assignment or Reset call) out of line where the owned type is complete");
        delete pointerOrNull;
    }
};

/// @brief A nothrow value deleter that can destroy an `ElementType*`.
///
/// Satisfied when `DeleterType` is a non-cv object type that is nothrow
/// destructible and whose lvalue is nothrow callable with an `ElementType*`.
/// Reference deleters are excluded. Individual `UniquePtr` operations add the
/// nothrow construction, move or assignment requirements they perform.
template <typename DeleterType, typename ElementType>
concept Deleter = std::is_object_v<DeleterType> && std::is_same_v<DeleterType, std::remove_cv_t<DeleterType>> &&
                  std::is_nothrow_destructible_v<DeleterType> && requires(DeleterType& deleter, ElementType* pointer) {
                      { deleter(pointer) } noexcept;
                  };
} // namespace ludus::foundation::core

namespace ludus::foundation::core::detail
{
// The primary DefaultDeleter's audited ordinary-delete protocol for ElementType.
template <typename DeleterType, typename ElementType>
concept OrdinaryDeleter =
    std::is_same_v<DeleterType, DefaultDeleter<ElementType>> && requires { typename DeleterType::OrdinaryDeleteTag; };

// Bare Release() is an audited escape only for stateless members of the
// DefaultDeleter family, including native specializations.
template <typename DeleterType, typename ElementType>
concept BareReleaseDeleter = std::is_same_v<DeleterType, DefaultDeleter<ElementType>> && std::is_empty_v<DeleterType>;

// Ordinary delete through DestinationType* destroys the exact SourceType object
// only for an identical object type or a virtual destructor. Kept as a separate
// disjunction so has_virtual_destructor is only asked of a related complete type.
template <typename SourceType, typename DestinationType>
concept DeleteCompatible = std::is_same_v<std::remove_cv_t<SourceType>, std::remove_cv_t<DestinationType>> ||
                           std::has_virtual_destructor_v<DestinationType>;

// Implicit conversion between two ordinary-delete owners.
template <typename SourceType, typename SourceDeleterType, typename DestinationType, typename DestinationDeleterType>
concept ImplicitOwnerConversion =
    (std::is_same_v<SourceType, DestinationType> == false) && OrdinaryDeleter<SourceDeleterType, SourceType> &&
    OrdinaryDeleter<DestinationDeleterType, DestinationType> && std::is_convertible_v<SourceType*, DestinationType*> &&
    DeleteCompatible<SourceType, DestinationType>;

// Explicit conversion through a deleter adapter constructed from the source
// deleter. Reusing one deleter type is accepted only for a cv addition, which
// never adjusts the object address the deleter receives.
template <typename SourceType, typename SourceDeleterType, typename DestinationType, typename DestinationDeleterType>
concept AdaptedOwnerConversion =
    (ImplicitOwnerConversion<SourceType, SourceDeleterType, DestinationType, DestinationDeleterType> == false) &&
    std::is_convertible_v<SourceType*, DestinationType*> &&
    std::is_nothrow_constructible_v<DestinationDeleterType, SourceDeleterType&&> &&
    ((std::is_same_v<SourceDeleterType, DestinationDeleterType> == false) ||
     std::is_same_v<std::remove_cv_t<SourceType>, std::remove_cv_t<DestinationType>>);
} // namespace ludus::foundation::core::detail

namespace ludus::foundation::core
{
/// @brief Move-only owner of one object and the deleter that destroys it.
///
/// The pointer and deleter form one ownership state: moves and swaps transfer
/// both, and destruction always uses the deleter that accompanied the pointer.
/// Every destruction, move and swap operation is `noexcept`. An empty owner
/// never invokes its deleter.
///
/// Callbacks run by the deleter may release other owners but must not access or
/// mutate the owner whose destruction, reset or assignment is executing. Do not
/// destroy an owner while holding a registry or publication lock: move it out
/// under the lock and release it after unlocking.
///
/// With an empty deleter, `sizeof(UniquePtr<T>) == sizeof(T*)`.
///
/// @tparam ElementType Owned object type. May be incomplete where the owner is
///         declared (PIMPL); it must be complete wherever the default deleter is
///         instantiated. Array types are rejected; use a Ludus container for
///         sequences and byte buffers.
/// @tparam DeleterType Value deleter satisfying `Deleter<DeleterType, ElementType>`.
///
/// @note Thread affinity: a single owner is not synchronized. Different owners
///       may be used concurrently; one owner variable needs external
///       synchronization for concurrent access.
template <typename ElementType, Deleter<ElementType> DeleterType = DefaultDeleter<ElementType>>
    requires(std::is_array_v<ElementType> == false)
class UniquePtr final
{
    template <typename OtherElementType, Deleter<OtherElementType> OtherDeleterType>
        requires(std::is_array_v<OtherElementType> == false)
    friend class UniquePtr;

public:
    /// @brief Constructs an empty owner with a value-initialized deleter.
    constexpr UniquePtr() noexcept
        requires std::is_nothrow_default_constructible_v<DeleterType>
        : mDeleter(), mPointer(nullptr)
    {
    }

    /// @brief Constructs an empty owner with a value-initialized deleter.
    constexpr UniquePtr(nullptr_t) noexcept
        requires std::is_nothrow_default_constructible_v<DeleterType>
        : mDeleter(), mPointer(nullptr)
    {
    }

    /// @brief Adopts `inoutPointer` with a value-initialized deleter and nulls the source.
    /// @param inoutPointer Uniquely owned object created for this deleter's protocol,
    ///        or `nullptr`. Set to `nullptr` on return.
    /// @pre No other owner holds the object.
    explicit UniquePtr(ElementType*& inoutPointer) noexcept
        requires std::is_nothrow_default_constructible_v<DeleterType>
        : mDeleter(), mPointer(inoutPointer)
    {
        inoutPointer = nullptr;
    }

    /// @brief Adopts `inoutPointer` together with the deleter that must destroy it.
    /// @param inoutPointer Uniquely owned object matching `deleter`'s protocol, or
    ///        `nullptr`. Set to `nullptr` on return.
    /// @param deleter Destruction protocol, moved into the owner.
    /// @pre No other owner holds the object.
    UniquePtr(ElementType*& inoutPointer, DeleterType deleter) noexcept
        requires std::is_nothrow_move_constructible_v<DeleterType>
        : mDeleter(Move(deleter)), mPointer(inoutPointer)
    {
        inoutPointer = nullptr;
    }

    /// @brief Owners are not copyable.
    UniquePtr(const UniquePtr&) = delete;

    /// @brief Takes the pointer and deleter of `other`, leaving `other` empty.
    UniquePtr(UniquePtr&& other) noexcept
        requires std::is_nothrow_move_constructible_v<DeleterType>
        : mDeleter(Move(other.mDeleter)), mPointer(other.mPointer)
    {
        other.mPointer = nullptr;
    }

    /// @brief Converts an ordinary-`delete` owner of a derived object (or a less
    ///        cv-qualified object) into an owner of `ElementType`.
    ///
    /// Available only between primary `DefaultDeleter` owners when the pointer
    /// conversion is accessible and unambiguous and either the object types
    /// differ only in cv-qualification or `ElementType` has a virtual destructor.
    /// `other` is left empty.
    template <typename OtherElementType, typename OtherDeleterType>
        requires detail::ImplicitOwnerConversion<OtherElementType, OtherDeleterType, ElementType, DeleterType> &&
                     std::is_nothrow_default_constructible_v<DeleterType>
    UniquePtr(UniquePtr<OtherElementType, OtherDeleterType>&& other) noexcept : mDeleter(), mPointer(other.mPointer)
    {
        other.mPointer = nullptr;
    }

    /// @brief Explicitly converts an owner through a deleter adapter.
    ///
    /// `DeleterType` is nothrow constructed from `other`'s deleter and must
    /// preserve its destruction protocol for the converted pointer. The same
    /// deleter type is accepted only when adding cv-qualification. `other` is
    /// left empty.
    template <typename OtherElementType, typename OtherDeleterType>
        requires detail::AdaptedOwnerConversion<OtherElementType, OtherDeleterType, ElementType, DeleterType>
    explicit UniquePtr(UniquePtr<OtherElementType, OtherDeleterType>&& other) noexcept
        : mDeleter(Move(other.mDeleter)), mPointer(other.mPointer)
    {
        other.mPointer = nullptr;
    }

    /// @brief Replaces this owner's state with `other`'s pointer and deleter.
    ///
    /// Self-move leaves the owner unchanged. Otherwise the complete source state
    /// is staged and `other` emptied first, then the previous object is destroyed
    /// with its own deleter, then the staged state is installed. The source is
    /// never read after the previous object is destroyed, so
    /// `root = Move(root->Child)` is valid.
    UniquePtr& operator=(UniquePtr&& other) noexcept
        requires std::is_nothrow_move_constructible_v<DeleterType> && std::is_nothrow_move_assignable_v<DeleterType>
    {
        if (this != &other)
        {
            ElementType* stagedPointer = other.mPointer;
            other.mPointer = nullptr;
            DeleterType stagedDeleter(Move(other.mDeleter));
            destroyOwned();
            mDeleter = Move(stagedDeleter);
            mPointer = stagedPointer;
        }
        return *this;
    }

    /// @brief Owners are not copy-assignable.
    UniquePtr& operator=(const UniquePtr&) = delete;

    /// @brief Replaces this owner's object with a converted ordinary-`delete` owner.
    ///
    /// Uses the same staging order as same-type move assignment and the same
    /// constraints as the implicit converting constructor. `other` is left empty.
    template <typename OtherElementType, typename OtherDeleterType>
        requires detail::ImplicitOwnerConversion<OtherElementType, OtherDeleterType, ElementType, DeleterType>
    UniquePtr& operator=(UniquePtr<OtherElementType, OtherDeleterType>&& other) noexcept
    {
        ElementType* stagedPointer = other.mPointer;
        other.mPointer = nullptr;
        destroyOwned();
        mPointer = stagedPointer;
        return *this;
    }

    /// @brief Destroys the owned object, if any, with the owner's deleter.
    ~UniquePtr()
    {
        destroyOwned();
    }

    /// @brief Borrows the owned pointer without transferring ownership.
    /// @return The owned object, or `nullptr` for an empty owner.
    [[nodiscard]] LUDUS_INLINE ElementType* Get() const noexcept
    {
        return mPointer;
    }

    /// @brief Inspects the destruction protocol paired with the owned pointer.
    [[nodiscard]] LUDUS_INLINE const DeleterType& GetDeleter() const noexcept
    {
        return mDeleter;
    }

    /// @brief Gives up ownership without destroying the object.
    ///
    /// Available only for stateless members of the `DefaultDeleter` family,
    /// including native specializations. Owners with stateful deleters transfer
    /// the whole owner instead.
    /// @return The previously owned pointer, or `nullptr`. The receiver must
    ///         destroy it with exactly the `DefaultDeleter<ElementType>` protocol.
    [[nodiscard]] ElementType* Release() noexcept
        requires detail::BareReleaseDeleter<DeleterType, ElementType>
    {
        ElementType* released = mPointer;
        mPointer = nullptr;
        return released;
    }

    /// @brief Destroys the owned object and adopts `pointerOrNull` with the existing deleter.
    ///
    /// `pointerOrNull == Get()` leaves the owner unchanged. Otherwise the owner is
    /// detached to empty before the previous object is destroyed, then adopts
    /// `pointerOrNull`.
    /// @param pointerOrNull Uniquely owned object matching this owner's deleter
    ///        protocol, or `nullptr` to leave the owner empty.
    void Reset(ElementType* pointerOrNull = nullptr) noexcept
    {
        if (pointerOrNull == mPointer)
        {
            return;
        }
        destroyOwned();
        mPointer = pointerOrNull;
    }

    /// @brief Accesses the owned object.
    /// @pre The owner is not empty. Development builds assert this; a dangling
    ///      external pointer is not detected.
    [[nodiscard]] LUDUS_INLINE ElementType& operator*() const noexcept
    {
        LUDUS_ASSERT(mPointer != nullptr);
        return *mPointer;
    }

    /// @brief Accesses a member of the owned object.
    /// @pre The owner is not empty. Development builds assert this.
    [[nodiscard]] LUDUS_INLINE ElementType* operator->() const noexcept
    {
        LUDUS_ASSERT(mPointer != nullptr);
        return mPointer;
    }

    /// @brief Reports whether the owner holds an object.
    [[nodiscard]] LUDUS_INLINE explicit operator bool() const noexcept
    {
        return mPointer != nullptr;
    }

    /// @brief Exchanges pointer and deleter with `other` as one pair.
    void Swap(UniquePtr& other) noexcept
        requires std::is_nothrow_move_constructible_v<DeleterType> && std::is_nothrow_move_assignable_v<DeleterType>
    {
        if (this == &other)
        {
            return;
        }
        ElementType* stagedPointer = mPointer;
        DeleterType stagedDeleter(Move(mDeleter));
        mDeleter = Move(other.mDeleter);
        mPointer = other.mPointer;
        other.mDeleter = Move(stagedDeleter);
        other.mPointer = stagedPointer;
    }

private:
    // Detach before invoking the deleter so a destruction callback never sees
    // this owner still holding the object being destroyed.
    void destroyOwned() noexcept
    {
        ElementType* owned = mPointer;
        mPointer = nullptr;
        if (owned != nullptr)
        {
            mDeleter(owned);
        }
    }

    LUDUS_NO_UNIQUE_ADDRESS DeleterType mDeleter;
    ElementType* mPointer;
};
} // namespace ludus::foundation::core

namespace ludus::foundation
{
/// @brief Public spelling of `core::UniquePtr`, including its deleter parameter.
template <typename ElementType, typename DeleterType = core::DefaultDeleter<ElementType>>
using UniquePtr = core::UniquePtr<ElementType, DeleterType>;
} // namespace ludus::foundation
