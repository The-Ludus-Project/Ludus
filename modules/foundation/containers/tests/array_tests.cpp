// Unit tests for Ludus::Array<T>. Validates lifetime (not just final values),
// growth, capacity, modifiers, copy/move, self-assignment, insert/erase,
// aliasing, and std interop across trivial / non-trivial / move-only /
// over-aligned / empty element types.

#include "lifetime_type.hpp"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <initializer_list>
#include <numeric>
#include <ranges>
#include <span>
#include <type_traits>

using ludus::foundation::Array;
using ludus::foundation::int32;
using ludus::foundation::uint8;
using ludus::foundation::usize;
namespace tst = ludus::containers::testing;

static_assert(std::is_constructible_v<Array<int32>, std::initializer_list<int32>>);
static_assert(std::is_constructible_v<Array<tst::MoveOnly>, std::initializer_list<tst::MoveOnly>> == false);

TEST_CASE("Array initializer lists supply elements rather than counts", "[array][initializer-list]")
{
    const Array<int32> direct{3, 7, 11};
    const Array<int32> copied = {3, 7, 11};
    REQUIRE(direct.GetSize() == 3);
    REQUIRE(direct[0] == 3);
    REQUIRE(direct[1] == 7);
    REQUIRE(direct[2] == 11);
    REQUIRE(copied == direct);

    const Array<int32> single{3};
    REQUIRE(single.GetSize() == 1);
    REQUIRE(single[0] == 3);
    const Array<int32> counted(3);
    REQUIRE(counted.GetSize() == 3);
    REQUIRE(counted[0] == 0);
    const Array<int32> filled(3, 7);
    REQUIRE(filled.GetSize() == 3);
    REQUIRE(filled[2] == 7);
}

TEST_CASE("Array empty initializer list preserves the empty representation", "[array][initializer-list]")
{
    const Array<int32> values(std::initializer_list<int32>{});
    REQUIRE(values.IsEmpty());
    REQUIRE(values.GetCapacity() == 0);
    REQUIRE(values.GetData() == nullptr);
}

TEST_CASE("Array initializer list copies outlive the backing list", "[array][initializer-list][lifetime]")
{
    tst::LifetimeLedger ledger;
    {
        Array<tst::Tracked> values = {tst::Tracked(&ledger, 7), tst::Tracked(&ledger, 11)};
        REQUIRE(ledger.CopyCtor == 2);
        REQUIRE(ledger.MoveCtor == 0);
        REQUIRE(ledger.Live() == 2);
        REQUIRE(values[0].Value() == 7);
        REQUIRE(values[1].Value() == 11);
        values.AddInPlace(&ledger, 13);
        REQUIRE(values[0].Value() == 7);
        REQUIRE(values[2].Value() == 13);
    }
    REQUIRE(ledger.Balanced());
}

TEST_CASE("Array initializer list accepts designated aggregate elements", "[array][initializer-list]")
{
    struct Entry
    {
        int32 Value;
    };
    const Array<Entry> entries = {{ .Value = 7 }, { .Value = 11 }};
    REQUIRE(entries.GetSize() == 2);
    REQUIRE(entries[0].Value == 7);
    REQUIRE(entries[1].Value == 11);
}

TEST_CASE("Array empty representation", "[array]")
{
    Array<int> v;
    REQUIRE(v.GetSize() == 0);
    REQUIRE(v.GetCapacity() == 0);
    REQUIRE(v.IsEmpty());
    REQUIRE(v.GetData() == nullptr);
    REQUIRE(v.begin() == v.end());
    REQUIRE(sizeof(Array<int>) == 3 * sizeof(void*)); // {ptr,size,capacity}
}

TEST_CASE("Array push_back grows and preserves order", "[array]")
{
    Array<int> v;
    for (int i = 0; i < 1000; ++i)
    {
        v.Add(i);
    }
    REQUIRE(v.GetSize() == 1000);
    REQUIRE(v.GetCapacity() >= 1000);
    for (int i = 0; i < 1000; ++i)
    {
        REQUIRE(v[static_cast<usize>(i)] == i);
    }
    REQUIRE(v.GetFirst() == 0);
    REQUIRE(v.GetLast() == 999);
}

