// Unit tests for Ludus::StaticArray<T, N>.

#include "lifetime_type.hpp"

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <numeric>
#include <type_traits>

using ludus::foundation::BorrowedView;
using ludus::foundation::StaticArray;
using ludus::foundation::usize;
namespace tst = ludus::containers::testing;

TEST_CASE("StaticArray layout matches a raw C array", "[static_array]")
{
    STATIC_REQUIRE(sizeof(StaticArray<int, 4>) == sizeof(int[4]));
    STATIC_REQUIRE(alignof(StaticArray<int, 4>) == alignof(int[4]));
    STATIC_REQUIRE(std::is_trivially_copyable_v<StaticArray<int, 4>>);
    STATIC_REQUIRE(std::is_trivially_destructible_v<StaticArray<int, 4>>);
    STATIC_REQUIRE(alignof(StaticArray<tst::OverAligned, 2>) == 128);
}

TEST_CASE("StaticArray aggregate init and access", "[static_array]")
{
    StaticArray<int, 3> a{1, 2, 3};
    REQUIRE(a.GetSize() == 3);
    REQUIRE(a.GetCapacity() == 3);
    REQUIRE_FALSE(a.IsEmpty());
    REQUIRE(a[0] == 1);
    REQUIRE(a[2] == 3);
    REQUIRE(a.GetFirst() == 1);
    REQUIRE(a.GetLast() == 3);
    a[1] = 20;
    REQUIRE(a[1] == 20);
}

TEST_CASE("StaticArray constexpr behavior", "[static_array]")
{
    constexpr StaticArray<int, 4> a{5, 6, 7, 8};
    STATIC_REQUIRE(a.GetSize() == 4);
    STATIC_REQUIRE(a[0] == 5);
    STATIC_REQUIRE(a.GetLast() == 8);
    STATIC_REQUIRE(a.GetFirst() == 5);
    constexpr int sum = [] {
        StaticArray<int, 3> v{1, 2, 3};
        int s = 0;
        for (int x : v)
        {
            s += x;
        }
        return s;
    }();
    STATIC_REQUIRE(sum == 6);
}

TEST_CASE("StaticArray zero-size handling", "[static_array]")
{
    StaticArray<int, 0> a{};
    REQUIRE(a.GetSize() == 0);
    REQUIRE(a.IsEmpty());
    REQUIRE(a.begin() == a.end());
    const BorrowedView<int> view = a;
    REQUIRE(view.IsEmpty());
    REQUIRE(view.IsValid());
    STATIC_REQUIRE(sizeof(StaticArray<int, 0>) >= 1); // empty class still has size >= 1
}

TEST_CASE("StaticArray iterators and std algorithms", "[static_array][interop]")
{
    StaticArray<int, 5> a{5, 3, 1, 4, 2};
    std::sort(a.begin(), a.end());
    REQUIRE(a[0] == 1);
    REQUIRE(a[4] == 5);
    const int total = std::accumulate(a.begin(), a.end(), 0);
    REQUIRE(total == 15);
    REQUIRE(std::find(a.begin(), a.end(), 4) != a.end());
}

TEST_CASE("StaticArray borrowed views and GetData", "[static_array][interop]")
{
    StaticArray<int, 3> a{7, 8, 9};
    int* d = a.GetData();
    REQUIRE(d == &a[0]);
    const BorrowedView<int> mutableView = a.AsView();
    REQUIRE(mutableView.GetSize() == 3);
    mutableView[0] = 70;
    REQUIRE(a[0] == 70);

    const StaticArray<int, 3>& cref = a;
    const BorrowedView<const int> constView = cref;
    REQUIRE(constView.GetSize() == 3);
    REQUIRE(constView[2] == 9);
}

TEST_CASE("StaticArray Fill and Swap", "[static_array]")
{
    StaticArray<int, 4> a{0, 0, 0, 0};
    a.Fill(9);
    REQUIRE(a[0] == 9);
    REQUIRE(a[3] == 9);

    StaticArray<int, 3> x{1, 2, 3};
    StaticArray<int, 3> y{4, 5, 6};
    x.Swap(y);
    REQUIRE(x[0] == 4);
    REQUIRE(y[0] == 1);
}

TEST_CASE("StaticArray comparison operators", "[static_array]")
{
    StaticArray<int, 3> a{1, 2, 3};
    StaticArray<int, 3> b{1, 2, 3};
    StaticArray<int, 3> c{1, 2, 4};
    REQUIRE(a == b);
    REQUIRE(a != c);
    REQUIRE(a < c);
}

TEST_CASE("StaticArray over-aligned element", "[static_array][alignment]")
{
    StaticArray<tst::OverAligned, 3> a{};
    REQUIRE(reinterpret_cast<std::uintptr_t>(a.GetData()) % 128 == 0);
    a[1].Value = 42;
    REQUIRE(a[1].Value == 42);
}

TEST_CASE("StaticArray deduction guide", "[static_array]")
{
    // CTAD works on the class template (the alias cannot deduce).
    ludus::foundation::core::StaticArray deduced{1, 2, 3, 4};
    STATIC_REQUIRE(std::is_same_v<decltype(deduced), ludus::foundation::core::StaticArray<int, 4>>);
    REQUIRE(deduced.GetSize() == 4);
}
