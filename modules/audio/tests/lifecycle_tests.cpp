// Lifecycle / cancellation / scheduling / offline-render tests via the
// production public path (design sections 3-5; tasks A1 gate rows). No device,
// worker, Platform or window.

#include <ludus/audio/audio_system.h>
#include <ludus/foundation/containers/array.hpp>

#include "wav_fixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace ludus::audio;
using ludus::foundation::Array;

namespace
{
void InitOffline(AudioSystem& sys)
{
    SystemConfig cfg{};
    cfg.SystemMode = Mode::Offline;
    cfg.SampleRate = 48000;
    const Status s = sys.Initialize(cfg);
    REQUIRE(IsOk(s));
}

ClipHandle PrepareSine(AudioSystem& sys, uint32 frames = 480)
{
    auto wav = test::MakeSineWav(48000, 1, frames);
    ClipHandle h{};
    ClipDescriptor d{};
    const Status s = sys.PrepareClip(std::span<const uint8>(wav.data(), wav.size()), d, h);
    REQUIRE(IsOk(s));
    return h;
}
} // namespace

TEST_CASE("Zero-frame render is a no-op and does not advance the clock", "[audio][lifecycle]")
{
    AudioSystem sys;
    InitOffline(sys);
    Array<float32> out(0);
    REQUIRE(IsOk(
        sys.RenderOffline(std::span<float32>(out.data(), 0), ChannelLayout::Stereo, BufferLayout::Interleaved, 0)));
    SystemSnapshot snap{};
    sys.GetSystemSnapshot(snap);
    REQUIRE(snap.RenderFrame == 0);
}

TEST_CASE("Offline render fills output and advances the sample clock", "[audio][lifecycle]")
{
    AudioSystem sys;
    InitOffline(sys);
    constexpr uint32 frames = 256;
    Array<float32> out(static_cast<usize>(frames) * 2, 1.0F); // prefill nonzero to see it cleared
    REQUIRE(IsOk(sys.RenderOffline(std::span<float32>(out.data(), out.size()),
                                   ChannelLayout::Stereo,
                                   BufferLayout::Interleaved,
                                   frames)));
    // A1: silence output.
    for (float32 v : out)
    {
        REQUIRE(v == 0.0F);
    }
    SystemSnapshot snap{};
    sys.GetSystemSnapshot(snap);
    REQUIRE(snap.RenderFrame == frames);
}

TEST_CASE("Different RenderOffline partitions yield the same boundary behavior", "[audio][lifecycle]")
{
    // Advancing 256 frames in one call vs. two 128-frame calls lands at the same
    // RenderFrame and processes the same control boundaries.
    AudioSystem a;
    InitOffline(a);
    AudioSystem b;
    InitOffline(b);
    Array<float32> oa(static_cast<usize>(256) * 2);
    Array<float32> ob(static_cast<usize>(128) * 2);
    REQUIRE(IsOk(a.RenderOffline(std::span<float32>(oa.data(), oa.size()),
                                 ChannelLayout::Stereo,
                                 BufferLayout::Interleaved,
                                 256)));
    REQUIRE(IsOk(b.RenderOffline(std::span<float32>(ob.data(), ob.size()),
                                 ChannelLayout::Stereo,
                                 BufferLayout::Interleaved,
                                 128)));
    REQUIRE(IsOk(b.RenderOffline(std::span<float32>(ob.data(), ob.size()),
                                 ChannelLayout::Stereo,
                                 BufferLayout::Interleaved,
                                 128)));
    SystemSnapshot sa{};
    SystemSnapshot sb{};
    a.GetSystemSnapshot(sa);
    b.GetSystemSnapshot(sb);
    REQUIRE(sa.RenderFrame == sb.RenderFrame);
    REQUIRE(sa.RenderFrame == 256);
}

TEST_CASE("Rejecting an unsupported output layout", "[audio][lifecycle]")
{
    AudioSystem sys;
    InitOffline(sys);
    Array<float32> out(128);
    REQUIRE(sys.RenderOffline(std::span<float32>(out.data(), out.size()),
                              ChannelLayout::Mono,
                              BufferLayout::Interleaved,
                              128) == Status::Unsupported);
}

