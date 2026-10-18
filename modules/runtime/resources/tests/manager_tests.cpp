#include <ludus/content/content.h>
#include <ludus/runtime/resources/manager.h>
#include <ludus/runtime/resources/mesh.h>

#include "internal/counters.hpp"
#include "internal/mesh_test_hooks.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <limits>

using namespace ludus::foundation;
namespace r = ludus::resources;
namespace c = ludus::content;
namespace
{
c::ResourceId Id(const char* name)
{
    c::ResourceId result;
    REQUIRE(result.Set(name));
    return result;
}
struct Backend final
{
    struct Job final
    {
        Backend* Owner = nullptr;
        bool Used = false;
        uint32 Payload = 17;
    } Jobs[16];
    uint64 Epoch = 1;
    uint32 Begins = 0, Polls = 0, Cancels = 0, Retires = 0;
    bool Complete = false, Drain = false, Busy = false, FailBeginOwned = false;
    r::Status Result = r::Status::Ok;
    static r::Status PollJob(void* p, void* job, const void*& output) noexcept
    {
        if (static_cast<Job*>(job)->Owner != static_cast<Backend*>(p))
        {
            return r::Status::InvalidHandle;
        }
        auto& b = *static_cast<Backend*>(p);
        ++b.Polls;
        if (b.Complete == false)
        {
            return r::Status::Pending;
        }
        if (b.Result == r::Status::Ok)
        {
            output = &static_cast<Job*>(job)->Payload;
        }
        return b.Result;
    }
    static r::Status RetireJob(void* p, void* input) noexcept
    {
        if (static_cast<Job*>(input)->Owner != static_cast<Backend*>(p))
        {
            return r::Status::InvalidHandle;
        }
        auto& b = *static_cast<Backend*>(p);
        ++b.Retires;
        if (b.Complete == false || b.Drain == false)
        {
            return r::Status::Pending;
        }
        static_cast<Job*>(input)->Used = false;
        return r::Status::Ok;
    }
    r::Adapter Descriptor()
    {
        r::Adapter a;
        a.Context = this;
        a.Peak = {10, 20};
        a.Epoch = [](void* p) noexcept { return static_cast<Backend*>(p)->Epoch; };
        a.Begin = [](void* p, const c::Resource&, void*& output) noexcept {
            auto& b = *static_cast<Backend*>(p);
            if (b.Busy)
            {
                return r::Status::QueueFull;
            }
            for (auto& job : b.Jobs)
            {
                if (job.Used == false)
                {
                    ++b.Begins;
                    job.Used = true;
                    job.Owner = &b;
                    output = &job;
                    return b.FailBeginOwned ? r::Status::Failed : r::Status::Pending;
                }
            }
            return r::Status::QueueFull;
        };
        a.Poll = &PollJob;
        a.Cancel = [](void* p, void*) noexcept { ++static_cast<Backend*>(p)->Cancels; };
        a.Retire = &RetireJob;
        a.Validate = [](void*, const void*) noexcept { return r::Status::Ok; };
        return a;
    }
};
struct Fixture final
{
    Backend BackendOwner;
    c::Catalog Catalog;
    r::Manager Manager;
    r::AssetHandle Asset, Other;
    r::ScopeHandle Scope;
    explicit Fixture(r::Config config = {})
    {
        c::Resource entry;
        entry.Id = Id("sound/one");
        REQUIRE(entry.Path.Set("one.json"));
        entry.Type = c::Kind::Sound;
        REQUIRE(Catalog.Put(entry) == c::Status::Ok);
        entry.Id = Id("sound/two");
        REQUIRE(Catalog.Put(entry) == c::Status::Ok);
        const auto adapter = BackendOwner.Descriptor();
        REQUIRE(Manager.Initialize(Catalog, &adapter, 1, config) == r::Status::Ok);
        REQUIRE(Manager.Resolve(Id("sound/one"), c::Kind::Sound, Asset) == r::Status::Ok);
        REQUIRE(Manager.Resolve(Id("sound/two"), c::Kind::Sound, Other) == r::Status::Ok);
        REQUIRE(Manager.OpenScope(Id("level/test"), Scope) == r::Status::Ok);
    }
    r::RequestHandle Begin(r::AssetHandle asset = {})
    {
        r::RequestHandle request;
        REQUIRE(Manager.BeginAcquire(Scope, asset.Owner == 0 ? Asset : asset, request) == r::Status::Ok);
        return request;
    }
    r::RequestInfo Observe(r::RequestHandle request)
    {
        r::RequestInfo info;
        REQUIRE(Manager.GetRequest(request, info) == r::Status::Ok);
        return info;
    }
    ~Fixture()
    {
        BackendOwner.Complete = true;
        BackendOwner.Drain = true;
        (void)Manager.Shutdown();
        for (uint32 i = 0; i < 16; ++i)
        {
            Manager.Service(64);
        }
    }
};
} // namespace
TEST_CASE("Exact typed lookup copies catalog metadata and never prepares", "[resources]")
{
    Fixture f;
    REQUIRE(f.BackendOwner.Begins == 0);
    r::AssetHandle missing;
    REQUIRE(f.Manager.Resolve(Id("sound/ONE"), c::Kind::Sound, missing) == r::Status::NotFound);
    REQUIRE(f.Manager.Resolve(Id("sound/one"), c::Kind::Music, missing) == r::Status::NotFound);
    REQUIRE(f.Manager.Resolve(Id("sound/one"), c::Kind::Sound, f.Asset) == r::Status::InvalidState);
    f.Catalog = {};
    const auto request = f.Begin();
    f.Manager.Service(64);
    REQUIRE(f.BackendOwner.Begins == 1);
    REQUIRE(f.Observe(request).State == r::RequestState::Working);
}
TEST_CASE("Two callers share preparation and cancel independently at every phase", "[resources]")
{
    Fixture f;
    const auto phase = GENERATE(0, 1, 2);
    r::ScopeHandle secondScope;
    REQUIRE(f.Manager.OpenScope(Id("preview/test"), secondScope) == r::Status::Ok);
    const auto first = f.Begin();
    r::RequestHandle second;
    REQUIRE(f.Manager.BeginAcquire(secondScope, f.Asset, second) == r::Status::Ok);
    REQUIRE(f.Manager.Inspect().PreparationBytes == 10);
    REQUIRE(f.Manager.Inspect().ResidentBytes == 20);
    if (phase != 0)
    {
        f.Manager.Service(64);
    }
    if (phase == 2)
    {
        f.BackendOwner.Complete = true;
        f.Manager.Service(64);
    }
    REQUIRE(f.Manager.Cancel(first) == r::Status::Ok);
    REQUIRE(f.Observe(first).State == r::RequestState::Cancelled);
    REQUIRE(f.BackendOwner.Cancels == 0);
    f.BackendOwner.Complete = true;
    f.Manager.Service(64);
    REQUIRE(f.BackendOwner.Begins == 1);
    REQUIRE(f.Observe(second).State == r::RequestState::Ready);
    r::Lease lease;
    REQUIRE(f.Manager.TakeLease(second, lease) == r::Status::Ok);
    REQUIRE(f.Manager.TakeLease(second, lease) == r::Status::InvalidState);
    REQUIRE(f.Manager.CloseScope(secondScope) == r::Status::Pending);
    r::ScopeInfo blockers;
    REQUIRE(f.Manager.GetScope(secondScope, blockers) == r::Status::Ok);
    REQUIRE(blockers.Name.View() == "preview/test");
    REQUIRE(blockers.Closing);
    REQUIRE(blockers.Leases == 1);
    REQUIRE(blockers.Requests == 0);
    const void* payload = nullptr;
    REQUIRE(f.Manager.Borrow(lease.Token(), c::Kind::Sound, payload) == r::Status::Ok);
    REQUIRE(*static_cast<const uint32*>(payload) == 17);
    REQUIRE(f.Manager.CloseScope(f.Scope) == r::Status::Ok);
    REQUIRE(lease.Release() == r::Status::Ok);
    REQUIRE(f.Manager.CloseScope(secondScope) == r::Status::Ok);
    REQUIRE(f.Manager.Inspect().ResidentBytes == 20);
    f.BackendOwner.Drain = true;
    f.Manager.Service(64);
    REQUIRE(f.Manager.Inspect().ResidentBytes == 0);
}
TEST_CASE("Final cancellation retains candidate and budget until late cleanup", "[resources]")
{
    Fixture f;
    const bool started = GENERATE(false, true);
    const auto request = f.Begin();
    if (started)
    {
        f.Manager.Service(64);
    }
    REQUIRE(f.Manager.Cancel(request) == r::Status::Ok);
    REQUIRE(f.Manager.Cancel(request) == r::Status::Ok);
    REQUIRE(f.BackendOwner.Cancels == (started ? 1U : 0U));
    f.Manager.Service(64);
    REQUIRE(f.Manager.Inspect().Versions == (started ? 1U : 0U));
    if (started)
    {
        REQUIRE(f.Manager.Inspect().PreparationBytes == 10);
        f.BackendOwner.Complete = true;
        f.Manager.Service(64);
        REQUIRE(f.Manager.Inspect().ResidentBytes == 20);
        f.BackendOwner.Drain = true;
        f.Manager.Service(64);
    }
    REQUIRE(f.Manager.Inspect().Versions == 0);
    REQUIRE(f.Observe(request).Result == r::Status::Cancelled);
    REQUIRE(f.Manager.EndRequest(request) == r::Status::Ok);
    REQUIRE(f.Manager.EndRequest(request) == r::Status::InvalidHandle);
}
TEST_CASE("Durable results and cleanup survive every fixed-table pressure", "[resources]")
{
    r::Config config;
    config.Requests = 2;
    config.Operations = 1;
    config.Versions = 1;
    config.Leases = 1;
    config.Scopes = 1;
    Fixture f(config);
    const auto a = f.Begin(), b = f.Begin();
    r::RequestHandle refused;
    REQUIRE(f.Manager.BeginAcquire(f.Scope, f.Asset, refused) == r::Status::QueueFull);
    REQUIRE(refused == r::RequestHandle{});
    r::ScopeHandle noScope;
    REQUIRE(f.Manager.OpenScope(Id("other"), noScope) == r::Status::QueueFull);
    f.BackendOwner.Complete = true;
    f.Manager.Service(64);
    for (uint32 i = 0; i < 30; ++i)
    {
        f.Manager.Service(64);
        REQUIRE(f.Observe(a).State == r::RequestState::Ready);
    }
    r::Lease one, two;
    REQUIRE(f.Manager.TakeLease(a, one) == r::Status::Ok);
    REQUIRE(f.Manager.TakeLease(b, two) == r::Status::QueueFull);
    REQUIRE(f.Observe(b).State == r::RequestState::Ready);
    REQUIRE(f.Manager.BeginAcquire(f.Scope, f.Other, refused) == r::Status::QueueFull);
    REQUIRE(f.Manager.Cancel(b) == r::Status::Ok);
    REQUIRE(f.Manager.CloseScope(f.Scope) == r::Status::Pending);
    REQUIRE(f.Manager.Shutdown() == r::Status::Pending);
    REQUIRE(one.Release() == r::Status::Ok);
    f.BackendOwner.Drain = true;
    f.Manager.Service(64);
    REQUIRE(f.Manager.Shutdown() == r::Status::Ok);
}
TEST_CASE("Peak reservations deduplicate and prevent overflow and early reuse", "[resources]")
{
    r::Config config;
    config.PreparationBytes = 10;
    config.ResidentBytes = 20;
    Fixture f(config);
    const auto a = f.Begin(), b = f.Begin();
    r::RequestHandle refused;
    REQUIRE(f.Manager.BeginAcquire(f.Scope, f.Other, refused) == r::Status::BudgetExceeded);
    REQUIRE(f.Manager.Inspect().Requests == 2);
    f.Manager.Service(64);
    REQUIRE(f.Manager.EndRequest(a) == r::Status::Ok);
    REQUIRE(f.Manager.EndRequest(b) == r::Status::Ok);
    f.BackendOwner.Complete = true;
    f.Manager.Service(64);
    REQUIRE(f.Manager.BeginAcquire(f.Scope, f.Other, refused) == r::Status::BudgetExceeded);
    f.BackendOwner.Drain = true;
    f.Manager.Service(64);
    REQUIRE(f.Manager.BeginAcquire(f.Scope, f.Other, refused) == r::Status::Ok);
    REQUIRE(f.Manager.Inspect().PreparationBytes == 10);
    const auto huge = 0xFFFFFFFFFFFFFFFFULL;
    r::Manager manager;
    auto adapter = f.BackendOwner.Descriptor();
    adapter.Peak = {huge, huge};
    REQUIRE(manager.Initialize(f.Catalog, &adapter, 1) == r::Status::Ok);
    r::ScopeHandle scope;
    r::AssetHandle asset;
    REQUIRE(manager.OpenScope(Id("scope"), scope) == r::Status::Ok);
    REQUIRE(manager.Resolve(Id("sound/one"), c::Kind::Sound, asset) == r::Status::Ok);
    refused = {};
    REQUIRE(manager.BeginAcquire(scope, asset, refused) == r::Status::BudgetExceeded);
}
TEST_CASE("Failed and partially accepted work stays durable and retireable", "[resources]")
{
    Fixture f;
    const bool failedBegin = GENERATE(false, true);
    f.BackendOwner.FailBeginOwned = failedBegin;
    f.BackendOwner.Result = r::Status::Failed;
    f.BackendOwner.Complete = true;
    const auto request = f.Begin();
    f.Manager.Service(64);
    REQUIRE(f.Observe(request).Result == r::Status::Failed);
    REQUIRE(f.Manager.Inspect().PreparationBytes == 10);
    r::Lease empty;
    REQUIRE(f.Manager.TakeLease(request, empty) == r::Status::Failed);
    f.BackendOwner.Drain = true;
    f.Manager.Service(64);
    REQUIRE(f.Manager.Inspect().ResidentBytes == 0);
    REQUIRE(f.Observe(request).Result == r::Status::Failed);
    REQUIRE(f.Manager.EndRequest(request) == r::Status::Ok);
    f.BackendOwner.FailBeginOwned = false;
    f.BackendOwner.Result = r::Status::Ok;
    const auto retry = f.Begin();
    f.Manager.Service(64);
    REQUIRE(f.Observe(retry).State == r::RequestState::Ready);
}
TEST_CASE("Adapter backpressure and bounded service retain accepted requests", "[resources]")
{
    Fixture f;
    f.BackendOwner.Busy = true;
    const auto request = f.Begin();
    f.Manager.Service(0);
    REQUIRE(f.BackendOwner.Begins == 0);
    f.Manager.Service(1);
    REQUIRE(f.Observe(request).State == r::RequestState::Queued);
    f.BackendOwner.Busy = false;
    f.BackendOwner.Complete = true;
    f.Manager.Service(64);
    REQUIRE(f.Observe(request).State == r::RequestState::Ready);
    const auto hit = f.Begin();
    REQUIRE(f.Observe(hit).State == r::RequestState::Ready);
    REQUIRE(f.BackendOwner.Begins == 1);
    REQUIRE(f.Manager.Inspect().ResidentBytes == 20);
}
TEST_CASE("Owner and generation validation rejects stale forged and consumed tokens", "[resources]")
{
    Fixture f, other;
    auto request = f.Begin();
    r::RequestInfo unchanged;
    unchanged.Result = r::Status::Failed;
    REQUIRE(other.Manager.GetRequest(request, unchanged) == r::Status::InvalidHandle);
    REQUIRE(unchanged.Result == r::Status::Failed);
    r::RequestHandle output;
    REQUIRE(f.Manager.BeginAcquire(other.Scope, f.Asset, output) == r::Status::InvalidHandle);
    REQUIRE(f.Manager.BeginAcquire(f.Scope, other.Asset, output) == r::Status::InvalidHandle);
    REQUIRE(f.Manager.EndRequest(request) == r::Status::Ok);
    const auto replacement = f.Begin();
    REQUIRE(replacement.Slot == request.Slot);
    REQUIRE(replacement.Generation != request.Generation);
    REQUIRE(f.Manager.Cancel(request) == r::Status::InvalidHandle);
    f.BackendOwner.Complete = true;
    f.BackendOwner.Drain = true;
    f.Manager.Service(64);
    r::Lease lease;
    REQUIRE(f.Manager.TakeLease(replacement, lease) == r::Status::Ok);
    const auto token = lease.Token();
    const void* payload = &f;
    REQUIRE(other.Manager.Borrow(token, c::Kind::Sound, payload) == r::Status::InvalidHandle);
    REQUIRE(f.Manager.Borrow(token, c::Kind::Music, payload) == r::Status::InvalidHandle);
    REQUIRE(payload == &f);
    auto forged = token;
    forged.Slot = 1000;
    REQUIRE(f.Manager.Release(forged) == r::Status::InvalidHandle);
    r::Lease moved(Move(lease));
    REQUIRE(lease.Token() == r::LeaseToken{});
    REQUIRE(moved.Release() == r::Status::Ok);
    REQUIRE(f.Manager.Release(token) == r::Status::InvalidHandle);
    f.Manager.Service(64);
    const auto next = f.Begin();
    f.Manager.Service(64);
    REQUIRE(f.Manager.TakeLease(next, lease) == r::Status::Ok);
    REQUIRE(lease.Token().Slot == token.Slot);
    REQUIRE(f.Manager.Release(token) == r::Status::InvalidHandle);
}
TEST_CASE("Epoch changes reject obsolete completion and preserve cleanup ownership", "[resources]")
{
    Fixture f;
    const bool published = GENERATE(false, true);
    const auto request = f.Begin();
    f.BackendOwner.Complete = published;
    f.Manager.Service(64);
    r::Lease lease;
    if (published)
    {
        REQUIRE(f.Manager.TakeLease(request, lease) == r::Status::Ok);
    }
    ++f.BackendOwner.Epoch;
    f.Manager.Service(64);
    if (published)
    {
        const void* payload = &f;
        REQUIRE(f.Manager.Borrow(lease.Token(), c::Kind::Sound, payload) == r::Status::DeviceLost);
        REQUIRE(payload == &f);
        REQUIRE(lease.Release() == r::Status::Ok);
    }
    else
    {
        REQUIRE(f.Observe(request).Result == r::Status::DeviceLost);
        REQUIRE(f.BackendOwner.Cancels == 1);
    }
    REQUIRE(f.Manager.Inspect().ResidentBytes == 20);
    f.BackendOwner.Complete = true;
    f.BackendOwner.Drain = true;
    f.Manager.Service(64);
    REQUIRE(f.Manager.Inspect().ResidentBytes == 0);
}
TEST_CASE("Initialization validates limits and uses fresh nonwrapping identity", "[resources]")
{
    Fixture f;
    r::Manager manager;
    auto adapter = f.BackendOwner.Descriptor();
    r::Config config;
    config.MetadataBytes = 0;
    REQUIRE(manager.Initialize(f.Catalog, &adapter, 1, config) == r::Status::BudgetExceeded);
    config = {};
    config.Requests = 33;
    REQUIRE(manager.Initialize(f.Catalog, &adapter, 1, config) == r::Status::InvalidState);
    REQUIRE(manager.Initialize(f.Catalog, nullptr, 1) == r::Status::InvalidState);
    REQUIRE(manager.Initialize(f.Catalog, &adapter, 1) == r::Status::Ok);
    r::AssetHandle old;
    REQUIRE(manager.Resolve(Id("sound/one"), c::Kind::Sound, old) == r::Status::Ok);
    REQUIRE(manager.Initialize(f.Catalog, &adapter, 1) == r::Status::InvalidState);
    REQUIRE(manager.Shutdown() == r::Status::Ok);
    REQUIRE(manager.Initialize(f.Catalog, &adapter, 1) == r::Status::Ok);
    r::AssetHandle fresh;
    REQUIRE(manager.Resolve(Id("sound/one"), c::Kind::Sound, fresh) == r::Status::Ok);
    REQUIRE(fresh.Owner != old.Owner);
    uint32 generation = 0xFFFFFFFEU;
    REQUIRE(r::internal::Advance(generation));
    REQUIRE(generation == 0xFFFFFFFFU);
    REQUIRE_FALSE(r::internal::Advance(generation));
    REQUIRE(generation == 0xFFFFFFFFU);
}

#include "mesh_allocation_cases.hpp"
#include "mesh_cases.hpp"

#include "dag_cases.hpp"

#include "scene_cases.hpp"
