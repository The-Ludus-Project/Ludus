// Thanks to Chris Lomont, "Floating-Point Tricks", Game Programming Gems 6,
// section 2.1, pp. 121-124: the representation discussion motivates the signed
// zero/subnormal bit-pattern checks below. These original tests do not set FP
// execution policy. Review: docs/architecture/primitive-types.md.
#include <ludus/foundation/base/byte_order.hpp>

#include <bit>
#include <limits>
#include <type_traits>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;

namespace
{
static_assert(std::is_same_v<usize, decltype(sizeof(0))>);
static_assert(std::bit_cast<uint32>(float32{1}) == 0x3F800000);
static_assert(std::bit_cast<uint64>(float64{1}) == 0x3FF0000000000000);
static_assert(std::bit_cast<uint32>(float32{-0.0f}) == 0x80000000);
static_assert(std::bit_cast<uint64>(float64{-0.0}) == 0x8000000000000000);
static_assert(std::bit_cast<uint32>(std::numeric_limits<float32>::denorm_min()) == 1);
static_assert(std::bit_cast<uint64>(std::numeric_limits<float64>::denorm_min()) == 1);
static_assert(UnsignedInteger<int32> == false && UnsignedInteger<bool> == false && UnsignedInteger<float32> == false);

template <typename ValueType>
concept HasByteCodec = requires(ValueType value, ValueType& out, std::span<uint8> bytes) {
    TryReadLittleEndian(bytes, out);
    TryWriteBigEndian(value, bytes);
};
static_assert(HasByteCodec<int32> == false && HasByteCodec<bool> == false && HasByteCodec<float32> == false);

constexpr bool VerifyConstexprBytes() noexcept
{
    uint8 bytes[5] = {0x78, 0x56, 0x34, 0x12, 0xAA};
    uint32 out = 42;
    if (TryReadLittleEndian(bytes, out) == false || out != 0x12345678 || TryWriteBigEndian(out, bytes) == false ||
        bytes[0] != 0x12 || bytes[3] != 0x78 || bytes[4] != 0xAA || TryReadBigEndian(bytes, out) == false ||
        out != 0x12345678)
    {
        return false;
    }
    const std::span<uint8> shortBytes{bytes, 3};
    return TryReadLittleEndian(shortBytes, out) == false && out == 0x12345678 &&
           TryWriteLittleEndian(uint32{0}, shortBytes) == false && bytes[0] == 0x12 && bytes[3] == 0x78;
}
static_assert(VerifyConstexprBytes());
static_assert(noexcept(TryReadBigEndian({}, *static_cast<uint64*>(nullptr))));
static_assert(noexcept(TryWriteLittleEndian(uint64{}, {})));
} // namespace

TEMPLATE_TEST_CASE("byte codecs match fixed byte vectors at unaligned offsets",
                   "[primitive][bytes]",
                   uint8,
                   uint16,
                   uint32,
                   uint64,
                   usize)
{
    // Neither read nor write needs typed alignment. Byte vector expectation is
    // independent of host byte order, rather than just a round-trip assertion.
    constexpr uint64 pattern = 0xFEDCBA9876543210;
    const auto value = static_cast<TestType>(pattern);
    uint8 bytes[10]{};
    bytes[0] = 0xA5;
    bytes[9] = 0x5A;
    std::span<uint8> buffer{bytes + 1, 8};
    REQUIRE(TryWriteLittleEndian(value, buffer));
    for (usize i = 0; i < sizeof(TestType); ++i)
    {
        REQUIRE(buffer[i] == static_cast<uint8>(pattern >> (i * 8)));
    }
    TestType decoded{};
    REQUIRE(TryReadLittleEndian(buffer, decoded));
    REQUIRE(decoded == value);
    REQUIRE(TryWriteBigEndian(value, buffer));
    for (usize i = 0; i < sizeof(TestType); ++i)
    {
        REQUIRE(buffer[i] == static_cast<uint8>(pattern >> ((sizeof(TestType) - 1 - i) * 8)));
    }
    REQUIRE(TryReadBigEndian(buffer, decoded));
    REQUIRE(decoded == value);
    REQUIRE(bytes[0] == 0xA5);
    REQUIRE(bytes[9] == 0x5A);
    for (usize i = sizeof(TestType); i < buffer.size(); ++i)
    {
        REQUIRE(buffer[i] == 0);
    }
}

TEMPLATE_TEST_CASE("truncated byte spans preserve the entire buffer and output",
                   "[primitive][bytes]",
                   uint8,
                   uint16,
                   uint32,
                   uint64,
                   usize)
{
    for (usize size = 0; size < sizeof(TestType); ++size)
    {
        uint8 bytes[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        const std::span<uint8> buffer{bytes, size};
        TestType decoded = 42;
        REQUIRE_FALSE(TryReadLittleEndian(buffer, decoded));
        REQUIRE_FALSE(TryReadBigEndian(buffer, decoded));
        REQUIRE(decoded == 42);
        REQUIRE_FALSE(TryWriteLittleEndian(TestType{0}, buffer));
        REQUIRE_FALSE(TryWriteBigEndian(TestType{0}, buffer));
        for (usize i = 0; i < sizeof(bytes); ++i)
        {
            REQUIRE(bytes[i] == i + 1);
        }
    }
    TestType decoded = 42;
    REQUIRE_FALSE(TryReadLittleEndian({}, decoded));
    REQUIRE_FALSE(TryReadBigEndian({}, decoded));
    REQUIRE_FALSE(TryWriteLittleEndian(TestType{0}, {}));
    REQUIRE_FALSE(TryWriteBigEndian(TestType{0}, {}));
    REQUIRE(decoded == 42);
}

TEMPLATE_TEST_CASE("zero and maximum bytes are encoded exactly",
                   "[primitive][bytes]",
                   uint8,
                   uint16,
                   uint32,
                   uint64,
                   usize)
{
    uint8 bytes[sizeof(TestType)]{};
    TestType decoded = 42;
    REQUIRE(TryReadBigEndian(bytes, decoded));
    REQUIRE(decoded == 0);
    REQUIRE(TryWriteBigEndian(std::numeric_limits<TestType>::max(), bytes));
    for (const auto byte : bytes)
    {
        REQUIRE(byte == 0xFF);
    }
    REQUIRE(TryReadLittleEndian(bytes, decoded));
    REQUIRE(decoded == std::numeric_limits<TestType>::max());
}
