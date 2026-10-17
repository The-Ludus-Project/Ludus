// Thanks to Ludus's prepared GameHost reload protocol (§7/8) and the scripting
// architecture's completed-tick rule. Stage a whole fresh VM and tagged state;
// publish without allocation, then retire all old native closures. See luau-s2.md.
#include <ludus/foundation/base/core.h>

#include <ludus/foundation/base/byte_order.hpp>

#include <cstring>
#include <span>

#include "packages.h"
#include "session.h"

namespace ludus::s1
{
int32 Install(lua_State* state) noexcept;
int32 PushArguments(lua_State* state) noexcept;
} // namespace ludus::s1
namespace ludus::s2
{
namespace
{
using namespace runtime::scripting;
bool Trusted(const Package& package, std::span<const Package* const> admitted) noexcept
{
    for (const Package* entry : admitted)
    {
        if (entry == &package)
        {
            return true;
        }
    }
    return false;
}
StateRecord Capture(const s1::State& state, uint32 schema) noexcept
{
    StateRecord record;
    record.Schema = schema;
    record.Count = 2;
    record.Fields[0] =
    {
        .Id = s1::STATE_INTERACTIONS_FIELD_ID,
        .Kind = FieldKind::Uint32,
        .Value = state.Interactions,
    };
    record.Fields[1] =
    {
        .Id = s1::STATE_OPENREQUESTED_FIELD_ID,
        .Kind = FieldKind::Boolean,
        .Value = state.OpenRequested ? 1u : 0u,
    };
    return record;
}
bool RestoreState(const StateRecord& record, const Package& package, s1::State& output) noexcept
{
    const FieldSpec fields[] = {
        {
            .Id = s1::STATE_INTERACTIONS_FIELD_ID,
            .Kind = FieldKind::Uint32,
            .Min = 0,
            .Max = package.StateMax,
            .Default = 0,
        },
        { .Id = s1::STATE_OPENREQUESTED_FIELD_ID, .Kind = FieldKind::Boolean, .Min = 0, .Max = 1, .Default = 0 }};
    StateRecord migrated;
    if (!MigrateState(record, package.Schema, fields, migrated))
    {
        return false;
    }
    output = { .Interactions = migrated.Fields[0].Value, .OpenRequested = migrated.Fields[1].Value != 0 };
    return s1::Validate(output);
}
} // namespace

bool Session::Initialize(const Package& package) noexcept
{
    static constexpr const Package* CATALOG[] = {&BASE, &REPLACEMENT, &FAILED, &NARROW};
    return Initialize(package, CATALOG);
}

bool Session::Initialize(const Package& package, std::span<const Package* const> admitted) noexcept
{
    if (mActive != nullptr || admitted.empty() || admitted.size() > 8 || !Trusted(package, admitted) ||
        !s1::Initialize(mWorld))
    {
        return false;
    }
    Vm().EnableDebugger(true);
    if (Vm().LoadPrograms(package.Programs, package.Count, s1::Install) != Status::Completed)
    {
        return false;
    }
    mActive = &package;
    mCatalog = admitted;
    mWorld.At = s1::Phase::EndTick;
    return true;
}

bool Session::SafePoint() const noexcept
{
    return mActive != nullptr && !mPending && !mWorld.Faulted && mWorld.At == s1::Phase::EndTick &&
           mWorld.CommandCount == 0 && !mVms[mBank].IsPaused();
}

Status Session::Begin(InvokeRequest request) noexcept
{
    if (!SafePoint() || request.Instance >= 2 || request.Amount < 1 || request.Amount > 10)
    {
        return Status::NotReady;
    }
    Discard(); // An intervening authoritative tick supersedes staged state.
    mCandidate =
    {
        .Owner = &mWorld,
        .Configuration = mWorld.Configuration,
        .CandidateState = mWorld.States[request.Instance],
        .Event =
        {
            .Target = s1::Reference(mWorld, request.Instance),
            .Alias = s1::Reference(mWorld, request.Instance),
            .Amount = request.Amount,
        },
        .Instance = uint64{1000} + request.Instance,
        .InstanceOrder = request.Instance,
    };
    Identity identity
    {
        .Asset = uint64{0x100} + request.Instance,
        .Revision = mActive->Revision,
        .Execution = mWorld.Execution,
        .Instance = mCandidate.Instance,
        .World = mWorld.Entities.GetWorld(),
        .Session = mWorld.Session,
        .Tick = mWorld.Tick,
        .Phase = static_cast<uint8>(s1::Phase::Gameplay),
        .EntitySlot = mCandidate.Event.Target.Slot,
        .EntityGeneration = mCandidate.Event.Target.Generation,
    };
    mWorld.At = s1::Phase::Gameplay;
    mPending = true;
    return Finish(Vm().Invoke(identity, &mCandidate, s1::PushArguments, mLast));
}

Status Session::Finish(Status status) noexcept
{
    if (status == Status::Paused)
    {
        return status;
    }
    mPending = false;
    if (status == Status::Completed && mCandidate.CandidateState.Interactions <= mActive->StateMax &&
        s1::Publish(mCandidate) == s1::Result::Accepted && s1::Apply(mWorld) == s1::Result::Applied)
    {
        mWorld.At = s1::Phase::EndTick;
        ++mWorld.Tick;
        return status;
    }
    mWorld.Faulted = true;
    Vm().Close();
    return status == Status::Completed ? Status::NativeRejected : status;
}

Status Session::Resume(ResumeMode mode) noexcept
{
    if (!mPending)
    {
        return Status::NotReady;
    }
    return Finish(Vm().Resume(mode, mLast));
}

int32 Session::Breakpoint(BreakpointSpec point) noexcept
{
    if (mActive == nullptr || point.Line < 1 || point.Line > 65536)
    {
        return -1;
    }
    const int32 resolved = Vm().Breakpoint(
    {
        .Asset = point.Asset,
        .Line = point.Line + mActive->FirstLine - 1,
        .Enabled = point.Enabled,
    });
    if (resolved < 0 && mPending && !Vm().IsPaused())
    {
        mWorld.Faulted = true;
        mPending = false;
    }
    return resolved < 0 ? resolved : resolved - mActive->FirstLine + 1;
}

Replacement Session::Prepare(const Package& package, std::string_view expected) noexcept
{
    if (!SafePoint())
    {
        return Replacement::NotSafePoint;
    }
    Discard();
    if (expected != mActive->Key)
    {
        return Replacement::Stale;
    }
    if (!Trusted(package, mCatalog) || package.Revision <= mActive->Revision || package.Schema < mActive->Schema ||
        std::strcmp(package.Pin, mActive->Pin) != 0 || std::strcmp(package.Contract, mActive->Contract) != 0 ||
        mWorld.Execution == ~uint64{0})
    {
        return Replacement::Rejected;
    }
    for (uint32 i = 0; i < 2; ++i)
    {
        if (!RestoreState(Capture(mWorld.States[i], mActive->Schema), package, mMigrated[i]))
        {
            return Replacement::Rejected;
        }
    }
    Runtime& candidate = mVms[1 - mBank];
    candidate.EnableDebugger(true);
    if (candidate.LoadPrograms(package.Programs, package.Count, s1::Install) != Status::Completed)
    {
        candidate.Close();
        return Replacement::Rejected;
    }
    mStaged = &package;
    mPreparedTick = mWorld.Tick;
    mPreparedExecution = mWorld.Execution;
    return Replacement::Ready;
}

Replacement Session::Commit(std::string_view expected) noexcept
{
    if (!SafePoint())
    {
        return Replacement::NotSafePoint;
    }
    if (mStaged == nullptr || expected != mActive->Key || mPreparedTick != mWorld.Tick ||
        mPreparedExecution != mWorld.Execution)
    {
        Discard();
        return Replacement::Stale;
    }
    const uint32 old = mBank;
    mBank = 1 - mBank;
    mActive = mStaged;
    mStaged = nullptr;
    for (uint32 i = 0; i < 2; ++i)
    {
        mWorld.States[i] = mMigrated[i];
    }
    ++mWorld.Execution;
    mVms[old].Close(); // All old callbacks, upvalues, thread roots and userdata die before unload.
    return Replacement::Committed;
}

void Session::Discard() noexcept
{
    mVms[1 - mBank].Close();
    mStaged = nullptr;
}
void Session::Close() noexcept
{
    mVms[0].Close();
    mVms[1].Close();
    mActive = nullptr;
    mStaged = nullptr;
    mPending = false;
    mCatalog = {};
}

bool Session::Checkpoint(std::span<uint8> output, usize& written) const noexcept
{
    if (!SafePoint() || output.size() < 168)
    {
        return false;
    }
    uint8 bytes[168] = {};
    (void)TryWriteLittleEndian(uint32{0x32504353}, std::span<uint8>{bytes});
    (void)TryWriteLittleEndian(uint32{1}, std::span<uint8>{bytes}.subspan(4));
    (void)TryWriteLittleEndian(mWorld.Session, std::span<uint8>{bytes}.subspan(8));
    (void)TryWriteLittleEndian(mWorld.Tick, std::span<uint8>{bytes}.subspan(16));
    std::memcpy(bytes + 24, mActive->Key, 64);
    for (uint32 i = 0; i < 2; ++i)
    {
        const auto record = Capture(mWorld.States[i], mActive->Schema);
        usize size = 0;
        if (!EncodeState(record, std::span<uint8>{bytes}.subspan(88 + static_cast<usize>(i) * 40, 40), size))
        {
            return false;
        }
    }
    std::memcpy(output.data(), bytes, sizeof(bytes));
    written = sizeof(bytes);
    return true;
}

bool Session::Restore(std::span<const uint8> input) noexcept
{
    if (!SafePoint() || input.size() != 168)
    {
        return false;
    }
    uint32 magic = 0;
    uint32 version = 0;
    uint64 session = 0;
    uint64 tick = 0;
    (void)TryReadLittleEndian(input, magic);
    (void)TryReadLittleEndian(input.subspan(4), version);
    (void)TryReadLittleEndian(input.subspan(8), session);
    (void)TryReadLittleEndian(input.subspan(16), tick);
    const Package* source = nullptr;
    for (const Package* package : mCatalog)
    {
        if (package != nullptr && package->Key != nullptr && std::memcmp(input.data() + 24, package->Key, 64) == 0)
        {
            source = package;
        }
    }
    if (magic != 0x32504353 || version != 1 || source == nullptr || session != mWorld.Session || tick == 0 ||
        source->Schema > mActive->Schema)
    {
        return false;
    }
    s1::State states[2];
    for (uint32 i = 0; i < 2; ++i)
    {
        StateRecord record;
        s1::State checked;
        if (!DecodeState(input.subspan(88 + static_cast<usize>(i) * 40, 40), record) ||
            record.Schema != source->Schema || !RestoreState(record, *source, checked) ||
            !RestoreState(record, *mActive, states[i]))
        {
            return false;
        }
    }
    Discard();
    for (uint32 i = 0; i < 2; ++i)
    {
        mWorld.States[i] = states[i];
        mWorld.Open[i] = states[i].OpenRequested;
    }
    mWorld.Tick = tick;
    return true;
}
} // namespace ludus::s2
