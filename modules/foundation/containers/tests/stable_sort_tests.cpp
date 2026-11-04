#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/stable_sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <algorithm>
#include <random>

#include <catch2/catch_test_macros.hpp>
using namespace ludus::foundation;
TEST_CASE("Caller-scratch stable sort matches an independent stable oracle", "[containers][sort]")
{
    struct Value
    {
        uint32 Key = 0;
        uint32 Ordinal = 0;
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
        Array<Value> expected = values;
        std::stable_sort(expected.begin(), expected.end(), less);
        REQUIRE(StableSort(values.GetData(), scratch.GetData(), count, less));
        CHECK(values == expected);
    }
    Value value{3, 4};
    Value scratch{};
    CHECK_FALSE(StableSort<Value>(nullptr, &scratch, 1, less));
    CHECK_FALSE(StableSort(&value, &value, 1, less));
    CHECK(value == Value{3, 4});
}

TEST_CASE("Stable sort bounds element writes across odd and even merge passes", "[containers][sort]")
{
    struct Value
    {
        usize Key = 0;
        usize* AssignmentCountOrNull = nullptr;

        Value& operator=(const Value& other) noexcept
        {
            if (this != &other)
            {
                if (AssignmentCountOrNull != nullptr)
                {
                    ++*AssignmentCountOrNull;
                }
                Key = other.Key;
            }
            return *this;
        }
    };
    constexpr usize MAX_COUNT = 33;
    StaticArray<Value, MAX_COUNT> values;
    StaticArray<Value, MAX_COUNT> scratch;
    const auto less = [](const Value& left, const Value& right) noexcept { return left.Key < right.Key; };
    for (usize count = 0; count <= MAX_COUNT; ++count)
    {
        usize assignments = 0;
        for (usize i = 0; i < count; ++i)
        {
            values[i].Key = count - i;
            values[i].AssignmentCountOrNull = &assignments;
            scratch[i].AssignmentCountOrNull = &assignments;
        }
        usize passes = 0;
        for (usize runLength = 1; runLength < count; runLength *= 2)
        {
            ++passes;
        }
        REQUIRE(StableSort(values.GetData(), scratch.GetData(), count, less) == true);
        // One write per element per merge level, plus at most one final copy.
        CHECK(assignments <= count * (passes + 1));
        for (usize i = 0; i < count; ++i)
        {
            CHECK(values[i].Key == i + 1);
        }
    }
}
