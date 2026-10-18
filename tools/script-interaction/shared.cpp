// Thanks to Celes, de Figueiredo and Ierusalimschy, "Binding C/C++ Objects to Lua",
// Game Programming Gems 6, §4.2, pp. 341–355, for separating wrapper representation,
// boundary checking and host lifetime. Entities remain game-owned; wrappers only
// carry checked identities. See docs/architecture/scripting-gems-review.md.
#include <ludus/foundation/base/core.h>

#include "shared.h"

namespace ludus::s1
{
bool Initialize(World& world) noexcept
{
    if (world.Entities.TryInitialize(2) != gameplay::world::Status::Success)
    {
        return false;
    }
    for (auto& door : world.Doors)
    {
        if (world.Entities.TryCreate(door) != gameplay::world::Status::Success)
        {
            return false;
        }
    }
    world.At = Phase::Gameplay;
    return true;
}

EntityRef Reference(const World& world, uint32 door) noexcept
{
    if (door >= 2)
    {
        return {};
    }
    const auto entity = world.Doors[door];
    return
    {
        .World = entity.World,
        .Session = world.Session,
        .Execution = world.Execution,
        .Slot = entity.Slot,
        .Generation = entity.Generation,
    };
}

bool IsActive(const World& world, EntityRef entity) noexcept
{
    return entity.Session == world.Session && entity.Execution == world.Execution &&
           world.Entities.IsActive({ .World = entity.World, .Slot = entity.Slot, .Generation = entity.Generation });
}

const char* ResultName(Result value) noexcept
{
    switch (value)
    {
        case Result::Accepted:
            return "Accepted";
        case Result::Applied:
            return "Applied";
        case Result::AlreadyOpen:
            return "AlreadyOpen";
        case Result::InvalidEntity:
            return "InvalidEntity";
        case Result::InvalidPhase:
            return "InvalidPhase";
        case Result::MissingCapability:
            return "MissingCapability";
        case Result::CapacityExceeded:
            return "CapacityExceeded";
        case Result::InvalidValue:
            return "InvalidValue";
        case Result::ScriptFault:
            return "ScriptFault";
        case Result::AllocationFailure:
            return "AllocationFailure";
        case Result::Interrupted:
            return "Interrupted";
        case Result::Halted:
            return "Halted";
    }
    return "InvalidValue";
}

CommandResult RequestDoorOpen(Transaction& transaction, EntityRef entity, uint32 power) noexcept
{
    World& world = *transaction.Owner;
    if (world.Faulted)
    {
        return { .Code = Result::Halted };
    }
    if (world.At != Phase::Gameplay)
    {
        return { .Code = Result::InvalidPhase };
    }
    if (transaction.Allowed != Capability::DoorControl)
    {
        return { .Code = Result::MissingCapability };
    }
    if (IsActive(world, entity) == false)
    {
        return { .Code = Result::InvalidEntity };
    }
    if (power < POWER_MIN || power > POWER_MAX)
    {
        return { .Code = Result::InvalidValue };
    }
    if (transaction.PendingCount >= 2 || world.NextToken > ~uint32{0} - transaction.PendingCount)
    {
        return { .Code = Result::CapacityExceeded };
    }
    const uint32 token = world.NextToken + static_cast<uint32>(transaction.PendingCount);
    transaction.Pending[transaction.PendingCount++] =
    {
        .Target = entity,
        .Power = power,
        .Token = token,
        .Instance = transaction.Instance,
    };
    return { .Code = Result::Accepted, .Token = token };
}

Result Publish(Transaction& transaction) noexcept
{
    World& world = *transaction.Owner;
    if (Validate(transaction.CandidateState) == false)
    {
        return Result::InvalidValue;
    }
    if (world.CommandCount > world.QueueLimit || transaction.PendingCount > world.QueueLimit - world.CommandCount ||
        world.CommandCount + transaction.PendingCount > 4 || transaction.PendingCount > ~uint32{0} - world.NextToken)
    {
        return Result::CapacityExceeded;
    }
    // Complete preflight precedes publication. No allocation or engine callback
    // occurs between publishing state and copying this fixed command batch.
    for (usize i = 0; i < transaction.PendingCount; ++i)
    {
        if (IsActive(world, transaction.Pending[i].Target) == false)
        {
            return Result::InvalidEntity;
        }
    }
    world.States[transaction.InstanceOrder] = transaction.CandidateState;
    for (usize i = 0; i < transaction.PendingCount; ++i)
    {
        world.Commands[world.CommandCount++] = transaction.Pending[i];
    }
    world.NextToken += static_cast<uint32>(transaction.PendingCount);
    return Result::Accepted;
}

namespace
{
bool Before(const EventRecord& lhs, const EventRecord& rhs) noexcept
{
    if (lhs.Tick != rhs.Tick)
    {
        return lhs.Tick < rhs.Tick;
    }
    if (lhs.At != rhs.At)
    {
        return lhs.At < rhs.At;
    }
    if (lhs.ProducerSequence != rhs.ProducerSequence)
    {
        return lhs.ProducerSequence < rhs.ProducerSequence;
    }
    return lhs.InstanceOrder < rhs.InstanceOrder;
}
} // namespace

Result Dispatch(World& world, const EventRecord* events, usize count, Handler handler, void* user) noexcept
{
    if (world.Faulted)
    {
        return Result::Halted;
    }
    if (world.At != Phase::Gameplay)
    {
        return Result::InvalidPhase;
    }
    if (events == nullptr || count > 8 || handler == nullptr || Validate(world.Configuration) == false)
    {
        return Result::InvalidValue;
    }
    EventRecord captured[8];
    for (usize i = 0; i < count; ++i)
    {
        const EventRecord record = events[i];
        if (record.Tick != world.Tick || record.At != Phase::Gameplay || record.InstanceOrder >= 2 ||
            Validate(record.Value) == false)
        {
            return Result::InvalidValue;
        }
        captured[i] = record;
        usize position = i;
        while (position > 0 && Before(captured[position], captured[position - 1]))
        {
            const EventRecord temporary = captured[position - 1];
            captured[position - 1] = captured[position];
            captured[position] = temporary;
            --position;
        }
    }
    for (usize i = 1; i < count; ++i)
    {
        if (Before(captured[i - 1], captured[i]) == false)
        {
            return Result::InvalidValue; // Ambiguous order is invalid.
        }
    }
    for (usize i = 0; i < count; ++i)
    {
        const EventRecord& event = captured[i];
        Transaction candidate
        {
            .Owner = &world,
            .Configuration = world.Configuration,
            .CandidateState = world.States[event.InstanceOrder],
            .Event = event.Value,
            .Instance = uint64{1000} + event.InstanceOrder,
            .InstanceOrder = event.InstanceOrder,
        };
        Result result = handler(candidate, user);
        if (result == Result::Accepted)
        {
            result = Publish(candidate);
        }
        if (result != Result::Accepted)
        {
            world.Faulted = true;
            return result;
        }
    }
    return Result::Accepted;
}

Result Apply(World& world) noexcept
{
    if (world.Faulted)
    {
        return Result::Halted;
    }
    if (world.At != Phase::Gameplay)
    {
        return Result::InvalidPhase;
    }
    world.OutcomeCount = 0;
    for (usize i = 0; i < world.CommandCount; ++i)
    {
        const Command command = world.Commands[i];
        Result result = Result::InvalidEntity;
        if (IsActive(world, command.Target))
        {
            for (usize door = 0; door < 2; ++door)
            {
                if (world.Doors[door].Slot != command.Target.Slot)
                {
                    continue;
                }
                result = world.Open[door] ? Result::AlreadyOpen : Result::Applied;
                world.Open[door] = true;
                break;
            }
        }
        world.Outcomes[world.OutcomeCount++] = { .Request = command, .Code = result };
    }
    world.CommandCount = 0;
    return Result::Applied;
}

Result NativeInteraction(Transaction& transaction, void* /*user*/) noexcept
{
    const uint32 amount = transaction.Event.Amount;
    if (transaction.CandidateState.Interactions > STATE_INTERACTIONS_MAX - amount)
    {
        return Result::InvalidValue;
    }
    transaction.CandidateState.Interactions += amount;
    if (transaction.CandidateState.Interactions >= transaction.Configuration.Threshold &&
        transaction.CandidateState.OpenRequested == false)
    {
        const CommandResult result = RequestDoorOpen(transaction, transaction.Event.Target, amount);
        if (result.Code == Result::Accepted)
        {
            transaction.CandidateState.OpenRequested = true;
        }
    }
    return Result::Accepted;
}
} // namespace ludus::s1
