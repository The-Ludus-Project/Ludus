#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;

namespace
{
// Exercise the real frontend's attribute spellings without executing a trap or
// an unreachable path. Clang targeting the Microsoft ABI is a separate case.
LUDUS_INLINE constexpr int32 ComputeInlineValue() noexcept
{
    return 1;
}

LUDUS_NOINLINE LUDUS_COLD int32 ComputeBranchValue(int32 value) noexcept
{
    if (LUDUS_LIKELY(value > 0) == true)
    {
        return value;
    }
    if (LUDUS_UNLIKELY(value < 0) == true)
    {
        return -1;
    }
    return 0;
}

[[maybe_unused]] void BreakInDebugger() noexcept
{
    LUDUS_DEBUG_BREAK();
}

[[maybe_unused]] void MarkUnreachable() noexcept
{
    LUDUS_UNREACHABLE();
}

static_assert(ComputeInlineValue() == 1);
#if defined(_MSC_VER) && !defined(__clang__)
static_assert(LUDUS_TARGET_COMPILER == LUDUS_CXX_MSVC);
static_assert(LUDUS_TARGET_OS == LUDUS_OS_WINDOWS);
static_assert(LUDUS_TARGET_ARCH == LUDUS_CPU_X86_64);
static_assert(LUDUS_POINTER_BITS == 64);
#elif defined(__clang__)
static_assert(LUDUS_TARGET_COMPILER == LUDUS_CXX_CLANG);
#endif
} // namespace

TEST_CASE("compiler abstraction compiles with the selected frontend", "[primitive][compiler]")
{
    REQUIRE(ComputeBranchValue(1) == 1);
    REQUIRE(ComputeBranchValue(0) == 0);
    REQUIRE(ComputeBranchValue(-1) == -1);
}
