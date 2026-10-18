#include "resource_adapter.h"

#include <ludus/audio/audio_system.h>
#include <ludus/content/content.h>
#include <ludus/runtime/resources/manager.h>
#include <ludus/runtime/resources/mesh.h>

#include "internal/worker_test_hooks.hpp"
#include "wav_fixture.h"

#include <atomic>
#include <filesystem>
#include <thread>
#include <time.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace ludus::foundation;
namespace content = ludus::content;
namespace audio = ludus::audio;
namespace
{
void WaitTurn()
{
    const timespec delay{0, 1000000};
    nanosleep(&delay, nullptr);
}
bool Wait(const std::atomic<bool>& flag)
{
    for (uint32 i = 0; i < 2000; ++i)
    {
        if (flag.load(std::memory_order_acquire))
        {
            return true;
        }
        WaitTurn();
    }
    return false;
}
struct Gate final
{
    std::atomic<bool> Entered{false};
    std::atomic<bool> Work{false};
    std::atomic<bool> Completed{false};
    std::atomic<bool> Exit{false};
    std::atomic<bool> Exited{false};
    bool Started = false;
    Gate()
    {
        audio::content::internal::SetWorkerTestHooks({
            this,
            [](void* context) noexcept {
                auto& gate = *static_cast<Gate*>(context);
                gate.Entered.store(true, std::memory_order_release);
                gate.Work.wait(false, std::memory_order_acquire);
            },
            [](void* context) noexcept {
                auto& gate = *static_cast<Gate*>(context);
                gate.Completed.store(true, std::memory_order_release);
                gate.Exit.wait(false, std::memory_order_acquire);
                gate.Exited.store(true, std::memory_order_release);
            },
        });
    }
    void AllowWork()
    {
        Work.store(true, std::memory_order_release);
        Work.notify_all();
    }
    void AllowExit()
    {
        Exit.store(true, std::memory_order_release);
        Exit.notify_all();
    }
    ~Gate()
    {
        AllowWork();
        AllowExit();
        if (Started)
        {
            while (Exited.load(std::memory_order_acquire) == false)
            {
                WaitTurn();
            }
        }
        audio::content::internal::SetWorkerTestHooks({});
    }
};
struct Fixture final
{
    char Root[64] = "/tmp/ludus-resource-stage1-XXXXXX";
    content::Catalog Catalog;
    Fixture()
    {
        REQUIRE(mkdtemp(Root) != nullptr);
        auto& catalog = Catalog;
        content::Resource source, definition;
        REQUIRE(source.Id.Set("source/hit"));
        REQUIRE(source.Path.Set("hit.wav"));
        REQUIRE(catalog.Put(source) == content::Status::Ok);
        REQUIRE(definition.Id.Set("sound/hit"));
        REQUIRE(definition.Path.Set("hit.json"));
        definition.Type = content::Kind::Sound;
        REQUIRE(catalog.Put(definition) == content::Status::Ok);
        audio::content::Sound sound;
        sound.Id = definition.Id;
        REQUIRE(sound.Bus.Set("master"));
        REQUIRE(sound.Group.Set("default"));
        sound.Variations[0] = source.Id;
        sound.VariationCount = 1;
        content::Bytes bytes;
        REQUIRE(catalog.Write(bytes) == content::Status::Ok);
        REQUIRE(content::SaveFile(Root, "catalog.json", bytes.Data(), nullptr) == content::Status::Ok);
        REQUIRE(audio::content::WriteSound(sound, bytes) == content::Status::Ok);
        REQUIRE(content::SaveFile(Root, "hit.json", bytes.Data(), nullptr) == content::Status::Ok);
        const auto wav = audio::test::MakeSineWav(48000, 1, 4800);
        REQUIRE(content::SaveFile(Root, "hit.wav", wav, nullptr) == content::Status::Ok);
    }
    // Best-effort temporary-directory cleanup: only std::filesystem::path allocation
    // can throw, and bad_alloc terminating the test process is acceptable here.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~Fixture()
    {
        std::error_code error;
        std::filesystem::remove_all(Root, error);
    }
};
constexpr std::string_view BUSES[] = {"master"};
constexpr std::string_view GROUPS[] = {"default"};
const audio::content::Routing ROUTING{BUSES, GROUPS};
} // namespace
namespace r = ludus::resources;
namespace
{
content::ResourceId Key(const char* name)
{
    content::ResourceId id;
    REQUIRE(id.Set(name));
    return id;
}
struct Drain final
{
    r::Manager& Manager;
    audio::AudioSystem& Audio;
    Gate& Worker;
    ~Drain()
    {
        Worker.AllowWork();
        Worker.AllowExit();
        Audio.StopAll();
        (void)Manager.Shutdown();
        float32 output[1024];
        for (uint32 i = 0; i < 2000 && Manager.Shutdown() != r::Status::Ok; ++i)
        {
            (void)Audio.RenderOffline(output, audio::ChannelLayout::Stereo, audio::BufferLayout::Interleaved, 512);
            Manager.Service(64);
            WaitTurn();
        }
    }
};
bool Pump(r::Manager& manager, r::RequestHandle request)
{
    for (uint32 i = 0; i < 2000; ++i)
    {
        manager.Service(64);
        r::RequestInfo info;
        if (manager.GetRequest(request, info) != r::Status::Ok)
        {
            return false;
        }
        if (info.State == r::RequestState::Ready)
        {
            return true;
        }
        if (info.State == r::RequestState::Failed)
        {
            return false;
        }
        WaitTurn();
    }
    return false;
}
} // namespace
TEST_CASE("Two scoped callers acquire one real sound with independent cancellation", "[resources][audio][concurrency]")
{
    Fixture fixture;
    audio::AudioSystem system;
    audio::SystemConfig config;
    config.SystemMode = audio::Mode::Offline;
    REQUIRE(system.Initialize(config) == audio::Status::Ok);
    content::Text<4096> root;
    REQUIRE(root.Set(fixture.Root));
    Gate gate;
    sample::SoundResourceAdapter adapter(system, fixture.Catalog, root, ROUTING);
    const math::Vector3 positions[]{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    const uint32 indices[]{0, 1, 2};
    r::MeshSource meshSource;
    meshSource.Resource.Type = content::Kind::Mesh;
    meshSource.Resource.Id = Key("mesh/triangle");
    REQUIRE(meshSource.Resource.Path.Set("triangle.mesh"));
    meshSource.Geometry = {positions, 3, indices, 3};
    REQUIRE(fixture.Catalog.Put(meshSource.Resource) == content::Status::Ok);
    r::MeshDefinitionAdapter meshes;
    REQUIRE(meshes.Initialize(&meshSource, 1) == r::Status::Ok);
    const r::Adapter descriptors[]{adapter.Descriptor(), meshes.Descriptor()};
    r::Manager manager;
    REQUIRE(manager.Initialize(fixture.Catalog, descriptors, 2) == r::Status::Ok);
    Drain drain{manager, system, gate};
    r::ScopeHandle a, b;
    r::AssetHandle asset;
    REQUIRE(manager.OpenScope(Key("level/one"), a) == r::Status::Ok);
    REQUIRE(manager.OpenScope(Key("preview/one"), b) == r::Status::Ok);
    REQUIRE(manager.Resolve(Key("sound/hit"), content::Kind::Sound, asset) == r::Status::Ok);
    // Snapshot lookup must not reopen a mutable catalog pathname.
    REQUIRE(unlink((std::string(fixture.Root) + "/catalog.json").c_str()) == 0);
    r::RequestHandle first, second;
    REQUIRE(manager.BeginAcquire(a, asset, first) == r::Status::Ok);
    REQUIRE(manager.BeginAcquire(b, asset, second) == r::Status::Ok);
    REQUIRE(manager.Inspect().Operations == 1);
    manager.Service(64);
    gate.Started = true;
    REQUIRE(Wait(gate.Entered));
    REQUIRE(manager.Cancel(first) == r::Status::Ok);
    REQUIRE(manager.CloseScope(a) == r::Status::Ok);
    r::AssetHandle meshAsset;
    r::ScopeHandle meshScope;
    r::RequestHandle meshRequest;
    REQUIRE(manager.OpenScope(Key("mesh/preview"), meshScope) == r::Status::Ok);
    REQUIRE(manager.Resolve(meshSource.Resource.Id, content::Kind::Mesh, meshAsset) == r::Status::Ok);
    REQUIRE(manager.BeginAcquire(meshScope, meshAsset, meshRequest) == r::Status::Ok);
    REQUIRE(Pump(manager, meshRequest)); // Audio worker is still gated before reading.
    r::Lease meshLease;
    REQUIRE(manager.TakeLease(meshRequest, meshLease) == r::Status::Ok);
    const void* meshPayload = nullptr;
    REQUIRE(manager.Borrow(meshLease.Token(), content::Kind::Mesh, meshPayload) == r::Status::Ok);
    CHECK(static_cast<const r::MeshDefinition*>(meshPayload)->VertexCount == 3);
    REQUIRE(meshLease.Release() == r::Status::Ok);
    REQUIRE(manager.CloseScope(meshScope) == r::Status::Ok);
    manager.Service(24);
    gate.AllowWork();
    REQUIRE(Wait(gate.Completed));
    REQUIRE(Pump(manager, second));
    REQUIRE_FALSE(gate.Exited.load());
    r::Lease lease;
    REQUIRE(manager.TakeLease(second, lease) == r::Status::Ok);
    REQUIRE(manager.CloseScope(b) == r::Status::Pending);
    const void* raw = nullptr;
    REQUIRE(manager.Borrow(lease.Token(), content::Kind::Sound, raw) == r::Status::Ok);
    const auto& sound = *static_cast<const sample::SoundResourceAdapter::Payload*>(raw);
    REQUIRE(sound.Definition.Id.View() == "sound/hit");
    REQUIRE(sound.Descriptor.VariationCount == 1);
    audio::PlayParams play;
    play.Clip = sound.Descriptor.Variations[0];
    audio::VoiceHandle voice;
    REQUIRE(system.PlayClip(play, voice) == audio::Status::Ok);
    REQUIRE(lease.Release() == r::Status::Ok);
    manager.Service(64);
    REQUIRE(manager.Inspect().ResidentBytes == descriptors[0].Peak.ResidentBytes);
    REQUIRE(manager.Shutdown() == r::Status::Pending);
    float32 output[1024];
    for (uint32 i = 0; i < 30; ++i)
    {
        REQUIRE(system.RenderOffline(output, audio::ChannelLayout::Stereo, audio::BufferLayout::Interleaved, 512) ==
                audio::Status::Ok);
        manager.Service(64);
    }
    REQUIRE(manager.Shutdown() == r::Status::Ok);
    REQUIRE(manager.Inspect().ResidentBytes == 0);
}
TEST_CASE("Final sound waiter cancellation drains completed worker without native exit",
          "[resources][audio][concurrency]")
{
    Fixture fixture;
    audio::AudioSystem system;
    REQUIRE(system.Initialize({}) == audio::Status::Ok);
    content::Text<4096> root;
    REQUIRE(root.Set(fixture.Root));
    Gate gate;
    sample::SoundResourceAdapter adapter(system, fixture.Catalog, root, ROUTING);
    const auto descriptor = adapter.Descriptor();
    r::Manager manager;
    REQUIRE(manager.Initialize(fixture.Catalog, &descriptor, 1) == r::Status::Ok);
    Drain drain{manager, system, gate};
    r::ScopeHandle scope;
    r::AssetHandle asset;
    r::RequestHandle request;
    REQUIRE(manager.OpenScope(Key("level/cancel"), scope) == r::Status::Ok);
    REQUIRE(manager.Resolve(Key("sound/hit"), content::Kind::Sound, asset) == r::Status::Ok);
    REQUIRE(manager.BeginAcquire(scope, asset, request) == r::Status::Ok);
    manager.Service(64);
    gate.Started = true;
    REQUIRE(Wait(gate.Entered));
    const bool late = GENERATE(false, true);
    if (late)
    {
        gate.AllowWork();
        REQUIRE(Wait(gate.Completed));
    }
    REQUIRE(manager.Cancel(request) == r::Status::Ok);
    manager.Service(64);
    REQUIRE(manager.Inspect().PreparationBytes == (late ? 0 : descriptor.Peak.PreparationBytes));
    if (late == false)
    {
        gate.AllowWork();
        REQUIRE(Wait(gate.Completed));
    }
    manager.Service(64);
    r::RequestInfo info;
    REQUIRE(manager.GetRequest(request, info) == r::Status::Ok);
    REQUIRE(info.State == r::RequestState::Cancelled);
    REQUIRE(manager.Inspect().ResidentBytes == 0);
    REQUIRE_FALSE(gate.Exited.load());
    REQUIRE(manager.Shutdown() == r::Status::Ok);
}
