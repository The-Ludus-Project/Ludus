#include <ludus/runtime/resources/manager.h>

#include "internal/counters.hpp"

#include <ludus/foundation/containers/static_bit_set.hpp>

#include <atomic>
#include <new>

namespace ludus::resources
{
namespace
{
constexpr uint32 NONE = 0xFFFFFFFFU;
constexpr uint32 MAX_ASSETS = 64, MAX_EDGES = 128, MAX_SNAPSHOTS = 4;
std::atomic<uint64> NEXT_OWNER{1};
uint64 ReserveOwner() noexcept
{
    auto owner = NEXT_OWNER.load(std::memory_order_relaxed);
    while (owner != 0xFFFFFFFFFFFFFFFFULL)
    {
        if (NEXT_OWNER.compare_exchange_weak(owner, owner + 1, std::memory_order_relaxed))
        {
            return owner;
        }
    }
    return 0;
}
template <typename Record>
uint32 Free(Record* records, uint32 count) noexcept
{
    for (uint32 i = 0; i < count; ++i)
    {
        if (records[i].Used == false && records[i].Generation != NONE)
        {
            return i;
        }
    }
    return NONE;
}
template <typename Record>
void Admit(Record& record) noexcept
{
    const auto generation = record.Generation;
    record = {};
    record.Generation = generation;
    (void)internal::Advance(record.Generation);
    record.Used = true;
}
void Append(FailureInfo& failure, const content::ResourceId& id) noexcept
{
    if (failure.Count < MAX_FAILURE_CHAIN)
    {
        failure.Chain[failure.Count++] = id;
    }
    else
    {
        failure.Truncated = true;
    }
}
} // namespace
// Thanks to Scott Bilas, “A Generic Handle-Based Resource Manager,” Game
// Programming Gems 1, §1.6 pp. 68–79, and James Boer, “Resource and Memory
// Management,” §1.7 pp. 80–87, for typed identity and pre-allocation reservations.
// Thanks to Julien Hamaide, “Multithread Job and Dependency System,” Game
// Programming Gems 7, §1.9 pp. 87–96, for pending counts and dependent wakeups.
// This original implementation keeps durable outcomes, immutable scoped snapshots,
// full-closure admission and callback-free atomic binding publication; workers never
// wait on other resources. Parent holds end at physical acknowledgment, not logical
// completion. No reference implementation is copied.
// Review:
// https://github.com/The-Ludus-Project/Ludus/blob/main/docs/architecture/resource-management-reference-review.md
struct Manager::Impl final
{
    Config Limits;
    uint64 Owner = 0, Desired = 0;
    bool Stopping = false;
    struct Asset final
    {
        content::ResourceId Id;
        content::Kind Type = content::Kind::Sound;
    } Assets[MAX_ASSETS];
    uint32 AssetCount = 0, Current = NONE;
    struct CatalogSnapshot final
    {
        uint32 Generation = 0;
        bool Used = false;
        uint64 Revision = 0;
        content::Resource Resources[MAX_ASSETS];
        Adapter Adapters[8];
        uint32 AdapterCount = 0;
        struct Edge final
        {
            uint32 Parent = NONE, Child = NONE;
            content::Text<96> Property;
        } Edges[MAX_EDGES];
        uint32 EdgeCount = 0;
        uint32 Children[MAX_ASSETS][MAX_DEPENDENCIES]{};
        uint32 ChildCount[MAX_ASSETS]{};
        uint32 Order[MAX_ASSETS]{};
        uint32 OrderCount = 0;
        uint32 Bindings[MAX_ASSETS]{};
        uint32 Roots[MAX_DEPENDENCIES]{};
        uint32 RootCount = 0;
    } Catalogs[MAX_SNAPSHOTS];
    struct Scope final
    {
        uint32 Generation = 0;
        bool Used = false, Closing = false;
        uint32 Catalog = NONE;
        content::ResourceId Name;
    } Scopes[16];
    struct Request final
    {
        uint32 Generation = 0;
        bool Used = false;
        uint32 Scope = NONE, Operation = NONE, Version = NONE;
        RequestInfo Info;
    } Requests[32];
    struct Operation final
    {
        uint32 Generation = 0;
        bool Used = false, Started = false, Cleaning = false;
        uint32 Version = NONE;
    } Operations[8];
    struct Error final
    {
        Status Cause = Status::Ok;
        uint32 Asset = NONE;
        uint32 Chain[MAX_FAILURE_CHAIN]{};
        uint32 Count = 0;
        bool Truncated = false;
        uint32 Edge = NONE;
    };
    struct Version final
    {
        uint32 Generation = 0;
        bool Used = false, Live = false, Retiring = false, PhysicalRetired = false;
        bool PreparationCharged = true, ResidentCharged = true;
        uint32 Catalog = NONE, Asset = NONE, AdapterIndex = NONE, Operation = NONE;
        uint64 Epoch = 0;
        void* Candidate = nullptr;
        const void* Payload = nullptr;
        struct Binding final
        {
            uint32 Version = NONE, Edge = NONE;
        } Dependencies[MAX_DEPENDENCIES];
        uint32 DependencyCount = 0, PendingDependencies = 0, WaitingMask = 0;
        Status DependencyResult = Status::Ok;
        Error Failure;
    } Versions[16];
    struct Hold final
    {
        uint32 Generation = 0;
        bool Used = false;
        uint32 Scope = NONE, Version = NONE;
    } Leases[64];
    struct Transaction final
    {
        uint32 Generation = 0;
        bool Used = false;
        uint32 Catalog = NONE;
        uint32 Roots[MAX_DEPENDENCIES]{};
        uint32 RootCount = 0;
        TransactionInfo Info;
    } Transactions[4];
    uint64 Preparation = 0, Resident = 0;
    uint32 Cursor = 0;
    struct Plan final
    {
        ludus::foundation::StaticBitSet<MAX_ASSETS> Wanted;
        uint32 Slots[MAX_ASSETS]{};
        uint32 NewAssets[MAX_DEPENDENCIES]{};
        uint32 VersionSlots[MAX_DEPENDENCIES]{}, OperationSlots[MAX_DEPENDENCIES]{};
        uint32 AdapterIndices[MAX_ASSETS]{};
        uint64 Epochs[MAX_ASSETS]{};
        uint32 Count = 0;
        uint64 Preparation = 0, Resident = 0;
    };
    template <HandleKind K, typename Record>
    bool Valid(Handle<K> handle, const Record* records, uint32 count) const noexcept
    {
        return handle.Owner == Owner && handle.Generation != 0 && handle.Slot < count && records[handle.Slot].Used &&
               records[handle.Slot].Generation == handle.Generation;
    }
    [[nodiscard]] bool ValidAsset(AssetHandle asset) const noexcept
    {
        return asset.Owner == Owner && asset.Generation == 1 && asset.Slot < AssetCount;
    }
    [[nodiscard]] uint32 FindAsset(const content::ResourceId& id) const noexcept
    {
        for (uint32 i = 0; i < AssetCount; ++i)
        {
            if (Assets[i].Id.View() == id.View())
            {
                return i;
            }
        }
        return NONE;
    }
    [[nodiscard]] uint32 FindAdapter(uint32 catalog, content::Kind kind) const noexcept
    {
        const auto& snapshot = Catalogs[catalog];
        for (uint32 i = 0; i < snapshot.AdapterCount; ++i)
        {
            if (snapshot.Adapters[i].Type == kind)
            {
                return i;
            }
        }
        return NONE;
    }
    Adapter& Provider(Version& version) noexcept
    {
        return Catalogs[version.Catalog].Adapters[version.AdapterIndex];
    }
    [[nodiscard]] const Adapter& Provider(const Version& version) const noexcept
    {
        return Catalogs[version.Catalog].Adapters[version.AdapterIndex];
    }
    [[nodiscard]] bool Scoped(uint32 catalog) const noexcept
    {
        for (uint32 i = 0; i < Limits.Scopes; ++i)
        {
            if (Scopes[i].Used && Scopes[i].Catalog == catalog)
            {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] bool Active(const Transaction& transaction) const noexcept
    {
        return transaction.Info.State == TransactionState::Preparing ||
               transaction.Info.State == TransactionState::Ready;
    }
    [[nodiscard]] bool Held(uint32 index) const noexcept
    {
        for (uint32 i = 0; i < Limits.Requests; ++i)
        {
            const auto& request = Requests[i];
            if (request.Used && (request.Version == index ||
                                 (request.Operation != NONE && Operations[request.Operation].Version == index)))
            {
                return true;
            }
        }
        for (uint32 i = 0; i < Limits.Leases; ++i)
        {
            if (Leases[i].Used && Leases[i].Version == index)
            {
                return true;
            }
        }
        for (uint32 i = 0; i < Limits.Versions; ++i)
        {
            if (Versions[i].Used)
            {
                for (uint32 j = 0; j < Versions[i].DependencyCount; ++j)
                {
                    if (Versions[i].Dependencies[j].Version == index)
                    {
                        return true;
                    }
                }
            }
        }
        for (uint32 i = 0; i < Limits.Transactions; ++i)
        {
            if (Transactions[i].Used && Active(Transactions[i]))
            {
                for (uint32 j = 0; j < Transactions[i].RootCount; ++j)
                {
                    if (Transactions[i].Roots[j] == index)
                    {
                        return true;
                    }
                }
            }
        }
        const auto& version = Versions[index];
        const auto& snapshot = Catalogs[version.Catalog];
        if (version.Catalog == Current || Scoped(version.Catalog))
        {
            for (uint32 j = 0; j < snapshot.RootCount; ++j)
            {
                if (snapshot.Roots[j] == index)
                {
                    return true;
                }
            }
        }
        return false;
    }
    void RetireIfIdle(uint32 index) noexcept
    {
        if (index != NONE && Versions[index].Used && Versions[index].Live && Held(index) == false)
        {
            Versions[index].Retiring = true;
        }
    }
    void CancelIfUnneeded(uint32 index) noexcept
    {
        if (index == NONE || Operations[index].Used == false || Operations[index].Cleaning ||
            Held(Operations[index].Version))
        {
            return;
        }
        auto& op = Operations[index];
        op.Cleaning = true;
        auto& version = Versions[op.Version];
        if (version.Candidate != nullptr)
        {
            auto& adapter = Provider(version);
            adapter.Cancel(adapter.Context, version.Candidate);
        }
    }
    [[nodiscard]] Error OwnError(const Version& version, Status cause) const noexcept
    {
        Error error;
        error.Cause = cause;
        error.Asset = version.Asset;
        error.Chain[0] = version.Asset;
        error.Count = 1;
        return error;
    }
    [[nodiscard]] Error InheritedError(const Version& parent, Version::Binding binding, Error error) const noexcept
    {
        if (error.Count == MAX_FAILURE_CHAIN)
        {
            error.Truncated = true;
        }
        else
        {
            ++error.Count;
        }
        for (uint32 i = error.Count - 1; i > 0; --i)
        {
            error.Chain[i] = error.Chain[i - 1];
        }
        error.Chain[0] = parent.Asset;
        error.Edge = binding.Edge;
        return error;
    }
    [[nodiscard]] Status Usable(const Version& version, Error* failure = nullptr) const noexcept
    {
        const auto reject = [&](Status cause) noexcept {
            if (failure != nullptr)
            {
                *failure = OwnError(version, cause);
            }
            return cause;
        };
        if (version.PhysicalRetired || version.Payload == nullptr)
        {
            return reject(Status::Failed);
        }
        const auto& adapter = Provider(version);
        if (version.Epoch != adapter.Epoch(adapter.Context))
        {
            return reject(Status::DeviceLost);
        }
        const auto result = adapter.Validate(adapter.Context, version.Payload);
        if (result != Status::Ok)
        {
            return reject(result);
        }
        for (uint32 i = 0; i < version.DependencyCount; ++i)
        {
            const auto binding = version.Dependencies[i];
            const auto& child = Versions[binding.Version];
            Error childError;
            const Status childResult = child.Used == false || child.Live == false
                                           ? Status::DependencyFailed
                                           : Usable(child, failure != nullptr ? &childError : nullptr);
            if (childResult != Status::Ok)
            {
                if (failure != nullptr)
                {
                    if (childError.Cause == Status::Ok)
                    {
                        childError = child.Failure.Cause != Status::Ok ? child.Failure : OwnError(child, childResult);
                    }
                    *failure = InheritedError(version, binding, childError);
                }
                return childResult;
            }
        }
        return Status::Ok;
    }
    [[nodiscard]] FailureInfo Describe(const Version& version) const noexcept
    {
        FailureInfo result;
        result.Cause = version.Failure.Cause;
        if (version.Failure.Asset != NONE)
        {
            result.Asset = Assets[version.Failure.Asset].Id;
        }
        result.Truncated = version.Failure.Truncated;
        for (uint32 i = 0; i < version.Failure.Count; ++i)
        {
            Append(result, Assets[version.Failure.Chain[i]].Id);
        }
        if (version.Failure.Edge != NONE)
        {
            result.Property = Catalogs[version.Catalog].Edges[version.Failure.Edge].Property;
        }
        return result;
    }
    void SetError(Version& version, Status cause) noexcept
    {
        version.Failure = OwnError(version, cause);
    }
    void DependencyError(Version& parent, Version::Binding binding, Status cause) noexcept
    {
        const auto& child = Versions[binding.Version];
        parent.DependencyResult = Status::DependencyFailed;
        parent.Failure =
            InheritedError(parent, binding, child.Failure.Cause == Status::Ok ? OwnError(child, cause) : child.Failure);
    }
    void WakeParents(uint32 childIndex, Status result) noexcept
    {
        for (uint32 i = 0; i < Limits.Versions; ++i)
        {
            auto& parent = Versions[i];
            if (parent.Used == false || parent.Live || parent.Operation == NONE ||
                Operations[parent.Operation].Cleaning)
            {
                continue;
            }
            for (uint32 j = 0; j < parent.DependencyCount; ++j)
            {
                if (parent.Dependencies[j].Version == childIndex && (parent.WaitingMask & (1U << j)) != 0)
                {
                    LUDUS_REQUIRE(parent.PendingDependencies != 0);
                    --parent.PendingDependencies;
                    parent.WaitingMask &= ~(1U << j);
                    if (result != Status::Ok && parent.DependencyResult == Status::Ok)
                    {
                        DependencyError(parent, parent.Dependencies[j], result);
                    }
                }
            }
        }
    }
    void Complete(uint32 index, Status result) noexcept
    {
        auto& op = Operations[index];
        auto& version = Versions[op.Version];
        if (result != Status::Ok && version.Failure.Cause == Status::Ok)
        {
            SetError(version, result);
        }
        for (uint32 i = 0; i < Limits.Requests; ++i)
        {
            auto& request = Requests[i];
            if (request.Used && request.Operation == index)
            {
                request.Operation = NONE;
                request.Info.State = result == Status::Ok ? RequestState::Ready : RequestState::Failed;
                request.Info.Result = result;
                if (result == Status::Ok)
                {
                    request.Version = op.Version;
                }
                else
                {
                    request.Info.Failure = Describe(version);
                }
            }
        }
        if (result == Status::Ok)
        {
            version.Live = true;
            Preparation -= Provider(version).Peak.PreparationBytes;
            version.PreparationCharged = false;
            version.Operation = NONE;
            op.Used = false;
            WakeParents(op.Version, result);
            RetireIfIdle(op.Version);
        }
        else
        {
            op.Cleaning = true;
            WakeParents(op.Version, result);
        }
    }
    void ReleaseDependencies(uint32 index) noexcept
    {
        auto& version = Versions[index];
        uint32 children[MAX_DEPENDENCIES]{};
        const auto count = version.DependencyCount;
        for (uint32 i = 0; i < count; ++i)
        {
            children[i] = version.Dependencies[i].Version;
        }
        version.DependencyCount = 0;
        for (uint32 i = 0; i < count; ++i)
        {
            CancelIfUnneeded(Versions[children[i]].Operation);
            RetireIfIdle(children[i]);
        }
    }
    void DropRecord(uint32 index) noexcept
    {
        auto& version = Versions[index];
        auto& catalog = Catalogs[version.Catalog];
        if (catalog.Bindings[version.Asset] == index)
        {
            catalog.Bindings[version.Asset] = NONE;
        }
        ReleaseDependencies(index);
        version.Used = false;
    }
    void Acknowledge(uint32 index) noexcept
    {
        auto& version = Versions[index];
        const auto& cost = Provider(version).Peak;
        if (version.PreparationCharged)
        {
            Preparation -= cost.PreparationBytes;
            version.PreparationCharged = false;
        }
        if (version.ResidentCharged)
        {
            Resident -= cost.ResidentBytes;
            version.ResidentCharged = false;
        }
        ReleaseDependencies(index);
        version.PhysicalRetired = true;
        version.Candidate = nullptr;
        version.Payload = nullptr;
        // Dependency ownership ends at this acknowledgment, even when a failed
        // child tombstone still has a parent reference used for failure propagation.
        if (Held(index) == false)
        {
            DropRecord(index);
        }
    }
    void ServiceOperation(uint32 index) noexcept
    {
        auto& op = Operations[index];
        if (op.Used == false)
        {
            return;
        }
        auto& version = Versions[op.Version];
        auto& adapter = Provider(version);
        if (op.Cleaning)
        {
            if (version.Candidate == nullptr || adapter.Retire(adapter.Context, version.Candidate) == Status::Ok)
            {
                version.Operation = NONE;
                op.Used = false;
                Acknowledge(op.Version);
            }
            return;
        }
        if (adapter.Epoch(adapter.Context) != version.Epoch)
        {
            if (version.Candidate != nullptr)
            {
                adapter.Cancel(adapter.Context, version.Candidate);
            }
            Complete(index, Status::DeviceLost);
            return;
        }
        if (version.DependencyResult != Status::Ok)
        {
            Complete(index, version.DependencyResult);
            return;
        }
        if (version.PendingDependencies != 0)
        {
            return;
        }
        if (op.Started == false)
        {
            DependencyView views[MAX_DEPENDENCIES];
            for (uint32 i = 0; i < version.DependencyCount; ++i)
            {
                const auto binding = version.Dependencies[i];
                const auto& child = Versions[binding.Version];
                const auto usable = Usable(child);
                if (usable != Status::Ok)
                {
                    DependencyError(version, binding, usable);
                    Complete(index, Status::DependencyFailed);
                    return;
                }
                views[i].Id = Assets[child.Asset].Id;
                views[i].Type = Assets[child.Asset].Type;
                views[i].Payload = child.Payload;
                views[i].Revision = Catalogs[child.Catalog].Revision;
                views[i].Epoch = child.Epoch;
                views[i].Property = Catalogs[version.Catalog].Edges[binding.Edge].Property;
            }
            const auto& resource = Catalogs[version.Catalog].Resources[version.Asset];
            const auto result =
                adapter.BeginBound != nullptr
                    ? adapter.BeginBound(adapter.Context, resource, views, version.DependencyCount, version.Candidate)
                    : adapter.Begin(adapter.Context, resource, version.Candidate);
            if (result == Status::QueueFull && version.Candidate == nullptr)
            {
                return;
            }
            if ((result != Status::Pending && result != Status::Ok) || version.Candidate == nullptr)
            {
                Complete(index, result == Status::Ok || result == Status::Pending ? Status::Failed : result);
                return;
            }
            op.Started = true;
            for (uint32 i = 0; i < Limits.Requests; ++i)
            {
                if (Requests[i].Used && Requests[i].Operation == index)
                {
                    Requests[i].Info.State = RequestState::Working;
                }
            }
        }
        const auto result = adapter.Poll(adapter.Context, version.Candidate, version.Payload);
        if (result == Status::Pending)
        {
            return;
        }
        Complete(index,
                 result == Status::Ok
                     ? (version.Payload == nullptr ? Status::Failed : Usable(version, &version.Failure))
                     : result);
    }
    void ServiceVersion(uint32 index) noexcept
    {
        auto& version = Versions[index];
        if (version.Used == false)
        {
            return;
        }
        if (version.PhysicalRetired)
        {
            if (Held(index) == false)
            {
                DropRecord(index);
            }
        }
        else if (version.Retiring)
        {
            auto& adapter = Provider(version);
            if (adapter.Retire(adapter.Context, version.Candidate) == Status::Ok)
            {
                Acknowledge(index);
            }
        }
    }
    void StopTransaction(uint32 index, Status reason) noexcept
    {
        auto& transaction = Transactions[index];
        if (Active(transaction) == false)
        {
            return;
        }
        transaction.Info.State = reason == Status::Cancelled ? TransactionState::Cancelled : TransactionState::Failed;
        transaction.Info.Result = reason;
        const auto count = transaction.RootCount;
        transaction.RootCount = 0;
        transaction.Catalog = NONE;
        for (uint32 i = 0; i < count; ++i)
        {
            const auto root = transaction.Roots[i];
            CancelIfUnneeded(Versions[root].Operation);
            RetireIfIdle(root);
        }
    }
    void ServiceTransaction(uint32 index) noexcept
    {
        auto& transaction = Transactions[index];
        if (transaction.Used == false || Active(transaction) == false)
        {
            return;
        }
        bool ready = true;
        for (uint32 i = 0; i < transaction.RootCount; ++i)
        {
            auto& root = Versions[transaction.Roots[i]];
            if (root.Failure.Cause != Status::Ok)
            {
                transaction.Info.Failure = Describe(root);
                StopTransaction(index,
                                root.DependencyResult != Status::Ok ? Status::DependencyFailed : root.Failure.Cause);
                return;
            }
            if (root.Live == false)
            {
                ready = false;
            }
            else
            {
                const auto usable = Usable(root, &root.Failure);
                if (usable != Status::Ok)
                {
                    transaction.Info.Failure = Describe(root);
                    StopTransaction(index, usable);
                    return;
                }
            }
        }
        if (ready)
        {
            transaction.Info.State = TransactionState::Ready;
            transaction.Info.Result = Status::Ok;
        }
    }
    void PruneCatalogs() noexcept
    {
        for (uint32 i = 0; i < Limits.CatalogSnapshots; ++i)
        {
            auto& snapshot = Catalogs[i];
            if (snapshot.Used == false || i == Current || Scoped(i))
            {
                continue;
            }
            const auto count = snapshot.RootCount;
            snapshot.RootCount = 0;
            for (uint32 j = 0; j < count; ++j)
            {
                RetireIfIdle(snapshot.Roots[j]);
            }
            bool held = false;
            for (uint32 j = 0; j < Limits.Versions; ++j)
            {
                held |= Versions[j].Used && Versions[j].Catalog == i;
            }
            for (uint32 j = 0; j < Limits.Transactions; ++j)
            {
                held |= Transactions[j].Used && Active(Transactions[j]) && Transactions[j].Catalog == i;
            }
            if (held == false)
            {
                snapshot.Used = false;
            }
        }
    }
    Status Visit(CatalogSnapshot& snapshot,
                 uint32 asset,
                 uint8* marks,
                 uint32* path,
                 uint32 depth,
                 FailureInfo& failure) noexcept
    {
        marks[asset] = 1;
        path[depth] = asset;
        for (uint32 i = 0; i < snapshot.ChildCount[asset]; ++i)
        {
            const auto edgeIndex = snapshot.Children[asset][i];
            const auto& edge = snapshot.Edges[edgeIndex];
            if (marks[edge.Child] == 1)
            {
                failure.Cause = Status::Cycle;
                failure.Asset = Assets[edge.Child].Id;
                failure.Property = edge.Property;
                for (uint32 j = 0; j <= depth; ++j)
                {
                    Append(failure, Assets[path[j]].Id);
                }
                Append(failure, Assets[edge.Child].Id);
                return Status::Cycle;
            }
            if (marks[edge.Child] == 0)
            {
                const auto result = Visit(snapshot, edge.Child, marks, path, depth + 1, failure);
                if (result != Status::Ok)
                {
                    return result;
                }
            }
        }
        marks[asset] = 2;
        snapshot.Order[snapshot.OrderCount++] = asset;
        return Status::Ok;
    }
    Status CopyCatalog(uint32 slot,
                       const content::Catalog& catalog,
                       const Adapter* adapters,
                       usize count,
                       const DependencyGraph& graph,
                       FailureInfo& failure) noexcept
    {
        if (graph.Revision == 0 || count > 8 || (count != 0 && adapters == nullptr) ||
            (graph.Count != 0 && graph.Edges == nullptr))
        {
            return Status::InvalidState;
        }
        if (graph.Count > MAX_EDGES)
        {
            return Status::LimitExceeded;
        }
        if (catalog.Entries().size() != AssetCount)
        {
            return Status::Conflict;
        }
        for (usize i = 0; i < count; ++i)
        {
            const auto& adapter = adapters[i];
            if (adapter.Epoch == nullptr || (adapter.Begin == nullptr && adapter.BeginBound == nullptr) ||
                adapter.Poll == nullptr || adapter.Cancel == nullptr || adapter.Retire == nullptr ||
                adapter.Validate == nullptr || adapter.MaxCandidates == 0 || adapter.MaxCandidates > 16 ||
                adapter.Epoch(adapter.Context) == 0)
            {
                return Status::InvalidState;
            }
            for (usize j = 0; j < i; ++j)
            {
                if (adapters[j].Type == adapter.Type)
                {
                    return Status::InvalidState;
                }
            }
        }
        auto& snapshot = Catalogs[slot];
        // Reset only the bookkeeping in place. Value-assigning this large record
        // materializes a full snapshot on the Wasm stack, exceeding its 64 KiB
        // default. Every counted resource/adapter/edge is overwritten below;
        // uncounted storage is never read. Preserve the nonwrapping generation.
        snapshot.Used = false;
        snapshot.Revision = graph.Revision;
        snapshot.AdapterCount = 0;
        snapshot.EdgeCount = 0;
        snapshot.OrderCount = 0;
        snapshot.RootCount = 0;
        for (uint32 i = 0; i < MAX_ASSETS; ++i)
        {
            snapshot.Bindings[i] = NONE;
            snapshot.ChildCount[i] = 0;
        }
        ludus::foundation::StaticBitSet<MAX_ASSETS> seen;
        for (const auto& resource : catalog.Entries())
        {
            const auto index = FindAsset(resource.Id);
            if (index == NONE || seen.IsSet(index) || Assets[index].Type != resource.Type)
            {
                return Status::Conflict;
            }
            seen.Set(index);
            snapshot.Resources[index] = resource;
        }
        snapshot.AdapterCount = static_cast<uint32>(count);
        for (uint32 i = 0; i < snapshot.AdapterCount; ++i)
        {
            snapshot.Adapters[i] = adapters[i];
        }
        for (usize i = 0; i < graph.Count; ++i)
        {
            const auto& input = graph.Edges[i];
            const auto parent = FindAsset(input.Parent), child = FindAsset(input.Child);
            if (parent == NONE || child == NONE)
            {
                failure.Cause = Status::NotFound;
                failure.Asset = parent == NONE ? input.Parent : input.Child;
                failure.Property = input.Property;
                Append(failure, input.Parent);
                Append(failure, input.Child);
                return Status::NotFound;
            }
            if (input.Property.Length > 96)
            {
                return Status::Failed;
            }
            bool duplicate = false;
            for (uint32 j = 0; j < snapshot.EdgeCount; ++j)
            {
                duplicate |= snapshot.Edges[j].Parent == parent && snapshot.Edges[j].Child == child;
            }
            if (duplicate)
            {
                continue;
            }
            if (snapshot.ChildCount[parent] == MAX_DEPENDENCIES)
            {
                return Status::LimitExceeded;
            }
            const auto index = snapshot.EdgeCount++;
            snapshot.Edges[index].Parent = parent;
            snapshot.Edges[index].Child = child;
            snapshot.Edges[index].Property = input.Property;
            snapshot.Children[parent][snapshot.ChildCount[parent]++] = index;
        }
        uint8 marks[MAX_ASSETS]{};
        uint32 path[MAX_ASSETS]{};
        for (uint32 i = 0; i < AssetCount; ++i)
        {
            if (marks[i] == 0)
            {
                const auto result = Visit(snapshot, i, marks, path, 0, failure);
                if (result != Status::Ok)
                {
                    return result;
                }
            }
        }
        (void)internal::Advance(snapshot.Generation);
        snapshot.Used = true;
        return Status::Ok;
    }
    Status Preflight(uint32 catalog, const uint32* roots, uint32 rootCount, Plan& plan) const noexcept
    {
        const auto& snapshot = Catalogs[catalog];
        for (uint32 i = 0; i < AssetCount; ++i)
        {
            plan.Slots[i] = NONE;
        }
        for (uint32 i = 0; i < rootCount; ++i)
        {
            plan.Wanted.Set(roots[i]);
        }
        // Each pass moves toward children; DAG depth is bounded by AssetCount.
        for (uint32 pass = 0; pass < AssetCount; ++pass)
        {
            for (uint32 i = 0; i < snapshot.EdgeCount; ++i)
            {
                if (plan.Wanted.IsSet(snapshot.Edges[i].Parent))
                {
                    plan.Wanted.Set(snapshot.Edges[i].Child);
                }
            }
        }
        uint32 closure = 0;
        for (uint32 rank = 0; rank < snapshot.OrderCount; ++rank)
        {
            const auto asset = snapshot.Order[rank];
            if (plan.Wanted.IsSet(asset) == false)
            {
                continue;
            }
            if (++closure > MAX_DEPENDENCIES)
            {
                return Status::LimitExceeded;
            }
            const auto adapterIndex = FindAdapter(catalog, Assets[asset].Type);
            if (adapterIndex == NONE)
            {
                return Status::Unsupported;
            }
            const auto& adapter = snapshot.Adapters[adapterIndex];
            if (snapshot.ChildCount[asset] != 0 && adapter.BeginBound == nullptr)
            {
                return Status::Unsupported;
            }
            const auto epoch = adapter.Epoch(adapter.Context);
            if (epoch == 0)
            {
                return Status::DeviceLost;
            }
            plan.AdapterIndices[asset] = adapterIndex;
            plan.Epochs[asset] = epoch;
            const auto bound = snapshot.Bindings[asset];
            if (bound != NONE && Versions[bound].Used && Versions[bound].Retiring == false &&
                Versions[bound].PhysicalRetired == false)
            {
                const auto& version = Versions[bound];
                if (version.Epoch != epoch)
                {
                    return Status::DeviceLost;
                }
                if (version.Live)
                {
                    const auto usable = Usable(version);
                    if (usable != Status::Ok)
                    {
                        return usable;
                    }
                    plan.Slots[asset] = bound;
                }
                else if (version.Operation != NONE && Operations[version.Operation].Cleaning == false)
                {
                    plan.Slots[asset] = bound;
                }
            }
            for (uint32 i = 0; i < Limits.Versions && plan.Slots[asset] == NONE; ++i)
            {
                const auto& version = Versions[i];
                if (version.Used && version.Catalog == catalog && version.Asset == asset && version.Epoch == epoch &&
                    version.Retiring == false && version.PhysicalRetired == false &&
                    ((version.Live && Usable(version) == Status::Ok) ||
                     (version.Operation != NONE && Operations[version.Operation].Cleaning == false)))
                {
                    bool exact = version.DependencyCount == snapshot.ChildCount[asset];
                    for (uint32 j = 0; j < version.DependencyCount && exact; ++j)
                    {
                        const auto child = snapshot.Edges[version.Dependencies[j].Edge].Child;
                        exact = version.Dependencies[j].Version == plan.Slots[child];
                    }
                    if (exact)
                    {
                        plan.Slots[asset] = i;
                    }
                }
            }
            if (plan.Slots[asset] != NONE)
            {
                continue;
            }
            // Reserve the adapter's finite candidate slots for the entire closure,
            // including residents and physical cleanup across retained snapshots.
            // Otherwise a leaf could occupy the last slot needed by its parent.
            uint32 occupied = 0;
            for (uint32 i = 0; i < Limits.Versions; ++i)
            {
                const auto& version = Versions[i];
                if (version.Used && version.PhysicalRetired == false)
                {
                    const auto& provider = Provider(version);
                    occupied += provider.Context == adapter.Context && provider.Type == adapter.Type ? 1U : 0U;
                }
            }
            for (uint32 i = 0; i < plan.Count; ++i)
            {
                const auto& provider = snapshot.Adapters[plan.AdapterIndices[plan.NewAssets[i]]];
                occupied += provider.Context == adapter.Context && provider.Type == adapter.Type ? 1U : 0U;
            }
            if (occupied >= adapter.MaxCandidates)
            {
                return Status::QueueFull;
            }
            plan.NewAssets[plan.Count++] = asset;
            const auto& cost = adapter.Peak;
            if (cost.PreparationBytes > Limits.PreparationBytes - Preparation - plan.Preparation ||
                cost.ResidentBytes > Limits.ResidentBytes - Resident - plan.Resident)
            {
                return Status::BudgetExceeded;
            }
            plan.Preparation += cost.PreparationBytes;
            plan.Resident += cost.ResidentBytes;
        }
        uint32 versions = 0, operations = 0;
        for (uint32 i = 0; i < Limits.Versions && versions < plan.Count; ++i)
        {
            if (Versions[i].Used == false && Versions[i].Generation != NONE)
            {
                plan.VersionSlots[versions++] = i;
            }
        }
        for (uint32 i = 0; i < Limits.Operations && operations < plan.Count; ++i)
        {
            if (Operations[i].Used == false && Operations[i].Generation != NONE)
            {
                plan.OperationSlots[operations++] = i;
            }
        }
        return versions == plan.Count && operations == plan.Count ? Status::Ok : Status::QueueFull;
    }
    void AdmitPlan(uint32 catalog, Plan& plan) noexcept
    {
        auto& snapshot = Catalogs[catalog];
        for (uint32 i = 0; i < plan.Count; ++i)
        {
            const auto asset = plan.NewAssets[i], versionIndex = plan.VersionSlots[i],
                       operationIndex = plan.OperationSlots[i];
            auto& version = Versions[versionIndex];
            auto& op = Operations[operationIndex];
            Admit(version);
            Admit(op);
            version.Catalog = catalog;
            version.Asset = asset;
            version.AdapterIndex = plan.AdapterIndices[asset];
            version.Epoch = plan.Epochs[asset];
            version.Operation = operationIndex;
            op.Version = versionIndex;
            plan.Slots[asset] = versionIndex;
        }
        for (uint32 i = 0; i < plan.Count; ++i)
        {
            const auto asset = plan.NewAssets[i];
            auto& version = Versions[plan.Slots[asset]];
            version.DependencyCount = snapshot.ChildCount[asset];
            for (uint32 j = 0; j < version.DependencyCount; ++j)
            {
                const auto edge = snapshot.Children[asset][j], child = plan.Slots[snapshot.Edges[edge].Child];
                version.Dependencies[j] = {child, edge};
                if (Versions[child].Live == false)
                {
                    ++version.PendingDependencies;
                    version.WaitingMask |= 1U << j;
                }
            }
        }
        Preparation += plan.Preparation;
        Resident += plan.Resident;
    }
    Status NewScope(uint32 catalog, const content::ResourceId& name, ScopeHandle& output) noexcept
    {
        if (output.Owner != 0 || output.Slot != 0 || output.Generation != 0 || content::ValidId(name.View()) == false)
        {
            return Status::InvalidState;
        }
        const auto slot = Free(Scopes, Limits.Scopes);
        if (slot == NONE)
        {
            return Status::QueueFull;
        }
        auto& scope = Scopes[slot];
        Admit(scope);
        scope.Catalog = catalog;
        scope.Name = name;
        output = {Owner, slot, scope.Generation};
        return Status::Ok;
    }
};
Manager::~Manager() noexcept
{
    LUDUS_REQUIRE(Shutdown() == Status::Ok);
    delete mImpl;
}
Status Manager::Initialize(const content::Catalog& catalog,
                           const Adapter* adapters,
                           usize count,
                           const Config& config,
                           const DependencyGraph& graph,
                           FailureInfo* diagnostic) noexcept
{
    if (mImpl != nullptr &&
        (mImpl->Stopping == false || Inspect().Versions != 0 || Inspect().Requests != 0 || Inspect().Leases != 0))
    {
        return Status::InvalidState;
    }
    if (config.Assets == 0 || config.Assets > MAX_ASSETS || config.Scopes == 0 || config.Scopes > 16 ||
        config.Requests == 0 || config.Requests > 32 || config.Operations == 0 || config.Operations > 8 ||
        config.Versions == 0 || config.Versions > 16 || config.Leases == 0 || config.Leases > 64 ||
        config.CatalogSnapshots == 0 || config.CatalogSnapshots > MAX_SNAPSHOTS || config.Transactions == 0 ||
        config.Transactions > 4)
    {
        return Status::InvalidState;
    }
    if (catalog.Entries().size() > config.Assets)
    {
        return Status::QueueFull;
    }
    if (sizeof(Impl) > config.MetadataBytes)
    {
        return Status::BudgetExceeded;
    }
    auto* candidate = new (std::nothrow) Impl();
    if (candidate == nullptr)
    {
        return Status::OutOfMemory;
    }
    candidate->Limits = config;
    for (const auto& entry : catalog.Entries())
    {
        auto& asset = candidate->Assets[candidate->AssetCount++];
        asset.Id = entry.Id;
        asset.Type = entry.Type;
    }
    FailureInfo failure;
    const auto copied = candidate->CopyCatalog(0, catalog, adapters, count, graph, failure);
    if (copied != Status::Ok)
    {
        if (diagnostic != nullptr)
        {
            failure.Cause = failure.Cause == Status::Ok ? copied : failure.Cause;
            *diagnostic = failure;
        }
        delete candidate;
        return copied;
    }
    candidate->Owner = ReserveOwner();
    if (candidate->Owner == 0)
    {
        delete candidate;
        return Status::QueueFull;
    }
    candidate->Current = 0;
    candidate->Desired = graph.Revision;
    delete mImpl;
    mImpl = candidate;
    return Status::Ok;
}
Status Manager::Resolve(const content::ResourceId& id, content::Kind type, AssetHandle& output) const noexcept
{
    if (output.Owner != 0 || output.Slot != 0 || output.Generation != 0 || mImpl == nullptr || mImpl->Stopping)
    {
        return Status::InvalidState;
    }
    const auto index = mImpl->FindAsset(id);
    if (index == NONE || mImpl->Assets[index].Type != type)
    {
        return Status::NotFound;
    }
    output = {mImpl->Owner, index, 1};
    return Status::Ok;
}
Status Manager::OpenScope(const content::ResourceId& name, ScopeHandle& output) noexcept
{
    return mImpl == nullptr || mImpl->Stopping ? Status::InvalidState : mImpl->NewScope(mImpl->Current, name, output);
}
Status Manager::BeginAcquire(ScopeHandle scope, AssetHandle asset, RequestHandle& output) noexcept
{
    if (output.Owner != 0 || output.Slot != 0 || output.Generation != 0 || mImpl == nullptr || mImpl->Stopping)
    {
        return Status::InvalidState;
    }
    auto& state = *mImpl;
    if (state.Valid(scope, state.Scopes, state.Limits.Scopes) == false || state.ValidAsset(asset) == false)
    {
        return Status::InvalidHandle;
    }
    if (state.Scopes[scope.Slot].Closing)
    {
        return Status::InvalidState;
    }
    const auto requestSlot = Free(state.Requests, state.Limits.Requests);
    if (requestSlot == NONE)
    {
        return Status::QueueFull;
    }
    const auto catalog = state.Scopes[scope.Slot].Catalog;
    Impl::Plan plan;
    const auto result = state.Preflight(catalog, &asset.Slot, 1, plan);
    if (result != Status::Ok)
    {
        return result;
    }
    state.AdmitPlan(catalog, plan);
    auto& request = state.Requests[requestSlot];
    Admit(request);
    request.Scope = scope.Slot;
    const auto version = plan.Slots[asset.Slot];
    if (state.Versions[version].Live)
    {
        request.Version = version;
        request.Info.State = RequestState::Ready;
        request.Info.Result = Status::Ok;
    }
    else
    {
        request.Operation = state.Versions[version].Operation;
        if (state.Operations[request.Operation].Started)
        {
            request.Info.State = RequestState::Working;
        }
    }
    output = {state.Owner, requestSlot, request.Generation};
    return Status::Ok;
}
Status Manager::GetRequest(RequestHandle request, RequestInfo& output) const noexcept
{
    if (mImpl == nullptr || mImpl->Valid(request, mImpl->Requests, mImpl->Limits.Requests) == false)
    {
        return Status::InvalidHandle;
    }
    output = mImpl->Requests[request.Slot].Info;
    return Status::Ok;
}
Status Manager::TakeLease(RequestHandle request, Lease& output) noexcept
{
    if (output.mManager != nullptr)
    {
        return Status::InvalidState;
    }
    if (mImpl == nullptr || mImpl->Valid(request, mImpl->Requests, mImpl->Limits.Requests) == false)
    {
        return Status::InvalidHandle;
    }
    auto& record = mImpl->Requests[request.Slot];
    if (record.Info.State != RequestState::Ready)
    {
        return record.Info.Result;
    }
    const auto usable = mImpl->Usable(mImpl->Versions[record.Version]);
    if (usable != Status::Ok)
    {
        return usable;
    }
    const auto slot = Free(mImpl->Leases, mImpl->Limits.Leases);
    if (slot == NONE)
    {
        return Status::QueueFull;
    }
    auto& hold = mImpl->Leases[slot];
    Admit(hold);
    hold.Scope = record.Scope;
    hold.Version = record.Version;
    record.Used = false;
    output.mManager = this;
    output.mToken = {mImpl->Owner, slot, hold.Generation};
    return Status::Ok;
}
Status Manager::Cancel(RequestHandle request) noexcept
{
    if (mImpl == nullptr || mImpl->Valid(request, mImpl->Requests, mImpl->Limits.Requests) == false)
    {
        return Status::InvalidHandle;
    }
    auto& record = mImpl->Requests[request.Slot];
    const auto operation = record.Operation, version = record.Version;
    record.Operation = NONE;
    record.Version = NONE;
    record.Info = {};
    record.Info.State = RequestState::Cancelled;
    record.Info.Result = Status::Cancelled;
    mImpl->CancelIfUnneeded(operation);
    mImpl->RetireIfIdle(version);
    return Status::Ok;
}
Status Manager::EndRequest(RequestHandle request) noexcept
{
    const auto result = Cancel(request);
    if (result == Status::Ok)
    {
        mImpl->Requests[request.Slot].Used = false;
    }
    return result;
}
Status Manager::Borrow(LeaseToken lease, content::Kind type, const void*& output) const noexcept
{
    if (mImpl == nullptr || mImpl->Valid(lease, mImpl->Leases, mImpl->Limits.Leases) == false)
    {
        return Status::InvalidHandle;
    }
    const auto& version = mImpl->Versions[mImpl->Leases[lease.Slot].Version];
    if (mImpl->Assets[version.Asset].Type != type)
    {
        return Status::InvalidHandle;
    }
    const auto result = mImpl->Usable(version);
    if (result == Status::Ok)
    {
        output = version.Payload;
    }
    return result;
}
Status Manager::BorrowDependency(LeaseToken parent,
                                 const content::ResourceId& child,
                                 content::Kind type,
                                 const void*& output) const noexcept
{
    if (mImpl == nullptr || mImpl->Valid(parent, mImpl->Leases, mImpl->Limits.Leases) == false)
    {
        return Status::InvalidHandle;
    }
    const auto& version = mImpl->Versions[mImpl->Leases[parent.Slot].Version];
    const auto usable = mImpl->Usable(version);
    if (usable != Status::Ok)
    {
        return usable;
    }
    for (uint32 i = 0; i < version.DependencyCount; ++i)
    {
        const auto& dependency = mImpl->Versions[version.Dependencies[i].Version];
        if (mImpl->Assets[dependency.Asset].Id.View() == child.View())
        {
            if (mImpl->Assets[dependency.Asset].Type != type)
            {
                return Status::InvalidHandle;
            }
            output = dependency.Payload;
            return Status::Ok;
        }
    }
    return Status::NotFound;
}
Status Manager::Release(LeaseToken lease) noexcept
{
    if (mImpl == nullptr || mImpl->Valid(lease, mImpl->Leases, mImpl->Limits.Leases) == false)
    {
        return Status::InvalidHandle;
    }
    auto& hold = mImpl->Leases[lease.Slot];
    hold.Used = false;
    mImpl->RetireIfIdle(hold.Version);
    return Status::Ok;
}
Status Manager::BeginReplacement(const content::Catalog& catalog,
                                 const Adapter* adapters,
                                 usize count,
                                 const DependencyGraph& graph,
                                 const AssetHandle* roots,
                                 usize rootCount,
                                 TransactionHandle& output,
                                 FailureInfo* diagnostic) noexcept
{
    if (mImpl == nullptr || mImpl->Stopping || output.Owner != 0 || output.Slot != 0 || output.Generation != 0)
    {
        return Status::InvalidState;
    }
    auto& state = *mImpl;
    if (graph.Revision == 0 || graph.Revision < state.Desired ||
        graph.Revision <= state.Catalogs[state.Current].Revision)
    {
        return Status::Conflict;
    }
    if (roots == nullptr || rootCount == 0 || rootCount > MAX_DEPENDENCIES)
    {
        return Status::InvalidState;
    }
    uint32 selected[MAX_DEPENDENCIES]{};
    uint32 selectedCount = 0;
    for (usize i = 0; i < rootCount; ++i)
    {
        if (state.ValidAsset(roots[i]) == false)
        {
            return Status::InvalidHandle;
        }
        bool duplicate = false;
        for (uint32 j = 0; j < selectedCount; ++j)
        {
            duplicate |= selected[j] == roots[i].Slot;
        }
        if (duplicate == false)
        {
            selected[selectedCount++] = roots[i].Slot;
        }
    }
    for (uint32 i = 0; i < state.Limits.Transactions; ++i)
    {
        if (state.Transactions[i].Used && state.Active(state.Transactions[i]) &&
            state.Transactions[i].Info.Revision == graph.Revision)
        {
            return Status::Conflict;
        }
    }
    if (graph.Revision > state.Desired)
    {
        state.Desired = graph.Revision;
        for (uint32 i = 0; i < state.Limits.Transactions; ++i)
        {
            if (state.Transactions[i].Used && state.Active(state.Transactions[i]))
            {
                state.StopTransaction(i, Status::Conflict);
            }
        }
        state.PruneCatalogs();
    }
    const auto transactionSlot = Free(state.Transactions, state.Limits.Transactions);
    const auto catalogSlot = Free(state.Catalogs, state.Limits.CatalogSnapshots);
    if (transactionSlot == NONE || catalogSlot == NONE)
    {
        return Status::QueueFull;
    }
    FailureInfo failure;
    const auto copied = state.CopyCatalog(catalogSlot, catalog, adapters, count, graph, failure);
    if (copied != Status::Ok)
    {
        if (diagnostic != nullptr)
        {
            failure.Cause = failure.Cause == Status::Ok ? copied : failure.Cause;
            *diagnostic = failure;
        }
        return copied;
    }
    Impl::Plan plan;
    const auto result = state.Preflight(catalogSlot, selected, selectedCount, plan);
    if (result != Status::Ok)
    {
        state.Catalogs[catalogSlot].Used = false;
        return result;
    }
    state.AdmitPlan(catalogSlot, plan);
    auto& transaction = state.Transactions[transactionSlot];
    Admit(transaction);
    transaction.Catalog = catalogSlot;
    transaction.Info.Revision = graph.Revision;
    transaction.RootCount = selectedCount;
    for (uint32 i = 0; i < selectedCount; ++i)
    {
        transaction.Roots[i] = plan.Slots[selected[i]];
    }
    for (uint32 i = 0; i < state.AssetCount; ++i)
    {
        if (plan.Wanted.IsSet(i))
        {
            state.Catalogs[catalogSlot].Bindings[i] = plan.Slots[i];
        }
    }
    output = {state.Owner, transactionSlot, transaction.Generation};
    return Status::Ok;
}
Status Manager::GetTransaction(TransactionHandle transaction, TransactionInfo& output) const noexcept
{
    if (mImpl == nullptr || mImpl->Valid(transaction, mImpl->Transactions, mImpl->Limits.Transactions) == false)
    {
        return Status::InvalidHandle;
    }
    output = mImpl->Transactions[transaction.Slot].Info;
    return Status::Ok;
}
Status Manager::OpenTransactionScope(TransactionHandle transaction,
                                     const content::ResourceId& name,
                                     ScopeHandle& output) noexcept
{
    if (mImpl == nullptr || mImpl->Stopping)
    {
        return Status::InvalidState;
    }
    if (mImpl->Valid(transaction, mImpl->Transactions, mImpl->Limits.Transactions) == false)
    {
        return Status::InvalidHandle;
    }
    const auto& record = mImpl->Transactions[transaction.Slot];
    return mImpl->Active(record) ? mImpl->NewScope(record.Catalog, name, output) : Status::InvalidState;
}
Status Manager::Commit(TransactionHandle transaction) noexcept
{
    if (mImpl == nullptr || mImpl->Stopping)
    {
        return Status::InvalidState;
    }
    auto& state = *mImpl;
    if (state.Valid(transaction, state.Transactions, state.Limits.Transactions) == false)
    {
        return Status::InvalidHandle;
    }
    auto& record = state.Transactions[transaction.Slot];
    if (state.Active(record) == false)
    {
        return record.Info.State == TransactionState::Committed ? Status::InvalidState : record.Info.Result;
    }
    state.ServiceTransaction(transaction.Slot);
    if (state.Active(record) == false)
    {
        return record.Info.Result;
    }
    if (record.Info.State != TransactionState::Ready)
    {
        return Status::Pending;
    }
    if (record.Info.Revision != state.Desired)
    {
        state.StopTransaction(transaction.Slot, Status::Conflict);
        return Status::Conflict;
    }
    // Preflight above is the last fallible/callback stage. The binding switch and
    // transfer below only write fixed owner-side records; retirement follows later.
    auto& snapshot = state.Catalogs[record.Catalog];
    snapshot.RootCount = record.RootCount;
    for (uint32 i = 0; i < record.RootCount; ++i)
    {
        snapshot.Roots[i] = record.Roots[i];
    }
    state.Current = record.Catalog;
    record.Catalog = NONE;
    record.RootCount = 0;
    record.Info.State = TransactionState::Committed;
    record.Info.Result = Status::Ok;
    return Status::Ok;
}
Status Manager::CancelTransaction(TransactionHandle transaction) noexcept
{
    if (mImpl == nullptr || mImpl->Valid(transaction, mImpl->Transactions, mImpl->Limits.Transactions) == false)
    {
        return Status::InvalidHandle;
    }
    if (mImpl->Transactions[transaction.Slot].Info.State == TransactionState::Committed)
    {
        return Status::InvalidState;
    }
    mImpl->StopTransaction(transaction.Slot, Status::Cancelled);
    return Status::Ok;
}
Status Manager::EndTransaction(TransactionHandle transaction) noexcept
{
    if (mImpl == nullptr || mImpl->Valid(transaction, mImpl->Transactions, mImpl->Limits.Transactions) == false)
    {
        return Status::InvalidHandle;
    }
    mImpl->StopTransaction(transaction.Slot, Status::Cancelled);
    mImpl->Transactions[transaction.Slot].Used = false;
    return Status::Ok;
}
Status Manager::GetScope(ScopeHandle scope, ScopeInfo& output) const noexcept
{
    if (mImpl == nullptr || mImpl->Valid(scope, mImpl->Scopes, mImpl->Limits.Scopes) == false)
    {
        return Status::InvalidHandle;
    }
    ScopeInfo info;
    info.Name = mImpl->Scopes[scope.Slot].Name;
    info.Closing = mImpl->Scopes[scope.Slot].Closing;
    info.Revision = mImpl->Catalogs[mImpl->Scopes[scope.Slot].Catalog].Revision;
    for (uint32 i = 0; i < mImpl->Limits.Requests; ++i)
    {
        info.Requests += mImpl->Requests[i].Used && mImpl->Requests[i].Scope == scope.Slot ? 1U : 0U;
    }
    for (uint32 i = 0; i < mImpl->Limits.Leases; ++i)
    {
        info.Leases += mImpl->Leases[i].Used && mImpl->Leases[i].Scope == scope.Slot ? 1U : 0U;
    }
    output = info;
    return Status::Ok;
}
Status Manager::CloseScope(ScopeHandle scope) noexcept
{
    if (mImpl == nullptr || mImpl->Valid(scope, mImpl->Scopes, mImpl->Limits.Scopes) == false)
    {
        return Status::InvalidHandle;
    }
    mImpl->Scopes[scope.Slot].Closing = true;
    for (uint32 i = 0; i < mImpl->Limits.Requests; ++i)
    {
        if (mImpl->Requests[i].Used && mImpl->Requests[i].Scope == scope.Slot)
        {
            (void)EndRequest({mImpl->Owner, i, mImpl->Requests[i].Generation});
        }
    }
    for (uint32 i = 0; i < mImpl->Limits.Leases; ++i)
    {
        if (mImpl->Leases[i].Used && mImpl->Leases[i].Scope == scope.Slot)
        {
            return Status::Pending;
        }
    }
    mImpl->Scopes[scope.Slot].Used = false;
    return Status::Ok;
}
void Manager::Service(uint32 workItems) noexcept
{
    if (mImpl == nullptr || workItems == 0)
    {
        return;
    }
    const auto total = mImpl->Limits.Operations + mImpl->Limits.Versions + mImpl->Limits.Transactions;
    if (workItems > total)
    {
        workItems = total;
    }
    for (uint32 i = 0; i < workItems; ++i)
    {
        const auto index = mImpl->Cursor;
        mImpl->Cursor = (index + 1) % total;
        if (index < mImpl->Limits.Operations)
        {
            mImpl->ServiceOperation(index);
        }
        else if (index < mImpl->Limits.Operations + mImpl->Limits.Versions)
        {
            mImpl->ServiceVersion(index - mImpl->Limits.Operations);
        }
        else
        {
            mImpl->ServiceTransaction(index - mImpl->Limits.Operations - mImpl->Limits.Versions);
        }
    }
    mImpl->PruneCatalogs();
}
Snapshot Manager::Inspect() const noexcept
{
    Snapshot result;
    if (mImpl == nullptr)
    {
        return result;
    }
    result.MetadataBytes = sizeof(Impl);
    result.PreparationBytes = mImpl->Preparation;
    result.ResidentBytes = mImpl->Resident;
    result.CurrentRevision = mImpl->Current != NONE ? mImpl->Catalogs[mImpl->Current].Revision : 0;
    result.DesiredRevision = mImpl->Desired;
    for (uint32 i = 0; i < mImpl->Limits.Requests; ++i)
    {
        result.Requests += mImpl->Requests[i].Used ? 1U : 0U;
    }
    for (uint32 i = 0; i < mImpl->Limits.Operations; ++i)
    {
        result.Operations += mImpl->Operations[i].Used ? 1U : 0U;
    }
    for (uint32 i = 0; i < mImpl->Limits.Versions; ++i)
    {
        result.Versions += mImpl->Versions[i].Used ? 1U : 0U;
    }
    for (uint32 i = 0; i < mImpl->Limits.Leases; ++i)
    {
        result.Leases += mImpl->Leases[i].Used ? 1U : 0U;
    }
    for (uint32 i = 0; i < mImpl->Limits.CatalogSnapshots; ++i)
    {
        result.CatalogSnapshots += mImpl->Catalogs[i].Used ? 1U : 0U;
    }
    for (uint32 i = 0; i < mImpl->Limits.Transactions; ++i)
    {
        result.Transactions += mImpl->Transactions[i].Used ? 1U : 0U;
    }
    return result;
}
Status Manager::Shutdown() noexcept
{
    if (mImpl == nullptr)
    {
        return Status::Ok;
    }
    mImpl->Stopping = true;
    for (uint32 i = 0; i < mImpl->Limits.Transactions; ++i)
    {
        if (mImpl->Transactions[i].Used)
        {
            (void)EndTransaction({mImpl->Owner, i, mImpl->Transactions[i].Generation});
        }
    }
    for (uint32 i = 0; i < mImpl->Limits.Scopes; ++i)
    {
        if (mImpl->Scopes[i].Used)
        {
            (void)CloseScope({mImpl->Owner, i, mImpl->Scopes[i].Generation});
        }
    }
    mImpl->Current = NONE;
    mImpl->PruneCatalogs();
    const auto snapshot = Inspect();
    return snapshot.Versions == 0 && snapshot.Leases == 0 && snapshot.Requests == 0 ? Status::Ok : Status::Pending;
}
Lease::~Lease() noexcept
{
    (void)Release();
}
Lease::Lease(Lease&& other) noexcept : mManager(other.mManager), mToken(other.mToken)
{
    other.mManager = nullptr;
    other.mToken = {};
}
Lease& Lease::operator=(Lease&& other) noexcept
{
    if (this != &other)
    {
        (void)Release();
        mManager = other.mManager;
        mToken = other.mToken;
        other.mManager = nullptr;
        other.mToken = {};
    }
    return *this;
}
LeaseToken Lease::Token() const noexcept
{
    return mToken;
}
Status Lease::Release() noexcept
{
    const auto result = mManager == nullptr ? Status::Ok : mManager->Release(mToken);
    mManager = nullptr;
    mToken = {};
    return result;
}
} // namespace ludus::resources