TEST_CASE("Array size <= capacity invariant across growth", "[array]")
{
    Array<int> v;
    for (int i = 0; i < 500; ++i)
    {
        v.Add(i);
        REQUIRE(v.GetSize() <= v.GetCapacity());
    }
}

TEST_CASE("Array lifetime ledger balances (non-trivial type)", "[array][lifetime]")
{
    tst::LifetimeLedger led;
    {
        Array<tst::Tracked> v;
        for (int i = 0; i < 200; ++i)
        {
            v.AddInPlace(&led, i);
        }
        REQUIRE(v.GetSize() == 200);
        // Values survived every reallocation.
        for (int i = 0; i < 200; ++i)
        {
            REQUIRE(v[static_cast<usize>(i)].Value() == i);
        }
    }
    REQUIRE(led.Balanced());
    REQUIRE(led.Live() == 0);
}

TEST_CASE("Array clear destroys elements, keeps capacity", "[array][lifetime]")
{
    tst::LifetimeLedger led;
    Array<tst::Tracked> v;
    v.EnsureCapacity(16); // no reallocation during the pushes => exact dtor accounting
    for (int i = 0; i < 10; ++i)
    {
        v.AddInPlace(&led, i);
    }
    const usize cap = v.GetCapacity();
    const long liveBeforeClear = led.Live();
    REQUIRE(liveBeforeClear == 10);
    v.Clear();
    REQUIRE(v.GetSize() == 0);
    REQUIRE(v.GetCapacity() == cap); // capacity retained
    REQUIRE(led.Live() == 0);        // Clear destroyed exactly the 10 live elements
}

TEST_CASE("Array reserve allocates once and preserves elements", "[array]")
{
    Array<int> v;
    v.EnsureCapacity(128);
    REQUIRE(v.GetCapacity() >= 128);
    const int* base = v.GetData();
    for (int i = 0; i < 128; ++i)
    {
        v.Add(i);
    }
    REQUIRE(v.GetData() == base); // no reallocation within reserved capacity
    REQUIRE(v.GetSize() == 128);
}

TEST_CASE("Array resize up and down", "[array][lifetime]")
{
    tst::LifetimeLedger led;
    {
        Array<tst::Tracked> v;
        v.EnsureCapacity(4);
        v.AddInPlace(&led, 1);
        v.AddInPlace(&led, 2);
        // Resize down destroys the tail.
        v.Resize(1);
        REQUIRE(v.GetSize() == 1);
        REQUIRE(v[0].Value() == 1);
    }
    REQUIRE(led.Live() == 0);

    Array<int> vi;
    vi.Resize(5);
    REQUIRE(vi.GetSize() == 5);
    for (int x : vi)
    {
        REQUIRE(x == 0); // value-initialized
    }
    vi.Resize(8, 7);
    REQUIRE(vi.GetSize() == 8);
    REQUIRE(vi[7] == 7);
    REQUIRE(vi[4] == 0);
}

TEST_CASE("Array resize preserves aliased scalar fill", "[array][resize][alias]")
{
    const bool fallible = GENERATE(false, true);
    const usize capacity = GENERATE(usize{3}, usize{8});
    const usize fillIndex = GENERATE(usize{0}, usize{1}, usize{2});
    CAPTURE(fallible, capacity, fillIndex);

    Array<int32> values;
    values.EnsureCapacity(capacity);
    values.Add(11);
    values.Add(22);
    values.Add(33);
    const int32 expected = values[fillIndex];
    if (fallible)
    {
        REQUIRE(values.TryResize(6, values[fillIndex]));
    }
    else
    {
        values.Resize(6, values[fillIndex]);
    }

    REQUIRE(values.GetSize() == 6);
    REQUIRE(values.GetCapacity() == (capacity < 6 ? 6 : capacity));
    REQUIRE(values[0] == 11);
    REQUIRE(values[1] == 22);
    REQUIRE(values[2] == 33);
    for (usize index = 3; index < values.GetSize(); ++index)
    {
        REQUIRE(values[index] == expected);
    }
}

