// Thanks to Roblox Corporation, Luau's VM protected-call contract at
// 1eca9fda3e4753a1592000f6cfdf659aaa778b7d (VM/src/ldo.cpp, lapi.cpp),
// https://github.com/luau-lang/luau/tree/1eca9fda3e4753a1592000f6cfdf659aaa778b7d
// and "Embedding a sandboxed Luau virtual machine", https://luau.org/sandbox/.
// We use POD trampolines, a bounded allocator, frozen globals and an explicit
// library allowlist. The reviewed loader ownership adjustment is documented in
// docs/architecture/luau-s0.md; this is not a general untrusted-mod sandbox.
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>

#include <cstring>
#include <type_traits>

#include <lua.h>
#include <lualib.h>

#include "internal/runtime.h"

#if !LUA_USE_LONGJMP || defined(__cpp_exceptions)
#    error S1 requires the reviewed exception-free Luau interpreter profile
#endif

namespace ludus::runtime::scripting
{
// Thanks to Roblox/Luau, "VM/src/lmem.cpp" at the pinned revision above:
// frealloc must never reject a shrink. Retain physical capacity until growth or
// free, counting headers and old+new storage against the physical heap cap.
// See docs/architecture/behavior-s6.md for the allocator/resource audit.
namespace
{
constexpr usize ALIGNMENT = 16;
struct alignas(ALIGNMENT) Allocation
{
    usize Bytes;
};
static_assert(sizeof(Allocation) == ALIGNMENT);
static_assert(std::is_trivially_destructible_v<Allocation>);
static_assert(std::is_trivially_copyable_v<Allocation>);
} // namespace

// Luau fixes its C allocator parameter order. Distinct wrapper types cannot
// change this external signature.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void* AllocateVm(void* user, void* block, usize old_size, usize new_size) noexcept
{
    auto* memory = static_cast<Memory*>(user);
    const auto& domain = foundation::GetSystemAllocationDomain();
    auto* previous = block != nullptr ? static_cast<Allocation*>(block) - 1 : nullptr;
    const usize old_bytes = previous != nullptr ? previous->Bytes : 0;
    if (new_size == 0)
    {
        memory->Live -= old_bytes;
        domain.Free(previous, old_bytes, ALIGNMENT);
        return nullptr;
    }
    if (previous != nullptr && new_size <= old_bytes - sizeof(Allocation))
    {
        return block; // Shrink/equal-size and retained-capacity reuse cannot fail.
    }
    ++memory->Attempts;
    if (new_size > ~usize{0} - sizeof(Allocation))
    {
        ++memory->Denied;
        return nullptr;
    }
    const usize bytes = new_size + sizeof(Allocation);
    if ((memory->FailAt != 0 && memory->Attempts >= memory->FailAt) || memory->Live > memory->Limit ||
        bytes > memory->Limit - memory->Live)
    {
        ++memory->Denied;
        return nullptr;
    }
    void* raw = domain.TryAllocate(bytes, ALIGNMENT);
    if (raw == nullptr)
    {
        ++memory->Denied;
        return nullptr;
    }
    const Allocation allocation{bytes};
    std::memcpy(raw, &allocation, sizeof(allocation)); // Starts the trivial header's lifetime.
    void* output = static_cast<Allocation*>(raw) + 1;
    const usize peak = memory->Live + bytes;
    if (peak > memory->Peak)
    {
        memory->Peak = peak;
    }
    if (block != nullptr)
    {
        std::memcpy(output, block, old_size);
        domain.Free(previous, old_bytes, ALIGNMENT);
    }
    memory->Live = memory->Live - old_bytes + bytes;
    return output;
}

namespace
{
static_assert(std::is_trivially_destructible_v<CallContext>);
static_assert(std::is_trivially_destructible_v<Identity>);

void Interrupt(lua_State* state, int32 gc) noexcept
{
    CallContext& context = GetCallContext(state);
    if (gc < 0 && ++context.Safepoints >= context.SafepointLimit)
    {
        context.Interrupted = true;
        luaL_error(state, "scripting safepoint limit exceeded");
    }
}

void CopyError(lua_State* state, Diagnostic& diagnostic) noexcept
{
    if (lua_type(state, -1) == LUA_TSTRING)
    {
        usize bytes = 0;
        const char* message = lua_tolstring(state, -1, &bytes);
        if (bytes >= sizeof(diagnostic.Message))
        {
            bytes = sizeof(diagnostic.Message) - 1;
        }
        std::memcpy(diagnostic.Message, message, bytes);
        diagnostic.Message[bytes] = '\0';
    }
}
} // namespace

CallContext& GetCallContext(lua_State* state) noexcept
{
    return *static_cast<CallContext*>(lua_callbacks(state)->userdata);
}

void Reject(lua_State* state, uint32 reason, const char* message) noexcept
{
    GetCallContext(state).NativeStatus = reason;
    luaL_error(state, "%s", message);
}

int32 Runtime::Initialize(lua_State* state) noexcept
{
    auto* runtime = static_cast<Runtime*>(lua_touserdata(state, 1));
    luaopen_base(state);
    lua_settop(state, 0);
    // Copy only the reviewed base functions into a new environment. Upstream
    // additions cannot silently widen this profile. No script protection can
    // catch our safepoint interruption. See behavior-s6.md for the source audit.
    constexpr const char* ALLOWED[] = {
        "assert",
        "error",
        "getmetatable",
        "ipairs",
        "next",
        "pairs",
        "rawequal",
        "rawget",
        "rawlen",
        "select",
        "tonumber",
        "tostring",
        "type",
        "typeof",
        "_VERSION",
    };
    lua_createtable(state, 0, 16);
    for (const char* name : ALLOWED)
    {
        lua_getglobal(state, name);
        lua_setfield(state, -2, name);
    }
    lua_pushvalue(state, -1);
    lua_setfield(state, -2, "_G");
    lua_replace(state, LUA_GLOBALSINDEX);
    if (runtime->mExecution.Installer != nullptr)
    {
        runtime->mExecution.Installer(state);
    }
    if (runtime->mPrograms[0].Asset != 0)
    {
        lua_pushcfunction(state, Import, "require");
        lua_setglobal(state, "require");
    }
    luaL_sandbox(state);
    for (usize i = 0; i < runtime->mProgramCount; ++i)
    {
        runtime->mLoading = i;
        const Program& program = runtime->mPrograms[i];
        const usize denied = runtime->mMemory.Denied;
        if (luau_load(state, program.Source, reinterpret_cast<const char*>(program.Code), program.Bytes, 0) != 0)
        {
            // Thanks to Roblox/Luau, VM/src/lvmload.cpp at the pinned revision:
            // luau_load recovers OOM and returns 1; lua_error re-raises it as a
            // runtime error. Preserve the explicit allocation cause separately.
            runtime->mLoadAllocationFailure = runtime->mMemory.Denied != denied;
            lua_error(state);
        }
        lua_call(state, 0, 1);
        if (lua_type(state, -1) != (program.Entrypoint ? LUA_TFUNCTION : LUA_TTABLE))
        {
            luaL_error(state, "invalid program export");
        }
        if (program.Entrypoint == false)
        {
            // S2 admits flat immutable constants; reject nested mutable exports.
            lua_pushnil(state);
            while (lua_next(state, -2) != 0)
            {
                const int32 kind = lua_type(state, -1);
                if (kind != LUA_TBOOLEAN && kind != LUA_TNUMBER && kind != LUA_TSTRING)
                {
                    luaL_error(state, "module exports must be primitive immutable constants");
                }
                lua_pop(state, 1);
            }
            lua_setreadonly(state, -1, 1);
        }
        runtime->mReferences[i] = lua_ref(state, -1);
        lua_pop(state, 1);
    }
    return 0;
}

int32 Runtime::Execute(lua_State* state) noexcept
{
    auto* runtime = static_cast<Runtime*>(lua_touserdata(state, 1));
    lua_settop(state, 0);
    lua_getref(state, runtime->mReferences[runtime->mSelected]);
    const int32 arguments = runtime->mExecution.Arguments(state);
    lua_call(state, arguments, 0);
    return 0;
}

int32 Runtime::Import(lua_State* state) noexcept
{
    auto* runtime = static_cast<Runtime*>(GetCallContext(state).DebugOwner);
    if (GetCallContext(state).Active || lua_gettop(state) != 1 || lua_type(state, 1) != LUA_TSTRING)
    {
        Reject(state, 7, "imports are literal catalog identities during initialization only");
    }
    usize length = 0;
    const char* text = lua_tolstring(state, 1, &length);
    if (length != 16)
    {
        Reject(state, 7, "invalid import identity");
    }
    uint64 asset = 0;
    for (usize i = 0; i < length; ++i)
    {
        const char digit = text[i];
        if ((digit < '0' || digit > '9') && (digit < 'a' || digit > 'f'))
        {
            Reject(state, 7, "invalid import identity");
        }
        asset = (asset << 4U) | static_cast<uint64>(digit <= '9' ? digit - '0' : digit - 'a' + 10);
    }
    const Program& current = runtime->mPrograms[runtime->mLoading];
    bool allowed = false;
    for (uint8 i = 0; i < current.DependencyCount; ++i)
    {
        allowed |= current.Dependencies[i] == asset;
    }
    if (allowed)
    {
        for (usize i = 0; i < runtime->mLoading; ++i)
        {
            if (runtime->mPrograms[i].Asset == asset)
            {
                lua_getref(state, runtime->mReferences[i]);
                return 1;
            }
        }
    }
    Reject(state, 7, "undeclared or unavailable import");
    return 0;
}

int32 Runtime::PrepareThread(lua_State* state) noexcept
{
    auto* runtime = static_cast<Runtime*>(lua_touserdata(state, 1));
    runtime->mThread = lua_newthread(state);
    runtime->mThreadRef = lua_ref(state, -1);
    lua_getref(runtime->mThread, runtime->mReferences[runtime->mSelected]);
    if (runtime->mExecution.Arguments(runtime->mThread) != 4)
    {
        luaL_error(state, "handler requires four arguments");
    }
    return 0;
}

namespace
{
template <usize N>
void Copy(char (&output)[N], const char* input) noexcept
{
    usize i = 0;
    if (input != nullptr)
    {
        for (; i + 1 < N && input[i] != '\0'; ++i)
        {
            output[i] = input[i];
        }
    }
    output[i] = '\0';
}
} // namespace

// Thanks to Roblox Corporation, pinned lua.h/ldebug.cpp and Luau C API
// "Debugging", https://luau.org/api/: source stops use actual VM break/step
// hooks. Snapshots copy bounded primitives, never evaluate user metamethods.
void Runtime::Capture(lua_State* state) noexcept
{
    const uint64 stop = mSnapshot.Stop;
    mSnapshot = {};
    mSnapshot.Stop = stop == ~uint64{0} ? stop : stop + 1;
    mSnapshot.Depth = lua_stackdepth(state);
    for (int32 level = 0; level < 8; ++level)
    {
        lua_Debug info = {};
        if (lua_getinfo(state, level, "sln", &info) == 0)
        {
            break;
        }
        DebugFrame& frame = mSnapshot.Frames[mSnapshot.FrameCount++];
        Copy(frame.Source, info.source);
        Copy(frame.Function, info.name);
        frame.Line = info.currentline;
    }
    mSnapshot.Truncated = mSnapshot.Depth > 8;
    for (int32 i = 1; i <= 17; ++i)
    {
        const char* name = lua_getlocal(state, 0, i);
        if (name == nullptr)
        {
            break;
        }
        if (i == 17)
        {
            mSnapshot.Truncated = true;
            lua_pop(state, 1);
            break;
        }
        DebugValue& value = mSnapshot.Locals[mSnapshot.LocalCount++];
        Copy(value.Name, name);
        switch (lua_type(state, -1))
        {
            case LUA_TNIL:
                value.Kind = DebugKind::Nil;
                break;
            case LUA_TBOOLEAN:
                value.Kind = DebugKind::Boolean;
                value.Boolean = lua_toboolean(state, -1) != 0;
                break;
            case LUA_TNUMBER:
                value.Number = lua_tonumber(state, -1);
                value.Kind = foundation::math::IsFinite(value.Number) ? DebugKind::Number : DebugKind::Nonfinite;
                break;
            case LUA_TSTRING: {
                usize bytes = 0;
                const char* text = lua_tolstring(state, -1, &bytes);
                value.Kind = DebugKind::StringBytes;
                value.Truncated = bytes > 64;
                if (bytes > 64)
                {
                    bytes = 64;
                }
                constexpr char HEX[] = "0123456789abcdef";
                for (usize j = 0; j < bytes; ++j)
                {
                    const auto byte = static_cast<uint8>(text[j]);
                    value.Bytes[j * 2] = HEX[byte >> 4U];
                    value.Bytes[j * 2 + 1] = HEX[byte & 15U];
                }
                break;
            }
            default:
                value.Kind = DebugKind::Opaque;
                break;
        }
        lua_pop(state, 1);
    }
}

void Runtime::OnBreak(lua_State* state, lua_Debug* debug) noexcept
{
    auto* runtime = static_cast<Runtime*>(GetCallContext(state).DebugOwner);
    if (runtime->mDebugEnabled && GetCallContext(state).Active)
    {
        lua_Debug info = {};
        (void)lua_getinfo(state, 0, "s", &info);
        const bool skip = runtime->mSkipBreak && debug->currentline == runtime->mStepLine &&
                          lua_stackdepth(state) == runtime->mStepDepth &&
                          std::strcmp(info.source == nullptr ? "" : info.source, runtime->mStepSource) == 0;
        runtime->mSkipBreak = false;
        if (skip)
        {
            return;
        } // Resume executes the stopped instruction once.

        runtime->Capture(state);
        lua_break(state);
    }
}

void Runtime::OnStep(lua_State* state, lua_Debug* debug) noexcept
{
    auto* runtime = static_cast<Runtime*>(GetCallContext(state).DebugOwner);
    if (runtime->mDebugEnabled == false || runtime->mMode == ResumeMode::Continue)
    {
        return;
    }
    const int32 depth = lua_stackdepth(state);
    lua_Debug info = {};
    (void)lua_getinfo(state, 0, "s", &info);
    const bool different = debug->currentline != runtime->mStepLine || depth != runtime->mStepDepth ||
                           std::strcmp(info.source == nullptr ? "" : info.source, runtime->mStepSource) != 0;
    if (different &&
        (runtime->mMode == ResumeMode::Into || (runtime->mMode == ResumeMode::Over && depth <= runtime->mStepDepth) ||
         (runtime->mMode == ResumeMode::Out && depth < runtime->mStepDepth)))
    {
        runtime->Capture(state);
        lua_break(state);
    }
}

Status Runtime::Resume(ResumeMode mode, Diagnostic& diagnostic) noexcept
{
    if (mEntered)
    {
        return Status::Reentrant;
    }
    if (mPaused == false || mThread == nullptr)
    {
        return Status::NotReady;
    }
    mMode = mode;
    mSkipBreak = true;
    mStepDepth = mSnapshot.Depth;
    mStepLine = mSnapshot.Frames[0].Line;
    Copy(mStepSource, mSnapshot.Frames[0].Source);
    lua_singlestep(mThread, mode == ResumeMode::Continue ? 0 : 1);
    mEntered = true;
    const int32 status = lua_resume(mThread, nullptr, 0);
    mEntered = false;
    return Finish(status, mThread, diagnostic);
}

int32 Runtime::SetBreakpoint(lua_State* state) noexcept
{
    auto* runtime = static_cast<Runtime*>(lua_touserdata(state, 1));
    lua_getref(state, runtime->mReferences[runtime->mSelected]);
    runtime->mBreakpointLine = lua_breakpoint(state, -1, runtime->mBreakpointLine, runtime->mBreakpointEnabled ? 1 : 0);
    return 0;
}

int32 Runtime::Breakpoint(BreakpointSpec point) noexcept
{
    if (mEntered || mState == nullptr || mDebugEnabled == false || point.Line < 1)
    {
        return -1;
    }
    for (usize i = 0; i < mProgramCount; ++i)
    {
        if (mPrograms[i].Asset == point.Asset && mPrograms[i].Entrypoint)
        {
            mSelected = i;
            mBreakpointLine = point.Line;
            mBreakpointEnabled = point.Enabled;
            mEntered = true;
            const int32 status = lua_cpcall(mState, SetBreakpoint, this);
            mEntered = false;
            if (status != LUA_OK)
            {
                Close();
                return -1;
            }
            return mBreakpointLine;
        }
    }
    return -1;
}

Runtime::~Runtime() noexcept
{
    Close();
}

void Runtime::Close() noexcept
{
    if (mEntered)
    {
        return; // Never destroy an executing VM through a nested native call.
    }
    if (mState != nullptr)
    {
        lua_close(mState);
    }
    mState = nullptr;
    mThread = nullptr;
    mThreadRef = -1;
    mPaused = false;
    mSkipBreak = false;
    mContext.User = nullptr;
    mContext.Active = false;
}

Status Runtime::Load(const uint8* bytecode, usize bytes, const char* source, Setup installer) noexcept
{
    const Program program{ .Code = bytecode, .Bytes = bytes, .Source = source };
    return LoadPrograms(&program, 1, installer);
}

Status Runtime::LoadPrograms(const Program* programs, usize count, Setup installer, void* setup_user) noexcept
{
    if (mEntered || mPaused)
    {
        return Status::Reentrant;
    }
    Close();
    if (programs == nullptr || count == 0 || count > 8)
    {
        return Status::InvalidArtifact;
    }
    for (usize i = 0; i < count; ++i)
    {
        const Program& program = programs[i];
        if (program.Code == nullptr || program.Bytes == 0 || program.Source == nullptr || program.DependencyCount > 8)
        {
            return Status::InvalidArtifact;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (program.Asset == programs[j].Asset)
            {
                return Status::InvalidArtifact;
            }
        }
        for (uint8 j = 0; j < program.DependencyCount; ++j)
        {
            bool found = false;
            for (usize k = 0; k < i; ++k)
            {
                found |= programs[k].Entrypoint == false && programs[k].Asset == program.Dependencies[j];
            }
            if (found == false)
            {
                return Status::InvalidArtifact;
            }
        }
        mPrograms[i] = program;
    }
    mProgramCount = count;
    mExecution = { .Installer = installer };
    mContext.DebugOwner = this;
    mContext.User = setup_user;
    mSnapshot = {};
    mContext.Safepoints = 0;
    mContext.Interrupted = false;
    mContext.NativeStatus = 0;
    mLoadAllocationFailure = false;
    mState = lua_newstate(AllocateVm, &mMemory);
    if (mState == nullptr)
    {
        return Status::AllocationFailure;
    }
    lua_callbacks(mState)->userdata = &mContext;
    lua_callbacks(mState)->interrupt = Interrupt;
    lua_callbacks(mState)->debugbreak = OnBreak;
    lua_callbacks(mState)->debugstep = OnStep;
    mEntered = true;
    const int32 status = lua_cpcall(mState, Initialize, this);
    mEntered = false;
    if (status == LUA_OK)
    {
        return Status::Completed;
    }
    const Status result = status == LUA_ERRMEM || mLoadAllocationFailure
                              ? Status::AllocationFailure
                              : (mContext.Interrupted ? Status::Interrupted : Status::ScriptFault);
    Close();
    return result;
}

Status Runtime::Invoke(Identity identity, void* user, Setup arguments, Diagnostic& diagnostic) noexcept
{
    if (mEntered || mPaused)
    {
        return Status::Reentrant;
    }
    diagnostic = {};
    diagnostic.Source = identity;
    if (mState == nullptr || arguments == nullptr || user == nullptr || mContext.Epoch == ~uint64{0})
    {
        diagnostic.Code = Status::NotReady;
        return diagnostic.Code;
    }
    bool found = false;
    for (usize i = 0; i < mProgramCount; ++i)
    {
        if (mPrograms[i].Entrypoint && (mPrograms[i].Asset == identity.Asset || mPrograms[i].Asset == 0))
        {
            mSelected = i;
            found = true;
            break;
        }
    }
    if (found == false)
    {
        diagnostic.Code = Status::InvalidArtifact;
        return diagnostic.Code;
    }
    mIdentity = identity;
    mSkipBreak = false;
    mMode = ResumeMode::Continue;
    ++mContext.Epoch;
    mContext.User = user;
    mContext.Active = true;
    mContext.Safepoints = 0;
    mContext.Interrupted = false;
    mContext.Operation = 0;
    mContext.NativeStatus = 0;
    mExecution.Arguments = arguments;
    mEntered = true;
    int32 status = lua_cpcall(mState, mDebugEnabled ? PrepareThread : Execute, this);
    lua_State* errors = mState;
    if (status == LUA_OK && mDebugEnabled)
    {
        errors = mThread;
        status = lua_resume(mThread, nullptr, 4);
    }
    mEntered = false;
    return Finish(status, errors, diagnostic);
}

Status Runtime::Finish(int32 status, lua_State* state, Diagnostic& diagnostic) noexcept
{
    diagnostic = {};
    diagnostic.Source = mIdentity;
    diagnostic.Operation = mContext.Operation;
    diagnostic.NativeStatus = mContext.NativeStatus;
    if (status == LUA_BREAK)
    {
        mPaused = true;
        diagnostic.Code = Status::Paused;
        return diagnostic.Code;
    }
    mPaused = false;
    mContext.Active = false;
    mContext.User = nullptr;
    diagnostic.Operation = mContext.Operation;
    diagnostic.NativeStatus = mContext.NativeStatus;
    if (status != LUA_OK)
    {
        diagnostic.Code =
            status == LUA_ERRMEM
                ? Status::AllocationFailure
                : (mContext.Interrupted ? Status::Interrupted
                                        : (mContext.NativeStatus != 0 ? Status::NativeRejected : Status::ScriptFault));
        CopyError(state, diagnostic);
        Close();
    }
    else if (mThreadRef != -1)
    {
        lua_unref(mState, mThreadRef);
        mThreadRef = -1;
        mThread = nullptr;
    }
    return diagnostic.Code;
}
} // namespace ludus::runtime::scripting
