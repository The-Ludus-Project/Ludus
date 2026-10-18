#include <ludus/foundation/base/core.h>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>

#include <type_traits>

#include <lua.h>

#include "acceptance.h"
#include "fixtures.h"
#include "internal/runtime.h"

namespace ludus::s1
{
using runtime::scripting::Diagnostic;
using runtime::scripting::Identity;
using runtime::scripting::Runtime;
using RuntimeStatus = runtime::scripting::Status;
int32 Install(lua_State* state) noexcept;
int32 PushArguments(lua_State* state) noexcept;
struct Provider
{
    Runtime Vm;
    Diagnostic Last;
    runtime::scripting::Setup Arguments = PushArguments;
};
using Program = CookedProgram;
static_assert(std::is_trivially_destructible_v<Transaction>);

Identity Origin(const Transaction& txn) noexcept
{
    Identity source
    {
        .Asset = 0xb123456789abcdefULL,
        .Revision = 1,
        .Execution = txn.Owner->Execution,
        .Instance = txn.Instance,
        .World = txn.Owner->Entities.GetWorld(),
        .Session = txn.Owner->Session,
        .Tick = txn.Owner->Tick,
        .Phase = static_cast<uint8>(txn.Owner->At),
        .EntitySlot = txn.Event.Target.Slot,
        .EntityGeneration = txn.Event.Target.Generation,
    };
    for (usize i = 0; i < sizeof(ENTRYPOINT); ++i)
    {
        source.Entrypoint[i] = ENTRYPOINT[i];
    }
    return source;
}
Result LuauInteraction(Transaction& txn, void* user) noexcept
{
    auto* provider = static_cast<Provider*>(user);
    const RuntimeStatus status = provider->Vm.Invoke(Origin(txn), &txn, provider->Arguments, provider->Last);
    switch (status)
    {
        case RuntimeStatus::Completed:
            return Result::Accepted;
        case RuntimeStatus::AllocationFailure:
            return Result::AllocationFailure;
        case RuntimeStatus::Interrupted:
            return Result::Interrupted;
        default:
            return Result::ScriptFault;
    }
}
bool Load(Provider& provider, Program program) noexcept
{
    return provider.Vm.Load(program.Code, program.Bytes, program.Name, Install) == RuntimeStatus::Completed;
}
bool FaultCase(Program program, RuntimeStatus expected) noexcept
{
    Provider provider;
    World world;
    if (Initialize(world) == false || Load(provider, program) == false)
    {
        return false;
    }
    const EventRecord event = Event(world, 0, 1, 2);
    const Result result = Dispatch(world, &event, 1, LuauInteraction, &provider);
    return result != Result::Accepted && world.Faulted && world.States[0].Interactions == 0 &&
           world.States[0].OpenRequested == false && world.CommandCount == 0 && world.NextToken == 1 &&
           provider.Last.Code == expected && provider.Last.Source.Asset == 0xb123456789abcdefULL &&
           provider.Last.Source.Execution == world.Execution && provider.Last.Source.Instance == 1000 &&
           provider.Last.Source.Tick == 1 && provider.Last.Source.EntitySlot == world.Doors[0].Slot &&
           provider.Last.Message[0] != '\0' && provider.Vm.GetMemory().Live == 0 &&
           Dispatch(world, &event, 1, LuauInteraction, &provider) == Result::Halted;
}
bool FaultOrdering() noexcept
{
    Provider provider;
    World world;
    if (Initialize(world) == false || Load(provider, {FAULT, sizeof(FAULT), "@fault.luau"}) == false)
    {
        return false;
    }
    const EventRecord events[] = {Event(world, 1, 3), Event(world, 0, 1, 2), Event(world, 1, 2, 10)};
    return Check(Dispatch(world, events, 3, LuauInteraction, &provider) == Result::ScriptFault && world.Faulted &&
                     world.States[0].Interactions == 2 && world.States[0].OpenRequested &&
                     world.States[1].Interactions == 0 && world.CommandCount == 1 && world.NextToken == 2 &&
                     provider.Last.Operation == OPERATION_ID && provider.Last.Source.Instance == 1001 &&
                     provider.Vm.GetMemory().Live == 0 && Apply(world) == Result::Halted,
                 "fault-discards-candidate-and-stops-tick");
}
int32 StressArguments(lua_State* state) noexcept
{
    // Small calls can allocate entirely from existing Luau pages. Force
    // protected argument-stack growth so this acceptance sweep is non-vacuous.
    lua_rawcheckstack(state, 8192);
    return PushArguments(state);
}
bool AllocationSweep() noexcept
{
    usize attempts = 0;
    usize start = 0;
    {
        Provider baseline;
        World world;
        if (Initialize(world) == false || Load(baseline, {ALLOCATION, sizeof(ALLOCATION), "@allocation.luau"}) == false)
        {
            return false;
        }
        baseline.Arguments = StressArguments;
        start = baseline.Vm.GetMemory().Attempts;
        const EventRecord event = Event(world, 0, 1, 2);
        if (Dispatch(world, &event, 1, LuauInteraction, &baseline) != Result::Accepted)
        {
            return false;
        }
        attempts = baseline.Vm.GetMemory().Attempts;
    }
    for (usize fail_at = start + 1; fail_at <= attempts; ++fail_at)
    {
        Provider provider;
        World world;
        if (Initialize(world) == false || Load(provider, {ALLOCATION, sizeof(ALLOCATION), "@allocation.luau"}) == false)
        {
            return false;
        }
        provider.Arguments = StressArguments;
        provider.Vm.SetAllocationFailure(fail_at);
        const EventRecord event = Event(world, 0, 1, 2);
        if (Dispatch(world, &event, 1, LuauInteraction, &provider) != Result::AllocationFailure ||
            world.States[0].Interactions != 0 || world.CommandCount != 0 || world.NextToken != 1 ||
            world.Faulted == false || provider.Vm.GetMemory().Live != 0 || provider.Vm.GetMemory().Denied == 0)
        {
            return Check(false, "invocation-allocation-failure-sweep");
        }
    }
    LUDUS_LOG_INFO(foundation::logging::LogCategory{"ScriptingS1"},
                   "S1 METRIC invocation-allocation-points={}",
                   attempts - start);
    return Check(attempts > start, "invocation-allocation-failure-sweep");
}
bool ExpiredState() noexcept
{
    Provider provider;
    World world;
    if (Initialize(world) == false ||
        Load(provider, {EXPIRED_STATE, sizeof(EXPIRED_STATE), "@expired_state.luau"}) == false)
    {
        return false;
    }
    const EventRecord event = Event(world, 0, 1);
    return Check(Dispatch(world, &event, 1, LuauInteraction, &provider) == Result::Accepted &&
                     Dispatch(world, &event, 1, LuauInteraction, &provider) == Result::ScriptFault &&
                     world.States[0].Interactions == 0 && provider.Last.NativeStatus == 4,
                 "retained-state-facade-expires");
}
struct Reentry
{
    Runtime* Vm;
    Transaction* Txn;
    bool Rejected = false;
};
int32 Reenter(lua_State* state) noexcept
{
    auto& call = runtime::scripting::GetCallContext(state);
    auto* test = static_cast<Reentry*>(call.User);
    Diagnostic nested;
    const bool invoke =
        test->Vm->Invoke(Origin(*test->Txn), test->Txn, PushArguments, nested) == RuntimeStatus::Reentrant;
    const bool load = test->Vm->Load(DOOR, sizeof(DOOR), "@door.luau", Install) == RuntimeStatus::Reentrant;
    test->Vm->Close();
    test->Rejected = invoke && load;
    call.User = test->Txn;
    return PushArguments(state);
}
bool Reentrancy() noexcept
{
    Provider provider;
    World world;
    if (Initialize(world) == false || Load(provider, {DOOR, sizeof(DOOR), "@door.luau"}) == false)
    {
        return false;
    }
    Transaction txn
    {
        .Owner = &world,
        .Configuration = world.Configuration,
        .Event = Event(world, 0, 1).Value,
        .Instance = 1000,
    };
    Reentry test{ .Vm = &provider.Vm, .Txn = &txn };
    return Check(provider.Vm.Invoke(Origin(txn), &test, Reenter, provider.Last) == RuntimeStatus::Completed &&
                     test.Rejected && txn.CandidateState.Interactions == 1,
                 "runtime-reentrancy-rejected");
}
bool BoundaryOutcomes() noexcept
{
    Provider provider;
    World world;
    if (Initialize(world) == false || Load(provider, {BLOCKED, sizeof(BLOCKED), "@blocked.luau"}) == false)
    {
        return false;
    }
    const EntityRef valid = Reference(world, 0);
    EntityRef invalid[] = {valid, valid, valid, valid, valid};
    ++invalid[0].World;
    ++invalid[1].Session;
    ++invalid[2].Execution;
    invalid[3].Slot = ~uint32{0};
    ++invalid[4].Generation;
    bool passed = true;
    for (const EntityRef ref : invalid)
    {
        Transaction txn
        {
            .Owner = &world,
            .Configuration = world.Configuration,
            .Event = { .Target = ref, .Alias = valid, .Amount = 1 },
            .Instance = 1000,
        };
        passed &= LuauInteraction(txn, &provider) == Result::Accepted && txn.PendingCount == 0;
    }
    Transaction txn
    {
        .Owner = &world,
        .Configuration = world.Configuration,
        .Event = Event(world, 0, 1).Value,
        .Instance = 1000,
    };
    txn.Allowed = Capability::None;
    passed &= LuauInteraction(txn, &provider) == Result::Accepted && txn.PendingCount == 0;
    txn.Allowed = Capability::DoorControl;
    world.At = Phase::Intent;
    passed &= LuauInteraction(txn, &provider) == Result::Accepted && txn.PendingCount == 0;
    passed &= provider.Last.Operation == OPERATION_ID && provider.Last.Source.Session == world.Session &&
              provider.Last.Source.World == world.Entities.GetWorld();
    return Check(passed, "luau-explicit-rejection-outcomes");
}
bool FrozenProjections() noexcept
{
    bool passed = true;
    for (uint32 choice = 1; choice <= 3; ++choice)
    {
        Provider provider;
        World world;
        if (Initialize(world) == false || Load(provider, {IMMUTABLE, sizeof(IMMUTABLE), "@immutable.luau"}) == false)
        {
            return false;
        }
        const EventRecord event = Event(world, 0, 1, choice);
        passed &= Dispatch(world, &event, 1, LuauInteraction, &provider) == Result::ScriptFault &&
                  world.States[0].Interactions == 0 && world.CommandCount == 0 && world.NextToken == 1;
    }
    return Check(passed, "frozen-event-api-result-projections");
}
bool NonStringError() noexcept
{
    Provider provider;
    World world;
    if (Initialize(world) == false || Load(provider, {NON_STRING, sizeof(NON_STRING), "@non_string.luau"}) == false)
    {
        return false;
    }
    const EventRecord event = Event(world, 0, 1);
    return Check(Dispatch(world, &event, 1, LuauInteraction, &provider) == Result::ScriptFault &&
                     provider.Last.Code == RuntimeStatus::ScriptFault && provider.Last.Message[0] == '\0' &&
                     world.States[0].Interactions == 0 && world.Faulted && provider.Vm.GetMemory().Live == 0,
                 "non-string-error-copy-is-bounded");
}
bool Semantics() noexcept
{
    Provider provider;
    bool passed = Load(provider, {DOOR, sizeof(DOOR), "@door.luau"}) && CommonAcceptance(LuauInteraction, &provider) &&
                  DomainAcceptance();
    passed &= FaultOrdering();
    Provider equivalent_fault;
    passed &= Load(equivalent_fault, {FAULT, sizeof(FAULT), "@fault.luau"}) &&
              CommonFaultAcceptance(LuauInteraction, &equivalent_fault);
    passed &= AllocationSweep();
    passed &= ExpiredState();
    passed &= Reentrancy();
    passed &= BoundaryOutcomes();
    passed &= FrozenProjections();
    passed &= NonStringError();
    passed &= Check(FaultCase({READONLY, sizeof(READONLY), "@readonly.luau"}, RuntimeStatus::ScriptFault),
                    "readonly-config-preserves-candidate");
    bool boundaries = true;
    for (const Program program : INVALID_PROGRAMS)
    {
        boundaries &= Check(FaultCase(program, RuntimeStatus::NativeRejected), program.Name);
    }
    passed &= Check(boundaries, "numeric-and-forged-reference-boundaries");
    Provider interrupted;
    World world;
    passed &= Initialize(world) && Load(interrupted, {INTERRUPT, sizeof(INTERRUPT), "@interrupt.luau"});
    interrupted.Vm.SetSafepointLimit(32);
    const EventRecord event = Event(world, 0, 1);
    passed &= Check(Dispatch(world, &event, 1, LuauInteraction, &interrupted) == Result::Interrupted && world.Faulted &&
                        world.States[0].Interactions == 0 && world.CommandCount == 0 &&
                        interrupted.Last.Code == RuntimeStatus::Interrupted,
                    "interrupt-preserves-candidate");
    Provider allowlist;
    World isolated;
    passed &= Initialize(isolated) && Load(allowlist, {ALLOWLIST, sizeof(ALLOWLIST), "@allowlist.luau"});
    const EventRecord harmless = Event(isolated, 0, 1);
    passed &= Check(Dispatch(isolated, &harmless, 1, LuauInteraction, &allowlist) == Result::Accepted,
                    "restricted-frozen-environment");
    return passed;
}
} // namespace ludus::s1
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
    const bool success = ludus::s1::Semantics();
    LogSystem::Shutdown();
    return success ? 0 : 1;
}
