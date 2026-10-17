// Shared/weak controller: combined allocation, counts and reclamation.
// Contract: docs/architecture/smart-pointers.md ("Shared object and
// controller", "Controller state and reclamation", "Allocation placement and
// retained bytes"). Thanks to the ISO C++ working draft's shared owner and weak
// observer clauses (https://eel.is/c++draft/util.smartptr.shared,
// https://eel.is/c++draft/util.smartptr.weak.obs) for the familiar strong/weak
// count and promotion semantics; failure reporting, saturation pinning and
// domain-owned storage are Ludus additions.
#include <ludus/foundation/memory/detail/shared_controller.hpp>

#include <ludus/foundation/base/assert.hpp>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/detail/object_layout.hpp>
#include <ludus/foundation/memory/owned_object.hpp>

#include "internal/shared_count.hpp"

#include <atomic>
#include <new>

namespace ludus::foundation::detail
{
// Lives at the start of the combined block; the object follows at its first
// aligned offset.
// Weak counts explicit weak observers plus one implicit hold for the whole
// strong lifetime and final destruction (not one per strong owner).
struct SharedController final
{
    std::atomic<uint64> Strong;
    std::atomic<uint64> Weak;
    void* Object;                              // Original, unadjusted object address.
    DestroySharedObjectFunction DestroyObject; // Exact created type's destructor.
    const AllocationDomain* Domain;            // Frees the block.
    void* Storage;                             // Combined block base (this controller).
    usize StorageBytes;
    usize StorageAlignment;
};

namespace
{
OwnershipStatus ToOwnershipStatus(ObjectLayoutCheck check) noexcept
{
    switch (check)
    {
        case ObjectLayoutCheck::Valid:
            return OwnershipStatus::Ok;
        case ObjectLayoutCheck::TooLarge:
            return OwnershipStatus::TooLarge;
        case ObjectLayoutCheck::Malformed:
            break;
    }
    return OwnershipStatus::InvalidState;
}
} // namespace

OwnershipStatus TryAllocateSharedController(const AllocationDomain& domain,
                                            usize objectBytes,
                                            usize objectAlignment,
                                            DestroySharedObjectFunction destroyObject,
                                            SharedAllocation& outAllocation) noexcept
{
    if (destroyObject == nullptr)
    {
        return OwnershipStatus::InvalidState;
    }
    const OwnershipStatus objectStatus = ToOwnershipStatus(CheckObjectLayout(objectBytes, objectAlignment));
    if (objectStatus != OwnershipStatus::Ok)
    {
        return objectStatus;
    }
    // The object starts at the first objectAlignment boundary after the
    // controller; the block is aligned for both, so both are aligned.
    usize paddedControllerBytes = 0;
    usize storageBytes = 0;
    if (TryAdd(sizeof(SharedController), objectAlignment - 1, paddedControllerBytes) == false)
    {
        return OwnershipStatus::TooLarge;
    }
    const usize objectOffset = paddedControllerBytes & ~(objectAlignment - 1);
    if (TryAdd(objectOffset, objectBytes, storageBytes) == false)
    {
        return OwnershipStatus::TooLarge;
    }
    const usize storageAlignment =
        objectAlignment > alignof(SharedController) ? objectAlignment : alignof(SharedController);
    const OwnershipStatus storageStatus = ToOwnershipStatus(CheckObjectLayout(storageBytes, storageAlignment));
    if (storageStatus != OwnershipStatus::Ok)
    {
        return storageStatus;
    }
    void* storageOrNull = domain.TryAllocate(storageBytes, storageAlignment);
    if (storageOrNull == nullptr)
    {
        return OwnershipStatus::OutOfMemory;
    }
    void* objectStorage = static_cast<uint8*>(storageOrNull) + objectOffset;
    SharedController* controller = ::new (storageOrNull) SharedController
    {
        .Strong = 1,
        .Weak = 1,
        .Object = objectStorage,
        .DestroyObject = destroyObject,
        .Domain = &domain,
        .Storage = storageOrNull,
        .StorageBytes = storageBytes,
        .StorageAlignment = storageAlignment,
    };
    outAllocation = SharedAllocation{ .ControllerOrNull = controller, .ObjectStorageOrNull = controller->Object };
    return OwnershipStatus::Ok;
}

void RetainSharedStrong(SharedController& inoutController) noexcept
{
    if (RetainCount(inoutController.Strong) == CountChange::Saturated)
    {
        LUDUS_CHECK(false, "SharedPtr strong count saturated; pinning object and storage");
    }
}

void ReleaseSharedStrong(SharedController& inoutController) noexcept
{
    if (ReleaseCount(inoutController.Strong) == CountChange::ReachedZero)
    {
        // The implicit weak hold keeps the controller alive while the
        // destructor releases weak or strong owners, including its own.
        inoutController.DestroyObject(inoutController.Object);
        ReleaseSharedWeak(inoutController);
    }
}

bool TryPromoteSharedWeak(SharedController& inoutController) noexcept
{
    const CountChange change = TryPromoteCount(inoutController.Strong);
    if (change == CountChange::Saturated)
    {
        LUDUS_CHECK(false, "SharedPtr strong count saturated; pinning object and storage");
    }
    return change != CountChange::Rejected;
}

void RetainSharedWeak(SharedController& inoutController) noexcept
{
    if (RetainCount(inoutController.Weak) == CountChange::Saturated)
    {
        LUDUS_CHECK(false, "WeakPtr count saturated; pinning controller storage");
    }
}

void ReleaseSharedWeak(SharedController& inoutController) noexcept
{
    if (ReleaseCount(inoutController.Weak) == CountChange::ReachedZero)
    {
        const AllocationDomain& domain = *inoutController.Domain;
        void* storage = inoutController.Storage;
        const usize storageBytes = inoutController.StorageBytes;
        const usize storageAlignment = inoutController.StorageAlignment;
        inoutController.~SharedController();
        domain.Free(storage, storageBytes, storageAlignment);
    }
}

bool IsSharedObjectExpired(const SharedController& controller) noexcept
{
    return controller.Strong.load(std::memory_order_relaxed) == 0;
}
} // namespace ludus::foundation::detail
