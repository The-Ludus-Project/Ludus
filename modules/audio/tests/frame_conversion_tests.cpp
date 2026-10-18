#include "internal/frame_conversion.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
using ludus::audio::internal::TryResampleFrame;

namespace
{
constexpr bool ConstexprConversion() noexcept
{
    uint64 frame = 42;
    return TryResampleFrame(441, 44100, 48000, frame) && frame == 480 &&
           TryResampleFrame(~uint64{0}, 1, 2, frame) == false && frame == 480;
}
static_assert(ConstexprConversion());
} // namespace

TEST_CASE("audio loop frame conversion retains nearest ties-up rounding", "[primitive][audio]")
{
    for (uint32 sourceRate = 1; sourceRate <= 8; ++sourceRate)
    {
        for (uint32 targetRate = 1; targetRate <= 8; ++targetRate)
        {
            for (uint64 sourceFrame = 0; sourceFrame < 128; ++sourceFrame)
            {
                uint64 result{};
                REQUIRE(TryResampleFrame(sourceFrame, sourceRate, targetRate, result));
                // Independent direct-product oracle; this small domain cannot overflow.
                REQUIRE(result == (sourceFrame * targetRate + sourceRate / 2) / sourceRate);
            }
        }
    }
}

TEST_CASE("audio loop frame conversion checks the final sum and preserves output", "[primitive][audio]")
{
    constexpr uint64 max = ~uint64{0};
    constexpr uint32 maxRate = ~uint32{0};
    uint64 result = 42;
    REQUIRE_FALSE(TryResampleFrame(max, 1, 2, result));
    REQUIRE(result == 42);
    // Whole quotient * 3 is exactly max; adding the rounded remainder must fail.
    REQUIRE_FALSE(TryResampleFrame((max / 3) * 2 + 1, 2, 3, result));
    REQUIRE(result == 42);
    REQUIRE_FALSE(TryResampleFrame(1, 0, 1, result));
    REQUIRE_FALSE(TryResampleFrame(1, 1, 0, result));
    REQUIRE(result == 42);
    REQUIRE(TryResampleFrame(max, 1, 1, result));
    REQUIRE(result == max);
    REQUIRE(TryResampleFrame(max, maxRate, maxRate, result));
    REQUIRE(result == max);
    REQUIRE(TryResampleFrame(result, 3, 1, result));
    REQUIRE(result == max / 3);
}
