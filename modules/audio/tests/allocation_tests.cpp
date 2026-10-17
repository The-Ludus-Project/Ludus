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
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}
void operator delete[](void* p) noexcept
{
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept
{
    std::free(p);
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
}
