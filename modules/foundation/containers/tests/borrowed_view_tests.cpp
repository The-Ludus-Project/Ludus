
#include <catch2/catch_test_macros.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>

#include <type_traits>

using namespace ludus::foundation;

TEST_CASE("Borrowed views preserve extent and distinguish invalid input from absence", "[containers]")
{
    const BorrowedView<const uint32> empty;
    CHECK(empty.IsValid() == true);
    CHECK(empty.IsEmpty() == true);
    CHECK(empty.GetDataOrNull() == nullptr);
    const BorrowedView<const uint32> invalid{nullptr, 1};
    CHECK(invalid.IsValid() == false);
    CHECK(invalid.IsEmpty() == false);
    CHECK(invalid.GetSize() == 1);
    uint32 elements[]{7, 9};
    const BorrowedView<uint32> writable{elements};
    writable[1] = 11;
    const BorrowedView<const uint32> readable{elements};
    CHECK(readable.GetSize() == 2);
    CHECK(readable.GetDataOrNull() == elements);
    CHECK(readable[1] == 11);
    const BorrowedView<const uint32> copied = readable;
    CHECK(copied.GetDataOrNull() == elements);
}

TEST_CASE("Borrowed views iterate empty and const storage without copying", "[containers]")
{
    const BorrowedView<const uint32> empty;
    CHECK(empty.begin() == nullptr);
    CHECK(empty.end() == nullptr);
    uint32 values[]{7, 9};
    const BorrowedView<uint32> writable{values};
    for (uint32& value : writable)
    {
        value += 1;
    }
    const BorrowedView<const uint32> readable{values};
    CHECK(readable.begin() == values);
    CHECK(readable.end() == values + readable.GetSize());
    uint32 sum = 0;
    for (const uint32 value : readable)
    {
        sum += value;
    }
    CHECK(sum == 18);
    const BorrowedView<const uint32> positionedEmpty{values, 0};
    CHECK(positionedEmpty.begin() == values);
    CHECK(positionedEmpty.end() == values);
}

TEST_CASE("Mutable borrowed views convert to read-only views of the same extent", "[containers]")
{
    STATIC_REQUIRE(std::is_convertible_v<BorrowedView<uint32>, BorrowedView<const uint32>>);
    STATIC_REQUIRE(std::is_convertible_v<BorrowedView<const uint32>, BorrowedView<uint32>> == false);
    STATIC_REQUIRE(std::is_convertible_v<BorrowedView<uint32>, BorrowedView<const uint64>> == false);
    uint32 values[]{3, 5};
    const BorrowedView<uint32> writable{values};
    const BorrowedView<const uint32> readable = writable;
    CHECK(readable.GetDataOrNull() == values);
    CHECK(readable.GetSize() == 2);
    const BorrowedView<uint32> invalid{nullptr, 1};
    const BorrowedView<const uint32> stillInvalid = invalid;
    CHECK(stillInvalid.IsValid() == false);
    CHECK(stillInvalid.GetSize() == 1);
}
