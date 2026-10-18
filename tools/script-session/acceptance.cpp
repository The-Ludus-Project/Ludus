#include <ludus/foundation/base/core.h>

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/logging/category.hpp>
#include <ludus/foundation/logging/log_format.hpp>

#include <cstring>
#include <span>

#include "packages.h"
#include "session.h"

namespace ludus::s2
{
namespace
{
constexpr foundation::logging::LogCategory LOG_S2{"S2"};
bool Expect(bool value, const char* label) noexcept
{
    if (value == false)
    {
        LUDUS_LOG_ERROR(LOG_S2, "Failed: {}", label);
    }
    return value;
}
} // namespace
bool Acceptance() noexcept
{
    using namespace runtime::scripting;
    StateRecord record;
    for (const FieldValue& field : record.Fields)
    {
        if (Expect(field.Id == 0 && field.Kind == FieldKind::Uint32 && field.Value == 0,
                   "default state field initialization") == false)
        {
            return false;
        }
    }
    record.Schema = 1;
    record.Count = 2;
    record.Fields[0] = { .Id = 11, .Kind = FieldKind::Uint32, .Value = 4 };
    record.Fields[1] = { .Id = 12, .Kind = FieldKind::Boolean, .Value = 1 };
    uint8 bytes[168] = {};
    usize written = 0;
    StateRecord decoded;
    if (Expect(EncodeState(record, bytes, written) && written == 40 && bytes[0] == 0x4c &&
                   DecodeState(std::span<const uint8>{bytes}.first(written), decoded) && decoded.Fields[0].Value == 4,
               "bounded little-endian state") == false)
    {
        return false;
    }
    const FieldSpec fields[] = {{ .Id = 12, .Kind = FieldKind::Boolean, .Min = 0, .Max = 1, .Default = 0 },
                                { .Id = 11, .Kind = FieldKind::Uint32, .Min = 0, .Max = 10, .Default = 0 },
                                { .Id = 13, .Kind = FieldKind::Uint32, .Min = 0, .Max = 10, .Default = 2 }};
    if (Expect(MigrateState(decoded, 2, fields, record) && record.Fields[1].Value == 4 && record.Fields[2].Value == 2,
               "stable tags and declared defaults") == false)
    {
        return false;
    }
    decoded.Schema = 99;
    if (Expect(DecodeState(std::span<const uint8>{bytes}.first(39), decoded) == false && decoded.Schema == 99,
               "truncated codec preserves output") == false)
    {
        return false;
    }
    (void)TryWriteLittleEndian(uint32{11}, std::span<uint8>{bytes}.subspan(28));
    if (Expect(DecodeState(std::span<const uint8>{bytes}.first(40), decoded) == false,
               "duplicate state tag rejected") == false)
    {
        return false;
    }
    Session session;
    if (Expect(session.Initialize(BASE) && session.Begin() == Status::Completed &&
                   session.Begin({ .Instance = 1, .Amount = 2 }) == Status::Completed,
               "two persistent instances") == false)
    {
        return false;
    }
    const uint64 execution = session.World().Execution;
    const uint64 tick = session.World().Tick;
    if (Expect(session.Prepare(FAILED, BASE.Key) == Replacement::Rejected && session.Active() == &BASE &&
                   session.World().Execution == execution && session.World().Tick == tick &&
                   session.World().States[1].Interactions == 2,
               "failed initialization retains entire active cohort") == false)
    {
        return false;
    }
    if (Expect(session.Prepare(NARROW, BASE.Key) == Replacement::Rejected, "narrowing migration rejected") == false)
    {
        return false;
    }
    if (Expect(session.Prepare(REPLACEMENT, "stale") == Replacement::Stale, "expected package precondition") == false)
    {
        return false;
    }
    Package forged = REPLACEMENT;
    if (Expect(session.Prepare(forged, BASE.Key) == Replacement::Rejected, "digest is not a trust root") == false)
    {
        return false;
    }
    session.FailCandidateAllocation(1);
    if (Expect(session.Prepare(REPLACEMENT, BASE.Key) == Replacement::Rejected && session.Active() == &BASE,
               "candidate OOM retains active") == false)
    {
        return false;
    }
    session.FailCandidateAllocation(0);
    if (Expect(session.Prepare(REPLACEMENT, BASE.Key) == Replacement::Ready && session.Begin() == Status::Completed &&
                   session.Commit(BASE.Key) == Replacement::Stale,
               "intervening tick cancels staged replacement") == false)
    {
        return false;
    }
    if (Expect(session.Checkpoint(bytes, written) && written == sizeof(bytes) &&
                   session.Prepare(REPLACEMENT, BASE.Key) == Replacement::Ready &&
                   session.Commit(BASE.Key) == Replacement::Committed && session.World().Execution == execution + 1 &&
                   session.World().States[0].Interactions == 2 && session.World().States[1].Interactions == 2 &&
                   session.Begin(
                   {
                       .Instance = 1,
                   }) == Status::Completed &&
                   session.World().States[1].Interactions == 4,
               "whole VM migration and changed dependency behavior") == false)
    {
        return false;
    }
    Session restored;
    if (Expect(restored.Initialize(REPLACEMENT) && restored.Restore(bytes) &&
                   restored.World().States[1].Interactions == 2,
               "checkpoint migrates into fresh native owner") == false)
    {
        return false;
    }
    session.Close();
    restored.Close();
    if (Expect(session.Heap() == 0 && restored.Heap() == 0, "all native closures retired") == false)
    {
        return false;
    }
    Session debug;
    if (Expect(debug.Initialize(BASE) && debug.Breakpoint({ .Asset = 0x100, .Line = 8 }) == 8 &&
                   debug.Begin() == Status::Paused,
               "real source breakpoint") == false)
    {
        return false;
    }
    if (Expect(debug.Pending() && debug.SafePoint() == false && debug.World().States[0].Interactions == 0 &&
                   debug.World().CommandCount == 0 && debug.Checkpoint(bytes, written) == false &&
                   debug.Prepare(REPLACEMENT, BASE.Key) == Replacement::NotSafePoint &&
                   debug.Begin({ .Instance = 1 }) == Status::NotReady,
               "partial tick cannot save reload or advance") == false)
    {
        return false;
    }
    const auto stop = debug.Vm().Inspect().Stop;
    const Status stepped = debug.Resume(ResumeMode::Into);
    LUDUS_LOG_INFO(LOG_S2,
                   "Debug step status {} depth {} line {} stop {}",
                   static_cast<uint8>(stepped),
                   debug.Vm().Inspect().Depth,
                   debug.Vm().Inspect().Frames[0].Line,
                   debug.Vm().Inspect().Stop);
    if (Expect(stepped == Status::Paused && debug.Vm().Inspect().Stop > stop && debug.Vm().Inspect().Depth > 1,
               "step into helper") == false)
    {
        return false;
    }
    bool local = false;
    for (uint32 i = 0; i < debug.Vm().Inspect().LocalCount; ++i)
    {
        const auto& value = debug.Vm().Inspect().Locals[i];
        local |= std::strcmp(value.Name, "amount") == 0 && value.Kind == DebugKind::Number && value.Number == 1;
    }
    if (Expect(local, "primitive local snapshot") == false)
    {
        return false;
    }
    if (Expect(debug.Resume(ResumeMode::Out) == Status::Paused && debug.Vm().Inspect().Depth == 1 &&
                   debug.World().Tick == 1 && debug.Resume(ResumeMode::Over) == Status::Paused &&
                   debug.Resume(ResumeMode::Continue) == Status::Completed &&
                   debug.World().States[0].Interactions == 1 && debug.World().Tick == 2,
               "step out over and one completed tick") == false)
    {
        return false;
    }
    debug.Close();
    for (uint32 i = 0; i < 32; ++i)
    {
        Session cycle;
        if (Expect(cycle.Initialize(BASE) && cycle.Prepare(REPLACEMENT, BASE.Key) == Replacement::Ready &&
                       cycle.Commit(BASE.Key) == Replacement::Committed &&
                       cycle.Breakpoint({ .Asset = 0x100, .Line = 8 }) == 8 && cycle.Begin() == Status::Paused,
                   "repeat replace and stop") == false)
        {
            return false;
        }
        cycle.Close();
        if (Expect(cycle.Heap() == 0 && cycle.World().States[0].Interactions == 0, "cancel unpublished effects") ==
            false)
        {
            return false;
        }
    }
    Session baseline;
    if (baseline.Initialize(BASE) == false || baseline.Prepare(REPLACEMENT, BASE.Key) != Replacement::Ready)
    {
        return false;
    }
    const usize allocations = baseline.CandidateAttempts();
    baseline.Close();
    for (usize failure = 1; failure <= allocations; ++failure)
    {
        Session attempt;
        if (attempt.Initialize(BASE) == false)
        {
            return false;
        }
        attempt.FailCandidateAllocation(failure);
        if (Expect(attempt.Prepare(REPLACEMENT, BASE.Key) == Replacement::Rejected && attempt.Active() == &BASE &&
                       attempt.World().States[0].Interactions == 0 && attempt.Begin() == Status::Completed,
                   "replacement allocation failure sweep") == false)
        {
            return false;
        }
        attempt.Close();
        if (Expect(attempt.Heap() == 0, "failed candidate heap retirement") == false)
        {
            return false;
        }
    }
    Session invocation;
    if (invocation.Initialize(BASE) == false || invocation.Breakpoint({ .Asset = 0x100, .Line = 8 }) != 8)
    {
        return false;
    }
    const usize before = invocation.Vm().GetMemory().Attempts;
    if (invocation.Begin() != Status::Paused)
    {
        return false;
    }
    const usize invocationAllocations = invocation.Vm().GetMemory().Attempts - before;
    invocation.Close();
    for (usize failure = 1; failure <= invocationAllocations; ++failure)
    {
        Session attempt;
        if (attempt.Initialize(BASE) == false || attempt.Breakpoint({ .Asset = 0x100, .Line = 8 }) != 8)
        {
            return false;
        }
        attempt.Vm().SetAllocationFailure(attempt.Vm().GetMemory().Attempts + failure);
        if (Expect(attempt.Begin() == Status::AllocationFailure && attempt.World().Faulted &&
                       attempt.World().States[0].Interactions == 0 && attempt.World().CommandCount == 0,
                   "debug setup and local capture allocation failure") == false)
        {
            return false;
        }
        attempt.Close();
        if (attempt.Heap() != 0)
        {
            return false;
        }
    }
    LUDUS_LOG_INFO(LOG_S2, "S2 ACCEPTANCE PASS: codec, imports, debug steps, cohort migration, retention, retirement");
    return true;
}
} // namespace ludus::s2
