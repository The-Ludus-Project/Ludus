#include <ludus/foundation/threading/jobs.hpp>

#include "startup_injection.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
using namespace ludus::foundation::threading;

namespace ludus::foundation::threading::startup_test
{
std::atomic<uint32> gStarts{0};
std::atomic<uint32> gJoins{0};
std::atomic<uint32> gFailStart{0};
bool shouldFailMutex = false;
bool shouldFailCondition = false;
} // namespace ludus::foundation::threading::startup_test

using namespace ludus::foundation::threading::startup_test;

TEST_CASE("Partial native startup joins every already-started worker and can retry", "[threading]")
{
    JobSystem system;
    for (const uint32 failAt : {1u, 2u, 4u})
    {
        gStarts = 0;
        gJoins = 0;
        gFailStart = failAt;
        const JobStatus status = system.Initialize({4});
        gFailStart = 0;
        REQUIRE(status == JobStatus::ThreadCreationFailed);
        REQUIRE(gJoins == failAt - 1);
        REQUIRE(system.Initialize({2}) == JobStatus::Ok);
        REQUIRE(system.Shutdown() == JobStatus::Ok);
        REQUIRE(gJoins == failAt + 1);
    }
}

TEST_CASE("Native synchronization initialization failure leaves a reusable system", "[threading]")
{
    JobSystem system;
    shouldFailMutex = true;
    const JobStatus mutexStatus = system.Initialize({1});
    shouldFailMutex = false;
    REQUIRE(mutexStatus == JobStatus::ThreadCreationFailed);
    shouldFailCondition = true;
    const JobStatus conditionStatus = system.Initialize({1});
    shouldFailCondition = false;
    REQUIRE(conditionStatus == JobStatus::ThreadCreationFailed);
    REQUIRE(system.Initialize({1}) == JobStatus::Ok);
}
