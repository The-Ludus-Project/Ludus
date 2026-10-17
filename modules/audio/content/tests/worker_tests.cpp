#include <ludus/audio/content/loader.h>

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
            while (!Exited.load(std::memory_order_acquire))
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
    Fixture()
    {
        REQUIRE(mkdtemp(Root) != nullptr);
        content::Catalog catalog;
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
content::Status Acquire(audio::content::Loader& loader, const Fixture& fixture, audio::content::Lease& lease)
{
    if (loader.Begin(fixture.Root, "sound/hit") != content::Status::Pending)
    {
        return content::Status::Invalid;
    }
    content::Diagnostic diagnostic;
    for (uint32 i = 0; i < 2000; ++i)
    {
        const auto result = loader.Poll(ROUTING, lease, diagnostic);
        if (result != content::Status::Pending)
        {
            return result;
        }
        WaitTurn();
    }
    return content::Status::Pending;
}
} // namespace

TEST_CASE("Cancellation drains without waiting or accepting a replacement request", "[content][concurrency]")
{
    Fixture fixture;
    audio::AudioSystem system;
    REQUIRE(system.Initialize({}) == audio::Status::Ok);
    audio::content::Loader loader(system);
    audio::content::Lease active;
    REQUIRE(Acquire(loader, fixture, active) == content::Status::Ok);
    const bool cancelAfterWork = GENERATE(false, true);
    Gate gate;
    REQUIRE(loader.Begin(fixture.Root, "sound/hit") == content::Status::Pending);
    gate.Started = true;
    REQUIRE(Wait(gate.Entered));
    if (!cancelAfterWork)
    {
        loader.RequestCancel();
        loader.RequestCancel();
    }
    audio::content::Lease output{63, 17, 29};
    content::Diagnostic diagnostic;
    diagnostic.Offset = 59;
    REQUIRE(loader.Poll(ROUTING, output, diagnostic) == content::Status::Pending);
    REQUIRE(diagnostic.Offset == 59);
    REQUIRE(output == audio::content::Lease{63, 17, 29});
    REQUIRE(loader.Begin(fixture.Root, "sound/other") == content::Status::Pending);
    gate.AllowWork();
    REQUIRE(Wait(gate.Completed));
    loader.RequestCancel();
    loader.RequestCancel();
    REQUIRE(loader.Poll(ROUTING, output, diagnostic) == content::Status::Cancelled);
    REQUIRE(output == audio::content::Lease{63, 17, 29});
    REQUIRE_FALSE(gate.Exited.load());
    REQUIRE(loader.Poll(ROUTING, output, diagnostic) == content::Status::NotFound);
    audio::content::Sound sound;
    audio::app::EventDescriptor descriptor;
    REQUIRE(loader.GetSound(active, sound, descriptor) == content::Status::Ok);
    REQUIRE(loader.Release(active) == content::Status::Ok);
}

TEST_CASE("Completed acquisition installs before native worker exit", "[content][concurrency]")
{
    Fixture fixture;
    audio::AudioSystem system;
    REQUIRE(system.Initialize({}) == audio::Status::Ok);
    audio::content::Loader loader(system);
    Gate gate;
    REQUIRE(loader.Begin(fixture.Root, "sound/hit") == content::Status::Pending);
    gate.Started = true;
    REQUIRE(Wait(gate.Entered));
    gate.AllowWork();
    REQUIRE(Wait(gate.Completed));
    audio::content::Lease lease;
    content::Diagnostic diagnostic;
    REQUIRE(loader.Poll(ROUTING, lease, diagnostic) == content::Status::Ok);
    REQUIRE_FALSE(gate.Exited.load());
    audio::content::Sound sound;
    audio::app::EventDescriptor descriptor;
    REQUIRE(loader.GetSound(lease, sound, descriptor) == content::Status::Ok);
    REQUIRE(system.IsClipReady(descriptor.Variations[0]));
    REQUIRE(loader.Release(lease) == content::Status::Ok);
}

TEST_CASE("Completed errors and shutdown never publish or wait for worker exit", "[content][concurrency]")
{
    Fixture fixture;
    audio::AudioSystem system;
    REQUIRE(system.Initialize({}) == audio::Status::Ok);
    audio::content::Loader loader(system);
    Gate gate;
    const bool shutdown = GENERATE(false, true);
    REQUIRE(loader.Begin(fixture.Root, shutdown ? "sound/hit" : "sound/missing") == content::Status::Pending);
    gate.Started = true;
    gate.AllowWork();
    REQUIRE(Wait(gate.Completed));
    if (shutdown)
    {
        REQUIRE(system.BeginShutdown() == audio::Status::Ok);
    }
    audio::content::Lease lease{63, 17, 29};
    content::Diagnostic diagnostic;
    REQUIRE(loader.Poll(ROUTING, lease, diagnostic) ==
            (shutdown ? content::Status::Cancelled : content::Status::NotFound));
    REQUIRE(lease == audio::content::Lease{63, 17, 29});
    REQUIRE_FALSE(gate.Exited.load());
}

