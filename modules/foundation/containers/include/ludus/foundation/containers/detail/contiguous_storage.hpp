#pragma once

// Lifetime and relocation primitives for Ludus contiguous containers.
//
// This is the ONLY place object lifetime is begun/ended in raw storage and the
// ONLY place memcpy/memmove touch element storage. Centralizing it means the
// (subtle) correctness argument lives in one auditable location:
//
//   * Objects are created with placement new and destroyed with an explicit
//     destructor call (the standard lifetime operations). Placement new / an
//     explicit ~T() is used instead of std::construct_at / std::destroy_at so
//     this header does not include <memory> (which is very heavy on libstdc++
//     and would tax every translation unit that uses an Array). Array is not a
//     constexpr container (see docs/architecture/containers.md), so the
//     constant-evaluation-only benefit of std::construct_at is not needed.
//   * The trivial fast paths (skip destructors, memcpy relocation) are guarded
//     by std::is_trivially_destructible_v / IsTriviallyRelocatable so they are
//     only taken where the standard (or an explicit, audited opt-in) permits.
//   * Nothing here relies on undefined behaviour. For a non-trivially-relocatable
//     type, relocation is move-construct + destroy, never a byte copy.
//
// Ludus is compiled with -fno-exceptions, so element constructors/moves cannot
// throw; these helpers therefore do not need rollback-on-throw logic. They do,
// however, track exactly how many objects have been constructed so the caller
// (Array) can destroy precisely the live range on teardown.
//
// This lives under include/.../detail because Array is a header template and
// must reach these helpers at the point of instantiation, so the file is
// installed with the SDK. It is NOT part of the supported API: consumers include
// array.hpp, never this file. It is under detail/ (not internal/) because the
// SDK-install validation forbids installing headers whose path contains
// "internal"; detail/ is the conventional "unstable implementation" marker for a
// header that must nonetheless ship.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/relocation.hpp>

#include <cstring> // std::memcpy / std::memmove for the trivial fast paths
#include <new>     // placement new (object lifetime start in raw storage)
#include <type_traits>

