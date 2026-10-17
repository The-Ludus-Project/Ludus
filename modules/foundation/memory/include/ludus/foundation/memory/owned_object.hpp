#pragma once

/// @file
/// @brief Fallible creation of uniquely owned objects in an allocation domain.
///
/// `TryCreateOwned<T>` allocates one exact `T` through an `AllocationDomain` and
/// writes `UniquePtr<T, DomainDeleter<T>>`. The deleter frees through the
/// original domain with the original size and alignment. The contract is
/// specified in docs/architecture/smart-pointers.md ("Fallible construction and
/// domain lifetime", delivery slice 2).

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/detail/object_layout.hpp>

#include <new>
#include <type_traits>

namespace ludus::foundation
{
/// @brief Result of a fallible ownership factory.
///
/// Every value other than `Ok` means the output owner was left unchanged and no
/// ownership was acquired.
enum class OwnershipStatus : uint8
{
    Ok,           ///< The output owner now holds the newly constructed object.
    InvalidState, ///< The output owner was not empty, or the layout is not an object layout.
    TooLarge,     ///< The object layout cannot be represented as an allocation request.
    OutOfMemory,  ///< The allocation domain could not provide the storage.
};

/// @brief Destroys one exact object and frees its storage through its original domain.
///
/// Stores only the domain pointer; `sizeof(T)` and `alignof(T)` come from the
/// element type, so an owner of an exact type is two pointer words. The only
/// supported conversion adds `const` through the explicit adapter constructor;
/// Derived-to-Base ownership is rejected because a base view can neither recover
/// the original allocation address nor its size.
///
/// A default-constructed deleter has no domain. It is valid for an empty owner
/// (for example an output passed to `TryCreateOwned`) but must never be invoked.
///
/// @tparam ElementType Exact owned object type, optionally `const`-qualified.
///         It may be incomplete where an owner is declared (PIMPL); it must be a
///         complete, nothrow-destructible, non-array type wherever the deleter is
///         invoked.
///
/// @note The domain and its callback context must outlive every owner holding a
///       deleter bound to it.
template <typename ElementType>
class DomainDeleter final
{
    template <typename OtherElementType>
    friend class DomainDeleter;

public:
    /// @brief Constructs a deleter without a domain, for an empty owner only.
    constexpr DomainDeleter() noexcept = default;

    /// @brief Binds the deleter to the domain that allocated the owned object.
    /// @param domain Domain whose `TryAllocate` returned the object's storage. Borrowed;
    ///        it must outlive the deleter and every owner holding it.
    constexpr explicit DomainDeleter(const AllocationDomain& domain) noexcept : mDomainOrNull(&domain) {}

    /// @brief Adapts the deleter of a mutable exact object to its `const` view.
    ///
    /// Adding `const` never adjusts the object address, so the domain and
    /// layout remain correct. Use it through the explicit `UniquePtr`
    /// adapter conversion.
    /// @param other Deleter of the same object type without the added `const`.
    template <typename OtherElementType>
        requires(std::is_same_v<std::add_const_t<OtherElementType>, ElementType> &&
                 std::is_same_v<OtherElementType, ElementType> == false)
    constexpr explicit DomainDeleter(DomainDeleter<OtherElementType>&& other) noexcept
        : mDomainOrNull(other.mDomainOrNull)
    {
    }

    /// @brief Inspects the domain that frees the owned object.
    /// @return The bound domain, or `nullptr` for a default-constructed deleter.
    [[nodiscard]] LUDUS_INLINE const AllocationDomain* GetDomainOrNull() const noexcept
    {
        return mDomainOrNull;
    }

