// Negative compilation cases for SharedPtr and WeakPtr
// (docs/architecture/smart-pointers.md, slice 3). The shared-ownership suite in
// tests/build_contract/verify_pointer_diagnostics.py compiles this file once per
// LUDUS_SHARED_POINTER_CASE value and requires each case to fail with its
// expected diagnostic. Without a case the file must compile: that control run
// proves the replayed compiler flags are valid, so a failure means the rule.
#include <ludus/foundation/base/core.h>

#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/owned_object.hpp>
#include <ludus/foundation/memory/shared_ptr.hpp>

namespace ludus_shared_pointer_compile_failures
{
using ludus::foundation::AllocationDomain;
using ludus::foundation::int32;
using ludus::foundation::OwnershipStatus;
using ludus::foundation::SharedPtr;
using ludus::foundation::TryMakeShared;
using ludus::foundation::WeakPtr;

struct Base
{
    int32 BaseValue = 0;
};
struct Derived final : Base
{
    int32 DerivedValue = 0;
};

#ifndef LUDUS_SHARED_POINTER_CASE
OwnershipStatus Create(const AllocationDomain& domain, SharedPtr<const Base>& outOwner) noexcept
{
    SharedPtr<Derived> created;
    const OwnershipStatus status = TryMakeShared<Derived>(domain, created);
    WeakPtr<Base> observer(created);
    outOwner = observer.Lock();
    return status;
}
#elif LUDUS_SHARED_POINTER_CASE == 1 // No owner adopts a raw pointer.
SharedPtr<int32> Adopt(int32* object) noexcept
{
    return SharedPtr<int32>(object);
}
#elif LUDUS_SHARED_POINTER_CASE == 2 // Reset cannot adopt a raw pointer either.
void Replace(SharedPtr<int32>& inoutOwner, int32* object) noexcept
{
    inoutOwner.Reset(object);
}
#elif LUDUS_SHARED_POINTER_CASE == 3 // Arrays use Ludus containers, not shared owners.
void Declare(SharedPtr<int32[]>& inoutOwner) noexcept
{
    inoutOwner.Reset();
}
#elif LUDUS_SHARED_POINTER_CASE == 4 // The factory rejects array element types.
OwnershipStatus Create(const AllocationDomain& domain, SharedPtr<int32[4]>& inoutOwner) noexcept
{
    return TryMakeShared<int32[4]>(domain, inoutOwner);
}
#elif LUDUS_SHARED_POINTER_CASE == 5 // Unchecked downcasts are forbidden.
SharedPtr<Derived> Downcast(const SharedPtr<Base>& owner) noexcept
{
    return SharedPtr<Derived>(owner);
}
#elif LUDUS_SHARED_POINTER_CASE == 6 // A possibly expired weak value is never adjusted to a base view.
WeakPtr<Base> Upcast(const WeakPtr<Derived>& observer) noexcept
{
    return WeakPtr<Base>(observer);
}
#elif LUDUS_SHARED_POINTER_CASE == 7 // Weak values give access only through Lock().
int32 Read(const WeakPtr<Base>& observer) noexcept
{
    return observer.Get()->BaseValue;
}
#endif
} // namespace ludus_shared_pointer_compile_failures
