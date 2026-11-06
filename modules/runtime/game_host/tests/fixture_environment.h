#pragma once

#include <ludus/foundation/base/config.h>

#include <cstdlib>

// Failure injection shared by the loader and reload tests: the fixture module reads
// LUDUS_FIXTURE_FAIL through the same C runtime as the test executable (the dynamic
// CRT on Windows), so setting it here reaches the loaded generation.
namespace ludus::runtime::game_host::tests
{
/// Select the fixture stage that fails, or clear the selection with nullptr.
[[nodiscard]] inline bool SetFixtureFailure(const char* stageOrNull) noexcept
{
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    // An empty value removes the variable, so getenv returns null as after unsetenv.
    return ::_putenv_s("LUDUS_FIXTURE_FAIL", stageOrNull == nullptr ? "" : stageOrNull) == 0;
#else
    return stageOrNull == nullptr ? ::unsetenv("LUDUS_FIXTURE_FAIL") == 0
                                  : ::setenv("LUDUS_FIXTURE_FAIL", stageOrNull, 1) == 0;
#endif
}
} // namespace ludus::runtime::game_host::tests
