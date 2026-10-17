// Allocation-count tests: verify Array's allocation behaviour precisely.
// We replace global operator new/delete with counting versions. This TU is built
// into its own executable (see CMakeLists) and only when sanitizers are OFF (the
// sanitizer runtimes provide their own operator new/delete).

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/fixed_array.hpp>
#include <ludus/foundation/containers/ring_buffer.hpp>
#include <ludus/foundation/containers/sorted_map.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/containers/static_bit_set.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <new>

namespace
{
struct AllocCounters
{
    long news = 0;
    long deletes = 0;
    std::size_t bytes = 0;
};
AllocCounters gCounters;
bool gTracking = false;
bool gRejectNothrowAllocation = false;

void Reset() noexcept
{
    gCounters = AllocCounters{};
}
} // namespace

// Global operator new/delete replacements routed through aligned malloc/free and
// counted when tracking is on. Signatures match the standard library exactly.
// The container allocates via the aligned nothrow new and frees via sized aligned
// delete; we count both. A helper does the aligned allocation for all overloads.
namespace
{
void* AlignedAlloc(std::size_t n, std::size_t al) noexcept
{
    if (al < alignof(std::max_align_t))
    {
        al = alignof(std::max_align_t);
    }
    std::size_t rounded = (n + al - 1) & ~(al - 1);
    if (rounded == 0)
    {
        rounded = al;
    }
    return std::aligned_alloc(al, rounded);
}
void CountNew(std::size_t n) noexcept
{
    if (gTracking)
    {
        ++gCounters.news;
        gCounters.bytes += n;
    }
}
void CountDelete(void* p) noexcept
{
    if (p != nullptr && gTracking)
    {
        ++gCounters.deletes;
    }
}
} // namespace

