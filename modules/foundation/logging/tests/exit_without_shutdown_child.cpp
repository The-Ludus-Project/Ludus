// Regression child for process exit without LogSystem::Shutdown() (issue 2188).
//
// exit() destroys FoundationLogging's process-lifetime state while its timed
// flush worker (synchronous mode) or its queue worker (asynchronous mode) is
// still running. Destroying a joinable worker thread used to call
// std::terminate, so the process aborted instead of exiting with its status.
// CTest runs each mode and requires exit status 0; an abort fails the test.

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/logging/category.hpp>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_system.hpp>

#include <cstdlib>
#include <cstring>

namespace
{
LUDUS_DEFINE_LOG_CATEGORY(LOG_EXIT_TEST, "ExitWithoutShutdownTest");

// Distinct nonzero results so a failed setup is not mistaken for the exit path.
constexpr ludus::foundation::int32 USAGE_FAILURE = 2;
constexpr ludus::foundation::int32 INITIALIZE_FAILURE = 3;
} // namespace

int main(int argc, char** argv)
{
    using namespace ludus::foundation::logging;
    if (argc != 2)
    {
        return USAGE_FAILURE;
    }
    LogConfig config{};
    if (std::strcmp(argv[1], "synchronous") == 0)
    {
        config.Mode = LogMode::Synchronous;
    }
    else if (std::strcmp(argv[1], "asynchronous") == 0)
    {
        config.Mode = LogMode::Asynchronous;
    }
    else
    {
        return USAGE_FAILURE;
    }
    // A positive interval starts the synchronous timed-flush worker; the
    // asynchronous backend always runs its own worker.
    config.FlushIntervalMilliseconds = 1;
    config.EnableConsole = true;
    config.EnableDebugger = false;
    config.EnableFile = false;
    const LogInitResult result = LogSystem::Initialize(config);
    if (LogSystem::IsInitialized() == false || result.EffectiveMode != config.Mode)
    {
        return INITIALIZE_FAILURE;
    }
    LUDUS_LOG_TEXT(LOG_EXIT_TEST, Info, "exiting without LogSystem::Shutdown");

    // The same path QCommandLineParser::process takes for an unknown option.
    std::exit(EXIT_SUCCESS);
}
