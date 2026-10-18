#include <ludus/foundation/base/core.h>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>

#include <span>
#include <string_view>

#include "packages.h"
#include "session.h"

#if LUDUS_TARGET_OS == LUDUS_OS_WEB
#    include <emscripten/emscripten.h>
#    define S2_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#    define S2_EXPORT
#endif
namespace ludus::s2
{
bool Acceptance() noexcept;
namespace
{
Session gSession;
uint8 gOutput[4096] = {};
usize gWritten = 0;
} // namespace
} // namespace ludus::s2
extern "C" S2_EXPORT ludus::foundation::int32 S2Start() noexcept
{
    ludus::s2::gSession.Close();
    return ludus::s2::gSession.Initialize(ludus::s2::BASE) ? 1 : 0;
}
extern "C" S2_EXPORT ludus::foundation::int32 S2Advance() noexcept
{
    return static_cast<ludus::foundation::int32>(ludus::s2::gSession.Begin());
}
extern "C" S2_EXPORT const char* S2Control(const char* request) noexcept
{
    using namespace ludus::s2;
    gWritten = 0;
    if (request == nullptr || gSession.Control(request, gOutput, gWritten) == false)
    {
        return nullptr;
    }
    if (gWritten >= sizeof(gOutput))
    {
        return nullptr;
    }
    gOutput[gWritten] = 0;
    return reinterpret_cast<const char*>(gOutput);
}
int main()
{
    using namespace ludus::foundation::logging;
    LogConfig config;
    config.EnableFile = false;
    config.EnableDebugger = false;
    if (LogSystem::Initialize(config).Status != LogStatus::Ok)
    {
        return 2;
    }
    const bool success = ludus::s2::Acceptance();
    LogSystem::Shutdown();
    return success ? 0 : 1;
}