TEST_CASE("Array resize preserves aliased nontrivial fill and lifetimes", "[array][resize][alias][lifetime]")
{
    const bool fallible = GENERATE(false, true);
    const usize capacity = GENERATE(usize{3}, usize{8});
    const usize fillIndex = GENERATE(usize{0}, usize{1}, usize{2});
    CAPTURE(fallible, capacity, fillIndex);

    tst::LifetimeLedger ledger;
    {
        Array<tst::Tracked> values;
        values.EnsureCapacity(capacity);
        values.AddInPlace(&ledger, 11);
        values.AddInPlace(&ledger, 22);
        values.AddInPlace(&ledger, 33);
        const int32 expected = values[fillIndex].Value();
        if (fallible)
        {
            REQUIRE(values.TryResize(6, values[fillIndex]));
        }
        else
        {
            values.Resize(6, values[fillIndex]);
        }

        REQUIRE(values.GetSize() == 6);
        REQUIRE(ledger.Live() == 6);
        REQUIRE(values[0].Value() == 11);
        REQUIRE(values[1].Value() == 22);
        REQUIRE(values[2].Value() == 33);
        for (usize index = 3; index < values.GetSize(); ++index)
        {
            REQUIRE(values[index].Value() == expected);
            REQUIRE(values[index].Canary() == tst::Tracked::kAlive);
        }
    }
    REQUIRE(ledger.Balanced());
}

TEST_CASE("Array resize keeps over-aligned aliased fill valid", "[array][resize][alias][alignment]")
{
    const bool fallible = GENERATE(false, true);
    const usize capacity = GENERATE(usize{1}, usize{4});
    Array<tst::OverAligned> values;
    values.EnsureCapacity(capacity);
    values.AddInPlace(42);
    if (fallible)
    {
        REQUIRE(values.TryResize(3, values[0]));
    }
    else
    {
        values.Resize(3, values[0]);
    }
    REQUIRE(values.GetSize() == 3);
    for (const auto& value : values)
    {
        REQUIRE(value.Value == 42);
    }
    // The type's alignment is also checked by UBSan on every element access.
    REQUIRE(reinterpret_cast<usize>(values.GetData()) % alignof(tst::OverAligned) == 0);
}

TEST_CASE("Array resize does not consume aliased fill when no elements are added", "[array][resize][alias][lifetime]")
{
    const bool fallible = GENERATE(false, true);
    tst::LifetimeLedger ledger;
    {
        Array<tst::Tracked> values;
        values.EnsureCapacity(3);
        values.AddInPlace(&ledger, 11);
        values.AddInPlace(&ledger, 22);
        values.AddInPlace(&ledger, 33);
        const auto constructions = ledger.Constructions();
        if (fallible)
        {
            REQUIRE(values.TryResize(3, values[1]));
            REQUIRE(values.TryResize(1, values[2])); // Fill itself is destroyed by shrinking.
        }
        else
        {
            values.Resize(3, values[1]);
            values.Resize(1, values[2]);
        }
        REQUIRE(values.GetSize() == 1);
        REQUIRE(values[0].Value() == 11);
        REQUIRE(ledger.Live() == 1);
        REQUIRE(ledger.Constructions() == constructions);
        if (fallible)
        {
            REQUIRE(values.TryResize(0, values[0]));
        }
        else
        {
            values.Resize(0, values[0]);
        }
        REQUIRE(values.IsEmpty());
        REQUIRE(values.GetCapacity() == 3);
        REQUIRE(ledger.Balanced());
        REQUIRE(ledger.Constructions() == constructions);
    }
    REQUIRE(ledger.Balanced());
}

