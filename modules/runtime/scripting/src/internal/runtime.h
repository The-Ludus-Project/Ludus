#pragma once
#include <ludus/foundation/base/core.h>

#include "internal/debug.h"

struct lua_State;
struct lua_Debug;

namespace ludus::runtime::scripting
{
using namespace foundation;

// Private S1 surface; owner-thread only. No VM/native pointer enters a saved
// gameplay record. See docs/architecture/luau-s1.md for ownership and limitations.
enum class Status : uint8
{
    Completed,
    InvalidArtifact,
    NotReady,
    Reentrant,
    ScriptFault,
    NativeRejected,
    AllocationFailure,
    Interrupted,
    Paused
};
struct Identity
{
    uint64 Asset = 0;
    uint64 Revision = 0;
    uint64 Execution = 0;
    uint64 Instance = 0;
    uint64 World = 0;
    uint64 Session = 0;
    uint64 Tick = 0;
    uint8 Phase = 0;
    uint32 EntitySlot = 0;
    uint32 EntityGeneration = 0;
    char Entrypoint[32] = "OnInteract";
};
struct Diagnostic
{
    Status Code = Status::Completed;
    Identity Source;
    uint32 Operation = 0;
    uint32 NativeStatus = 0;
    char Message[192] = {};
};
struct Memory
{
    usize Live = 0;
    usize Peak = 0;
    usize Attempts = 0;
    usize FailAt = 0;
    usize Denied = 0;
    usize Limit = usize{8} * 1024 * 1024;
};
// Private allocator boundary, shared with its direct contract tests. Physical
// accounting includes retained payload capacity and allocator headers.
[[nodiscard]] void* AllocateVm(void* user, void* block, usize old_size, usize new_size) noexcept;
// This record, every generated binding local, and every crossed VM frame must
// stay trivially destructible. Native services finish before the next VM call.
struct CallContext
{
    void* User = nullptr;
    uint64 Epoch = 0;
    bool Active = false;
    bool Interrupted = false;
    uint32 Safepoints = 0;
    uint32 SafepointLimit = 100000;
    uint32 Operation = 0;
    uint32 NativeStatus = 0;
    void* DebugOwner = nullptr;
};
using Setup = int32 (*)(lua_State*) noexcept;
[[nodiscard]] CallContext& GetCallContext(lua_State* state) noexcept;
// Records a contract violation before entering protected VM error recovery.
void Reject(lua_State* state, uint32 reason, const char* message) noexcept;

class Runtime final
{
public:
    Runtime() noexcept = default;
    ~Runtime() noexcept;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    // Trusted bytecode from the paired cooker only. Loading closes any previous
    // program; immutable package replacement belongs to S2. Installer pushes no
    // return values. Program must return one non-yielding handler function.
    [[nodiscard]] Status Load(const uint8* bytecode, usize bytes, const char* source, Setup installer) noexcept;
    // Dependency-first catalog, at most eight entries. Input/code/source storage
    // is immutable and borrowed until Close. Initializers have no active services.
    [[nodiscard]] Status
    LoadPrograms(const Program* programs, usize count, Setup installer, void* setup_user = nullptr) noexcept;
    // Arguments pushes config/state/event/API. All effects live in User's POD
    // candidate. Fault destroys this VM and requires an explicit new Load.
    [[nodiscard]] Status Invoke(Identity identity, void* user, Setup arguments, Diagnostic& diagnostic) noexcept;
    // A paused invocation retains its borrowed POD candidate until completion or
    // Close. No other invocation/load may enter; the owner pumps its event loop.
    [[nodiscard]] Status Resume(ResumeMode mode, Diagnostic& diagnostic) noexcept;
    [[nodiscard]] int32 Breakpoint(BreakpointSpec point) noexcept;
    [[nodiscard]] bool IsPaused() const noexcept
    {
        return mPaused;
    }
    [[nodiscard]] const DebugSnapshot& Inspect() const noexcept
    {
        return mSnapshot;
    }
    void EnableDebugger(bool enabled) noexcept
    {
        if (mEntered == false && mPaused == false)
        {
            mDebugEnabled = enabled;
        }
    }
    void Close() noexcept;
    [[nodiscard]] const Memory& GetMemory() const noexcept
    {
        return mMemory;
    }
    // Private acceptance hooks, not a shipping control surface.
    void SetAllocationFailure(usize attempt) noexcept
    {
        mMemory.FailAt = attempt;
    }
    void SetHeapLimit(usize bytes) noexcept
    {
        mMemory.Limit = bytes;
    }
    void SetSafepointLimit(uint32 count) noexcept
    {
        mContext.SafepointLimit = count;
    }

private:
    struct Execution
    {
        Setup Installer = nullptr;
        Setup Arguments = nullptr;
    };
    static int32 Initialize(lua_State* state) noexcept;
    static int32 Execute(lua_State* state) noexcept;
    static int32 PrepareThread(lua_State* state) noexcept;
    static int32 SetBreakpoint(lua_State* state) noexcept;
    static int32 Import(lua_State* state) noexcept;
    static void OnBreak(lua_State* state, lua_Debug* debug) noexcept;
    static void OnStep(lua_State* state, lua_Debug* debug) noexcept;
    void Capture(lua_State* state) noexcept;
    [[nodiscard]] Status Finish(int32 status, lua_State* state, Diagnostic& diagnostic) noexcept;
    lua_State* mState = nullptr;
    Memory mMemory;
    CallContext mContext;
    Execution mExecution;
    bool mEntered = false;
    bool mLoadAllocationFailure = false;
    Program mPrograms[8];
    int32 mReferences[8] = {};
    usize mProgramCount = 0;
    usize mLoading = 0;
    usize mSelected = 0;
    lua_State* mThread = nullptr;
    int32 mThreadRef = -1;
    bool mDebugEnabled = false;
    bool mPaused = false;
    bool mSkipBreak = false;
    Identity mIdentity;
    DebugSnapshot mSnapshot;
    ResumeMode mMode = ResumeMode::Continue;
    int32 mStepDepth = 0;
    int32 mStepLine = 0;
    char mStepSource[96] = {};
    int32 mBreakpointLine = 0;
    bool mBreakpointEnabled = false;
};
} // namespace ludus::runtime::scripting
