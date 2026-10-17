#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/stable_sort.hpp>
#include <random>
using namespace ludus::foundation;
TEST_CASE("Caller-scratch stable sort matches an independent stable oracle", "[containers][sort]")
{
    struct Value
    {
        uint32 Key = 0, Ordinal = 0;
        bool operator==(const Value&) const = default;
    };
    const auto less = [](const Value& a, const Value& b) noexcept { return a.Key < b.Key; };
    // A fixed seed keeps the generated test inputs reproducible.
    // NOLINTNEXTLINE(bugprone-random-generator-seed)
    std::mt19937 random(71);
    for (usize count = 0; count <= 257; ++count)
    {
        Array<Value> values(count);
        Array<Value> scratch(count);
        for (usize i = 0; i < count; ++i)
        {
            values[i] = {static_cast<uint32>(random() % 13), static_cast<uint32>(i)};
        }
        auto expected = values;
        std::stable_sort(expected.begin(), expected.end(), less);
        REQUIRE(StableSort(values.GetData(), scratch.GetData(), count, less));
        CHECK(values == expected);
    }
    Value value{3, 4}, scratch{};
    CHECK_FALSE(StableSort<Value>(nullptr, &scratch, 1, less));
    CHECK_FALSE(StableSort(&value, &value, 1, less));
    CHECK(value == Value{3, 4});
}