TEST_CASE("Array resize accepts external fill on empty and populated storage", "[array][resize][lifetime]")
{
    const bool fallible = GENERATE(false, true);
    tst::LifetimeLedger ledger;
    {
        const tst::Tracked fill(&ledger, 42);
        Array<tst::Tracked> values;
        for (const usize newSize : {usize{2}, usize{5}})
        {
            if (fallible)
            {
                REQUIRE(values.TryResize(newSize, fill));
            }
            else
            {
                values.Resize(newSize, fill);
            }
            REQUIRE(values.GetSize() == newSize);
            for (const auto& value : values)
            {
                REQUIRE(value.Value() == 42);
            }
        }
        REQUIRE(fill.Value() == 42);
        REQUIRE(ledger.Live() == 6); // Five elements and the external fill object.
    }
    REQUIRE(ledger.Balanced());
}

TEST_CASE("Array resize value-initializes the added tail", "[array][resize]")
{
    const bool fallible = GENERATE(false, true);
    const usize capacity = GENERATE(usize{1}, usize{4});
    Array<int32> values;
    values.EnsureCapacity(capacity);
    values.Add(42);
    if (fallible)
    {
        REQUIRE(values.TryResize(3));
    }
    else
    {
        values.Resize(3);
    }
    REQUIRE(values.GetSize() == 3);
    REQUIRE(values[0] == 42);
    REQUIRE(values[1] == 0);
    REQUIRE(values[2] == 0);
}

TEST_CASE("Array TryResize rejects overflow without consuming aliased fill", "[array][resize][alias][lifetime]")
{
    tst::LifetimeLedger ledger;
    {
        Array<tst::Tracked> values;
        values.EnsureCapacity(1);
        values.AddInPlace(&ledger, 42);
        const auto* original = values.GetData();
        const auto constructions = ledger.Constructions();
        REQUIRE_FALSE(values.TryResize(Array<tst::Tracked>::GetMaxSize() + 1, values[0]));
        REQUIRE(values.GetData() == original);
        REQUIRE(values.GetSize() == 1);
        REQUIRE(values.GetCapacity() == 1);
        REQUIRE(values[0].Value() == 42);
        REQUIRE(ledger.Constructions() == constructions);
        REQUIRE(ledger.Live() == 1);
    }
    REQUIRE(ledger.Balanced());
}

TEST_CASE("Array count/fill constructors", "[array]")
{
    Array<int> a(5);
    REQUIRE(a.GetSize() == 5);
    for (int x : a)
    {
        REQUIRE(x == 0);
    }
    Array<int> b(3, 9);
    REQUIRE(b.GetSize() == 3);
    REQUIRE(b[0] == 9);
    REQUIRE(b[2] == 9);
}

TEST_CASE("Array copy is deep and independent", "[array][lifetime]")
{
    tst::LifetimeLedger led;
    {
        Array<tst::Tracked> a;
        for (int i = 0; i < 5; ++i)
        {
            a.AddInPlace(&led, i);
        }
        Array<tst::Tracked> b = a;
        REQUIRE(b.GetSize() == 5);
        b[0].SetValue(999);
        REQUIRE(a[0].Value() == 0); // independent storage
        REQUIRE(b[0].Value() == 999);
    }
    REQUIRE(led.Live() == 0);
}

TEST_CASE("Array move leaves source empty and valid", "[array][lifetime]")
{
    tst::LifetimeLedger led;
    {
        Array<tst::Tracked> a;
        for (int i = 0; i < 5; ++i)
        {
            a.AddInPlace(&led, i);
        }
        const long ctorsBefore = led.Constructions();
        Array<tst::Tracked> b = std::move(a);
        REQUIRE(led.Constructions() == ctorsBefore); // move steals; no element moves
        REQUIRE(b.GetSize() == 5);
        REQUIRE(a.GetSize() == 0); // NOLINT: intentional moved-from inspection
        REQUIRE(a.GetCapacity() == 0);
        REQUIRE(a.GetData() == nullptr);
        a.Add(tst::Tracked(&led, 42)); // reuse moved-from
        REQUIRE(a.GetSize() == 1);
    }
    REQUIRE(led.Live() == 0);
}

