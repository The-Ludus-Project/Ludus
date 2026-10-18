#include <ludus/foundation/base/core.h>
#include <ludus/foundation/logging/log_format.hpp>

#include "acceptance.h"

namespace ludus::s1
{
inline constexpr foundation::logging::LogCategory LOG_S1{"ScriptingS1"};
bool Check(bool success, const char* name) noexcept
{
    if (success)
    {
        LUDUS_LOG_INFO(LOG_S1, "S1 PASS {}", name);
    }
    else
    {
        LUDUS_LOG_ERROR(LOG_S1, "S1 FAIL {}", name);
    }
    return success;
}
EventRecord Event(const World& world, uint32 instance, uint64 sequence, uint32 amount) noexcept
{
    return
    {
        .Tick = world.Tick,
        .At = Phase::Gameplay,
        .ProducerSequence = sequence,
        .InstanceOrder = instance,
        .Value = { .Target = Reference(world, instance), .Alias = Reference(world, instance), .Amount = amount },
    };
}
bool CommonAcceptance(Handler handler, void* user) noexcept
{
    World world;
    if (Initialize(world) == false)
    {
        return Check(false, "world-initialization");
    }
    bool passed = true;
    EventRecord first = Event(world, 0, 1);
    passed &=
        Check(Dispatch(world, &first, 1, handler, user) == Result::Accepted && world.States[0].Interactions == 1 &&
                  world.States[0].OpenRequested == false && world.CommandCount == 0 && world.Open[0] == false,
              "explicit-state-before-threshold");
    // Deliberately out of order. Script and native providers obey producer order;
    // instance/language grouping must not change the token/effect order.
    EventRecord batch[] = {Event(world, 0, 4), Event(world, 1, 2, 2)};
    passed &=
        Check(Dispatch(world, batch, 2, handler, user) == Result::Accepted && world.States[0].Interactions == 2 &&
                  world.States[1].Interactions == 2 && world.States[0].OpenRequested && world.States[1].OpenRequested &&
                  world.CommandCount == 2 && world.Commands[0].Instance == 1001 && world.Commands[0].Token == 1 &&
                  world.Commands[1].Instance == 1000 && world.Commands[1].Token == 2 && world.Open[0] == false &&
                  world.Open[1] == false,
              "ordered-atomic-command-publication");
    world.At = Phase::Intent;
    passed &= Check(Apply(world) == Result::InvalidPhase && world.Open[0] == false, "phase-checked-application");
    world.At = Phase::Gameplay;
    passed &= Check(Apply(world) == Result::Applied && world.Open[0] && world.Open[1] && world.OutcomeCount == 2 &&
                        world.Outcomes[0].Request.Token == 1 && world.Outcomes[0].Code == Result::Applied &&
                        world.Outcomes[1].Code == Result::Applied,
                    "native-luau-equivalent-effects");
    const EventRecord duplicate[] = {first, first};
    passed &=
        Check(Dispatch(world, duplicate, 2, handler, user) == Result::InvalidValue && world.States[0].Interactions == 2,
              "ambiguous-event-order-rejected");
    return passed;
}
bool CommonFaultAcceptance(Handler handler, void* user) noexcept
{
    World world;
    if (Initialize(world) == false)
    {
        return false;
    }
    const EventRecord events[] = {Event(world, 1, 3), Event(world, 0, 1, 2), Event(world, 1, 2, 10)};
    return Check(Dispatch(world, events, 3, handler, user) == Result::ScriptFault && world.Faulted &&
                     world.States[0].Interactions == 2 && world.States[0].OpenRequested &&
                     world.States[1].Interactions == 0 && world.CommandCount == 1 && world.NextToken == 2 &&
                     Apply(world) == Result::Halted,
                 "equivalent-native-luau-fault-policy");
}
bool DomainAcceptance() noexcept
{
    World world;
    if (Initialize(world) == false)
    {
        return false;
    }
    Transaction txn{ .Owner = &world, .Configuration = world.Configuration, .Instance = 1000 };
    const EntityRef valid = Reference(world, 0);
    bool passed = true;
    EntityRef invalid[] = {valid, valid, valid, valid, valid};
    ++invalid[0].World;
    ++invalid[1].Session;
    ++invalid[2].Execution;
    invalid[3].Slot = ~uint32{0};
    ++invalid[4].Generation;
    for (const EntityRef ref : invalid)
    {
        passed &= RequestDoorOpen(txn, ref, 1).Code == Result::InvalidEntity;
    }
    passed &= Check(txn.PendingCount == 0, "full-identity-checks");
    world.At = Phase::Intent;
    passed &= Check(RequestDoorOpen(txn, valid, 1).Code == Result::InvalidPhase, "command-phase-check");
    world.At = Phase::Gameplay;
    txn.Allowed = Capability::None;
    passed &= Check(RequestDoorOpen(txn, valid, 1).Code == Result::MissingCapability, "command-capability-check");
    txn.Allowed = Capability::DoorControl;
    passed &= Check(RequestDoorOpen(txn, valid, 0).Code == Result::InvalidValue &&
                        RequestDoorOpen(txn, valid, POWER_MAX + 1).Code == Result::InvalidValue,
                    "native-numeric-boundaries");
    world.QueueLimit = 0;
    txn.CandidateState.Interactions = 2;
    passed &=
        Check(RequestDoorOpen(txn, valid, 1).Code == Result::Accepted && Publish(txn) == Result::CapacityExceeded &&
                  world.CommandCount == 0 && world.States[0].Interactions == 0 && world.NextToken == 1,
              "queue-capacity-preserves-state-and-effects");
    world.QueueLimit = 4;
    passed &= Check(Publish(txn) == Result::Accepted && world.States[0].Interactions == 2 && world.CommandCount == 1,
                    "publish-after-complete-preflight");
    world.Entities.SetStructuralPhase(true);
    passed &= world.Entities.TryRelease(world.Doors[0]) == gameplay::world::Status::Success;
    gameplay::world::EntityId replacement;
    passed &= world.Entities.TryCreate(replacement) == gameplay::world::Status::Success;
    passed &= Check(IsActive(world, valid) == false && Apply(world) == Result::Applied &&
                        world.Outcomes[0].Code == Result::InvalidEntity && world.Open[0] == false,
                    "accepted-command-can-fail-at-application");
    return passed;
}
} // namespace ludus::s1
