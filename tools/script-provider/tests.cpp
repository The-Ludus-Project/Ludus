#include <ludus/runtime/behavior/behavior.h>

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/array.hpp>

#include <cstring>
#include <fstream>
#include <iterator>

#include <catch2/catch_test_macros.hpp>

#include "contract.h"

namespace
{
using namespace ludus::runtime::behavior;
using namespace ludus::foundation;
Array<uint8> Package()
{
    std::ifstream file(LUDUS_BEHAVIOR_PACKAGE, std::ios::binary);
    Array<uint8> bytes;
    for (std::istreambuf_iterator<char> it(file); it != std::istreambuf_iterator<char>(); ++it)
    {
        bytes.Add(static_cast<uint8>(*it));
    }
    return bytes;
}
Invocation Input(uint64 asset = 0x100)
{
    Invocation input
    {
        .Asset = asset,
        .Revision = 1,
        .Instance = 50,
        .World = 60,
        .Session = 0xf123456789abcdefULL,
        .Execution = 0xa123456789abcdefULL,
        .Tick = 1,
        .Phase = 3,
        .Capabilities = 3,
    };
    input.Config = ludus::sample::MakeConfig({});
    input.State = ludus::sample::MakeState({});
    input.Event =
        ludus::sample::MakeEvent({ .Target = {input.World, input.Session, input.Execution, 0, 1}, .Amount = 1 });
    return input;
}
bool Alive(void*, EntityRef ref) noexcept
{
    return ref.Slot == 0 && ref.Generation == 1;
}
Status Native(Transaction& transaction, void*) noexcept
{
    ludus::sample::State state;
    ludus::sample::Config config;
    ludus::sample::Event event;
    if (!ludus::sample::TryReadState(transaction.State(), state) ||
        !ludus::sample::TryReadConfig(transaction.Input().Config, config) ||
        !ludus::sample::TryReadEvent(transaction.Input().Event, event))
    {
        return Status::InvalidInput;
    }
    state.Interactions += event.Amount;
    if (state.Interactions >= config.Threshold && !state.OpenRequested)
    {
        const auto result = ludus::sample::RequestDoorOpen(transaction, event.Target, event.Amount);
        if (result.Status == CommandStatus::Accepted)
        {
            state.OpenRequested = true;
        }
    }
    transaction.State() = ludus::sample::MakeState(state);
    return Status::Completed;
}
void Same(const Outcome& a, const Outcome& b)
{
    REQUIRE(a.State.Count == b.State.Count);
    for (uint32 i = 0; i < a.State.Count; ++i)
    {
        CHECK(a.State.Items[i].Id == b.State.Items[i].Id);
        CHECK(a.State.Items[i].Data.Type == b.State.Items[i].Data.Type);
        CHECK(a.State.Items[i].Data.Scalar == b.State.Items[i].Data.Scalar);
    }
    REQUIRE(a.Count == b.Count);
    for (uint32 i = 0; i < a.Count; ++i)
    {
        CHECK(a.Commands[i].Operation == b.Commands[i].Operation);
        CHECK(a.Commands[i].Token == b.Commands[i].Token);
        CHECK(a.Commands[i].Count == b.Commands[i].Count);
        for (uint32 j = 0; j < a.Commands[i].Count; ++j)
        {
            CHECK(a.Commands[i].Arguments[j].Type == b.Commands[i].Arguments[j].Type);
            CHECK(a.Commands[i].Arguments[j].Scalar == b.Commands[i].Arguments[j].Scalar);
            const auto x = a.Commands[i].Arguments[j].Entity, y = b.Commands[i].Arguments[j].Entity;
            CHECK(x.World == y.World);
            CHECK(x.Session == y.Session);
            CHECK(x.Execution == y.Execution);
            CHECK(x.Slot == y.Slot);
            CHECK(x.Generation == y.Generation);
        }
    }
}
} // namespace
TEST_CASE("S4 aggregate defaults retain valid value kinds in every bounded slot")
{
    const auto checkValue = [](const Value& value) {
        CHECK(value.Type == Kind::Uint32);
        CHECK(value.Scalar == 0);
        CHECK(value.Entity.World == 0);
        CHECK(value.Entity.Session == 0);
        CHECK(value.Entity.Execution == 0);
        CHECK(value.Entity.Slot == 0);
        CHECK(value.Entity.Generation == 0);
    };
    const Record record{};
    CHECK(record.Count == 0);
    for (const Field& field : record.Items)
    {
        CHECK(field.Id == 0);
        checkValue(field.Data);
    }
    const Command command{};
    CHECK(command.Count == 0);
    for (const Value& value : command.Arguments)
    {
        checkValue(value);
    }
    const Outcome outcome{};
    CHECK(outcome.Count == 0);
    for (const Command& staged : outcome.Commands)
    {
        CHECK(staged.Count == 0);
        for (const Value& value : staged.Arguments)
        {
            checkValue(value);
        }
    }
}