TEST_CASE("Array self copy- and move-assignment are safe", "[array]")
{
    Array<int> v;
    for (int i = 0; i < 10; ++i)
    {
        v.Add(i);
    }
#if defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wself-assign-overloaded"
#    pragma clang diagnostic ignored "-Wself-move"
#endif
    v = v; // NOLINT: exercising the self-copy-assign guard on purpose
    REQUIRE(v.GetSize() == 10);
    REQUIRE(v[5] == 5);
    v = std::move(v); // NOLINT: exercising the self-move-assign guard on purpose
#if defined(__clang__)
#    pragma clang diagnostic pop
#endif
    REQUIRE(v.GetSize() == 10);
    REQUIRE(v[9] == 9);
}

TEST_CASE("Array move-only element type (UniquePtr-like)", "[array][lifetime]")
{
    tst::LifetimeLedger led;
    {
        Array<tst::MoveOnly> v;
        for (int i = 0; i < 50; ++i)
        {
            v.AddInPlace(&led, i);
        }
        REQUIRE(v.GetSize() == 50);
        REQUIRE(v[10].Value() == 10);
        Array<tst::MoveOnly> moved = std::move(v);
        REQUIRE(moved.GetSize() == 50);
    }
    REQUIRE(led.Live() == 0);
}

TEST_CASE("Array pop_back", "[array][lifetime]")
{
    tst::LifetimeLedger led;
    Array<tst::Tracked> v;
    v.AddInPlace(&led, 1);
    v.AddInPlace(&led, 2);
    v.RemoveLast();
    REQUIRE(v.GetSize() == 1);
    REQUIRE(v.GetLast().Value() == 1);
    v.RemoveLast();
    REQUIRE(v.IsEmpty());
    REQUIRE(led.Live() == 0);
}

TEST_CASE("Array insert/erase ordered", "[array]")
{
    Array<int> v;
    for (int i = 0; i < 5; ++i)
    {
        v.Add(i * 10); // 0 10 20 30 40
    }
    v.InsertAt(2, 99); // 0 10 99 20 30 40
    REQUIRE(v.GetSize() == 6);
    REQUIRE(v[2] == 99);
    REQUIRE(v[3] == 20);
    REQUIRE(v[5] == 40);

    v.RemoveAt(2); // back to 0 10 20 30 40
    REQUIRE(v.GetSize() == 5);
    REQUIRE(v[2] == 20);

    v.InsertAt(0, -1); // -1 0 10 20 30 40
    REQUIRE(v.GetFirst() == -1);
    v.InsertAt(v.GetSize(), 100); // append via insert-at-end
    REQUIRE(v.GetLast() == 100);
}

TEST_CASE("Array insert/erase with lifetime tracking", "[array][lifetime]")
{
    tst::LifetimeLedger led;
    {
        Array<tst::Tracked> v;
        for (int i = 0; i < 6; ++i)
        {
            v.AddInPlace(&led, i);
        }
        v.InsertAtInPlace(3, &led, 100);
        REQUIRE(v[3].Value() == 100);
        REQUIRE(v[4].Value() == 3);
        v.RemoveAt(0);
        REQUIRE(v[0].Value() == 1);
        v.RemoveRange(1, 3);
        REQUIRE(v.GetSize() == 4);
    }
    REQUIRE(led.Live() == 0);
}

TEST_CASE("Array erase range full and partial", "[array]")
{
    Array<int> v;
    for (int i = 0; i < 10; ++i)
    {
        v.Add(i);
    }
    v.RemoveRange(2, 5); // remove 2,3,4
    REQUIRE(v.GetSize() == 7);
    REQUIRE(v[2] == 5);
    v.RemoveRange(0, v.GetSize()); // clear via range
    REQUIRE(v.IsEmpty());
}

