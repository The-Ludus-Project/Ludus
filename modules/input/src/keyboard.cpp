#include <ludus/input/keyboard.h>

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/owned_object.hpp>

#include "internal/log_categories.h"
#include "internal/reducer.hpp"

namespace ludus::input
{
// Production reducer: the frozen capacities. SeqGuard = 0 means the real
// uint64 sequence space (tests use a tiny guard to probe exhaustion).
struct InputSystem::Impl final
{
    internal::Reducer<PENDING_CAPACITY, ACTION_CAPACITY, BINDING_CAPACITY, 0> Core;
};

InputSystem::InputSystem() noexcept
{
    // The single allocation in the system (K12: no hot-path allocation). Report
    // failure explicitly via IsValid(); never throw. The owner frees through the
    // same domain when the system is destroyed.
    const ludus::foundation::OwnershipStatus status =
        ludus::foundation::TryCreateOwned<Impl>(ludus::foundation::GetSystemAllocationDomain(), mImpl);
    if (status != ludus::foundation::OwnershipStatus::Ok)
    {
        LUDUS_LOG_ERROR(LOG_INPUT, "Failed to allocate input reducer storage");
    }
}

InputSystem::~InputSystem() noexcept = default;

AdmissionStatus InputSystem::Ingest(const KeyboardRecord& record) noexcept
{
    if (mImpl.Get() == nullptr)
    {
        return AdmissionStatus::RejectedInvalid;
    }
    return mImpl->Core.ingest(record);
}

bool InputSystem::GetLiveDown(Key key) const noexcept
{
    return mImpl && mImpl->Core.getLiveDown(key);
}

StepStatus InputSystem::ConsumeStep(uint64 stepId) noexcept
{
    if (mImpl.Get() == nullptr)
    {
        return StepStatus::InvalidStep;
    }
    return mImpl->Core.consumeStep(stepId);
}

const KeyboardSnapshot& InputSystem::GetKeyboardSnapshot() const noexcept
{
    // A valid system always has a published (initially neutral) snapshot. For an
    // invalid system return a shared neutral snapshot so callers never deref null.
    static const KeyboardSnapshot kNeutral{};
    if (mImpl.Get() == nullptr)
    {
        return kNeutral;
    }
    return mImpl->Core.snapshot();
}

std::span<const StepEvent> InputSystem::GetStepEvents() const noexcept
{
    if (mImpl.Get() == nullptr)
    {
        return {};
    }
    return std::span<const StepEvent>(mImpl->Core.stepEvents(), mImpl->Core.stepEventCount());
}

ActionStatus InputSystem::GetAction(ActionId id, ActionState& out) const noexcept
{
    if (mImpl.Get() == nullptr)
    {
        return ActionStatus::InvalidAction;
    }
    return mImpl->Core.getAction(id, out);
}

BindingStatus InputSystem::ReplaceBindings(const BindingMap& map) noexcept
{
    if (mImpl.Get() == nullptr)
    {
        return BindingStatus::CapacityExceeded;
    }
    return mImpl->Core.replaceBindings(map.Bindings);
}

uint32 InputSystem::GetMapVersion() const noexcept
{
    return mImpl ? mImpl->Core.mapVersion() : 0;
}

void InputSystem::RequestReset(ResetReason reason, const FocusBaseline& baseline) noexcept
{
    if (mImpl)
    {
        mImpl->Core.requestReset(reason, baseline);
    }
}

const InputCounters& InputSystem::GetCounters() const noexcept
{
    static const InputCounters kEmpty{};
    if (mImpl.Get() == nullptr)
    {
        return kEmpty;
    }
    return mImpl->Core.counters();
}

void InputSystem::SetDebugTrace(InputDebugTrace* trace) noexcept
{
    if (mImpl)
    {
        mImpl->Core.setTrace(trace);
    }
}
} // namespace ludus::input
