// The container headers are span-free; span_compatibility.hpp adapts them to
// the std::span APIs that ADR 0003 still keeps for compatibility.

#include <catch2/catch_test_macros.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/fixed_array.hpp>
#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <span>
#include <type_traits>
#include <utility>

using ludus::foundation::Array;
using ludus::foundation::BorrowedView;
using ludus::foundation::FixedArray;
using ludus::foundation::MakeBorrowedView;
using ludus::foundation::MakeSpan;
using ludus::foundation::StaticArray;
using ludus::foundation::uint32;
using ludus::foundation::uint8;
using ludus::foundation::usize;

namespace
{
constexpr usize SMALL_CAPACITY = 4;

template <typename ContainerType>
concept CanMakeSpanFromTemporary = requires { MakeSpan(std::declval<ContainerType>()); };

[[nodiscard]] usize SumBytes(std::span<const uint8> bytes) noexcept
{
    usize sum = 0;
    for (const uint8 byte : bytes)
    {
        sum += byte;
    }
    return sum;
}
} // namespace

TEST_CASE("MakeSpan borrows the live elements of contiguous containers", "[containers][span-compatibility]")
{
    Array<uint8> bytes;
    bytes.Add(1);
    bytes.Add(2);
    const std::span<uint8> mutableSpan = MakeSpan(bytes);
    REQUIRE(mutableSpan.data() == bytes.GetData());
    REQUIRE(mutableSpan.size() == 2);
    mutableSpan[0] = 3;
    REQUIRE(bytes[0] == 3);
    const Array<uint8>& readOnly = bytes;
    STATIC_REQUIRE(std::is_same_v<decltype(MakeSpan(readOnly)), std::span<const uint8>>);
    REQUIRE(SumBytes(MakeSpan(readOnly)) == 5);

    StaticArray<uint32, SMALL_CAPACITY> words{1, 2, 3, 4};
    REQUIRE(MakeSpan(words).size() == SMALL_CAPACITY);
    FixedArray<uint32, SMALL_CAPACITY> prefix;
    REQUIRE(prefix.TryAdd(9U));
    REQUIRE(MakeSpan(prefix).size() == 1);
    REQUIRE(MakeSpan(prefix)[0] == 9U);

    const Array<uint8> empty;
    REQUIRE(MakeSpan(empty).empty() == true);
}

TEST_CASE("MakeSpan rejects temporaries whose span would dangle", "[containers][span-compatibility]")
{
    STATIC_REQUIRE(CanMakeSpanFromTemporary<Array<uint8>&> == true);
    STATIC_REQUIRE(CanMakeSpanFromTemporary<const Array<uint8>&> == true);
    STATIC_REQUIRE(CanMakeSpanFromTemporary<Array<uint8>> == false);
    STATIC_REQUIRE(CanMakeSpanFromTemporary<const Array<uint8>> == false);
}

TEST_CASE("Spans and borrowed views convert in both directions", "[containers][span-compatibility]")
{
    uint32 values[]{5, 6, 7};
    const BorrowedView<uint32> view{values};
    const std::span<uint32> span = MakeSpan(view);
    REQUIRE(span.data() == values);
    REQUIRE(span.size() == 3);
    const BorrowedView<const uint32> back = MakeBorrowedView(std::span<const uint32>(span));
    REQUIRE(back.GetDataOrNull() == values);
    REQUIRE(back.GetSize() == 3);
    REQUIRE(MakeSpan(BorrowedView<const uint32>()).empty() == true);
    const BorrowedView<const uint32> fromEmpty = MakeBorrowedView(std::span<const uint32>());
    REQUIRE(fromEmpty.IsValid() == true);
    REQUIRE(fromEmpty.IsEmpty() == true);

    Array<uint32> copied;
    REQUIRE(copied.TryAddRange(MakeBorrowedView(span)) == true);
    REQUIRE(copied.GetSize() == 3);
    REQUIRE(copied[2] == 7U);
}
