#pragma once

#include <ludus/foundation/base/types.h>

#include <atomic>

// Fault state shared by the startup test cases and the platform's injection
// layer: GNU --wrap on Linux, or the private native seam on macOS and Windows.
namespace ludus::foundation::threading::startup_test
{
extern std::atomic<uint32> gStarts;
extern std::atomic<uint32> gJoins;
extern std::atomic<uint32> gFailStart;
extern bool shouldFailMutex;
extern bool shouldFailCondition;

/// Counts one native thread start attempt and returns true when it must fail.
inline bool CountStartAndCheckFailure() noexcept
{
    const uint32 call = ++gStarts;
    return call == gFailStart.load();
}
} // namespace ludus::foundation::threading::startup_test