namespace ludus::foundation::core::internal
{
// Raw byte copy/move of `count` trivially-relocatable elements. Centralizes the
// only two byte-level operations in this header so the object-representation copy
// is expressed once. The void* casts are explicit (the element type may itself be
// a pointer type, e.g. Array<T*>, which is a correct and common use).
template <typename ElementType>
LUDUS_INLINE void RawCopyBytes(ElementType* dst, const ElementType* src, usize count) noexcept
{
    // NOLINTNEXTLINE(bugprone-sizeof-expression,bugprone-multi-level-implicit-pointer-conversion)
    std::memcpy(static_cast<void*>(dst), static_cast<const void*>(src), count * sizeof(ElementType));
}

template <typename ElementType>
LUDUS_INLINE void RawMoveBytes(ElementType* dst, const ElementType* src, usize count) noexcept
{
    // NOLINTNEXTLINE(bugprone-sizeof-expression,bugprone-multi-level-implicit-pointer-conversion)
    std::memmove(static_cast<void*>(dst), static_cast<const void*>(src), count * sizeof(ElementType));
}

// Construct one object at raw storage `at`, forwarding arguments (placement new).
// `at` may be a pointer-to-pointer for Array<T*>; the void* placement address is
// deliberate.
template <typename ElementType, typename... Args>
LUDUS_INLINE ElementType* ConstructAt(ElementType* at, Args&&... args)
{
    // NOLINTNEXTLINE(bugprone-multi-level-implicit-pointer-conversion)
    return ::new (static_cast<void*>(at)) ElementType(static_cast<Args&&>(args)...);
}

// Destroy one object (explicit destructor call). No-op for trivially
// destructible types.
template <typename ElementType>
LUDUS_INLINE void DestroyAt(ElementType* at) noexcept
{
    if constexpr (std::is_trivially_destructible_v<ElementType> == false)
    {
        at->~ElementType();
    }
}

// Destroy the live range [first, last). No-op (compiled away) for trivially
// destructible element types.
template <typename ElementType>
LUDUS_INLINE void DestroyRange(ElementType* first, ElementType* last) noexcept
{
    if constexpr (std::is_trivially_destructible_v<ElementType> == false)
    {
        for (; first != last; ++first)
        {
            first->~ElementType();
        }
    }
}

// Value-initialize `count` objects at uninitialized `dst` (T() — zeroes scalars).
template <typename ElementType>
void UninitializedValueConstruct(ElementType* dst, usize count)
{
    for (usize i = 0; i < count; ++i)
    {
        ConstructAt(dst + i);
    }
}

// Copy-construct `count` copies of `value` into uninitialized `dst`.
template <typename ElementType>
void UninitializedFill(ElementType* dst, usize count, const ElementType& value)
{
    for (usize i = 0; i < count; ++i)
    {
        ConstructAt(dst + i, value);
    }
}

// Copy-construct from [src, src+count) into uninitialized `dst`.
// Trivial fast path: memcpy (dst and src must not overlap).
template <typename ElementType>
void UninitializedCopy(ElementType* dst, const ElementType* src, usize count)
{
    if (count == 0)
    {
        return;
    }
    if constexpr (std::is_trivially_copyable_v<ElementType>)
    {
        RawCopyBytes(dst, src, count);
        return;
    }
    else
    {
        for (usize i = 0; i < count; ++i)
        {
            ConstructAt(dst + i, src[i]);
        }
    }
}

// Relocate [src, src+count) into uninitialized `dst` (non-overlapping blocks):
// for each element, "move-construct at dst then destroy source", i.e. the source
// range is left with no live objects. This is exactly what reallocation needs.
//
//   * Trivially relocatable (incl. trivially copyable): one memcpy, and NO
//     source destructors (the bytes ARE the moved-to objects). Legal today for
//     trivially copyable; legal for opt-in types by the LUDUS_TRIVIALLY_RELOCATABLE
//     contract.
//   * Otherwise: per-element move-construct then destroy the source object.
template <typename ElementType>
void UninitializedRelocate(ElementType* dst, ElementType* src, usize count)
{
    if (count == 0)
    {
        return;
    }
    if constexpr (IsTriviallyRelocatable<ElementType>)
    {
        RawCopyBytes(dst, src, count);
    }
    else
    {
        for (usize i = 0; i < count; ++i)
        {
            ConstructAt(dst + i, static_cast<ElementType&&>(src[i]));
            DestroyAt(src + i);
        }
    }
}

// Move a *live* range of trivially-relocatable/copyable or movable elements
// within the SAME buffer, possibly overlapping, toward higher addresses by
// `shift` slots. Used by ordered Insert to open a gap. The last `shift` elements
// move into uninitialized storage; the rest are move-assigned. To keep the
// reasoning simple and correct for all types, callers use the element-count
// helpers below rather than open-coding pointer arithmetic.

// Shift the live range [first, last) right by one slot to open a gap at `first`.
// Precondition: slot at `last` is uninitialized (capacity available). After the
// call, [first+1, last+1) hold the elements and `first` is a live, moved-from
// object ready to be assigned the inserted value.
template <typename ElementType>
void ShiftRightByOne(ElementType* first, ElementType* last)
{
    if (first == last)
    {
        return;
    }
    if constexpr (IsTriviallyRelocatable<ElementType>)
    {
        RawMoveBytes(first + 1, first, static_cast<usize>(last - first));
    }
    else
    {
        // General path: move-construct the last element into the fresh slot, then
        // move-assign the remainder backwards.
        ElementType* dst = last;     // uninitialized
        ElementType* src = last - 1; // last live element
        ConstructAt(dst, static_cast<ElementType&&>(*src));
        while (src != first)
        {
            --dst;
            --src;
            *dst = static_cast<ElementType&&>(*src);
        }
        // *first is now a moved-from live object; caller assigns into it.
    }
}

// Erase-shift: after destroying the element at `pos`, move the live range
// (pos+1, last) left by one to fill the gap, leaving one moved-from live object
// at (last-1) for the caller to destroy. Used by ordered Erase.
template <typename ElementType>
void ShiftLeftByOne(ElementType* pos, ElementType* last)
{
    // pos is the (now to-be-overwritten) slot; [pos+1, last) are live.
    if constexpr (IsTriviallyRelocatable<ElementType>)
    {
        if (last - (pos + 1) > 0)
        {
            RawMoveBytes(pos, pos + 1, static_cast<usize>(last - (pos + 1)));
        }
    }
    else
    {
        ElementType* dst = pos;
        ElementType* src = pos + 1;
        for (; src != last; ++src, ++dst)
        {
            *dst = static_cast<ElementType&&>(*src);
        }
        // (last-1) is now a moved-from live object; caller destroys it.
    }
}
} // namespace ludus::foundation::core::internal
