#pragma once
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/gameplay/world/entity_id.h>

#include <limits>

namespace ludus::world_demo
{
using namespace foundation;
using gameplay::world::EntityId;
using gameplay::world::Status;

/// Generation-checked handle to one cancellation scope.
///
/// The default value names no scope: work carrying it is never cancelled. A token whose scope was closed reads as
/// cancelled, so work that outlives its owner never resumes against a recycled scope.
struct CancellationToken final
{
    /// One-based slot in the owning table; zero names no scope.
    uint32 Slot = 0;
    /// Slot generation the token was issued for; closing the scope retires it.
    uint32 Generation = 0;
    [[nodiscard]] constexpr bool operator==(const CancellationToken&) const noexcept = default;
};

/// Bounded table of cancellation scopes owned by one world or session (docs/architecture/gameplay-foundations.md).
///
/// A scope is opened by its owner, cancelled at most once and closed when the owner retires it. Work records a token at
/// admission and the consumer revalidates it at delivery, so a cancellation that arrives after a batch was captured
/// still suppresses the undelivered work. A scope may name an owning entity; retiring that entity cancels the scope.
/// Every operation is bounded by Capacity and allocates nothing. Not thread-safe: the owner calls it from its own
/// thread at its declared phases.
template <usize Capacity>
class CancellationScopes final
{
public:
    static_assert(Capacity > 0);

    /// Open a scope, optionally owned by an entity whose retirement cancels it.
    /// @param owner Owning entity, or the default EntityId for a scope without an owning entity.
    /// @param outToken Receives the scope's token; unchanged on failure.
    /// @return CapacityExceeded when every slot is open or retired by generation exhaustion.
    [[nodiscard]] Status Open(EntityId owner, CancellationToken& outToken) noexcept
    {
        for (usize index = 0; index < Capacity; ++index)
        {
            Scope& scope = mScopes[index];
            if (scope.IsOpen == true || scope.Generation == std::numeric_limits<uint32>::max())
            {
                continue;
            }
            ++scope.Generation;
            scope.Owner = owner;
            scope.IsOpen = true;
            scope.IsCancelled = false;
            outToken = { .Slot = static_cast<uint32>(index + 1), .Generation = scope.Generation };
            return Status::Success;
        }
        return Status::CapacityExceeded;
    }
    /// Cancel an open scope.
    /// @return true only for the call that cancelled an open, not yet cancelled scope; false for the default token,
    /// a closed or stale token and every repeated cancellation.
    [[nodiscard]] bool Cancel(CancellationToken token) noexcept
    {
        Scope* scopeOrNull = findOpen(token);
        if (scopeOrNull == nullptr || scopeOrNull->IsCancelled == true)
        {
            return false;
        }
        scopeOrNull->IsCancelled = true;
        ++mCancellations;
        return true;
    }
    /// Whether work carrying `token` must not run: the scope was cancelled, or it was closed and its generation
    /// retired. The default token is never cancelled.
    [[nodiscard]] bool IsCancelled(CancellationToken token) const noexcept
    {
        if (token.Slot == 0)
        {
            return false;
        }
        const Scope* scopeOrNull = findOpen(token);
        return scopeOrNull == nullptr || scopeOrNull->IsCancelled == true;
    }
    /// Whether `token` names a scope that is still open, cancelled or not.
    [[nodiscard]] bool IsOpen(CancellationToken token) const noexcept
    {
        return findOpen(token) != nullptr;
    }
    /// Close a scope and retire its generation; tokens issued for it then read as cancelled.
    /// @return InvalidEntity for the default, a closed or a stale token; nothing changes.
    [[nodiscard]] Status Close(CancellationToken token) noexcept
    {
        Scope* scopeOrNull = findOpen(token);
        if (scopeOrNull == nullptr)
        {
            return Status::InvalidEntity;
        }
        scopeOrNull->IsOpen = false;
        scopeOrNull->IsCancelled = false;
        scopeOrNull->Owner = {};
        return Status::Success;
    }
    /// Cancel every open scope owned by `owner`, for entity retirement.
    /// @return Scopes this call cancelled; already cancelled scopes are not counted again.
    usize CancelOwnedBy(EntityId owner) noexcept
    {
        if (owner == EntityId{})
        {
            return 0;
        }
        usize cancelled = 0;
        for (Scope& scope : mScopes)
        {
            if (scope.IsOpen == true && scope.IsCancelled == false && scope.Owner == owner)
            {
                scope.IsCancelled = true;
                ++mCancellations;
                ++cancelled;
            }
        }
        return cancelled;
    }
    /// Open scopes, cancelled or not.
    [[nodiscard]] usize GetOpenCount() const noexcept
    {
        usize count = 0;
        for (const Scope& scope : mScopes)
        {
            count += scope.IsOpen == true ? 1 : 0;
        }
        return count;
    }
    /// Scopes cancelled over the table's lifetime, each counted once.
    [[nodiscard]] LUDUS_INLINE uint64 GetCancellations() const noexcept
    {
        return mCancellations;
    }

private:
    struct Scope final
    {
        EntityId Owner{};
        uint32 Generation = 0;
        bool IsOpen = false;
        bool IsCancelled = false;
    };
    [[nodiscard]] Scope* findOpen(CancellationToken token) noexcept
    {
        if (token.Slot == 0 || token.Slot > Capacity)
        {
            return nullptr;
        }
        Scope& scope = mScopes[token.Slot - 1];
        return scope.IsOpen == true && scope.Generation == token.Generation ? &scope : nullptr;
    }
    [[nodiscard]] const Scope* findOpen(CancellationToken token) const noexcept
    {
        if (token.Slot == 0 || token.Slot > Capacity)
        {
            return nullptr;
        }
        const Scope& scope = mScopes[token.Slot - 1];
        return scope.IsOpen == true && scope.Generation == token.Generation ? &scope : nullptr;
    }
    foundation::StaticArray<Scope, Capacity> mScopes{};
    uint64 mCancellations = 0;
};
} // namespace ludus::world_demo