TEST_CASE("Array EraseUnordered is O(1) swap-with-last", "[array]")
{
    Array<int> v;
    for (int i = 0; i < 5; ++i)
    {
        v.Add(i); // 0 1 2 3 4
    }
    v.RemoveAtSwap(1); // last (4) moves into slot 1 -> 0 4 2 3
    REQUIRE(v.GetSize() == 4);
    REQUIRE(v[1] == 4);
    v.RemoveAtSwap(3); // erase last element directly
    REQUIRE(v.GetSize() == 3);
    REQUIRE(v[0] == 0);
}

TEST_CASE("Array append from span (bulk)", "[array][interop]")
{
    Array<int> v;
    int src[] = {1, 2, 3, 4, 5};
    v.AddRange(std::span<const int>(src, 5));
    REQUIRE(v.GetSize() == 5);
    v.AddRange(std::span<const int>(src, 5));
    REQUIRE(v.GetSize() == 10);
    REQUIRE(v[9] == 5);
    // Append empty is a no-op.
    v.AddRange(std::span<const int>());
    REQUIRE(v.GetSize() == 10);
}

TEST_CASE("Array From factory", "[array]")
{
    int src[] = {9, 8, 7};
    Array<int> v = Array<int>::FromRange(std::span<const int>(src, 3));
    REQUIRE(v.GetSize() == 3);
    REQUIRE(v[0] == 9);
    REQUIRE(v[2] == 7);
}

TEST_CASE("Array swap", "[array]")
{
    Array<int> a;
    a.Add(1);
    a.Add(2);
    Array<int> b;
    b.Add(9);
    a.Swap(b);
    REQUIRE(a.GetSize() == 1);
    REQUIRE(a[0] == 9);
    REQUIRE(b.GetSize() == 2);
    REQUIRE(b[1] == 2);
}

TEST_CASE("Array shrink_to_fit", "[array]")
{
    Array<int> v;
    v.EnsureCapacity(100);
    v.Add(1);
    v.Add(2);
    REQUIRE(v.GetCapacity() >= 100);
    v.TrimCapacity();
    REQUIRE(v.GetCapacity() == 2);
    REQUIRE(v.GetSize() == 2);
    REQUIRE(v[0] == 1);
    v.Clear();
    v.TrimCapacity();
    REQUIRE(v.GetCapacity() == 0);
    REQUIRE(v.GetData() == nullptr);
}

TEST_CASE("Array std interop: ranges, algorithms, span, pointer+count", "[array][interop]")
{
    Array<int> v;
    for (int i = 5; i >= 1; --i)
    {
        v.Add(i);
    }
    std::sort(v.begin(), v.end());
    REQUIRE(v[0] == 1);
    REQUIRE(std::is_sorted(v.begin(), v.end()));
    REQUIRE(std::accumulate(v.begin(), v.end(), 0) == 15);
    REQUIRE(std::ranges::find(v, 3) != v.end());

    // range-for
    int sum = 0;
    for (int x : v)
    {
        sum += x;
    }
    REQUIRE(sum == 15);

    // std::span conversion (GPU/C pointer+count idiom)
    std::span<const int> s = v;
    REQUIRE(s.size() == 5);
    REQUIRE(s.data() == v.GetData());
}

TEST_CASE("Array over-aligned elements", "[array][alignment]")
{
    Array<tst::OverAligned> v;
    for (int i = 0; i < 20; ++i)
    {
        v.AddInPlace(i);
    }
    REQUIRE(reinterpret_cast<std::uintptr_t>(v.GetData()) % 128 == 0);
    REQUIRE(v[19].Value == 19);
}

TEST_CASE("Array empty element type", "[array]")
{
    Array<tst::Empty> v;
    for (int i = 0; i < 10; ++i)
    {
        v.AddInPlace();
    }
    REQUIRE(v.GetSize() == 10);
    v.RemoveLast();
    REQUIRE(v.GetSize() == 9);
}

TEST_CASE("Array equality", "[array]")
{
    Array<int> a;
    Array<int> b;
    a.Add(1);
    a.Add(2);
    b.Add(1);
    b.Add(2);
    REQUIRE(a == b);
    b.Add(3);
    REQUIRE_FALSE(a == b);
}