TEST_CASE("S4 native and Luau share copied state and ordered checked commands")
{
    const auto bytes = Package();
    LuauProvider provider;
    REQUIRE(provider.Load(ludus::sample::SCHEMA, bytes) == Status::Completed);
    auto input = Input();
    Outcome luau, native;
    Diagnostic diagnostic;
    for (uint32 tick = 1; tick <= 3; ++tick)
    {
        input.Tick = tick;
        REQUIRE(provider.Invoke(input, {nullptr, Alive}, luau, diagnostic) == Status::Completed);
        REQUIRE(ExecuteNative(ludus::sample::SCHEMA, input, {nullptr, Alive}, Native, nullptr, native) ==
                Status::Completed);
        Same(luau, native);
        input.State = luau.State;
    }
    input = Input(0x200);
    REQUIRE(provider.Invoke(input, {nullptr, Alive}, luau, diagnostic) == Status::Completed);
    REQUIRE(luau.Count == 1);
    CHECK(luau.Commands[0].Operation == 201);
    CHECK(luau.Commands[0].Arguments[0].Type == Kind::Boolean);
    CHECK(luau.Commands[0].Arguments[0].Scalar == 0);
    CHECK(provider.LiveBytes() > 0);
    REQUIRE(provider.Close() == Status::Completed);
    CHECK(provider.LiveBytes() == 0);
}
TEST_CASE("S4 rejected candidates retain active VM and immutable caller records")
{
    auto bytes = Package();
    LuauProvider provider;
    REQUIRE(provider.Load(ludus::sample::SCHEMA, bytes) == Status::Completed);
    auto invalid = bytes;
    invalid[16] ^= 1;
    CHECK(provider.Load(ludus::sample::SCHEMA, invalid) == Status::InvalidPackage);
    CHECK(provider.Load(ludus::sample::SCHEMA, bytes, 1) == Status::AllocationFailure);
    auto input = Input();
    Outcome output;
    output.Count = 9;
    Diagnostic diagnostic;
    input.Revision = 2;
    CHECK(provider.Invoke(input, {nullptr, Alive}, output, diagnostic) == Status::InvalidInput);
    CHECK(output.Count == 9);
    input.Revision = 1;
    input.Event.Items[0].Data.Entity.Execution = 1;
    CHECK(provider.Invoke(input, {nullptr, Alive}, output, diagnostic) == Status::InvalidInput);
    CHECK(output.Count == 9);
    input = Input();
    REQUIRE(provider.Invoke(input, {nullptr, Alive}, output, diagnostic) == Status::Completed);
    CHECK(output.State.Items[0].Data.Scalar == 1);
}
TEST_CASE("S4 phase and capability rejection leave staged effects empty")
{
    auto bytes = Package();
    LuauProvider provider;
    REQUIRE(provider.Load(ludus::sample::SCHEMA, bytes) == Status::Completed);
    auto input = Input();
    input.State.Items[0].Data.Scalar = 1;
    Outcome output;
    Diagnostic diagnostic;
    input.Phase = 2;
    REQUIRE(provider.Invoke(input, {nullptr, Alive}, output, diagnostic) == Status::Completed);
    CHECK(output.Count == 0);
    CHECK(output.State.Items[1].Data.Scalar == 0);
    CHECK(diagnostic.NativeStatus == static_cast<uint32>(CommandStatus::InvalidPhase));
    input.Phase = 3;
    input.Capabilities = 0;
    REQUIRE(provider.Invoke(input, {nullptr, Alive}, output, diagnostic) == Status::Completed);
    CHECK(output.Count == 0);
    CHECK(diagnostic.NativeStatus == static_cast<uint32>(CommandStatus::MissingCapability));
}
TEST_CASE("S4 faults discard staged commands and retire stale facades and loops")
{
    const auto bytes = Package();
    LuauProvider provider;
    Outcome output;
    output.Count = 7;
    Diagnostic diagnostic;
    REQUIRE(provider.Load(ludus::sample::SCHEMA, bytes) == Status::Completed);
    auto input = Input(0x300);
    CHECK(provider.Invoke(input, {nullptr, Alive}, output, diagnostic) == Status::ScriptFault);
    CHECK(output.Count == 7);
    CHECK(provider.LiveBytes() == 0);
    CHECK(provider.Invoke(input, {nullptr, Alive}, output, diagnostic) == Status::NotReady);
    REQUIRE(provider.Load(ludus::sample::SCHEMA, bytes) == Status::Completed);
    input = Input(0x400);
    REQUIRE(provider.Invoke(input, {nullptr, Alive}, output, diagnostic) == Status::Completed);
    output.Count = 7;
    CHECK(provider.Invoke(input, {nullptr, Alive}, output, diagnostic) == Status::ScriptFault);
    CHECK(output.Count == 7);
    CHECK(provider.LiveBytes() == 0);
    REQUIRE(provider.Load(ludus::sample::SCHEMA, bytes, ludus::foundation::usize{8} * 1024 * 1024, 20) ==
            Status::Completed);
    input = Input(0x500);
    CHECK(provider.Invoke(input, {nullptr, Alive}, output, diagnostic) == Status::Interrupted);
    CHECK(output.Count == 7);
    CHECK(provider.LiveBytes() == 0);
}
TEST_CASE("S4 declared state codec and migration preserve failed outputs")
{
    const auto state = ludus::sample::MakeState({ .Interactions = 7, .OpenRequested = true });
    uint8 bytes[236]{};
    usize written = 999;
    REQUIRE(EncodeState(ludus::sample::SCHEMA, state, bytes, written));
    Record restored;
    REQUIRE(DecodeState(ludus::sample::SCHEMA, {bytes, written}, restored));
    CHECK(restored.Items[0].Data.Scalar == 7);
    CHECK(restored.Items[1].Data.Scalar == 1);
    bytes[8] ^= 1;
    restored.Items[0].Data.Scalar = 99;
    CHECK_FALSE(DecodeState(ludus::sample::SCHEMA, {bytes, written}, restored));
    CHECK(restored.Items[0].Data.Scalar == 99);
    written = 999;
    CHECK_FALSE(EncodeState(ludus::sample::SCHEMA, state, {bytes, 2}, written));
    CHECK(written == 999);
    const FieldSpec added[] = {{10, "Interactions", Kind::Uint32, 0, 100, 0},
                               {11, "OpenRequested", Kind::Boolean, 0, 1, 0},
                               {12, "NewValue", Kind::Uint32, 0, 100, 5}};
    auto target = ludus::sample::SCHEMA;
    target.State = added;
    REQUIRE(MigrateState(state, target, restored));
    CHECK(restored.Count == 3);
    CHECK(restored.Items[2].Data.Scalar == 5);
    target.State = {added + 1, 2};
    CHECK_FALSE(MigrateState(state, target, restored));
    CHECK(restored.Count == 3);
}
TEST_CASE("S4 command capacity and opaque binding failures preserve transaction boundaries")
{
    const auto bytes = Package();
    LuauProvider provider;
    Outcome output;
    Diagnostic diagnostic;
    REQUIRE(provider.Load(ludus::sample::SCHEMA, bytes) == Status::Completed);
    auto input = Input(0x800);
    REQUIRE(provider.Invoke(input, {nullptr, Alive}, output, diagnostic) == Status::Completed);
    REQUIRE(output.Count == 16);
    for (uint32 i = 0; i < 16; ++i)
    {
        CHECK(output.Commands[i].Operation == 201);
        CHECK(output.Commands[i].Token == i + 1);
    }
    CHECK(diagnostic.NativeStatus == static_cast<uint32>(CommandStatus::CapacityExceeded));
    input = Input(0x700);
    output.Count = 9;
    CHECK(provider.Invoke(input, {nullptr, Alive}, output, diagnostic) == Status::ScriptFault);
    CHECK(output.Count == 9);
    CHECK(provider.LiveBytes() == 0);
}
TEST_CASE("S4 validation services cannot reenter or retire their provider")
{
    struct Context
    {
        LuauProvider* Provider;
        Status Nested = Status::Completed;
    };
    const auto bytes = Package();
    LuauProvider provider;
    REQUIRE(provider.Load(ludus::sample::SCHEMA, bytes) == Status::Completed);
    Context context{&provider};
    const Services services{&context, [](void* user, EntityRef ref) noexcept {
                                auto& state = *static_cast<Context*>(user);
                                state.Nested = state.Provider->Close();
                                return ref.Generation == 1;
                            }};
    Outcome output;
    Diagnostic diagnostic;
    REQUIRE(provider.Invoke(Input(), services, output, diagnostic) == Status::Completed);
    CHECK(context.Nested == Status::Reentrant);
    CHECK(provider.LiveBytes() > 0);
}

