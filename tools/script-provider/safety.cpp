// Production-provider recovery tests supplement the static nonlocal-jump audit
// in docs/architecture/behavior-s6.md; ASan alone cannot prove that C++ rule.
#include <ludus/runtime/behavior/behavior.h>

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/target.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>

#include <cstring>

#include "fault_allocator.h"

#include "contract.h"
#include "package.h"

namespace
{
using namespace ludus::runtime::behavior;
using namespace ludus::foundation;
constexpr usize HEAP_LIMIT = usize{8} * 1024 * 1024;
constexpr logging::LogCategory CATEGORY{"BehaviorS6"};

bool Check(bool valid, const char* name) noexcept
{
    if (valid)
    {
        LUDUS_LOG_INFO(CATEGORY, "S6 PASS {}", name);
    }
    else
    {
        LUDUS_LOG_ERROR(CATEGORY, "S6 FAIL {}", name);
    }
    return valid;
}
bool Alive(void*, EntityRef reference) noexcept
{
    return reference.Slot == 0 && reference.Generation == 1;
}
Invocation Input(uint64 asset) noexcept
{
    Invocation input
    {
        .Asset = asset,
        .Revision = 1,
        .Instance = 1,
        .World = 2,
        .Session = 3,
        .Execution = 4,
        .Tick = 1,
        .Phase = 3,
        .Capabilities = 3,
    };
    input.Config = ludus::sample::MakeConfig({});
    input.State = ludus::sample::MakeState({});
    input.Event = ludus::sample::MakeEvent({ .Target = {2, 3, 4, 0, 1}, .Amount = 1 });
    return input;
}
Status Load(LuauProvider& provider, uint32 safepoints = 100000) noexcept
{
    return provider.Load(ludus::sample::SCHEMA, ludus::sample::PACKAGE, HEAP_LIMIT, safepoints);
}
bool Boundaries() noexcept
{
    LuauProvider provider;
    Outcome output;
    Diagnostic diagnostic;
    if (Load(provider) != Status::Completed ||
        provider.Invoke(Input(0x1001), {nullptr, Alive}, output, diagnostic) != Status::Completed ||
        output.Count != 1 || output.State.Items[0].Data.Scalar != 1)
    {
        return Check(false, "explicit-frozen-environment");
    }
    Check(true, "explicit-frozen-environment");
    constexpr uint64 FAULT_ASSETS[] = {0x1003, 0x1004};
    for (uint64 asset : FAULT_ASSETS)
    {
        const uint32 cases = asset == 0x1003 ? 4 : 8;
        for (uint32 amount = 1; amount <= cases; ++amount)
        {
            if (Load(provider) != Status::Completed)
            {
                return false;
            }
            auto input = Input(asset);
            input.Event = ludus::sample::MakeEvent({ .Target = {2, 3, 4, 0, 1}, .Amount = amount });
            output.Count = 13;
            output.State.Items[0].Data.Scalar = 77;
            if (provider.Invoke(input, {nullptr, Alive}, output, diagnostic) != Status::ScriptFault ||
                output.Count != 13 || output.State.Items[0].Data.Scalar != 77 || provider.LiveBytes() != 0)
            {
                return Check(false, "readonly-numeric-forged-value-boundaries");
            }
        }
    }
    return Check(true, "readonly-numeric-forged-value-boundaries");
}
bool InterruptAndLifetime() noexcept
{
    LuauProvider provider;
    Outcome output;
    output.Count = 13;
    Diagnostic diagnostic;
    if (Load(provider, 32) != Status::Completed ||
        provider.Invoke(Input(0x1006), {nullptr, Alive}, output, diagnostic) != Status::Interrupted ||
        output.Count != 13 || provider.LiveBytes() != 0)
    {
        return Check(false, "uncatchable-interrupt-discards-effects");
    }
    Check(true, "uncatchable-interrupt-discards-effects");
    if (Load(provider) != Status::Completed ||
        provider.Invoke(Input(0x1007), {nullptr, Alive}, output, diagnostic) != Status::ScriptFault ||
        output.Count != 13 || provider.LiveBytes() != 0)
    {
        return Check(false, "stack-fault-discards-effects");
    }
    Check(true, "stack-fault-discards-effects");
    if (Load(provider) != Status::Completed ||
        provider.Invoke(Input(0x1008), {nullptr, Alive}, output, diagnostic) != Status::Completed)
    {
        return false;
    }
    output.Count = 13;
    return Check(provider.Invoke(Input(0x1008), {nullptr, Alive}, output, diagnostic) == Status::ScriptFault &&
                     output.Count == 13 && provider.LiveBytes() == 0,
                 "retained-state-facade-expires");
}
bool SteppingBudget() noexcept
{
    LuauProvider provider;
    Outcome output;
    output.Count = 13;
    Diagnostic diagnostic;
    if (Load(provider, 32) != Status::Completed || provider.Breakpoint(0x1006, 1) < 1)
    {
        return false;
    }
    auto status = provider.BeginDebug(Input(0x1006), {nullptr, Alive}, output, diagnostic);
    for (uint32 i = 0; status == Status::Paused && i < 128; ++i)
    {
        DebugSnapshot snapshot;
        if (provider.Inspect(snapshot) == false || snapshot.Paused == false || output.Count != 13)
        {
            return false;
        }
        status = provider.ResumeDebug(snapshot.Stop, DebugMode::Into, output, diagnostic);
    }
    return Check(status == Status::Interrupted && output.Count == 13 && provider.LiveBytes() == 0,
                 "debug-resumes-share-one-safepoint-budget");
}
bool MetadataAndEntities() noexcept
{
    LuauProvider provider;
    if (Load(provider) != Status::Completed)
    {
        return false;
    }
    const usize live = provider.LiveBytes();
    uint8 bytes[sizeof(ludus::sample::PACKAGE)];
    std::memcpy(bytes, ludus::sample::PACKAGE, sizeof(bytes));
    // Fuzz only bounded metadata. Arbitrary bytecode is outside owner admission.
    for (usize size = 0; size < sizeof(bytes); ++size)
    {
        if (size >= 16)
        {
            (void)TryWriteLittleEndian(static_cast<uint32>(size), std::span<uint8>{bytes}.subspan(12));
        }
        if (provider.Load(ludus::sample::SCHEMA, {bytes, size}) != Status::InvalidPackage ||
            provider.LiveBytes() != live)
        {
            return Check(false, "metadata-rejection-retains-active-package");
        }
    }
    Check(true, "metadata-rejection-retains-active-package");
    for (uint32 field = 0; field < 5; ++field)
    {
        auto input = Input(0x1001);
        auto& reference = input.Event.Items[0].Data.Entity;
        switch (field)
        {
            case 0:
                ++reference.World;
                break;
            case 1:
                ++reference.Session;
                break;
            case 2:
                ++reference.Execution;
                break;
            case 3:
                ++reference.Slot;
                break;
            default:
                ++reference.Generation;
                break;
        }
        Outcome output;
        output.Count = 13;
        Diagnostic diagnostic;
        if (provider.Invoke(input, {nullptr, Alive}, output, diagnostic) != Status::InvalidInput ||
            output.Count != 13 || provider.LiveBytes() != live)
        {
            return false;
        }
    }
    return Check(true, "all-entity-identity-boundaries");
}
struct ServiceContext
{
    LuauProvider* Provider = nullptr;
    uint32 Acquired = 0;
    uint32 Released = 0;
    bool Fenced = true;
};
struct ServiceResource
{
    ServiceContext& Context;
    ~ServiceResource() noexcept
    {
        ++Context.Released;
    }
};
bool CheckedService(void* user, EntityRef reference) noexcept
{
    auto& context = *static_cast<ServiceContext*>(user);
    ++context.Acquired;
    ServiceResource resource{context}; // Completes before the next allocating VM operation.
    Outcome output;
    Diagnostic diagnostic;
    DebugSnapshot snapshot;
    context.Fenced = context.Fenced && Load(*context.Provider) == Status::Reentrant &&
                     context.Provider->Close() == Status::Reentrant &&
                     context.Provider->Invoke(Input(0x1001), {}, output, diagnostic) == Status::Reentrant &&
                     context.Provider->BeginDebug(Input(0x1001), {}, output, diagnostic) == Status::Reentrant &&
                     context.Provider->ResumeDebug(1, DebugMode::Continue, output, diagnostic) == Status::Reentrant &&
                     context.Provider->Breakpoint(0x1001, 1) == -1 && context.Provider->Inspect(snapshot) == false;
    return Alive(nullptr, reference);
}
bool ServiceBoundary() noexcept
{
    LuauProvider provider;
    if (Load(provider) != Status::Completed)
    {
        return false;
    }
    ServiceContext context{&provider};
    Outcome output;
    output.Count = 13;
    Diagnostic diagnostic;
    const auto status = provider.Invoke(Input(0x1004), {&context, CheckedService}, output, diagnostic);
    return Check(status == Status::ScriptFault && output.Count == 13 && provider.LiveBytes() == 0 && context.Fenced &&
                     context.Acquired >= 2 && context.Released == context.Acquired,
                 "native-service-resources-finish-before-vm-recovery");
}
Status Journey(LuauProvider& provider, bool debug, Outcome& output, Diagnostic& diagnostic) noexcept
{
    auto status = Load(provider);
    if (status != Status::Completed)
    {
        return status;
    }
    if (debug == false)
    {
        return provider.Invoke(Input(0x1002), {nullptr, Alive}, output, diagnostic);
    }
    if (provider.Breakpoint(0x1005, 1) < 1)
    {
        return provider.LiveBytes() == 0 ? Status::AllocationFailure : Status::InvalidInput;
    }
    status = provider.BeginDebug(Input(0x1005), {nullptr, Alive}, output, diagnostic);
    for (uint32 stops = 0; status == Status::Paused && stops < 32; ++stops)
    {
        DebugSnapshot snapshot;
        if (provider.Inspect(snapshot) == false || snapshot.Paused == false || snapshot.FrameCount == 0 ||
            output.Count != 13)
        {
            return Status::InvalidInput;
        }
        status = provider.ResumeDebug(snapshot.Stop, DebugMode::Into, output, diagnostic);
    }
    return status;
}
bool AllocationSweep(bool debug) noexcept
{
    usize attempts = 0;
    {
        LuauProvider provider;
        Outcome output;
        output.Count = 13;
        Diagnostic diagnostic;
        ludus::s6::FailAllocationsFrom(0);
        const auto status = Journey(provider, debug, output, diagnostic);
        attempts = ludus::s6::AllocationAttempts();
        ludus::s6::StopAllocationFailure();
        if (status != Status::Completed || output.Count != 1 || output.State.Items[0].Data.Scalar != 1 ||
            provider.Close() != Status::Completed || provider.LiveBytes() != 0 || attempts == 0)
        {
            return false;
        }
    }
    for (usize attempt = 1; attempt <= attempts; ++attempt)
    {
        LuauProvider provider;
        Outcome output;
        output.Count = 13;
        output.State.Items[0].Data.Scalar = 77;
        Diagnostic diagnostic;
        ludus::s6::FailAllocationsFrom(attempt);
        const auto status = Journey(provider, debug, output, diagnostic);
        ludus::s6::StopAllocationFailure();
        if (status != Status::AllocationFailure || output.Count != 13 || output.State.Items[0].Data.Scalar != 77 ||
            provider.LiveBytes() != 0 || provider.Close() != Status::Completed)
        {
            LUDUS_LOG_ERROR(CATEGORY,
                            "S6 allocation failure point {} / {}, status {}",
                            attempt,
                            attempts,
                            static_cast<uint32>(status));
            return false;
        }
    }
    LUDUS_LOG_INFO(CATEGORY, "S6 METRIC {}-allocation-points={}", debug ? "debug" : "invoke", attempts);
    return Check(true, debug ? "production-debug-allocation-sweep" : "production-invoke-allocation-sweep");
}
bool ReplacementSweep() noexcept
{
    usize attempts = 0;
    {
        LuauProvider provider;
        if (Load(provider) != Status::Completed)
        {
            return false;
        }
        ludus::s6::FailAllocationsFrom(0);
        const auto status = Load(provider);
        attempts = ludus::s6::AllocationAttempts();
        ludus::s6::StopAllocationFailure();
        if (status != Status::Completed || attempts == 0)
        {
            return false;
        }
    }
    for (usize attempt = 1; attempt <= attempts; ++attempt)
    {
        LuauProvider provider;
        if (Load(provider) != Status::Completed)
        {
            return false;
        }
        const usize live = provider.LiveBytes();
        ludus::s6::FailAllocationsFrom(attempt);
        const auto status = Load(provider);
        ludus::s6::StopAllocationFailure();
        Outcome output;
        Diagnostic diagnostic;
        if (status != Status::AllocationFailure || provider.LiveBytes() != live ||
            provider.Invoke(Input(0x1001), {nullptr, Alive}, output, diagnostic) != Status::Completed ||
            output.Count != 1 || provider.Close() != Status::Completed || provider.LiveBytes() != 0)
        {
            return false;
        }
    }
    LUDUS_LOG_INFO(CATEGORY, "S6 METRIC replacement-allocation-points={}", attempts);
    return Check(true, "failed-candidate-allocation-retains-active-provider");
}
} // namespace

int main()
{
    logging::LogConfig config;
    config.EnableFile = false;
    config.EnableDebugger = false;
    if (logging::LogSystem::Initialize(config).Status != logging::LogStatus::Ok)
    {
        return 2;
    }
    LUDUS_LOG_INFO(CATEGORY,
                   "S6 TARGET os={} arch={} pointer_bits={}",
                   TargetOsName(kTarget.Os),
                   TargetArchName(kTarget.Arch),
                   kTarget.PointerBits);
    const bool passed = Boundaries() && InterruptAndLifetime() && SteppingBudget() && MetadataAndEntities() &&
                        ServiceBoundary() && AllocationSweep(false) && AllocationSweep(true) && ReplacementSweep();
    logging::LogSystem::Shutdown();
    return passed ? 0 : 1;
}
