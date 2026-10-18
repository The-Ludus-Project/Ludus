#pragma once

// Included in the existing Catch2 translation unit; each fixture owns its profile.
namespace dag_cases
{
struct GraphBackend final
{
    struct Payload final
    {
        uint32 Value = 0;
        const Payload* Children[8]{};
        usize Count = 0;
    };
    struct Job final
    {
        GraphBackend* Owner = nullptr;
        bool Used = false;
        uint32 Node = 0;
        Payload Data;
    } Jobs[16];
    uint32 Revision = 1;
    uint64 Epoch = 1;
    uint32 Begins[4]{}, Retires[4]{}, Cancels = 0;
    bool Complete[4]{true, true, true, true};
    bool Drain[4]{true, true, true, true};
    r::Status Result[4]{r::Status::Ok, r::Status::Ok, r::Status::Ok, r::Status::Ok};
    bool FailOwned = false;
    bool Invalid[4]{};
    static uint32 Node(const c::ResourceId& id) noexcept
    {
        return id.View() == "root" ? 0U : id.View() == "left" ? 1U : id.View() == "right" ? 2U : 3U;
    }
    static r::Status PollJob(void* context, void* candidate, const void*& payload) noexcept
    {
        auto& self = *static_cast<GraphBackend*>(context);
        const auto& job = *static_cast<Job*>(candidate);
        if (static_cast<Job*>(candidate)->Owner != static_cast<GraphBackend*>(context))
        {
            return r::Status::InvalidHandle;
        }
        if (self.Complete[job.Node] == false)
        {
            return r::Status::Pending;
        }
        const auto result = self.Result[job.Node];
        if (result == r::Status::Ok)
        {
            payload = &job.Data;
        }
        return result;
    }
    static r::Status RetireJob(void* context, void* candidate) noexcept
    {
        auto& self = *static_cast<GraphBackend*>(context);
        auto& job = *static_cast<Job*>(candidate);
        if (static_cast<Job*>(candidate)->Owner != static_cast<GraphBackend*>(context))
        {
            return r::Status::InvalidHandle;
        }
        if (self.Drain[job.Node] == false)
        {
            return r::Status::Pending;
        }
        ++self.Retires[job.Node];
        job.Used = false;
        return r::Status::Ok;
    }
    static r::Status ValidatePayload(void* context, const void* payload) noexcept
    {
        return static_cast<GraphBackend*>(context)->Invalid[static_cast<const Payload*>(payload)->Value % 10]
                   ? r::Status::Failed
                   : r::Status::Ok;
    }
    r::Adapter Descriptor() noexcept
    {
        r::Adapter adapter;
        adapter.Context = this;
        adapter.Peak = {10, 20};
        adapter.Epoch = [](void* context) noexcept { return static_cast<GraphBackend*>(context)->Epoch; };
        adapter.BeginBound = [](void* context,
                                const c::Resource& resource,
                                const r::DependencyView* children,
                                usize count,
                                void*& candidate) noexcept {
            auto& self = *static_cast<GraphBackend*>(context);
            const auto node = Node(resource.Id);
            for (auto& job : self.Jobs)
            {
                if (job.Used == false)
                {
                    job = {};
                    job.Owner = &self;
                    job.Used = true;
                    job.Node = node;
                    job.Data.Value = self.Revision * 10 + node;
                    job.Data.Count = count;
                    for (usize i = 0; i < count; ++i)
                    {
                        job.Data.Children[i] = static_cast<const Payload*>(children[i].Payload);
                    }
                    ++self.Begins[node];
                    candidate = &job;
                    return self.FailOwned && node == 0 ? r::Status::OutOfMemory : r::Status::Pending;
                }
            }
            return r::Status::QueueFull;
        };
        adapter.Poll = &PollJob;
        adapter.Cancel = [](void* context, void*) noexcept { ++static_cast<GraphBackend*>(context)->Cancels; };
        adapter.Retire = &RetireJob;
        adapter.Validate = &ValidatePayload;
        return adapter;
    }
};
struct Fixture final
{
    GraphBackend Initial;
    c::Catalog Catalog;
    r::DependencyEdge Edges[5];
    r::Manager Manager;
    r::AssetHandle Root;
    r::ScopeHandle Scope;
    explicit Fixture(r::Config config = {}, uint32 capacity = 16)
    {
        for (const char* name : {"root", "left", "right", "leaf"})
        {
            c::Resource resource;
            resource.Type = c::Kind::Sound;
            resource.Id = Id(name);
            REQUIRE(resource.Path.Set(name));
            REQUIRE(Catalog.Put(resource) == c::Status::Ok);
        }
        Edge(0, "root", "left");
        Edge(1, "root", "right");
        Edge(2, "left", "leaf");
        Edge(3, "right", "leaf");
        Edge(4, "left", "leaf"); // One hold, first authoring property wins.
        auto adapter = Initial.Descriptor();
        adapter.MaxCandidates = capacity;
        REQUIRE(Manager.Initialize(Catalog, &adapter, 1, config, Graph()) == r::Status::Ok);
        REQUIRE(Manager.Resolve(Id("root"), c::Kind::Sound, Root) == r::Status::Ok);
        REQUIRE(Manager.OpenScope(Id("original"), Scope) == r::Status::Ok);
    }
    void Edge(usize index, const char* parent, const char* child)
    {
        Edges[index].Parent = Id(parent);
        Edges[index].Child = Id(child);
        REQUIRE(Edges[index].Property.Set(child));
    }
    [[nodiscard]] r::DependencyGraph Graph(uint64 revision = 1) const noexcept
    {
        return {Edges, 5, revision};
    }
    void Pump(uint32 count = 12) noexcept
    {
        for (uint32 i = 0; i < count; ++i)
        {
            Manager.Service(64);
        }
    }
    r::RequestHandle Acquire(r::ScopeHandle scope)
    {
        r::RequestHandle request;
        REQUIRE(Manager.BeginAcquire(scope, Root, request) == r::Status::Ok);
        return request;
    }
    r::TransactionHandle Replace(GraphBackend& backend, uint64 revision)
    {
        r::TransactionHandle transaction;
        const auto adapter = backend.Descriptor();
        REQUIRE(Manager.BeginReplacement(Catalog, &adapter, 1, Graph(revision), &Root, 1, transaction) ==
                r::Status::Ok);
        return transaction;
    }
    const GraphBackend::Payload* Borrow(r::Lease& lease)
    {
        const void* payload = nullptr;
        REQUIRE(Manager.Borrow(lease.Token(), c::Kind::Sound, payload) == r::Status::Ok);
        return static_cast<const GraphBackend::Payload*>(payload);
    }
    ~Fixture()
    {
        for (uint32 i = 0; i < 4; ++i)
        {
            Initial.Complete[i] = true;
            Initial.Drain[i] = true;
        }
        (void)Manager.Shutdown();
        Pump(32);
    }
};
TEST_CASE("Required diamond shares its leaf and starts parents only after readiness", "[resources][dag]")
{
    Fixture f;
    f.Initial.Complete[3] = false;
    const auto first = f.Acquire(f.Scope);
    const auto second = f.Acquire(f.Scope);
    CHECK(f.Manager.Inspect().PreparationBytes == 40);
    CHECK(f.Manager.Inspect().ResidentBytes == 80);
    f.Pump();
    CHECK(f.Initial.Begins[0] == 0);
    CHECK(f.Initial.Begins[1] == 0);
    CHECK(f.Initial.Begins[2] == 0);
    CHECK(f.Initial.Begins[3] == 1);
    REQUIRE(f.Manager.Cancel(first) == r::Status::Ok);
    CHECK(f.Initial.Cancels == 0);
    f.Initial.Complete[3] = true;
    f.Pump();
    r::Lease lease;
    REQUIRE(f.Manager.TakeLease(second, lease) == r::Status::Ok);
    const auto* root = f.Borrow(lease);
    REQUIRE(root->Count == 2);
    CHECK(root->Children[0]->Children[0] == root->Children[1]->Children[0]);
    CHECK(root->Children[0]->Count == 1);
    const void* child = nullptr;
    REQUIRE(f.Manager.BorrowDependency(lease.Token(), Id("left"), c::Kind::Sound, child) == r::Status::Ok);
    CHECK(child == root->Children[0]);
    REQUIRE(f.Manager.BorrowDependency(lease.Token(), Id("leaf"), c::Kind::Sound, child) == r::Status::NotFound);
    CHECK(child == root->Children[0]);
    f.Initial.Drain[0] = false;
    REQUIRE(lease.Release() == r::Status::Ok);
    f.Pump();
    CHECK(f.Manager.Inspect().ResidentBytes == 80);
    CHECK(f.Initial.Retires[3] == 0);
    f.Initial.Drain[0] = true;
    f.Initial.Drain[1] = false;
    f.Pump();
    CHECK(f.Initial.Retires[0] == 1);
    CHECK(f.Initial.Retires[2] == 1);
    CHECK(f.Initial.Retires[3] == 0);
    CHECK(f.Manager.Inspect().ResidentBytes == 40);
    f.Initial.Drain[1] = true;
    f.Pump();
    CHECK(f.Initial.Retires[3] == 1);
    CHECK(f.Manager.Inspect().Versions == 0);
}
TEST_CASE("Whole required closure admission fails without callbacks or partial reservations", "[resources][dag]")
{
    r::Config config;
    const auto limit = GENERATE(0, 1, 2);
    if (limit == 0)
    {
        config.Operations = 3;
    }
    if (limit == 1)
    {
        config.Versions = 3;
    }
    if (limit == 2)
    {
        config.ResidentBytes = 79;
    }
    Fixture f(config);
    r::RequestHandle request;
    CHECK(f.Manager.BeginAcquire(f.Scope, f.Root, request) ==
          (limit == 2 ? r::Status::BudgetExceeded : r::Status::QueueFull));
    CHECK(request.Owner == 0);
    f.Pump();
    CHECK(f.Initial.Begins[3] == 0);
    CHECK(f.Manager.Inspect().Versions == 0);
    CHECK(f.Manager.Inspect().ResidentBytes == 0);
}
TEST_CASE("Missing required identities and cycles report named authoring provenance", "[resources][dag]")
{
    Fixture f;
    r::Manager manager;
    const auto descriptor = f.Initial.Descriptor();
    r::FailureInfo failure;
    f.Edge(4, "leaf", "root");
    CHECK(manager.Initialize(f.Catalog, &descriptor, 1, {}, f.Graph(), &failure) == r::Status::Cycle);
    CHECK(failure.Cause == r::Status::Cycle);
    CHECK(failure.Asset.View() == "leaf");
    CHECK(failure.Count == 4);
    CHECK(failure.Chain[0].View() == failure.Chain[3].View());
    CHECK(failure.Chain[1].View() == "root");
    f.Edge(4, "root", "absent");
    CHECK(manager.Initialize(f.Catalog, &descriptor, 1, {}, f.Graph(), &failure) == r::Status::NotFound);
    CHECK(failure.Asset.View() == "absent");
    CHECK(failure.Property.View() == "absent");
    CHECK(failure.Chain[0].View() == "root");
    CHECK(failure.Chain[1].View() == "absent");
}
TEST_CASE("Dependency failure survives physical cleanup and permits a clean retry", "[resources][dag]")
{
    Fixture f;
    f.Initial.Result[3] = r::Status::OutOfMemory;
    const auto failed = f.Acquire(f.Scope);
    f.Pump();
    r::RequestInfo info;
    REQUIRE(f.Manager.GetRequest(failed, info) == r::Status::Ok);
    CHECK(info.State == r::RequestState::Failed);
    CHECK(info.Result == r::Status::DependencyFailed);
    CHECK(info.Failure.Cause == r::Status::OutOfMemory);
    CHECK(info.Failure.Asset.View() == "leaf");
    REQUIRE(info.Failure.Count == 3);
    CHECK(info.Failure.Chain[0].View() == "root");
    CHECK(info.Failure.Chain[2].View() == "leaf");
    CHECK(info.Failure.Property.View() == "left");
    CHECK(f.Initial.Begins[0] == 0);
    CHECK(f.Manager.Inspect().Versions == 0);
    CHECK(f.Manager.Inspect().ResidentBytes == 0);
    f.Initial.Result[3] = r::Status::Ok;
    const auto retry = f.Acquire(f.Scope);
    f.Pump();
    r::Lease lease;
    REQUIRE(f.Manager.TakeLease(retry, lease) == r::Status::Ok);
    REQUIRE(f.Manager.GetRequest(failed, info) == r::Status::Ok);
    CHECK(info.Failure.Cause == r::Status::OutOfMemory);
}
TEST_CASE("Replacement publishes whole exact DAG while old scopes keep their version", "[resources][replacement]")
{
    // Provider contexts must outlive the fixture's manager shutdown.
    GraphBackend replacement;
    replacement.Revision = 2;
    Fixture f;
    r::Lease oldLease;
    auto request = f.Acquire(f.Scope);
    f.Pump();
    REQUIRE(f.Manager.TakeLease(request, oldLease) == r::Status::Ok);
    const auto* original = f.Borrow(oldLease);
    auto transaction = f.Replace(replacement, 2);
    CHECK(f.Manager.Commit(transaction) == r::Status::Pending);
    f.Pump();
    r::TransactionInfo info;
    REQUIRE(f.Manager.GetTransaction(transaction, info) == r::Status::Ok);
    CHECK(info.State == r::TransactionState::Ready);
    CHECK(f.Manager.Inspect().CurrentRevision == 1);
    r::ScopeHandle preview;
    REQUIRE(f.Manager.OpenTransactionScope(transaction, Id("preview"), preview) == r::Status::Ok);
    request = f.Acquire(preview);
    r::Lease previewLease;
    REQUIRE(f.Manager.TakeLease(request, previewLease) == r::Status::Ok);
    const auto* prepared = f.Borrow(previewLease);
    CHECK(prepared->Value == 20);
    CHECK(prepared->Children[0]->Children[0]->Value == 23);
    REQUIRE(f.Manager.Commit(transaction) == r::Status::Ok);
    CHECK(f.Manager.Inspect().CurrentRevision == 2);
    CHECK(f.Borrow(oldLease) == original);
    request = f.Acquire(f.Scope);
    r::Lease oldAgain;
    REQUIRE(f.Manager.TakeLease(request, oldAgain) == r::Status::Ok);
    CHECK(f.Borrow(oldAgain) == original);
    r::ScopeHandle current;
    REQUIRE(f.Manager.OpenScope(Id("current"), current) == r::Status::Ok);
    request = f.Acquire(current);
    r::Lease currentLease;
    REQUIRE(f.Manager.TakeLease(request, currentLease) == r::Status::Ok);
    CHECK(f.Borrow(currentLease) == prepared);
    REQUIRE(f.Manager.EndTransaction(transaction) == r::Status::Ok);
    REQUIRE(previewLease.Release() == r::Status::Ok);
    REQUIRE(currentLease.Release() == r::Status::Ok);
    REQUIRE(oldAgain.Release() == r::Status::Ok);
    REQUIRE(oldLease.Release() == r::Status::Ok);
    f.Pump();
    CHECK(f.Manager.Inspect().Versions == 4); // Current roots remain pinned without a lease.
}
TEST_CASE("Failed and superseded replacements retain last good and durable outcomes", "[resources][replacement]")
{
    GraphBackend good, failed, later;
    good.Revision = 2;
    failed.Revision = 3;
    later.Revision = 4;
    Fixture f;
    const auto first = f.Replace(good, 2);
    f.Pump();
    REQUIRE(f.Manager.Commit(first) == r::Status::Ok);
    REQUIRE(f.Manager.EndTransaction(first) == r::Status::Ok);
    failed.Result[3] = r::Status::Failed;
    const auto broken = f.Replace(failed, 3);
    f.Pump();
    r::TransactionInfo info;
    REQUIRE(f.Manager.GetTransaction(broken, info) == r::Status::Ok);
    CHECK(info.State == r::TransactionState::Failed);
    CHECK(info.Failure.Asset.View() == "leaf");
    CHECK(f.Manager.Commit(broken) == r::Status::DependencyFailed);
    CHECK(f.Manager.Inspect().CurrentRevision == 2);
    later.Complete[3] = false;
    const auto pending = f.Replace(later, 4);
    f.Pump();
    auto invalidGraph = f.Graph(5);
    invalidGraph.Count = 129;
    const auto descriptor = later.Descriptor();
    r::TransactionHandle rejected;
    CHECK(f.Manager.BeginReplacement(f.Catalog, &descriptor, 1, invalidGraph, &f.Root, 1, rejected) ==
          r::Status::LimitExceeded);
    CHECK(f.Manager.Commit(pending) == r::Status::Conflict);
    CHECK(f.Manager.Inspect().CurrentRevision == 2);
    CHECK(f.Manager.Inspect().DesiredRevision == 5);
    later.Complete[3] = true;
    f.Pump();
    REQUIRE(f.Manager.EndTransaction(pending) == r::Status::Ok);
    REQUIRE(f.Manager.EndTransaction(broken) == r::Status::Ok);
    const auto retry = f.Replace(later, 5);
    f.Pump();
    REQUIRE(f.Manager.Commit(retry) == r::Status::Ok);
    CHECK(f.Manager.Inspect().CurrentRevision == 5);
}
TEST_CASE("Private candidate leases survive cancellation and shutdown requires physical proof",
          "[resources][replacement]")
{
    GraphBackend candidate;
    candidate.Revision = 2;
    Fixture f;
    const auto transaction = f.Replace(candidate, 2);
    f.Pump();
    r::ScopeHandle preview;
    REQUIRE(f.Manager.OpenTransactionScope(transaction, Id("private"), preview) == r::Status::Ok);
    const auto request = f.Acquire(preview);
    r::Lease lease;
    REQUIRE(f.Manager.TakeLease(request, lease) == r::Status::Ok);
    REQUIRE(f.Manager.CancelTransaction(transaction) == r::Status::Ok);
    CHECK(f.Borrow(lease)->Value == 20);
    CHECK(f.Manager.CloseScope(preview) == r::Status::Pending);
    candidate.Drain[0] = false;
    REQUIRE(lease.Release() == r::Status::Ok);
    REQUIRE(f.Manager.CloseScope(preview) == r::Status::Ok);
    f.Pump();
    CHECK(f.Manager.Inspect().ResidentBytes == 80);
    CHECK(f.Manager.Shutdown() == r::Status::Pending);
    candidate.Drain[0] = true;
    f.Pump();
    CHECK(f.Manager.Shutdown() == r::Status::Ok);
    CHECK(f.Manager.Inspect().ResidentBytes == 0);
}
} // namespace dag_cases

