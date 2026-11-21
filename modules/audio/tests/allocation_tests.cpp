// Zero-allocation assertion over the warm submit/render path (design section 4;
// AU04). Uses the same malloc-wrap pattern as the containers/input alloc tests.
// Excluded under sanitizers (they intercept the allocator themselves).

#include <ludus/audio/audio_system.h>
#include <ludus/foundation/containers/array.hpp>

#include "wav_fixture.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <new>

namespace
{
std::atomic<bool> gTrackAllocs{false};
std::atomic<ludus::foundation::uint64> gAllocCount{0};
std::atomic<ludus::foundation::uint64> gFreeCount{0};
void FreeTracked(void* pointerOrNull) noexcept
{
    if (pointerOrNull != nullptr && gTrackAllocs.load(std::memory_order_relaxed) == true)
    {
        gFreeCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(pointerOrNull);
}
} // namespace

// libc++ names the replaceable allocation functions' parameters with reserved
// identifiers (`__sz`), so these replacements cannot match its declarations.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t size)
{
    if (gTrackAllocs.load(std::memory_order_relaxed))
    {
        gAllocCount.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr)
    {
        throw std::bad_alloc();
    }
    return p;
}

void* operator new[](std::size_t size)
{
    if (gTrackAllocs.load(std::memory_order_relaxed))
    {
        gAllocCount.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr)
    {
        throw std::bad_alloc();
    }
    return p;
}

void operator delete(void* p) noexcept
{
    FreeTracked(p);
}
void operator delete(void* p, std::size_t) noexcept
{
    FreeTracked(p);
}
void operator delete[](void* p) noexcept
{
    FreeTracked(p);
}
void operator delete[](void* p, std::size_t) noexcept
{
    FreeTracked(p);
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name)

using namespace ludus::audio;
using ludus::foundation::Array;

TEST_CASE("Warm submit + render performs no allocation or free", "[audio][alloc]")
{
    AudioSystem sys;
    SystemConfig cfg{};
    cfg.SystemMode = Mode::Offline;
    cfg.SampleRate = 48000;
    REQUIRE(IsOk(sys.Initialize(cfg)));

    auto wav = test::MakeSineWav(48000, 1, 4800);
    ClipHandle clip{};
    ClipDescriptor d{};
    REQUIRE(IsOk(sys.PrepareClip(std::span<const uint8>(wav.data(), wav.size()), d, clip)));

    // Warm up: one play + render so any first-touch lazy state is realized.
    VoiceHandle v{};
    REQUIRE(IsOk(sys.PlayClip({ .Clip = clip }, v)));
    Array<float32> out(static_cast<usize>(256) * 2);
    REQUIRE(IsOk(sys.RenderOffline(std::span<float32>(out.data(), out.size()),
                                   ChannelLayout::Stereo,
                                   BufferLayout::Interleaved,
                                   256)));
    sys.Service();

    // Now measure a steady run of submit + render + service.
    gFreeCount.store(0, std::memory_order_relaxed);
    gAllocCount.store(0, std::memory_order_relaxed);
    gTrackAllocs.store(true, std::memory_order_relaxed);
    for (int i = 0; i < 64; ++i)
    {
        VoiceHandle vv{};
        (void)sys.PlayClip({ .Clip = clip }, vv);
        (void)sys.RenderOffline(std::span<float32>(out.data(), out.size()),
                                ChannelLayout::Stereo,
                                BufferLayout::Interleaved,
                                256);
        sys.Service();
        (void)sys.Stop(vv);
    }
    gTrackAllocs.store(false, std::memory_order_relaxed);

    REQUIRE(gAllocCount.load(std::memory_order_relaxed) == 0);
    REQUIRE(gFreeCount.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("Warm music group admission, cue revision and rendering allocate nothing", "[audio][alloc][music]")
{
    using namespace ludus::audio;
    using namespace ludus::foundation;
    AudioSystem system;
    REQUIRE(system.Initialize({}) == Status::Ok);
    const Array<uint8> wave = test::MakeSineWav(DEFAULT_SAMPLE_RATE, 2, 8192);
    StreamDescriptor descriptor;
    descriptor.Looping = true;
    StreamHandle initial[2];
    StreamHandle incoming[2];
    for (usize i = 0; i < 2; ++i)
    {
        REQUIRE(system.PrepareStream(std::span<const uint8>(wave.data(), wave.size()), descriptor, initial[i]) ==
                Status::Ok);
        REQUIRE(system.PrepareStream(std::span<const uint8>(wave.data(), wave.size()), descriptor, incoming[i]) ==
                Status::Ok);
    }
    StreamHandle overflow;
    REQUIRE(system.PrepareStream(std::span<const uint8>(wave.data(), wave.size()), descriptor, overflow) ==
            Status::StreamCapacity);
    float32 samples[CONTROL_QUANTUM_FRAMES * 2]{};
    REQUIRE(system.RenderOffline(samples, ChannelLayout::Stereo, BufferLayout::Interleaved, CONTROL_QUANTUM_FRAMES) ==
            Status::Ok);
    VoiceHandle outgoing;
    VoiceHandle candidate;
    const float32 gains[] = {0.5F, 0};
    const StreamHandle duplicate[] = {initial[0], initial[0]};
    REQUIRE(system.PlayStreamGroup(duplicate, gains, 0, PRIORITY_MAX, outgoing) == Status::InvalidArgument);
    gFreeCount.store(0);
    gAllocCount.store(0);
    gTrackAllocs.store(true);
    const Status start = system.PlayStreamGroup(initial, gains, 0, PRIORITY_MAX, outgoing);
    const Status render =
        system.RenderOffline(samples, ChannelLayout::Stereo, BufferLayout::Interleaved, CONTROL_QUANTUM_FRAMES);
    Status fill = Status::Ok;
    for (usize i = 0; i <= COMMAND_QUEUE_CAPACITY && fill == Status::Ok; ++i)
    {
        fill = system.SetStreamGroupGains(outgoing, gains, {});
    }
    VoiceHandle rejected = outgoing;
    const Status full =
        system.ScheduleStreamGroup(incoming, gains, outgoing, { .StartFrame = 1024, .DurationFrames = 32 }, rejected);
    SystemSnapshot snapshot;
    system.GetSystemSnapshot(snapshot);
    Status drain = Status::Ok;
    while (snapshot.QueueDepth != 0 && drain == Status::Ok)
    {
        drain = system.RenderOffline(samples, ChannelLayout::Stereo, BufferLayout::Interleaved, CONTROL_QUANTUM_FRAMES);
        system.GetSystemSnapshot(snapshot);
    }
    const uint64 revisedFrame = snapshot.RenderFrame + uint64{CONTROL_QUANTUM_FRAMES} * 2;
    const Status schedule = system.ScheduleStreamGroup(incoming,
                                                       gains,
                                                       outgoing,
                                                       { .StartFrame = revisedFrame + 512, .DurationFrames = 32 },
                                                       candidate);
    const Status revise = system.ReviseStreamTransition(candidate, revisedFrame);
    const Status intensity =
        system.SetStreamGroupGains(candidate, gains, { .StartFrame = revisedFrame + 256, .DurationFrames = 128 });
    for (usize i = 0; i < 8; ++i)
    {
        (void)system.RenderOffline(samples, ChannelLayout::Stereo, BufferLayout::Interleaved, CONTROL_QUANTUM_FRAMES);
        system.Service();
    }
    gTrackAllocs.store(false);
    REQUIRE(gAllocCount.load() == 0);
    REQUIRE(gFreeCount.load() == 0);
    REQUIRE(fill == Status::QueueFull);
    REQUIRE(full == Status::QueueFull);
    REQUIRE(rejected == outgoing);
    REQUIRE(drain == Status::Ok);
    REQUIRE(start == Status::Ok);
    REQUIRE(render == Status::Ok);
    REQUIRE(schedule == Status::Ok);
    REQUIRE(revise == Status::Ok);
    REQUIRE(intensity == Status::Ok);
    VoiceInfo info;
    REQUIRE(system.GetVoiceInfo(candidate, info) == Status::Ok);
    REQUIRE(info.StartFrame == revisedFrame);
    REQUIRE(info.CursorFrame > 0);
}