TEST_CASE("Array zero and one element edge cases", "[array]")
{
    Array<int> v;
    REQUIRE(v.TryEnsureCapacity(0));
    REQUIRE(v.GetCapacity() == 0);
    v.Add(42);
    REQUIRE(v.GetSize() == 1);
    REQUIRE(v.GetFirst() == v.GetLast());
    v.RemoveLast();
    REQUIRE(v.IsEmpty());
}

TEST_CASE("Array GetMaxSize overflow defense (fallible)", "[array][overflow]")
{
    Array<int> v;
    // A request beyond MaxSize must fail gracefully, not wrap/allocate tiny.
    REQUIRE_FALSE(v.TryEnsureCapacity(Array<int>::GetMaxSize() + 1));
    REQUIRE(v.GetSize() == 0);
    REQUIRE(v.GetCapacity() == 0);
}

// ---------------------------------------------------------------------------
// Regression tests for the self-referential insertion bug found in audit
// (heap-use-after-free / silent wrong value when an argument aliased an element
// and the operation reallocated or shifted). The standard vector guarantees these are
// well-defined; Ludus::Array must match.
// ---------------------------------------------------------------------------

TEST_CASE("Array self-referential Add triggering reallocation", "[array][regression][aliasing]")
{
    Array<int> v;
    v.EnsureCapacity(4);
    v.Add(10);
    v.Add(20);
    v.Add(30);
    v.Add(40);   // size == capacity == 4
    v.Add(v[0]); // arg aliases storage that reallocation frees
    REQUIRE(v.GetSize() == 5);
    REQUIRE(v[4] == 10);
    // Also v.GetLast() self-reference across several growths.
    for (int i = 0; i < 100; ++i)
    {
        v.Add(v.GetLast());
    }
    REQUIRE(v.GetLast() == 10);
}

TEST_CASE("Array self-referential AddInPlace triggering reallocation", "[array][regression][aliasing]")
{
    Array<int> v;
    v.EnsureCapacity(2);
    v.Add(7);
    v.Add(8);           // full
    v.AddInPlace(v[1]); // realloc happens; arg aliases v[1]
    REQUIRE(v.GetSize() == 3);
    REQUIRE(v[2] == 8);
}

TEST_CASE("Array self-referential InsertAtAtInPlace without reallocation", "[array][regression][aliasing]")
{
    Array<int> v;
    for (int i = 0; i < 6; ++i)
    {
        v.Add(i * 10); // 0 10 20 30 40 50
    }
    const int expected = v[5];  // 50
    v.InsertAtInPlace(1, v[5]); // v[5] is shifted during the op
    REQUIRE(v.GetSize() == 7);
    REQUIRE(v[1] == expected);
    REQUIRE(v[2] == 10); // original v[1] shifted right
}

TEST_CASE("Array self-referential InsertAtAtInPlace triggering reallocation", "[array][regression][aliasing]")
{
    Array<int> v;
    v.EnsureCapacity(4);
    v.Add(1);
    v.Add(2);
    v.Add(3);
    v.Add(4); // full
    const int expected = v[3];
    v.InsertAtInPlace(1, v[3]); // realloc + insert of a self-reference
    REQUIRE(v.GetSize() == 5);
    REQUIRE(v[1] == expected);
}

TEST_CASE("Array self-referential InsertAt of a shifted element", "[array][regression][aliasing]")
{
    Array<int> v;
    v.EnsureCapacity(8);
    for (int i = 0; i < 5; ++i)
    {
        v.Add(i); // 0 1 2 3 4
    }
    v.InsertAt(1, v[2]); // v[2] (==2) is shifted right during Insert
    REQUIRE(v[1] == 2);
}

