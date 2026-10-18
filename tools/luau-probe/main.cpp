// Thanks to Roblox Corporation, Luau's pinned VM/include/lua.h, VM/src/ldo.cpp,
// VM/src/lvmload.cpp and tests/Conformance.test.cpp, for the protected-call and
// debugger contracts: https://github.com/luau-lang/luau/tree/1eca9fda3e4753a1592000f6cfdf659aaa778b7d
// We adapt these contracts to POD-only host trampolines and fail-point sweeps.
// Thanks to Waldemar Celes, Luiz Henrique de Figueiredo and Roberto Ierusalimschy,
// "Binding C/C++ Objects to Lua", Game Programming Gems 6, 4.2, pp. 341–355,
// for checking arguments at the binding boundary. This probe uses scalar values,
// never owning engine-object userdata. See docs/architecture/luau-s0.md.

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>

#include <chrono>
#include <cstdlib>
#include <type_traits>

#include <lua.h>
#include <lualib.h>

#include "fixtures.h"

#if !LUA_USE_LONGJMP
#    error S0 requires longjmp recovery, never C++ exceptions
#endif
#if defined(__cpp_exceptions)
#    error S0 interpreter and host trampolines must compile without C++ exceptions
#endif

namespace ludus::luau_probe
{
using namespace foundation;
using namespace foundation::logging;

inline constexpr LogCategory LOG_S0{"LuauS0"};
inline constexpr usize HEAP_LIMIT = usize{8} * 1024 * 1024;

struct Allocator
{
    usize Live = 0;
    usize Peak = 0;
    usize Attempts = 0;
    usize FailAt = 0;
    usize Denied = 0;
};

// No RAII, locks, containers, engine services or diagnostics in any callback or
// trampoline. Nonlocal recovery only crosses trivially destructible host frames.
// Luau fixes the parameter order in lua_Alloc; it cannot use distinct wrappers.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void* Allocate(void* userdata, void* block, usize old_size, usize new_size) noexcept
{
    auto* allocator = static_cast<Allocator*>(userdata);
    if (new_size == 0)
    {
        allocator->Live -= old_size;
        std::free(block);
        return nullptr;
    }
    ++allocator->Attempts;
    const usize retained = allocator->Live - old_size;
    if ((allocator->FailAt != 0 && allocator->Attempts >= allocator->FailAt) || new_size > HEAP_LIMIT - retained)
    {
        ++allocator->Denied;
        return nullptr; // Rejected realloc leaves the original allocation intact.
    }
    void* result = std::realloc(block, new_size);
    if (result != nullptr)
    {
        allocator->Live = retained + new_size;
        if (allocator->Live > allocator->Peak)
        {
            allocator->Peak = allocator->Live;
        }
    }
    return result;
}

struct Context
{
    const uint8* Bytecode = nullptr;
    usize Bytes = 0;
    const char* Name = nullptr;
    int32 LoadStatus = -1;
    int32 Result = -1;
    uint32 Interrupts = 0;
    uint32 InterruptLimit = 0;
    uint32 BindingCalls = 0;
    lua_State* Thread = nullptr;
    int32 ThreadRef = LUA_NOREF;
    int32 BreakLine = 0;
    int32 StepLine = 0;
    int32 BreakLocal = -1;
    int32 StepLocal = -1;
    uint32 BreakHits = 0;
    uint32 StepHits = 0;
    uint32 Node = 0;
};
static_assert(std::is_trivially_destructible_v<Allocator>);
static_assert(std::is_trivially_destructible_v<Context>);

Context* GetContext(lua_State* state) noexcept
{
    return static_cast<Context*>(lua_callbacks(state)->userdata);
}

int32 HostAdd(lua_State* state) noexcept
{
    // Check exact types rather than accepting Luau's numeric string coercion.
    if (lua_gettop(state) != 2 || lua_type(state, 1) != LUA_TNUMBER || lua_type(state, 2) != LUA_TNUMBER)
    {
        luaL_error(state, "host_add expects exactly two numbers");
    }
    const float64 lhs = lua_tonumber(state, 1);
    const float64 rhs = lua_tonumber(state, 2);
    ++GetContext(state)->BindingCalls;
    lua_pushnumber(state, lhs + rhs);
    return 1;
}

void Interrupt(lua_State* state, int32 gc) noexcept
{
    Context* context = GetContext(state);
    if (gc < 0 && context->InterruptLimit != 0 && ++context->Interrupts >= context->InterruptLimit)
    {
        luaL_error(state, "s0 safepoint budget exceeded");
    }
}

int32 Initialize(lua_State* state) noexcept
{
    luaopen_base(state); // Deliberately exclude os/io/debug/require and host I/O.
    lua_settop(state, 0);
    lua_pushnil(state);
    lua_setglobal(state, "print");
    lua_pushcfunction(state, HostAdd, "host_add");
    lua_setglobal(state, "host_add");
    luaL_sandbox(state);
    return 0;
}

int32 Execute(lua_State* state) noexcept
{
    Context* context = GetContext(state);
    context->LoadStatus =
        luau_load(state, context->Name, reinterpret_cast<const char*>(context->Bytecode), context->Bytes, 0);
    if (context->LoadStatus != 0)
    {
        return 0;
    }
    lua_call(state, 0, 1);
    if (lua_type(state, -1) == LUA_TNUMBER)
    {
        context->Result = lua_tointeger(state, -1);
    }
    return 0;
}

struct Outcome
{
    bool Created = false;
    int32 InitStatus = -1;
    int32 CallStatus = -1;
    Context Execution;
    Allocator Memory;
};

struct RunLimits
{
    usize FailAt = 0;
    uint32 InterruptLimit = 0;
};

Outcome Run(const uint8* bytecode, usize bytes, const char* name, RunLimits limits = {}) noexcept
{
    Outcome outcome;
    outcome.Memory.FailAt = limits.FailAt;
    outcome.Execution.Bytecode = bytecode;
    outcome.Execution.Bytes = bytes;
    outcome.Execution.Name = name;
    outcome.Execution.InterruptLimit = limits.InterruptLimit;
    lua_State* state = lua_newstate(Allocate, &outcome.Memory);
    if (state != nullptr)
    {
        outcome.Created = true;
        lua_callbacks(state)->userdata = &outcome.Execution;
        lua_callbacks(state)->interrupt = Interrupt;
        outcome.InitStatus = lua_cpcall(state, Initialize, nullptr);
        if (outcome.InitStatus == LUA_OK)
        {
            outcome.CallStatus = lua_cpcall(state, Execute, nullptr);
        }
        // Destroy even a failed VM; no object owns native resources/finalizers.
        lua_close(state);
    }
    return outcome;
}

bool Check(bool condition, const char* name) noexcept
{
    if (condition)
    {
        LUDUS_LOG_INFO(LOG_S0, "S0 PASS {}", name);
    }
    else
    {
        LUDUS_LOG_ERROR(LOG_S0, "S0 FAIL {}", name);
    }
    return condition;
}

bool Sweep(const uint8* bytecode, usize bytes, const char* name, int32 expected) noexcept
{
    const Outcome baseline = Run(bytecode, bytes, name);
    if (Check(baseline.CallStatus == LUA_OK && baseline.Execution.Result == expected && baseline.Memory.Live == 0,
              name) == false)
    {
        return false;
    }
    for (usize fail_at = 1; fail_at <= baseline.Memory.Attempts; ++fail_at)
    {
        const Outcome failed = Run(bytecode, bytes, name, { .FailAt = fail_at, .InterruptLimit = 0 });
        const bool reported = failed.Created == false || failed.InitStatus == LUA_ERRMEM ||
                              failed.CallStatus == LUA_ERRMEM || failed.Execution.LoadStatus == 1;
        if (reported == false || failed.Memory.Live != 0 || failed.Memory.Denied == 0)
        {
            LUDUS_LOG_ERROR(LOG_S0,
                            "S0 FAIL allocation-point={} init={} call={} load={} live={}",
                            fail_at,
                            failed.InitStatus,
                            failed.CallStatus,
                            failed.Execution.LoadStatus,
                            failed.Memory.Live);
            return false;
        }
    }
    LUDUS_LOG_INFO(LOG_S0,
                   "S0 METRIC {} allocation-points={} heap-peak={}",
                   name,
                   baseline.Memory.Attempts,
                   baseline.Memory.Peak);
    return true;
}

void Breakpoint(lua_State* state, lua_Debug* /*debug*/) noexcept
{
    Context* context = GetContext(state);
    if (++context->BreakHits == 1)
    {
        lua_Debug info = {};
        lua_getinfo(state, 0, "l", &info);
        context->BreakLine = info.currentline;
        context->Node = info.currentline == 3 ? 1001 : 0;
        if (lua_getlocal(state, 0, 1) != nullptr)
        {
            context->BreakLocal = lua_tointeger(state, -1);
            lua_pop(state, 1);
        }
        lua_break(state);
    }
}

void Step(lua_State* state, lua_Debug* debug) noexcept
{
    Context* context = GetContext(state);
    if (debug->currentline == 4 && context->StepHits == 0)
    {
        ++context->StepHits;
        context->StepLine = debug->currentline;
        if (lua_getlocal(state, 0, 1) != nullptr)
        {
            context->StepLocal = lua_tointeger(state, -1);
            lua_pop(state, 1);
        }
        lua_break(state);
    }
}

int32 PrepareDebug(lua_State* state) noexcept
{
    Context* context = GetContext(state);
    context->Thread = lua_newthread(state);
    context->ThreadRef = lua_ref(state, -1); // Root thread before cpcall discards its stack.
    context->LoadStatus =
        luau_load(context->Thread, "@debug.luau", reinterpret_cast<const char*>(DEBUG), sizeof(DEBUG), 0);
    if (context->LoadStatus == 0)
    {
        context->BreakLine = lua_breakpoint(context->Thread, -1, 3, 1);
    }
    return 0;
}

bool DebugProbe() noexcept
{
    Allocator memory;
    Context context;
    lua_State* state = lua_newstate(Allocate, &memory);
    if (state == nullptr)
    {
        return Check(false, "debug-create");
    }
    lua_callbacks(state)->userdata = &context;
    lua_callbacks(state)->debugbreak = Breakpoint;
    lua_callbacks(state)->debugstep = Step;
    bool passed = lua_cpcall(state, Initialize, nullptr) == LUA_OK &&
                  lua_cpcall(state, PrepareDebug, nullptr) == LUA_OK && context.LoadStatus == 0;
    if (passed)
    {
        passed = lua_resume(context.Thread, nullptr, 0) == LUA_BREAK && context.BreakLine == 3 &&
                 context.Node == 1001 && context.BreakLocal == 20;
    }
    if (passed)
    {
        lua_singlestep(context.Thread, 1);
        passed =
            lua_resume(context.Thread, nullptr, 0) == LUA_BREAK && context.StepLine == 4 && context.StepLocal == 42;
    }
    if (passed)
    {
        lua_singlestep(context.Thread, 0);
        passed = lua_resume(context.Thread, nullptr, 0) == LUA_OK && lua_tointeger(context.Thread, -1) == 42;
    }
    lua_close(state);
    return Check(passed && memory.Live == 0, "break-step-locals-node-resume");
}

bool Semantics() noexcept
{
    bool passed = true;
    const Outcome basic = Run(BASIC, sizeof(BASIC), "@basic.luau");
    passed &= Check(basic.CallStatus == LUA_OK && basic.Execution.Result == 42 && basic.Execution.BindingCalls == 1 &&
                        basic.Memory.Live == 0,
                    "native-binding-and-library-profile");
    const Outcome script_error = Run(ERROR, sizeof(ERROR), "@error.luau");
    passed &= Check(script_error.CallStatus == LUA_ERRRUN && script_error.Memory.Live == 0, "protected-script-error");
    const Outcome binding_error = Run(BINDING_ERROR, sizeof(BINDING_ERROR), "@binding_error.luau");
    passed &= Check(binding_error.CallStatus == LUA_ERRRUN && binding_error.Execution.BindingCalls == 0 &&
                        binding_error.Memory.Live == 0,
                    "protected-binding-error");
    const Outcome stack = Run(STACK, sizeof(STACK), "@stack.luau");
    passed &= Check(stack.CallStatus == LUA_ERRRUN && stack.Memory.Live == 0, "protected-stack-overflow");
    const Outcome interrupt = Run(INTERRUPT,
                                  sizeof(INTERRUPT),
                                  "@interrupt.luau",
                                  {
                                      .FailAt = 0,
                                      .InterruptLimit = 64,
                                  });
    passed &=
        Check(interrupt.CallStatus == LUA_ERRRUN && interrupt.Execution.Interrupts == 64 && interrupt.Memory.Live == 0,
              "bounded-loop-interrupt");
    passed &= Sweep(CONSTANTS, sizeof(CONSTANTS), "constant-loader-oom-sweep", 6);
    passed &= Sweep(ALLOCATION, sizeof(ALLOCATION), "execution-oom-sweep", 1024);
    passed &= DebugProbe();
    // End-to-end batch timing includes VM creation, setup, load and destruction.
    // This diagnostic is not a shipping latency budget or a pure-call benchmark.
    const auto start = std::chrono::steady_clock::now();
    const Outcome dispatch = Run(DISPATCH, sizeof(DISPATCH), "@dispatch.luau");
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    passed &= Check(dispatch.CallStatus == LUA_OK && dispatch.Execution.Result == 10000 &&
                        dispatch.Execution.BindingCalls == 10000 && dispatch.Memory.Live == 0,
                    "dispatch-batch");
    LUDUS_LOG_INFO(LOG_S0,
                   "S0 METRIC dispatch-batch calls={} elapsed-ns={} heap-peak={}",
                   dispatch.Execution.BindingCalls,
                   elapsed,
                   dispatch.Memory.Peak);
    // A fresh VM must remain usable after all injected failures.
    const Outcome recovery = Run(BASIC, sizeof(BASIC), "@recovery.luau");
    passed &= Check(recovery.CallStatus == LUA_OK && recovery.Execution.Result == 42 && recovery.Memory.Live == 0,
                    "fresh-vm-recovery");
    return passed;
}
} // namespace ludus::luau_probe

int main()
{
    using namespace ludus::foundation::logging;
    LogConfig config;
    config.EnableFile = false;
    config.EnableDebugger = false;
    const auto initialized = LogSystem::Initialize(config);
    if (initialized.Status != LogStatus::Ok)
    {
        return 2;
    }
    const bool passed = ludus::luau_probe::Semantics();
    LogSystem::Shutdown();
    return passed ? 0 : 1;
}
