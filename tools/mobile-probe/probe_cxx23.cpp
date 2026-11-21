// Mobile M0 probe: a FoundationBase-style C++23 translation unit.
//
// It includes the real Band 0 headers (config, compiler, types) and exercises the
// C++23 language features and the ADR 0003 header allowlist that the engine uses,
// so a mobile toolchain that cannot compile it is rejected before an engine port
// begins. It must stay warning-clean under -Wall -Wextra -Wpedantic -Werror and
// -fno-exceptions. See docs/development/mobile-support-plan.md#acceptance-probes.
#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/types.h>

#include <atomic>
#include <bit>
#include <concepts>
#include <type_traits>
#include <utility>

using namespace ludus::foundation::core;

namespace LudusMobileProbe
{
enum class Target : uint8
{
    Unknown = 0,
    Ios = 1,
    Android = 2,
};

template <typename Value>
concept UnsignedInteger = std::unsigned_integral<Value>;

struct Counter
{
    // Deducing this (C++23) in a shape FoundationBase containers use.
    template <typename Self>
    [[nodiscard]] LUDUS_INLINE auto&& GetValue(this Self&& self) noexcept
    {
        return std::forward<Self>(self).mValue;
    }

    uint32 mValue = 0;
};

struct Grid
{
    // Multidimensional subscript (C++23).
    [[nodiscard]] constexpr uint32 operator[](usize row, usize column) const noexcept
    {
        return mCells[row * COLUMN_COUNT + column];
    }

    static constexpr usize ROW_COUNT = 2;
    static constexpr usize COLUMN_COUNT = 2;
    uint32 mCells[ROW_COUNT * COLUMN_COUNT] = {1, 2, 3, 4};
};

template <UnsignedInteger Value>
[[nodiscard]] constexpr Value RotateLeftOne(Value value) noexcept
{
    return std::rotl(value, 1);
}

[[nodiscard]] consteval Target GetCompiledTarget() noexcept
{
#if LUDUS_TARGET_OS == LUDUS_OS_IOS
    return Target::Ios;
#elif LUDUS_TARGET_OS == LUDUS_OS_ANDROID
    return Target::Android;
#else
    return Target::Unknown;
#endif
}

[[nodiscard]] constexpr uint32 SumGrid(const Grid& grid) noexcept
{
    if consteval
    {
        return grid[0, 0] + grid[1, 1];
    }
    else
    {
        return grid[0, 0] + grid[0, 1] + grid[1, 0] + grid[1, 1];
    }
}

static_assert(sizeof(uint8) == 1);
static_assert(sizeof(uint32) == 4);
static_assert(sizeof(uint64) == 8);
static_assert(sizeof(float32) == 4);
static_assert(sizeof(float64) == 8);
static_assert(RotateLeftOne<uint8>(0x81) == 0x03);
static_assert(SumGrid(Grid{}) == 5);
static_assert(std::to_underlying(Target::Android) == 2);
static_assert(std::is_trivially_copyable_v<Counter>);
static_assert(std::atomic<uint64>::is_always_lock_free);

std::atomic<uint32> gProbeCalls{0};
} // namespace LudusMobileProbe

/// Returns 0 when the probe ran as expected for the compiled target.
extern "C" int LudusMobileProbeCxx23() noexcept
{
    LudusMobileProbe::Counter counter;
    counter.GetValue() = LudusMobileProbe::gProbeCalls.fetch_add(1, std::memory_order_relaxed);
    const LudusMobileProbe::Grid grid;
    const uint32 sum = LudusMobileProbe::SumGrid(grid);
    const bool isTargetKnown = LudusMobileProbe::GetCompiledTarget() != LudusMobileProbe::Target::Unknown;
    return (sum == 10 && isTargetKnown == true) ? 0 : 1;
}
