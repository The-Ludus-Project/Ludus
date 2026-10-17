#include <ludus/foundation/logging/log_system.hpp>

#include "internal/backend.hpp"

#include <catch2/catch_test_macros.hpp>

#include <new>

namespace
{
bool gFailBackendAllocation = false;
}

// Injection applies only to the private backend's nothrow allocation. Other
// test/runtime allocations retain the standard allocator and matching delete.
// libc++ names the replaceable allocation functions' parameters with reserved
// identifiers (`__sz`), so these replacements cannot match its declarations.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)
void* operator new(ludus::foundation::usize size, const std::nothrow_t&) noexcept
{
    if (gFailBackendAllocation && size == sizeof(ludus::foundation::logging::internal::AsyncBackend))
    {
        return nullptr;
    }
    try
    {
        return ::operator new(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void* operator new(ludus::foundation::usize size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    if (gFailBackendAllocation && size == sizeof(ludus::foundation::logging::internal::AsyncBackend))
    {
        return nullptr;
    }
    try
    {
        return ::operator new(size, alignment);
    }
    catch (...)
    {
        return nullptr;
    }
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name)

TEST_CASE("Async owner allocation failure preserves synchronous logging", "[logging][allocation]")
{
    using namespace ludus::foundation::logging;
    LogConfig config;
    config.Mode = LogMode::Asynchronous;
    config.EnableConsole = false;
    config.EnableFile = false;
    config.EnableDebugger = false;
    config.FlushIntervalMilliseconds = 1000;
    gFailBackendAllocation = true;
    const LogInitResult result = LogSystem::Initialize(config);
    gFailBackendAllocation = false;
    const bool initialized = LogSystem::IsInitialized();
    const LogStatus shutdown = LogSystem::Shutdown();
    REQUIRE(result.Status == LogStatus::Degraded);
    REQUIRE(result.EffectiveMode == LogMode::Synchronous);
    REQUIRE(initialized);
    REQUIRE(shutdown == LogStatus::Ok);
}