namespace dag_cases
{
TEST_CASE("Activated bindings cannot finance replacement with unretired last-good bytes", "[resources][replacement]")
{
    GraphBackend good, next;
    r::Config config;
    config.ResidentBytes = 100;
    Fixture f(config);
    const auto transaction = f.Replace(good, 2);
    f.Pump();
    REQUIRE(f.Manager.Commit(transaction) == r::Status::Ok);
    REQUIRE(f.Manager.EndTransaction(transaction) == r::Status::Ok);
    r::TransactionHandle output;
    const auto descriptor = next.Descriptor();
    CHECK(f.Manager.BeginReplacement(f.Catalog, &descriptor, 1, f.Graph(3), &f.Root, 1, output) ==
          r::Status::BudgetExceeded);
    CHECK(output.Owner == 0);
    CHECK(f.Manager.Inspect().CurrentRevision == 2);
    CHECK(f.Manager.Inspect().ResidentBytes == 80);
    CHECK(next.Begins[3] == 0);
    r::ScopeHandle current;
    REQUIRE(f.Manager.OpenScope(Id("last-good"), current) == r::Status::Ok);
    const auto request = f.Acquire(current);
    r::Lease lease;
    REQUIRE(f.Manager.TakeLease(request, lease) == r::Status::Ok);
    CHECK(f.Borrow(lease)->Children[0]->Children[0]->Value == 13);
}
TEST_CASE("Multi-root activation waits for every root and deduplicates root identities", "[resources][replacement]")
{
    GraphBackend candidate;
    Fixture f;
    r::AssetHandle right;
    REQUIRE(f.Manager.Resolve(Id("right"), c::Kind::Sound, right) == r::Status::Ok);
    r::AssetHandle roots[]{f.Root, right, f.Root};
    r::TransactionHandle transaction;
    const auto descriptor = candidate.Descriptor();
    candidate.Complete[0] = false;
    REQUIRE(f.Manager.BeginReplacement(f.Catalog, &descriptor, 1, f.Graph(2), roots, 3, transaction) == r::Status::Ok);
    f.Pump();
    CHECK(f.Manager.Inspect().Versions == 4);
    CHECK(f.Manager.Commit(transaction) == r::Status::Pending);
    CHECK(f.Manager.Inspect().CurrentRevision == 1);
    candidate.Complete[0] = true;
    f.Pump();
    REQUIRE(f.Manager.Commit(transaction) == r::Status::Ok);
    CHECK(f.Manager.Inspect().CurrentRevision == 2);
    CHECK(candidate.Begins[2] == 1);
    r::TransactionInfo preserved;
    preserved.Revision = 99;
    const auto stale = transaction;
    REQUIRE(f.Manager.EndTransaction(transaction) == r::Status::Ok);
    CHECK(f.Manager.GetTransaction(stale, preserved) == r::Status::InvalidHandle);
    CHECK(preserved.Revision == 99);
}
TEST_CASE("Transaction table saturation never blocks cancellation or physical cleanup", "[resources][replacement]")
{
    GraphBackend candidate;
    r::Config config;
    config.Transactions = 1;
    Fixture f(config);
    candidate.Complete[3] = false;
    const auto first = f.Replace(candidate, 2);
    f.Pump();
    r::TransactionHandle output;
    const auto descriptor = candidate.Descriptor();
    CHECK(f.Manager.BeginReplacement(f.Catalog, &descriptor, 1, f.Graph(3), &f.Root, 1, output) ==
          r::Status::QueueFull);
    CHECK(f.Manager.Commit(first) == r::Status::Conflict);
    CHECK(f.Manager.CancelTransaction(first) == r::Status::Ok);
    REQUIRE(f.Manager.EndTransaction(first) == r::Status::Ok);
    candidate.Complete[3] = true;
    f.Pump();
    CHECK(f.Manager.Inspect().Versions == 0);
    const auto retry = f.Replace(candidate, 3);
    f.Pump();
    REQUIRE(f.Manager.Commit(retry) == r::Status::Ok);
}
TEST_CASE("Required parent owns a real immutable mesh until parent retirement acknowledgment", "[resources][dag][mesh]")
{
    const math::Vector3 positions[]{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    const uint32 indices[]{0, 1, 2};
    r::MeshSource source;
    source.Resource.Type = c::Kind::Mesh;
    source.Resource.Id = Id("mesh");
    REQUIRE(source.Resource.Path.Set("mesh.bin"));
    source.Geometry = {positions, 3, indices, 3};
    r::MeshDefinitionAdapter mesh;
    REQUIRE(mesh.Initialize(&source, 1) == r::Status::Ok);
    GraphBackend parent;
    c::Catalog catalog;
    REQUIRE(catalog.Put(source.Resource) == c::Status::Ok);
    c::Resource root;
    root.Type = c::Kind::Sound; // Test-only parent payload contract, no audio installation.
    root.Id = Id("root");
    REQUIRE(root.Path.Set("root.bin"));
    REQUIRE(catalog.Put(root) == c::Status::Ok);
    const r::Adapter adapters[]{parent.Descriptor(), mesh.Descriptor()};
    r::DependencyEdge edge;
    edge.Parent = root.Id;
    edge.Child = source.Resource.Id;
    r::Manager manager;
    REQUIRE(manager.Initialize(catalog, adapters, 2, {}, {&edge, 1, 1}) == r::Status::Ok);
    struct Cleanup final
    {
        r::Manager& Manager;
        GraphBackend& Parent;
        ~Cleanup()
        {
            Parent.Drain[0] = true;
            while (Manager.Shutdown() != r::Status::Ok)
            {
                Manager.Service(64);
            }
        }
    } cleanup{manager, parent};
    r::ScopeHandle scope;
    r::AssetHandle asset;
    REQUIRE(manager.OpenScope(Id("real-mesh"), scope) == r::Status::Ok);
    REQUIRE(manager.Resolve(root.Id, c::Kind::Sound, asset) == r::Status::Ok);
    r::RequestHandle request;
    REQUIRE(manager.BeginAcquire(scope, asset, request) == r::Status::Ok);
    for (uint32 turn = 0; turn < 8; ++turn)
    {
        manager.Service(64);
    }
    r::Lease lease;
    REQUIRE(manager.TakeLease(request, lease) == r::Status::Ok);
    const void* payload = nullptr;
    REQUIRE(manager.BorrowDependency(lease.Token(), source.Resource.Id, c::Kind::Mesh, payload) == r::Status::Ok);
    const auto* definition = static_cast<const r::MeshDefinition*>(payload);
    CHECK(definition->VertexCount == 3);
    CHECK(definition->Positions[1].X == 1);
    parent.Drain[0] = false;
    REQUIRE(lease.Release() == r::Status::Ok);
    for (uint32 turn = 0; turn < 8; ++turn)
    {
        manager.Service(64);
    }
    CHECK(manager.Inspect().Versions == 2);
    CHECK(definition->Positions[1].X == 1); // Parent's submitted use still owns its exact child.
    parent.Drain[0] = true;
    for (uint32 turn = 0; turn < 8; ++turn)
    {
        manager.Service(64);
    }
    CHECK(manager.Inspect().Versions == 0);
    CHECK(manager.Inspect().ResidentBytes == 0);
}
} // namespace dag_cases

namespace dag_cases
{
TEST_CASE("Partial parent failure keeps children and bytes through delayed retirement", "[resources][dag]")
{
    Fixture f;
    f.Initial.FailOwned = true;
    f.Initial.Drain[0] = false;
    const auto request = f.Acquire(f.Scope);
    f.Pump();
    r::RequestInfo info;
    REQUIRE(f.Manager.GetRequest(request, info) == r::Status::Ok);
    CHECK(info.Result == r::Status::OutOfMemory);
    CHECK(f.Manager.Inspect().Versions == 4);
    CHECK(f.Manager.Inspect().ResidentBytes == 80);
    CHECK(f.Initial.Retires[3] == 0);
    f.Initial.Drain[0] = true;
    f.Pump();
    CHECK(f.Manager.Inspect().ResidentBytes == 0);
    CHECK(f.Manager.Inspect().PreparationBytes == 0);
    CHECK(f.Initial.Retires[3] == 1);
}
TEST_CASE("An activated snapshot refuses implicit rebinding after an epoch change", "[resources][replacement]")
{
    GraphBackend candidate;
    Fixture f;
    const auto transaction = f.Replace(candidate, 2);
    f.Pump();
    REQUIRE(f.Manager.Commit(transaction) == r::Status::Ok);
    r::ScopeHandle current;
    REQUIRE(f.Manager.OpenScope(Id("epoch"), current) == r::Status::Ok);
    const auto request = f.Acquire(current);
    r::Lease lease;
    REQUIRE(f.Manager.TakeLease(request, lease) == r::Status::Ok);
    candidate.Epoch = 2;
    const void* payload = &candidate;
    CHECK(f.Manager.Borrow(lease.Token(), c::Kind::Sound, payload) == r::Status::DeviceLost);
    CHECK(payload == &candidate);
    r::RequestHandle output;
    CHECK(f.Manager.BeginAcquire(current, f.Root, output) == r::Status::DeviceLost);
    CHECK(output.Owner == 0);
    CHECK(f.Manager.Inspect().Versions == 4);
}
} // namespace dag_cases

namespace dag_cases
{
TEST_CASE("Commit preflight reports a no-longer-usable descendant without publishing", "[resources][replacement]")
{
    GraphBackend candidate;
    Fixture f;
    const auto transaction = f.Replace(candidate, 2);
    f.Pump();
    candidate.Invalid[3] = true;
    CHECK(f.Manager.Commit(transaction) == r::Status::Failed);
    CHECK(f.Manager.Inspect().CurrentRevision == 1);
    r::TransactionInfo info;
    REQUIRE(f.Manager.GetTransaction(transaction, info) == r::Status::Ok);
    CHECK(info.State == r::TransactionState::Failed);
    CHECK(info.Failure.Asset.View() == "leaf");
    CHECK(info.Failure.Cause == r::Status::Failed);
    REQUIRE(info.Failure.Count == 3);
    CHECK(info.Failure.Chain[0].View() == "root");
    CHECK(info.Failure.Chain[2].View() == "leaf");
    CHECK(info.Failure.Property.View() == "left");
}
} // namespace dag_cases

namespace dag_cases
{
TEST_CASE("Parent publication preserves descendant provenance if readiness changes during preparation",
          "[resources][dag]")
{
    Fixture f;
    f.Initial.Complete[0] = false;
    const auto request = f.Acquire(f.Scope);
    f.Pump();
    REQUIRE(f.Initial.Begins[0] == 1);
    f.Initial.Invalid[3] = true;
    f.Initial.Complete[0] = true;
    f.Pump();
    r::RequestInfo info;
    REQUIRE(f.Manager.GetRequest(request, info) == r::Status::Ok);
    CHECK(info.State == r::RequestState::Failed);
    CHECK(info.Failure.Asset.View() == "leaf");
    CHECK(info.Failure.Cause == r::Status::Failed);
    REQUIRE(info.Failure.Count == 3);
    CHECK(info.Failure.Chain[0].View() == "root");
    CHECK(info.Failure.Chain[2].View() == "leaf");
    CHECK(f.Manager.Inspect().Versions == 0);
}
} // namespace dag_cases

namespace dag_cases
{
TEST_CASE("Adapter slot capacity admits no partial closure and counts retained snapshots",
          "[resources][dag][replacement]")
{
    const auto capacity = GENERATE(3U, 4U);
    Fixture f({}, capacity);
    auto descriptor = f.Initial.Descriptor();
    descriptor.MaxCandidates = capacity;
    r::TransactionHandle transaction;
    const auto admitted = f.Manager.BeginReplacement(f.Catalog, &descriptor, 1, f.Graph(2), &f.Root, 1, transaction);
    if (capacity == 3)
    {
        CHECK(admitted == r::Status::QueueFull);
        f.Pump();
        CHECK(f.Initial.Begins[3] == 0);
        CHECK(f.Manager.Inspect().Versions == 0);
    }
    else
    {
        REQUIRE(admitted == r::Status::Ok);
        f.Pump();
        REQUIRE(f.Manager.Commit(transaction) == r::Status::Ok);
        REQUIRE(f.Manager.EndTransaction(transaction) == r::Status::Ok);
        r::TransactionHandle rejected;
        CHECK(f.Manager.BeginReplacement(f.Catalog, &descriptor, 1, f.Graph(3), &f.Root, 1, rejected) ==
              r::Status::QueueFull);
        CHECK(f.Manager.Inspect().CurrentRevision == 2);
        CHECK(f.Manager.Inspect().Versions == 4);
        CHECK(f.Initial.Begins[3] == 1);
    }
}
TEST_CASE("Move assignment empties the source even when it outlives its manager", "[resources][ownership]")
{
    r::Lease source;
    {
        Fixture f;
        const auto request = f.Acquire(f.Scope);
        f.Pump();
        REQUIRE(f.Manager.TakeLease(request, source) == r::Status::Ok);
        r::Lease target;
        target = Move(source);
        CHECK(source.Token().Owner == 0);
        CHECK(source.Release() == r::Status::Ok);
        CHECK(f.Manager.Inspect().Leases == 1);
        CHECK(f.Borrow(target)->Value == 10);
        REQUIRE(target.Release() == r::Status::Ok);
        // Populate it again so scope exit tests the destructor without calling
        // Release on the moved-from wrapper first.
        const auto again = f.Acquire(f.Scope);
        f.Pump();
        REQUIRE(f.Manager.TakeLease(again, source) == r::Status::Ok);
        target = Move(source);
        REQUIRE(target.Release() == r::Status::Ok);
    }
    CHECK(source.Release() == r::Status::Ok);
}
} // namespace dag_cases

namespace dag_cases
{
TEST_CASE("Reused snapshots reset dependency and publication bookkeeping in place", "[resources][replacement]")
{
    GraphBackend candidate;
    r::Config config;
    config.CatalogSnapshots = 2;
    Fixture f(config);
    REQUIRE(f.Manager.CloseScope(f.Scope) == r::Status::Ok);
    const auto first = f.Replace(candidate, 2);
    f.Pump();
    REQUIRE(f.Manager.Commit(first) == r::Status::Ok);
    REQUIRE(f.Manager.EndTransaction(first) == r::Status::Ok);
    f.Pump();
    const auto descriptor = candidate.Descriptor();
    r::TransactionHandle leaf;
    REQUIRE(f.Manager.BeginReplacement(f.Catalog, &descriptor, 1, {nullptr, 0, 3}, &f.Root, 1, leaf) == r::Status::Ok);
    f.Pump();
    REQUIRE(f.Manager.Commit(leaf) == r::Status::Ok);
    REQUIRE(f.Manager.EndTransaction(leaf) == r::Status::Ok);
    f.Pump();
    CHECK(f.Manager.Inspect().Versions == 1);
    CHECK(f.Manager.Inspect().ResidentBytes == 20);
    r::ScopeHandle current;
    REQUIRE(f.Manager.OpenScope(Id("reused"), current) == r::Status::Ok);
    const auto request = f.Acquire(current);
    r::Lease lease;
    REQUIRE(f.Manager.TakeLease(request, lease) == r::Status::Ok);
    CHECK(f.Borrow(lease)->Count == 0);
    REQUIRE(lease.Release() == r::Status::Ok);
    REQUIRE(f.Manager.CloseScope(current) == r::Status::Ok);
    const auto diamond = f.Replace(candidate, 4);
    f.Pump();
    REQUIRE(f.Manager.Commit(diamond) == r::Status::Ok);
    REQUIRE(f.Manager.EndTransaction(diamond) == r::Status::Ok);
    f.Pump();
    CHECK(f.Manager.Inspect().Versions == 4);
    CHECK(f.Manager.Inspect().ResidentBytes == 80);
}
} // namespace dag_cases