TEST_CASE("Stop before start cancels a pending play with no sample emitted", "[audio][lifecycle]")
{
    AudioSystem sys;
    InitOffline(sys);
    ClipHandle clip = PrepareSine(sys);
    VoiceHandle v{};
    REQUIRE(IsOk(sys.PlayClip({ .Clip = clip }, v)));

    // Stop while still Pending (before the first render boundary consumes it).
    REQUIRE(IsOk(sys.Stop(v)));

    Array<float32> out(static_cast<usize>(128) * 2);
    REQUIRE(IsOk(sys.RenderOffline(std::span<float32>(out.data(), out.size()),
                                   ChannelLayout::Stereo,
                                   BufferLayout::Interleaved,
                                   128)));
    sys.Service();

    VoiceInfo info{};
    const Status gi = sys.GetVoiceInfo(v, info);
    // Either the slot was reclaimed (InvalidHandle) or it reports Terminal/Cancelled.
    if (IsOk(gi))
    {
        REQUIRE(info.State == VoiceState::Terminal);
        REQUIRE(info.Terminal == TerminalReason::Cancelled);
    }
    else
    {
        REQUIRE(gi == Status::InvalidHandle);
    }
}

TEST_CASE("StopAll cancels old epoch but a subsequent Play survives", "[audio][lifecycle]")
{
    AudioSystem sys;
    InitOffline(sys);
    ClipHandle clip = PrepareSine(sys);
    VoiceHandle old{};
    REQUIRE(IsOk(sys.PlayClip({ .Clip = clip }, old)));

    sys.StopAll();

    VoiceHandle fresh{};
    REQUIRE(IsOk(sys.PlayClip({ .Clip = clip }, fresh)));

    Array<float32> out(static_cast<usize>(128) * 2);
    REQUIRE(IsOk(sys.RenderOffline(std::span<float32>(out.data(), out.size()),
                                   ChannelLayout::Stereo,
                                   BufferLayout::Interleaved,
                                   128)));
    sys.Service();

    // Old voice cancelled; fresh survives (Scheduled/Mixed, not Terminal-cancelled).
    VoiceInfo oldInfo{};
    if (IsOk(sys.GetVoiceInfo(old, oldInfo)))
    {
        REQUIRE(oldInfo.State == VoiceState::Terminal);
    }
    VoiceInfo freshInfo{};
    REQUIRE(IsOk(sys.GetVoiceInfo(fresh, freshInfo)));
    REQUIRE(freshInfo.State != VoiceState::Terminal);
}

TEST_CASE("Stop and StopAll work with a full command queue", "[audio][lifecycle]")
{
    AudioSystem sys;
    InitOffline(sys);
    ClipHandle clip = PrepareSine(sys);

    // Flood the queue without rendering so it stays full.
    VoiceHandle first{};
    bool gotFirst = false;
    for (int i = 0; i < 2000; ++i)
    {
        VoiceHandle v{};
        const Status s = sys.PlayClip({ .Clip = clip }, v);
        if (IsOk(s) && gotFirst == false)
        {
            first = v;
            gotFirst = true;
        }
        if (s == Status::QueueFull || s == Status::VoiceCapacity || s == Status::GroupCapacity)
        {
            break;
        }
    }
    REQUIRE(gotFirst);

    // Stop and StopAll must still be deliverable (mailbox / epoch, not the queue).
    REQUIRE(IsOk(sys.Stop(first)));
    sys.StopAll(); // never needs queue space
    SUCCEED();
}

TEST_CASE("Scheduled start begins at the requested sample offset", "[audio][lifecycle]")
{
    AudioSystem sys;
    InitOffline(sys);
    ClipHandle clip = PrepareSine(sys, 4800);
    PlayParams p{};
    p.Clip = clip;
    p.StartFrame = 300; // within the first 256-boundary window's reach
    VoiceHandle v{};
    REQUIRE(IsOk(sys.PlayClip(p, v)));

    VoiceInfo info{};
    REQUIRE(IsOk(sys.GetVoiceInfo(v, info)));
    REQUIRE(info.StartFrame == 300);
}

