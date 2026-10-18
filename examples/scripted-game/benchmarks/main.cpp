// S7 reference measurement harness, not an engine scheduler/capacity promise.
// Original equivalent native handler and paired-authored text/sequence inputs;
// see docs/architecture/behavior-s7.md. Timing and allocator counting run in
// different executables so the counting replacement never affects timings.
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/target.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/runtime/behavior/behavior.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <new>

#include "contract.h"
#include "package.h"
#include "probe.h"

namespace
{
using namespace ludus::foundation;
namespace behavior = ludus::runtime::behavior;
namespace project = ludus::sample;
constexpr uint32 SAMPLES = LUDUS_S7_SAMPLES;
constexpr uint32 WARMUP = LUDUS_S7_WARMUP;
constexpr uint32 ACTIVE = LUDUS_S7_ACTIVE;
constexpr uint32 INACTIVE = LUDUS_S7_INACTIVE;
constexpr uint32 TOTAL = ACTIVE + INACTIVE;
static_assert(SAMPLES >= 100 && SAMPLES <= 1000 && WARMUP >= 1 && WARMUP <= 1000);
static_assert(ACTIVE >= 1 && TOTAL <= 4096);
static_assert(LUDUS_S7_HEAP >= 65536 && LUDUS_S7_HEAP <= 64 * 1024 * 1024);
static_assert(LUDUS_S7_SAFEPOINTS > 0);
uint64 entityChecks = 0;
enum class Role : uint8
{
    Empty,
    Scalar,
    Operation,
    Encounter
};
struct Case
{
    const char* Name;
    Role Handler;
    uint64 Asset;
    uint32 Ready;
};
constexpr Case CASES[] = {
    {"native.empty", Role::Empty, 0, 1},
    {"luau.empty", Role::Empty, 0x102, 1},
    {"native.scalar", Role::Scalar, 0, 1},
    {"luau.scalar", Role::Scalar, 0x103, 1},
    {"native.operation", Role::Operation, 0, 1},
    {"luau.operation", Role::Operation, 0x104, 1},
    {"native.encounter", Role::Encounter, 0, ACTIVE},
    {"luau.encounter", Role::Encounter, 0x101, ACTIVE},
    {"graph.encounter", Role::Encounter, 0x100, ACTIVE},
    {"native.dormant", Role::Encounter, 0, 0},
    {"luau.dormant", Role::Encounter, 0x101, 0},
    {"graph.dormant", Role::Encounter, 0x100, 0},
};
void Text(const char* text) noexcept
{
    (void)std::fwrite(text, 1, std::strlen(text), stdout);
}
void Number(uint64 value) noexcept
{
    char digits[20];
    usize offset = sizeof(digits);
    do
    {
        digits[--offset] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    (void)std::fwrite(digits + offset, 1, sizeof(digits) - offset, stdout);
}
bool Alive(void*, behavior::EntityRef ref) noexcept
{
    ++entityChecks;
    return ref.World == 2 && ref.Session == 3 && ref.Execution == 4 && ref.Slot < TOTAL && ref.Generation == 1;
}
behavior::Invocation Input() noexcept
{
    behavior::Invocation input
    {
        .Asset = 0x101,
        .Revision = 1,
        .Instance = 1,
        .World = 2,
        .Session = 3,
        .Execution = 4,
        .Tick = 1,
        .Phase = 3,
        .Capabilities = 1,
    };
    input.Config = project::MakeConfig({});
    input.State = project::MakeState({});
    input.Event = project::MakeEvent({ .Target = {2, 3, 4, 0, 1}, .Amount = 1 });
    return input;
}
behavior::Status Native(behavior::Transaction& transaction, void* user) noexcept
{
    const auto role = *static_cast<const Role*>(user);
    if (role == Role::Empty)
    {
        return behavior::Status::Completed;
    }
    project::State state;
    project::Event event;
    if (project::TryReadState(transaction.State(), state) == false ||
        project::TryReadEvent(transaction.Input().Event, event) == false)
    {
        return behavior::Status::InvalidInput;
    }
    if (role == Role::Scalar || role == Role::Encounter)
    {
        state.Interactions += event.Amount;
    }
    if (role == Role::Operation || (role == Role::Encounter && state.Interactions >= 2 && state.OpenRequested == false))
    {
        const auto result = project::RequestDoorOpen(transaction, event.Target, event.Amount);
        if (role == Role::Encounter && result.Status == behavior::CommandStatus::Accepted)
        {
            state.OpenRequested = true;
        }
    }
    transaction.State() = project::MakeState(state);
    return behavior::Status::Completed;
}
bool Equal(const behavior::Value& left, const behavior::Value& right) noexcept
{
    return left.Type == right.Type && left.Scalar == right.Scalar && left.Entity.World == right.Entity.World &&
           left.Entity.Session == right.Entity.Session && left.Entity.Execution == right.Entity.Execution &&
           left.Entity.Slot == right.Entity.Slot && left.Entity.Generation == right.Entity.Generation;
}
bool Equal(const behavior::Outcome& left, const behavior::Outcome& right) noexcept
{
    if (left.State.Count != right.State.Count || left.Count != right.Count)
    {
        return false;
    }
    for (uint32 i = 0; i < left.State.Count; ++i)
    {
        if (left.State.Items[i].Id != right.State.Items[i].Id ||
            Equal(left.State.Items[i].Data, right.State.Items[i].Data) == false)
        {
            return false;
        }
    }
    for (uint32 i = 0; i < left.Count; ++i)
    {
        if (left.Commands[i].Operation != right.Commands[i].Operation ||
            left.Commands[i].Token != right.Commands[i].Token || left.Commands[i].Count != right.Commands[i].Count)
        {
            return false;
        }
        for (uint32 j = 0; j < left.Commands[i].Count; ++j)
        {
            if (Equal(left.Commands[i].Arguments[j], right.Commands[i].Arguments[j]) == false)
            {
                return false;
            }
        }
    }
    return true;
}
bool Load(behavior::LuauProvider& provider) noexcept
{
    return provider.Load(project::SCHEMA, project::PACKAGE, LUDUS_S7_HEAP, LUDUS_S7_SAFEPOINTS) ==
           behavior::Status::Completed;
}
bool Verify() noexcept
{
    behavior::LuauProvider provider;
    if (Load(provider) == false)
    {
        return false;
    }
    for (Role role : {Role::Empty, Role::Scalar, Role::Operation, Role::Encounter})
    {
        for (const uint32 initial : {0U, 1U, 2U, 89U})
        {
            for (const uint32 amount : {1U, 2U, 10U})
            {
                for (const bool opened : {false, true})
                {
                    for (const uint64 capabilities : {uint64{0}, uint64{1}})
                    {
                        auto input = Input();
                        input.Capabilities = capabilities;
                        input.State = project::MakeState({ .Interactions = initial, .OpenRequested = opened });
                        input.Event = project::MakeEvent({ .Target = {2, 3, 4, 0, 1}, .Amount = amount });
                        behavior::Outcome native;
                        if (behavior::ExecuteNative(project::SCHEMA, input, {nullptr, Alive}, Native, &role, native) !=
                            behavior::Status::Completed)
                        {
                            return false;
                        }
                        const uint64 asset = role == Role::Empty       ? 0x102
                                             : role == Role::Scalar    ? 0x103
                                             : role == Role::Operation ? 0x104
                                                                       : 0x101;
                        for (const uint64 candidate : {asset, role == Role::Encounter ? uint64{0x100} : asset})
                        {
                            input.Asset = candidate;
                            behavior::Outcome actual;
                            behavior::Diagnostic diagnostic;
                            if (provider.Invoke(input, {nullptr, Alive}, actual, diagnostic) !=
                                    behavior::Status::Completed ||
                                Equal(native, actual) == false)
                            {
                                return false;
                            }
                        }
                    }
                }
            }
        }
    }
    return provider.Close() == behavior::Status::Completed && provider.LiveBytes() == 0;
}
bool ObserveEdit(const char* path) noexcept
{
    auto* file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0)
    {
        (void)std::fclose(file);
        return false;
    }
    const auto length = std::ftell(file);
    if (length <= 0 || length > int64{8} * 1024 * 1024 || std::fseek(file, 0, SEEK_SET) != 0)
    {
        (void)std::fclose(file);
        return false;
    }
    const auto size = static_cast<usize>(length);
    auto* bytes = new (std::nothrow) uint8[size];
    if (bytes == nullptr)
    {
        (void)std::fclose(file);
        return false;
    }
    const bool read = std::fread(bytes, 1, size, file) == size;
    const bool closed = std::fclose(file) == 0;
    behavior::LuauProvider provider;
    bool ok = read && closed &&
              provider.Load(project::SCHEMA, {bytes, size}, LUDUS_S7_HEAP, LUDUS_S7_SAFEPOINTS) ==
                  behavior::Status::Completed;
    auto input = Input();
    for (uint32 tick = 1; ok && tick <= 3; ++tick)
    {
        input.Tick = tick;
        behavior::Outcome output;
        behavior::Diagnostic diagnostic;
        ok = provider.Invoke(input, {nullptr, Alive}, output, diagnostic) == behavior::Status::Completed;
        project::State state;
        ok = ok && project::TryReadState(output.State, state) && state.Interactions == tick &&
             state.OpenRequested == (tick == 3) && output.Count == (tick == 3 ? 1U : 0U);
        input.State = output.State;
    }
    ok = provider.Close() == behavior::Status::Completed && provider.LiveBytes() == 0 && ok;
    delete[] bytes;
    return ok;
}
bool Batch(const Case& item, behavior::LuauProvider& provider, behavior::Record* states, uint64& commands) noexcept
{
    for (uint32 slot = 0; slot < item.Ready; ++slot)
    {
        if (states[slot].Items[0].Data.Scalar >= 100)
        {
            states[slot] = project::MakeState({});
        }
        auto input = Input();
        input.Asset = item.Asset == 0 ? 0x101 : item.Asset;
        input.Instance = uint64{slot} + 1;
        input.State = states[slot];
        input.Event = project::MakeEvent({ .Target = {2, 3, 4, slot, 1}, .Amount = 1 });
        behavior::Outcome output;
        behavior::Diagnostic diagnostic;
        Role role = item.Handler;
        const auto status =
            item.Asset == 0 ? behavior::ExecuteNative(project::SCHEMA, input, {nullptr, Alive}, Native, &role, output)
                            : provider.Invoke(input, {nullptr, Alive}, output, diagnostic);
        if (status != behavior::Status::Completed)
        {
            return false;
        }
        states[slot] = output.State;
        commands += output.Count;
    }
    return true;
}
bool Measure(const Case& item) noexcept
{
    behavior::LuauProvider provider;
    if (item.Asset != 0 && Load(provider) == false)
    {
        return false;
    }
    auto* states = new (std::nothrow) behavior::Record[TOTAL];
    if (states == nullptr)
    {
        return false;
    }
    for (uint32 i = 0; i < TOTAL; ++i)
    {
        states[i] = project::MakeState({});
    }
    uint64 commands = 0;
    for (uint32 i = 0; i < WARMUP; ++i)
    {
        if (Batch(item, provider, states, commands) == false)
        {
            delete[] states;
            return false;
        }
    }
    for (uint32 i = 0; i < TOTAL; ++i)
    {
        states[i] = project::MakeState({});
    }
    commands = entityChecks = 0;
    uint64 timings[SAMPLES] = {};
    usize liveMaximum = provider.LiveBytes();
    ludus::s7::BeginAllocations();
    for (uint32 i = 0; i < SAMPLES; ++i)
    {
        const uint64 begin = time::NowTicks();
        const bool ok = Batch(item, provider, states, commands);
        const uint64 end = time::NowTicks();
        if (ok == false || end < begin)
        {
            (void)ludus::s7::EndAllocations();
            delete[] states;
            return false;
        }
        timings[i] = end - begin;
        const usize live = provider.LiveBytes();
        if (live > liveMaximum)
        {
            liveMaximum = live;
        }
    }
    const auto allocations = ludus::s7::EndAllocations();
    uint64 interactions = 0;
    uint64 opened = 0;
    for (uint32 i = 0; i < TOTAL; ++i)
    {
        project::State state;
        if (project::TryReadState(states[i], state) == false)
        {
            delete[] states;
            return false;
        }
        if (i >= item.Ready && (state.Interactions != 0 || state.OpenRequested))
        {
            delete[] states;
            return false;
        }
        interactions += state.Interactions;
        opened += state.OpenRequested ? 1U : 0U;
    }
    delete[] states;
    const uint64 expectedCommands = item.Handler == Role::Operation   ? uint64{SAMPLES} * item.Ready
                                    : item.Handler == Role::Encounter ? uint64{(SAMPLES + 98) / 100} * item.Ready
                                                                      : 0;
    if (commands != expectedCommands || (item.Asset == 0 && allocations.Requests != 0) ||
        (item.Ready == 0 && (allocations.Requests != 0 || entityChecks != 0)))
    {
        return false;
    }
    if (provider.Close() != behavior::Status::Completed || provider.LiveBytes() != 0)
    {
        return false;
    }
    Text("{\"name\":\"");
    Text(item.Name);
    Text("\",\"ready\":");
    Number(item.Ready);
    Text(",\"dormant\":");
    Number(TOTAL - item.Ready);
    Text(",\"commands\":");
    Number(commands);
    Text(",\"interactions\":");
    Number(interactions);
    Text(",\"opened\":");
    Number(opened);
    Text(",\"entity_checks\":");
    Number(entityChecks);
    Text(",\"aligned_requests\":");
    Number(allocations.Requests);
    Text(",\"aligned_requested_bytes\":");
    Number(allocations.RequestedBytes);
    Text(",\"boundary_live_max_bytes\":");
    Number(liveMaximum);
    Text(",\"samples_ns\":[");
    for (uint32 i = 0; i < SAMPLES; ++i)
    {
        if (i != 0)
        {
            Text(",");
        }
        Number(timings[i]);
    }
    std::sort(timings, timings + SAMPLES);
    Text("],\"p50_ns\":");
    Number(timings[(SAMPLES * 50 + 99) / 100 - 1]);
    Text(",\"p95_ns\":");
    Number(timings[(SAMPLES * 95 + 99) / 100 - 1]);
    Text(",\"p99_ns\":");
    Number(timings[(SAMPLES * 99 + 99) / 100 - 1]);
    Text(",\"worst_ns\":");
    Number(timings[SAMPLES - 1]);
    Text("}");
    return true;
}
} // namespace
int main(int argc, char** argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--observe-edit") == 0)
    {
        if (ObserveEdit(argv[2]) == false)
        {
            return 6;
        }
        Text("S7 saved threshold-three outcome observed PASS\n");
        return 0;
    }
    const bool verifyOnly = argc == 2 && std::strcmp(argv[1], "--verify") == 0;
    if (argc != 2 || (verifyOnly == false && std::strcmp(argv[1], "--measure") != 0))
    {
        return 2;
    }
    if (Verify() == false)
    {
        return 3;
    }
    if (verifyOnly)
    {
        Text("S7 native/text/sequence state and command equivalence PASS\n");
        return 0;
    }
    Text("{\"version\":1,\"instrumented\":");
#if defined(LUDUS_S7_ALLOCATION_PROBE)
    Text("true");
#else
    Text("false");
#endif
    Text(",\"target\":{\"os\":\"");
    Text(TargetOsName(kTarget.Os));
    Text("\",\"arch\":\"");
    Text(TargetArchName(kTarget.Arch));
    Text("\",\"pointer_bits\":");
    Number(kTarget.PointerBits);
    Text("},\"samples\":");
    Number(SAMPLES);
    Text(",\"warmup\":");
    Number(WARMUP);
    Text(",\"active\":");
    Number(ACTIVE);
    Text(",\"inactive\":");
    Number(INACTIVE);
    Text(",\"contract\":\"");
    Text(project::SCHEMA.Digest);
    Text("\",\"package\":\"");
    Text(project::PACKAGE_KEY);
    Text("\",\"cases\":[");
    bool first = true;
    for (const auto& item : CASES)
    {
        if (first == false)
        {
            Text(",");
        }
        first = false;
        if (Measure(item) == false)
        {
            return 4;
        }
    }
    Text("],\"equivalence\":true,\"live_after_close\":0}\n");
    return std::ferror(stdout) == 0 ? 0 : 5;
}