TEST_CASE("S4 package metadata rejection preserves the active catalog")
{
    using namespace ludus::foundation;
    const auto good = Package();
    LuauProvider provider;
    REQUIRE(provider.Load(ludus::sample::SCHEMA, good) == Status::Completed);
    auto bytes = good;
    bytes.Add(0);
    REQUIRE(provider.Load(ludus::sample::SCHEMA, bytes) == Status::InvalidPackage);
    bytes = good;
    REQUIRE(TryWriteLittleEndian(uint32{9}, std::span<uint8>{bytes}.subspan(112)));
    REQUIRE(provider.Load(ludus::sample::SCHEMA, bytes) == Status::InvalidPackage);
    bytes = good;
    REQUIRE(TryWriteLittleEndian(uint32{1}, std::span<uint8>{bytes}.subspan(116)));
    REQUIRE(provider.Load(ludus::sample::SCHEMA, bytes) == Status::InvalidPackage);
    Outcome output;
    Diagnostic diagnostic;
    REQUIRE(provider.Invoke(Input(), {nullptr, Alive}, output, diagnostic) == Status::Completed);
    auto reordered = Input();
    const auto field = reordered.State.Items[0];
    reordered.State.Items[0] = reordered.State.Items[1];
    reordered.State.Items[1] = field;
    Outcome native;
    REQUIRE(ExecuteNative(ludus::sample::SCHEMA, reordered, {nullptr, Alive}, Native, nullptr, native) ==
            Status::Completed);
    REQUIRE(provider.Invoke(reordered, {nullptr, Alive}, output, diagnostic) == Status::Completed);
    ludus::sample::State native_state;
    ludus::sample::State luau_state;
    REQUIRE(ludus::sample::TryReadState(native.State, native_state));
    REQUIRE(ludus::sample::TryReadState(output.State, luau_state));
    REQUIRE(native_state.Interactions == luau_state.Interactions);
    REQUIRE(native_state.OpenRequested == luau_state.OpenRequested);
    reordered.Event.Items[0].Data.Entity.Generation = 2;
    output.Count = 9;
    REQUIRE(provider.Invoke(reordered, {nullptr, Alive}, output, diagnostic) == Status::InvalidInput);
    REQUIRE(output.Count == 9);
}