// libc++ names the replaceable allocation functions' parameters with reserved
// identifiers (`__sz`), so these replacements cannot match its declarations.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t n)
{
    CountNew(n);
    void* p = AlignedAlloc(n, alignof(std::max_align_t));
    if (p == nullptr)
    {
        throw std::bad_alloc{};
    }
    return p;
}
void* operator new(std::size_t n, std::align_val_t al)
{
    CountNew(n);
    void* p = AlignedAlloc(n, static_cast<std::size_t>(al));
    if (p == nullptr)
    {
        throw std::bad_alloc{};
    }
    return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
    CountNew(n);
    return gRejectNothrowAllocation ? nullptr : AlignedAlloc(n, alignof(std::max_align_t));
}
void* operator new(std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept
{
    CountNew(n);
    return gRejectNothrowAllocation ? nullptr : AlignedAlloc(n, static_cast<std::size_t>(al));
}
void operator delete(void* p) noexcept
{
    CountDelete(p);
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept
{
    CountDelete(p);
    std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept
{
    CountDelete(p);
    std::free(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept
{
    CountDelete(p);
    std::free(p);
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name)

using ludus::foundation::Array;
using ludus::foundation::StaticArray;

namespace
{
struct TrackGuard
{
    TrackGuard()
    {
        Reset();
        gTracking = true;
    }
    ~TrackGuard()
    {
        gTracking = false;
    }
};
} // namespace

TEST_CASE("Default-constructed Array never allocates", "[alloc]")
{
    TrackGuard g;
    {
        Array<int> v;
        (void)v.IsEmpty();
    }
    REQUIRE(gCounters.news == 0);
    REQUIRE(gCounters.deletes == 0);
}

TEST_CASE("StaticArray never allocates", "[alloc]")
{
    TrackGuard g;
    {
        StaticArray<int, 32> a{};
        a.Fill(7);
        volatile int sink = a[0];
        (void)sink;
    }
    REQUIRE(gCounters.news == 0);
}

TEST_CASE("FixedArray never allocates, including copies, moves and insertion", "[alloc][fixed-array]")
{
    TrackGuard guard;
    {
        ludus::foundation::FixedArray<ludus::foundation::uint64, 64> values;
        for (ludus::foundation::uint64 i = 0; i < 64; ++i)
        {
            REQUIRE(values.TryInsertAt(0, i) != nullptr);
        }
        REQUIRE(values.TryAdd(1U) == false);
        ludus::foundation::FixedArray<ludus::foundation::uint64, 64> copy = values;
        ludus::foundation::FixedArray<ludus::foundation::uint64, 64> moved(static_cast<decltype(copy)&&>(copy));
        values.RemoveAt(3);
        values.Clear();
        REQUIRE(moved.GetSize() == 64);
    }
    REQUIRE(gCounters.news == 0);
    REQUIRE(gCounters.deletes == 0);
}

TEST_CASE("RingBuffer never allocates, including wraparound, copies and moves", "[alloc][ring-buffer]")
{
    TrackGuard guard;
    {
        ludus::foundation::RingBuffer<ludus::foundation::uint64, 16> ring;
        for (ludus::foundation::uint64 i = 0; i < 1000; ++i)
        {
            if (ring.IsFull() == true)
            {
                ring.RemoveFirst();
            }
            REQUIRE(ring.TryAdd(i));
        }
        ludus::foundation::RingBuffer<ludus::foundation::uint64, 16> copy = ring;
        ludus::foundation::RingBuffer<ludus::foundation::uint64, 16> moved(static_cast<decltype(copy)&&>(copy));
        ring.Clear();
        REQUIRE(moved.GetSize() == 16);
        REQUIRE(moved.GetFirst() == 984U);
    }
    REQUIRE(gCounters.news == 0);
    REQUIRE(gCounters.deletes == 0);
}

TEST_CASE("StaticBitSet never allocates", "[alloc][bit-set]")
{
    TrackGuard guard;
    {
        ludus::foundation::StaticBitSet<200> bits;
        bits.SetAll();
        bits <<= 65;
        bits.Flip(3);
        const ludus::foundation::StaticBitSet<200> copy = ~bits;
        REQUIRE((copy | bits).AreAllSet());
        REQUIRE(bits.FindNextSet(0) == 3);
    }
    REQUIRE(gCounters.news == 0);
    REQUIRE(gCounters.deletes == 0);
}

TEST_CASE("Array initializer list allocates exactly its element storage", "[alloc][initializer-list]")
{
    TrackGuard guard;
    {
        Array<ludus::foundation::int32> values{3, 7, 11};
        REQUIRE(values.GetSize() == 3);
        REQUIRE(gCounters.news == 1);
        REQUIRE(gCounters.bytes == 3 * sizeof(ludus::foundation::int32));
    }
    REQUIRE(gCounters.deletes == 1);
}

TEST_CASE("Empty Array initializer list never allocates", "[alloc][initializer-list]")
{
    TrackGuard guard;
    {
        Array<int> values(std::initializer_list<int>{});
        (void)values.IsEmpty();
    }
    REQUIRE(gCounters.news == 0);
    REQUIRE(gCounters.deletes == 0);
}

TEST_CASE("EnsureCapacity(n) + n Adds allocates exactly once", "[alloc]")
{
    TrackGuard g;
    {
        Array<int> v;
        v.EnsureCapacity(256);
        for (int i = 0; i < 256; ++i)
        {
            v.Add(i);
        }
        REQUIRE(gCounters.news == 1);
    }
    REQUIRE(gCounters.news == 1);
    REQUIRE(gCounters.deletes == 1); // symmetric free on destruction
}

TEST_CASE("Clear does not free or allocate", "[alloc]")
{
    Array<int> v;
    v.EnsureCapacity(64);
    for (int i = 0; i < 64; ++i)
    {
        v.Add(i);
    }
    {
        TrackGuard g;
        v.Clear();
        REQUIRE(gCounters.news == 0);
        REQUIRE(gCounters.deletes == 0);
    }
    REQUIRE(v.GetCapacity() >= 64);
}

TEST_CASE("Growth reallocates a bounded number of times", "[alloc]")
{
    TrackGuard g;
    {
        Array<int> v;
        for (int i = 0; i < 10000; ++i)
        {
            v.Add(i);
        }
        // Geometric growth => O(log n) reallocations, far fewer than n.
        REQUIRE(gCounters.news < 40);
        REQUIRE(gCounters.news > 1);
    }
}

TEST_CASE("Allocation/deallocation symmetry after churn", "[alloc]")
{
    TrackGuard g;
    {
        Array<int> v;
        for (int i = 0; i < 1000; ++i)
        {
            v.Add(i);
        }
        v.TrimCapacity();
        Array<int> moved = std::move(v);
        moved.Clear();
        moved.TrimCapacity();
    }
    REQUIRE(gCounters.news == gCounters.deletes);
}

namespace
{
struct RejectNothrowAllocation
{
    RejectNothrowAllocation() noexcept
    {
        gRejectNothrowAllocation = true;
    }
    ~RejectNothrowAllocation()
    {
        gRejectNothrowAllocation = false;
    }
};
struct MoveMarker
{
    ludus::foundation::int32 Value;
    explicit MoveMarker(ludus::foundation::int32 value) noexcept : Value(value) {}
    MoveMarker(const MoveMarker&) = delete;
    MoveMarker& operator=(const MoveMarker&) = delete;
    MoveMarker(MoveMarker&& other) noexcept : Value(other.Value)
    {
        other.Value = -1;
    }
    MoveMarker& operator=(MoveMarker&& other) noexcept
    {
        if (this != &other)
        {
            Value = other.Value;
            other.Value = -1;
        }
        return *this;
    }
};
} // namespace

TEST_CASE("Fallible ordered insertion allocates before consuming aliased rvalues", "[alloc][insert][oom]")
{
    Array<MoveMarker> values;
    values.EnsureCapacity(1);
    values.AddInPlace(17);
    auto* original = values.GetData();
    MoveMarker* result = nullptr;
    {
        RejectNothrowAllocation reject;
        result = values.TryInsertAt(0, static_cast<MoveMarker&&>(values[0]));
    }
    REQUIRE(result == nullptr);
    REQUIRE(values.GetData() == original);
    REQUIRE(values.GetCapacity() == 1);
    REQUIRE(values.GetSize() == 1);
    REQUIRE(values[0].Value == 17);
    {
        RejectNothrowAllocation reject;
        result = values.TryAddInPlace(static_cast<MoveMarker&&>(values[0]));
    }
    REQUIRE(result == nullptr);
    REQUIRE(values.GetData() == original);
    REQUIRE(values.GetSize() == 1);
    REQUIRE(values[0].Value == 17);
}

TEST_CASE("SortedMap allocation failure and duplicates preserve move-only arguments", "[alloc][sorted-map][oom]")
{
    ludus::foundation::SortedMap<ludus::foundation::uint32, MoveMarker> map;
    map.EnsureCapacity(1);
    map.AddInPlace(10, 17);
    MoveMarker supplied(99);
    decltype(map)::InsertResult result;
    MoveMarker* original = map.Find(10);
    {
        RejectNothrowAllocation reject;
        result = map.TryAddInPlace(5, static_cast<MoveMarker&&>(*original));
    }
    REQUIRE(result.Value == nullptr);
    REQUIRE_FALSE(result.Inserted);
    REQUIRE(map.GetCapacity() == 1);
    REQUIRE(map.GetSize() == 1);
    REQUIRE(map.Find(10) == original);
    REQUIRE(original->Value == 17);
    {
        RejectNothrowAllocation reject;
        result = map.TryAddInPlace(5, static_cast<MoveMarker&&>(supplied));
    }
    REQUIRE(result.Value == nullptr);
    REQUIRE(supplied.Value == 99);
    {
        RejectNothrowAllocation reject;
        result = map.TryAddInPlace(10, static_cast<MoveMarker&&>(supplied));
    }
    REQUIRE(result.Value == original);
    REQUIRE_FALSE(result.Inserted);
    REQUIRE(supplied.Value == 99);
    REQUIRE_FALSE(map.TryEnsureCapacity(static_cast<ludus::foundation::usize>(-1)));
    REQUIRE(map.Find(10) == original);
}

TEST_CASE("SortedMap reserve, lookup and reuse avoid per-entry allocation", "[alloc][sorted-map]")
{
    ludus::foundation::SortedMap<ludus::foundation::uint32, ludus::foundation::uint32> map;
    Reset();
    gTracking = true;
    map.EnsureCapacity(16);
    const long allocationsAfterReserve = gCounters.news;
    for (ludus::foundation::uint32 key = 16; key != 0; --key)
    {
        map.AddInPlace(key, key);
        (void)map.Find(key);
    }
    map.Clear();
    map.AddInPlace(7, 7U);
    const long allocationsAfterReuse = gCounters.news;
    map.Reset();
    const long frees = gCounters.deletes;
    gTracking = false;
    REQUIRE(allocationsAfterReserve == 1);
    REQUIRE(allocationsAfterReuse == 1);
    REQUIRE(frees == 1);
}