TEST_CASE("Array self-referential Add of non-trivial type across growth", "[array][regression][aliasing][lifetime]")
{
    tst::LifetimeLedger led;
    {
        Array<tst::Tracked> v;
        v.EnsureCapacity(2);
        v.AddInPlace(&led, 1);
        v.AddInPlace(&led, 2); // full
        // Self-referential copy of an existing element while reallocating. The
        // Tracked canary + ASan would catch a use-after-free of the old block.
        v.Add(v[0]);
        REQUIRE(v.GetSize() == 3);
        REQUIRE(v[2].Value() == 1);
        // Force several more growths with self-references.
        for (int i = 0; i < 50; ++i)
        {
            v.Add(v.GetLast());
        }
        REQUIRE(v.GetLast().Value() == 1);
    }
    REQUIRE(led.Live() == 0); // no leak / no double-destroy
}

TEST_CASE("Array self-referential AddInPlace of move-eligible relocatable across growth",
          "[array][regression][aliasing]")
{
    // Relocatable opts into memcpy relocation; the new element must still be
    // constructed from the aliased source before the old block is freed.
    tst::LifetimeLedger led;
    Array<tst::Relocatable> v;
    v.EnsureCapacity(2);
    v.AddInPlace(&led, 100);
    v.AddInPlace(&led, 200); // full
    v.Add(v[1]);             // copy of v[1] while reallocating (memcpy path)
    REQUIRE(v.GetSize() == 3);
    REQUIRE(v[2].Value == 200);
}

namespace
{
template <typename T>
concept HasCopyFillResize = requires(Array<T>& values, const T& value) {
    values.TryResize(2, value);
    values.Resize(2, value);
};
static_assert(HasCopyFillResize<int32>);
static_assert(HasCopyFillResize<tst::MoveOnly> == false);
} // namespace
TEST_CASE("Array value resize preserves move-only elements and rejects overflow", "[array][resize][move-only]")
{
    const bool fallible = GENERATE(false, true);
    const usize capacity = GENERATE(usize{1}, usize{8});
    tst::LifetimeLedger ledger;
    {
        Array<tst::MoveOnly> values;
        values.EnsureCapacity(capacity);
        values.Add(tst::MoveOnly{&ledger, 41});
        if (fallible)
        {
            REQUIRE(values.TryResize(4));
        }
        else
        {
            values.Resize(4);
        }
        REQUIRE(values.GetSize() == 4);
        CHECK(values[0].Value() == 41);
        CHECK(values[3].Value() == 0);
        const auto* pointer = values.GetData();
        REQUIRE_FALSE(values.TryResize(Array<tst::MoveOnly>::GetMaxSize() + 1));
        CHECK(values.GetData() == pointer);
        CHECK(values.GetSize() == 4);
        CHECK(values[0].Value() == 41);
        REQUIRE(values.TryResize(1));
        CHECK(values[0].Value() == 41);
        REQUIRE(values.TryResize(0));
    }
    CHECK(ledger.Balanced());
}

TEST_CASE("TryAddRange grows geometrically so repeated small appends stay linear", "[containers][array]")
{
    // Regression: exact-fit growth made byte writers quadratic (editor scene cook hung on macOS).
    Array<uint8> bytes;
    constexpr usize APPEND_COUNT = 1U << 20U;
    constexpr usize WORD_BYTES = 4;
    const ludus::foundation::StaticArray<uint8, WORD_BYTES> word{1, 2, 3, 4};
    usize reallocations = 0;
    const uint8* previousOrNull = bytes.GetData();
    bool isAppended = true;
    for (usize index = 0; index < APPEND_COUNT; ++index)
    {
        isAppended =
            bytes.TryAddRange(std::span<const uint8>(word.GetData(), word.GetSize())) == true && isAppended == true;
        if (bytes.GetData() != previousOrNull)
        {
            ++reallocations;
            previousOrNull = bytes.GetData();
        }
    }
    REQUIRE(isAppended == true);
    CHECK(bytes.GetSize() == APPEND_COUNT * WORD_BYTES);
    CHECK(bytes.GetCapacity() >= bytes.GetSize());
    // Doubling reaches 4 MiB in about 22 steps; exact-fit growth would reallocate a million times.
    CHECK(reallocations < 64);
}
