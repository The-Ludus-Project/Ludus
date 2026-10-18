#pragma once

// Out-of-line shared/weak controller operations behind shared_ptr.hpp.
//
// Installed with the SDK because the SharedPtr, WeakPtr and TryMakeShared
// templates call these functions at their point of instantiation. It is NOT
// part of the supported API: consumers include shared_ptr.hpp, never this file.
// The controller layout, counters, memory ordering, saturation diagnostics and
// allocation arithmetic stay in src/shared_controller.cpp. The contract is
// docs/architecture/smart-pointers.md ("Shared object and controller" and
// "Controller state and reclamation").
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/owned_object.hpp>

namespace ludus::foundation::detail
{
// Opaque: only src/shared_controller.cpp knows its layout.
struct SharedController;

// Destroys the exact object a controller owns. Never frees storage.
using DestroySharedObjectFunction = void (*)(void* object) noexcept;

// Per-type destruction thunk: runs ObjectType's destructor on the original,
// unadjusted object address, so an upcast view never chooses the destructor.
template <typename ObjectType>
void DestroySharedObject(void* object) noexcept
{
    static_cast<ObjectType*>(object)->~ObjectType();
}

// One combined controller/object allocation whose object is not yet constructed.
struct SharedAllocation
{
    SharedController* ControllerOrNull = nullptr;
    void* ObjectStorageOrNull = nullptr;
};

// Validates the combined layout with checked arithmetic, allocates it once
// through `domain`, and initializes the controller with one strong owner and
// the implicit weak hold. The caller must construct the object in
// ObjectStorageOrNull before any other operation. Only Ok writes outAllocation.
[[nodiscard]] OwnershipStatus TryAllocateSharedController(const AllocationDomain& domain,
                                                          usize objectBytes,
                                                          usize objectAlignment,
                                                          DestroySharedObjectFunction destroyObject,
                                                          SharedAllocation& outAllocation) noexcept;

// Adds one strong owner. The caller already holds a strong owner.
void RetainSharedStrong(SharedController& inoutController) noexcept;
// Removes one strong owner; the last one destroys the object, then releases
// the implicit weak hold.
void ReleaseSharedStrong(SharedController& inoutController) noexcept;
// Adds one strong owner only while the object is live. The caller holds a weak hold.
[[nodiscard]] bool TryPromoteSharedWeak(SharedController& inoutController) noexcept;
// Adds one weak hold. The caller already holds a strong owner or weak hold.
void RetainSharedWeak(SharedController& inoutController) noexcept;
// Removes one weak hold; the last one frees the combined storage.
void ReleaseSharedWeak(SharedController& inoutController) noexcept;
// Snapshot of whether the final strong release has committed.
[[nodiscard]] bool IsSharedObjectExpired(const SharedController& controller) noexcept;
} // namespace ludus::foundation::detail