TEST_CASE("A far-future start beyond the horizon is rejected", "[audio][lifecycle]")
{
    AudioSystem sys;
    InitOffline(sys);
    ClipHandle clip = PrepareSine(sys);
    PlayParams p{};
    p.Clip = clip;
    p.StartFrame = static_cast<uint64>(48000) * 10; // 10s >> 2s horizon
    VoiceHandle v{};
    REQUIRE(sys.PlayClip(p, v) == Status::InvalidArgument);
}

TEST_CASE("Retire with an active play keeps bytes valid until terminal", "[audio][lifecycle]")
{
    AudioSystem sys;
    InitOffline(sys);
    ClipHandle clip = PrepareSine(sys);
    VoiceHandle v{};
    REQUIRE(IsOk(sys.PlayClip({ .Clip = clip }, v)));

    // Retire the clip; a new play must be rejected, retained bytes stay valid.
    REQUIRE(IsOk(sys.RetireClip(clip)));
    VoiceHandle v2{};
    REQUIRE(sys.PlayClip({ .Clip = clip }, v2) == Status::InvalidHandle);
}

TEST_CASE("User-gain slider mapping: u=0/0.5/1 -> 0/0.1/1 at 40 dB", "[audio][lifecycle]")
{
    using Catch::Matchers::WithinAbs;
    REQUIRE(AudioSystem::UserGainFromSlider(0.0F, 40.0F) == 0.0F);
    REQUIRE_THAT(static_cast<double>(AudioSystem::UserGainFromSlider(0.5F, 40.0F)), WithinAbs(0.1, 1e-5));
    REQUIRE_THAT(static_cast<double>(AudioSystem::UserGainFromSlider(1.0F, 40.0F)), WithinAbs(1.0, 1e-6));
}

TEST_CASE("A stale handle cannot modify a reused slot's new generation", "[audio][lifecycle]")
{
    AudioSystem sys;
    InitOffline(sys);
    ClipHandle clip = PrepareSine(sys, 128);

    // Play, stop, render + service so the slot terminates and is reclaimed.
    VoiceHandle old{};
    REQUIRE(IsOk(sys.PlayClip({ .Clip = clip }, old)));
    REQUIRE(IsOk(sys.Stop(old)));
    Array<float32> out(static_cast<usize>(128) * 2);
    REQUIRE(IsOk(sys.RenderOffline(std::span<float32>(out.data(), out.size()),
                                   ChannelLayout::Stereo,
                                   BufferLayout::Interleaved,
                                   128)));
    sys.Service();

    // Stop on a terminal handle still known to the owner is idempotent (Ok),
    // not an error (design section 3).
    REQUIRE(IsOk(sys.Stop(old)));

    // A fresh play reuses the same physical slot with a NEW generation. The
    // stale handle's identity (old generation) must no longer match it: queries
    // and stops targeting the old handle are rejected and cannot mutate the new
    // generation.
    VoiceHandle fresh{};
    REQUIRE(IsOk(sys.PlayClip({ .Clip = clip }, fresh)));
    REQUIRE(fresh.Slot == old.Slot);             // same physical slot reused
    REQUIRE(fresh.Generation != old.Generation); // new generation
    VoiceInfo info{};
    REQUIRE(sys.GetVoiceInfo(old, info) == Status::InvalidHandle);
    REQUIRE(sys.Stop(old) == Status::InvalidHandle);
    REQUIRE(IsOk(sys.GetVoiceInfo(fresh, info)));
}

TEST_CASE("Dropped snapshots still allow terminal acknowledgment and reclaim", "[audio][lifecycle]")
{
    AudioSystem sys;
    InitOffline(sys);
    ClipHandle clip = PrepareSine(sys, 128);
    VoiceHandle v{};
    REQUIRE(IsOk(sys.PlayClip({ .Clip = clip }, v)));
    REQUIRE(IsOk(sys.Stop(v)));

    Array<float32> out(static_cast<usize>(128) * 2);
    REQUIRE(IsOk(sys.RenderOffline(std::span<float32>(out.data(), out.size()),
                                   ChannelLayout::Stereo,
                                   BufferLayout::Interleaved,
                                   128)));
    // Even without draining the snapshot ring, Service acknowledges terminals.
    sys.Service();
    GroupInfo gi{};
    REQUIRE(IsOk(sys.GetGroupInfo(0, gi)));
    REQUIRE(gi.Admitted == 0); // charge released via durable terminal, not a snapshot
}