    /// @brief Destroys `pointer` and frees its exact layout through the bound domain.
    /// @param pointer Object created by `TryCreateOwned` through the bound domain.
    ///        `UniquePtr` never passes `nullptr`.
    /// @pre The deleter is bound to a live domain. Violations terminate through
    ///      `LUDUS_REQUIRE` rather than freeing through an unknown backend.
    void operator()(ElementType* pointer) const noexcept
    {
        using ObjectType = std::remove_cv_t<ElementType>;
        static_assert(std::is_array_v<ElementType> == false,
                      "DomainDeleter does not own arrays; use a Ludus container for sequences");
        static_assert(
            requires { sizeof(ElementType); },
            "DomainDeleter requires a complete type: define the owner's destructor (and any move "
            "assignment or Reset call) out of line where the owned type is complete");
        static_assert(std::is_nothrow_destructible_v<ObjectType>, "DomainDeleter requires a nothrow destructor");
        LUDUS_REQUIRE(mDomainOrNull != nullptr, "A domain deleter without a domain cannot free an object");
        ObjectType* object = const_cast<ObjectType*>(pointer);
        object->~ObjectType();
        mDomainOrNull->Free(static_cast<void*>(object), sizeof(ObjectType), alignof(ObjectType));
    }

private:
    const AllocationDomain* mDomainOrNull = nullptr;
};
} // namespace ludus::foundation

namespace ludus::foundation
{
/// @brief Allocates one exact object through `domain` and writes its owner.
///
/// The sequence is: reject a nonempty output, validate the layout with checked
/// `usize` arithmetic, request exactly `sizeof(ElementType)` bytes at
/// `alignof(ElementType)` (including over-aligned types), and only then forward
/// `arguments` to the constructor. Allocation failure therefore never consumes
/// move arguments. The resulting owner frees through `domain` with that same
/// layout.
///
/// Construction must not fail. A type with fallible setup uses a nothrow inert
/// constructor and an explicit status operation; its factory creates a private
/// candidate here, runs setup, lets the candidate roll back on failure, and
/// moves only a complete object into its own output.
///
/// @tparam ElementType Exact, non-array, non-cv object type, nothrow constructible
///         from `ArgumentTypes` and nothrow destructible. Convert the owner to a
///         `const` view afterwards through the explicit adapter if needed.
/// @param domain Allocation domain for the object. Borrowed; it must outlive the
///        returned owner.
/// @param inoutOwner Output owner. Must be empty on entry. Written only on `Ok`;
///        every other status leaves it unchanged.
/// @param arguments Constructor arguments, forwarded only after allocation succeeds.
/// @return `Ok` with `inoutOwner` holding the object; `InvalidState` if
///         `inoutOwner` was not empty; `TooLarge` if the layout cannot be
///         requested; `OutOfMemory` if `domain` returned no storage.
/// @note Thread affinity: thread safe as far as `domain`'s callbacks are; the
///       output owner itself is not synchronized.
template <typename ElementType, typename... ArgumentTypes>
[[nodiscard]] OwnershipStatus TryCreateOwned(const AllocationDomain& domain,
                                             UniquePtr<ElementType, DomainDeleter<ElementType>>& inoutOwner,
                                             ArgumentTypes&&... arguments) noexcept
    requires(std::is_object_v<ElementType> && std::is_array_v<ElementType> == false &&
             std::is_same_v<ElementType, std::remove_cv_t<ElementType>> &&
             std::is_nothrow_constructible_v<ElementType, ArgumentTypes&&...> &&
             std::is_nothrow_destructible_v<ElementType>)
{
    if (inoutOwner.Get() != nullptr)
    {
        return OwnershipStatus::InvalidState;
    }
    const detail::ObjectLayoutCheck layoutCheck = detail::CheckObjectLayout(sizeof(ElementType), alignof(ElementType));
    if (layoutCheck == detail::ObjectLayoutCheck::Malformed)
    {
        return OwnershipStatus::InvalidState;
    }
    if (layoutCheck == detail::ObjectLayoutCheck::TooLarge)
    {
        return OwnershipStatus::TooLarge;
    }
    void* storageOrNull = domain.TryAllocate(sizeof(ElementType), alignof(ElementType));
    if (storageOrNull == nullptr)
    {
        return OwnershipStatus::OutOfMemory;
    }
    ElementType* object = ::new (storageOrNull) ElementType(Forward<ArgumentTypes>(arguments)...);
    inoutOwner = UniquePtr<ElementType, DomainDeleter<ElementType>>(object, DomainDeleter<ElementType>(domain));
    return OwnershipStatus::Ok;
}
} // namespace ludus::foundation