TEST_CASE("S4 native invalid state discards earlier staged effects")
{
    auto input = Input();
    Outcome output;
    output.Count = 9;
    const auto handler = [](Transaction& transaction, void*) noexcept {
        const auto result = ludus::sample::RequestSignal(transaction, true);
        if (result.Status != CommandStatus::Accepted)
        {
            return Status::InvalidInput;
        }
        transaction.State() = ludus::sample::MakeState({ .Interactions = 101 });
        return Status::Completed;
    };
    CHECK(ExecuteNative(ludus::sample::SCHEMA, input, {nullptr, Alive}, handler, nullptr, output) ==
          Status::InvalidInput);
    CHECK(output.Count == 9);
    CHECK(input.State.Items[0].Data.Scalar == 0);
}

TEST_CASE("installed provider debug pauses retain copied inputs and unpublished effects", "[behavior][debug]")
{
    LuauProvider provider;
    auto bytes = Package();
    REQUIRE(provider.Load(ludus::sample::SCHEMA, bytes) == Status::Completed);
    // The source's first executable line follows the injected declarations.
    const auto line = provider.Breakpoint(0x100, 1);
    REQUIRE(line > 0);
    auto input = Input();
    Outcome output;
    output.Count = 9;
    Diagnostic diagnostic;
    REQUIRE(provider.BeginDebug(input, {nullptr, Alive}, output, diagnostic) == Status::Paused);
    REQUIRE(output.Count == 9);
    DebugSnapshot paused;
    REQUIRE(provider.Inspect(paused));
    REQUIRE(paused.Paused);
    REQUIRE(paused.FrameCount > 0);
    REQUIRE(paused.Stop != 0);
    input.State.Items[0].Data.Scalar = 99; // Paused provider owns an independent copied invocation.
    REQUIRE(provider.Load(ludus::sample::SCHEMA, bytes) == Status::Paused);
    REQUIRE(provider.ResumeDebug(paused.Stop + 1, DebugMode::Continue, output, diagnostic) == Status::InvalidInput);
    REQUIRE(output.Count == 9);
    REQUIRE(provider.Breakpoint(0x100, line, false) > 0);
    REQUIRE(provider.ResumeDebug(paused.Stop, DebugMode::Continue, output, diagnostic) == Status::Completed);
    REQUIRE(output.State.Items[0].Data.Scalar == 1);
    REQUIRE(provider.ResumeDebug(paused.Stop, DebugMode::Continue, output, diagnostic) == Status::InvalidInput);
    REQUIRE(provider.Load(ludus::sample::SCHEMA, bytes) == Status::Completed);
    REQUIRE(provider.Breakpoint(0x100, line) > 0);
    input = Input();
    REQUIRE(provider.BeginDebug(input, {nullptr, Alive}, output, diagnostic) == Status::Paused);
    DebugSnapshot replacement;
    REQUIRE(provider.Inspect(replacement));
    REQUIRE(replacement.Stop > paused.Stop); // Stops cannot alias after a VM replacement.
    REQUIRE(provider.ResumeDebug(paused.Stop, DebugMode::Continue, output, diagnostic) == Status::InvalidInput);
    REQUIRE(provider.Close() == Status::Completed); // Cancels the partial tick before releasing candidate storage.
    REQUIRE(provider.LiveBytes() == 0);
}
TEST_CASE("debug faults and interrupts discard candidates and require explicit restart")
{
    const auto bytes = Package();
    LuauProvider provider;
    Outcome output;
    output.Count = 13;
    Diagnostic diagnostic;
    REQUIRE(provider.Load(ludus::sample::SCHEMA, bytes) == Status::Completed);
    auto input = Input(0x300);
    CHECK(provider.BeginDebug(input, {nullptr, Alive}, output, diagnostic) == Status::ScriptFault);
    CHECK(output.Count == 13);
    CHECK(provider.LiveBytes() == 0);
    REQUIRE(provider.Load(ludus::sample::SCHEMA, bytes, usize{8} * 1024 * 1024, 20) == Status::Completed);
    input = Input(0x500);
    CHECK(provider.BeginDebug(input, {nullptr, Alive}, output, diagnostic) == Status::Interrupted);
    CHECK(output.Count == 13);
    CHECK(provider.LiveBytes() == 0);
    REQUIRE(provider.Close() == Status::Completed);
}