TEST_CASE("Blocking cancellation still waits for native worker exit", "[content][concurrency]")
{
    Fixture fixture;
    audio::AudioSystem system;
    REQUIRE(system.Initialize({}) == audio::Status::Ok);
    audio::content::Loader loader(system);
    Gate gate;
    REQUIRE(loader.Begin(fixture.Root, "sound/hit") == content::Status::Pending);
    gate.Started = true;
    gate.AllowWork();
    REQUIRE(Wait(gate.Completed));
    std::thread release([&] { gate.AllowExit(); });
    loader.Cancel();
    release.join();
    REQUIRE(gate.Exited.load());
    audio::content::Lease lease;
    content::Diagnostic diagnostic;
    REQUIRE(loader.Poll(ROUTING, lease, diagnostic) == content::Status::NotFound);
}

TEST_CASE("Leases cannot release another loader or a reused revision", "[content][ownership]")
{
    Fixture fixture;
    audio::AudioSystem system;
    REQUIRE(system.Initialize({}) == audio::Status::Ok);
    audio::content::Loader first(system), second(system);
    audio::content::Lease a, b;
    REQUIRE(Acquire(first, fixture, a) == content::Status::Ok);
    REQUIRE(Acquire(second, fixture, b) == content::Status::Ok);
    REQUIRE(a.Slot == b.Slot);
    REQUIRE(a.Generation == b.Generation);
    REQUIRE(a.Owner != b.Owner);
    audio::content::Sound sound;
    audio::app::EventDescriptor descriptor;
    REQUIRE(second.GetSound(a, sound, descriptor) == content::Status::Invalid);
    REQUIRE(second.Release(a) == content::Status::Invalid);
    REQUIRE(second.GetSound(b, sound, descriptor) == content::Status::Ok);
    REQUIRE(first.Release(a) == content::Status::Ok);
    REQUIRE(first.Release(a) == content::Status::Invalid);
    audio::content::Lease replacement;
    REQUIRE(Acquire(first, fixture, replacement) == content::Status::Ok);
    REQUIRE(replacement.Slot == a.Slot);
    REQUIRE(replacement.Generation != a.Generation);
    REQUIRE(first.GetSound(a, sound, descriptor) == content::Status::Invalid);
    REQUIRE(first.Release(a) == content::Status::Invalid);
    REQUIRE(first.GetSound(replacement, sound, descriptor) == content::Status::Ok);
    REQUIRE(system.BeginShutdown() == audio::Status::Ok);
    REQUIRE(first.GetSound(replacement, sound, descriptor) == content::Status::Cancelled);
    REQUIRE(first.Release(replacement) == content::Status::Ok);
    REQUIRE(second.Release(b) == content::Status::Ok);
}

TEST_CASE("Revision saturation preserves cancellation and acquisition error status", "[content][capacity]")
{
    Fixture fixture;
    audio::AudioSystem system;
    REQUIRE(system.Initialize({}) == audio::Status::Ok);
    audio::content::Loader loader(system);
    audio::content::Lease leases[64];
    for (auto& lease : leases)
    {
        REQUIRE(Acquire(loader, fixture, lease) == content::Status::Ok);
    }
    audio::content::Lease output{63, 17, 29};
    REQUIRE(Acquire(loader, fixture, output) == content::Status::Limit);
    REQUIRE(output == audio::content::Lease{63, 17, 29});
    Gate gate;
    REQUIRE(loader.Begin(fixture.Root, "sound/hit") == content::Status::Pending);
    gate.Started = true;
    REQUIRE(Wait(gate.Entered));
    loader.RequestCancel();
    gate.AllowWork();
    REQUIRE(Wait(gate.Completed));
    content::Diagnostic diagnostic;
    REQUIRE(loader.Poll(ROUTING, output, diagnostic) == content::Status::Cancelled);
    REQUIRE(output == audio::content::Lease{63, 17, 29});
    for (auto lease : leases)
    {
        REQUIRE(loader.Release(lease) == content::Status::Ok);
    }
}

TEST_CASE("Loader destruction retires its issued revisions and does not reuse owner identity", "[content][ownership]")
{
    Fixture fixture;
    audio::AudioSystem system;
    REQUIRE(system.Initialize({}) == audio::Status::Ok);
    audio::content::Lease old;
    audio::ClipHandle clip;
    {
        audio::content::Loader loader(system);
        REQUIRE(Acquire(loader, fixture, old) == content::Status::Ok);
        audio::content::Sound sound;
        audio::app::EventDescriptor descriptor;
        REQUIRE(loader.GetSound(old, sound, descriptor) == content::Status::Ok);
        clip = descriptor.Variations[0];
    }
    REQUIRE_FALSE(system.IsClipReady(clip));
    audio::content::Loader replacement(system);
    audio::content::Lease current;
    REQUIRE(Acquire(replacement, fixture, current) == content::Status::Ok);
    REQUIRE(current.Slot == old.Slot);
    REQUIRE(current.Generation == old.Generation);
    REQUIRE(current.Owner != old.Owner);
    REQUIRE(replacement.Release(old) == content::Status::Invalid);
    REQUIRE(replacement.Release(current) == content::Status::Ok);
}
