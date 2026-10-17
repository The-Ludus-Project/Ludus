#include "internal/native_test_hooks.hpp"
#include "startup_injection.hpp"

using namespace ludus::foundation::threading::startup_test;

// Definitions for the private native seam compiled into the fault backend.
namespace ludus::foundation::threading::detail::test
{
bool ShouldFailThreadStart() noexcept
{
    return CountStartAndCheckFailure();
}
void RecordThreadJoin() noexcept
{
    ++gJoins;
}
bool ShouldFailMutexInitialization() noexcept
{
    return shouldFailMutex;
}
bool ShouldFailConditionInitialization() noexcept
{
    return shouldFailCondition;
}
} // namespace ludus::foundation::threading::detail::test
