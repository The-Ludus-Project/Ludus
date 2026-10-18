#include "../../base/tests/assert_transport.hpp"
#include <ludus/foundation/base/assert.hpp>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_system.hpp>

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace
{
bool gAssertInSink = false;
}
// Linker test interception executes while the real logger holds its dispatch lock.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" ludus::foundation::usize
__real_fwrite(const void*, ludus::foundation::usize, ludus::foundation::usize, std::FILE*);
extern "C" ludus::foundation::usize
__wrap_fwrite(const void* data, ludus::foundation::usize size, ludus::foundation::usize count, std::FILE* stream)
{
    if (gAssertInSink)
    {
        gAssertInSink = false;
        (void)LUDUS_CHECK(false, "inside real console sink with logger lock held");
    }
    return __real_fwrite(data, size, count, stream);
}
// NOLINTEND(bugprone-reserved-identifier)

int main(int argc, char** argv)
{
    ConfigureTestTransport();
    using namespace ludus::foundation::logging;
    if (argc != 2 || LogSystem::IsInitialized())
    {
        return 1;
    }
    if (std::strcmp(argv[1], "emergency") == 0)
    {
        // Prove the new Base report adapter reaches the configured transport,
        // independently of stdio/stderr and without initializing Logging.
        (void)::close(STDERR_FILENO);
        LUDUS_LOG_TEXT(LogCategory{"AssertionTransportTest"}, Warning, "shared Base emergency transport");
        return 0;
    }
    if (std::strcmp(argv[1], "pre-fatal") == 0)
    {
        LUDUS_FATAL("before logger initialization");
    }
    if (LUDUS_CHECK(false, "pre-init Check"))
    {
        return 2;
    }
    LogConfig config{};
    config.EnableConsole = std::strcmp(argv[1], "sink") == 0;
    config.EnableDebugger = false;
    config.EnableFile = false;
    LogSystem::Initialize(config);
    if (LogSystem::IsInitialized() == false)
    {
        return 4;
    }
    if (config.EnableConsole)
    {
        gAssertInSink = true;
        LUDUS_LOG_TEXT(LogCategory{"AssertionSinkTest"}, Warning, "sink probe");
        if (gAssertInSink)
        {
            return 6;
        }
    }
    LogSystem::Shutdown();
    if (LogSystem::IsInitialized() || LUDUS_CHECK(false, "post-shutdown Check"))
    {
        return 3;
    }
    if (std::strcmp(argv[1], "post-fatal") == 0)
    {
        LUDUS_FATAL("after logger shutdown");
    }
    return 0;
}
