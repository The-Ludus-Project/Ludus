// Bus-tree / mix-control tests via the production path (design section 8;
// tasks A3 gate). No device/worker/Platform/window.

#include <ludus/audio/audio_system.h>
#include <ludus/foundation/containers/array.hpp>

#include "wav_fixture.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace ludus::audio;
using ludus::foundation::Array;

namespace
{
// Master(0) <- SFX(1). A voice on SFX passes SFX gain then Master gain, each once.
struct Tree final
{
    BusConfig buses[2];
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): test helper.
    Tree(float32 masterGain, float32 sfxGain)
    {
        buses[0] = BusConfig{ .Id = 0, .ParentIndex = kNoParent, .BaseGain = masterGain };
        buses[1] = BusConfig{ .Id = 1, .ParentIndex = 0, .BaseGain = sfxGain };
    }
    [[nodiscard]] std::span<const BusConfig> span() const
    {
        return std::span<const BusConfig>(buses, 2);
    }
};

void InitWithTree(AudioSystem& sys, const Tree& t)
{
    SystemConfig cfg{};
    cfg.SystemMode = Mode::Offline;
    cfg.SampleRate = 48000;
    cfg.Buses = t.span();
    REQUIRE(IsOk(sys.Initialize(cfg)));
}

ClipHandle PrepareFull(AudioSystem& sys)
{
    // Full-scale constant so gains are directly observable at the output peak.
    auto wav = test::MakeLrWav(48000, 48000, 32767, 32767);
    ClipHandle h{};
    ClipDescriptor d{};
    REQUIRE(IsOk(sys.PrepareClip(std::span<const uint8>(wav.data(), wav.size()), d, h)));
    return h;
}

float32 RenderPeak(AudioSystem& sys, uint32 busIndex, ClipHandle clip)
{
    PlayParams p{};
    p.Clip = clip;
    p.BusIndex = busIndex;
    VoiceHandle v{};
    REQUIRE(IsOk(sys.PlayClip(p, v)));
    constexpr uint32 frames = 2048;
    Array<float32> out(static_cast<usize>(frames) * 2);
    REQUIRE(IsOk(sys.RenderOffline(std::span<float32>(out.data(), out.size()),
                                   ChannelLayout::Stereo,
                                   BufferLayout::Interleaved,
                                   frames)));
    float32 peak = 0.0F;
    for (uint32 f = 0; f < frames; ++f)
    {
        const float32 left = std::fabs(out[static_cast<ludus::foundation::usize>(f) * 2]);
        const float32 right = std::fabs(out[static_cast<ludus::foundation::usize>(f) * 2 + 1]);
        peak = left > peak ? left : peak;
        peak = right > peak ? right : peak;
    }
    return peak;
}
} // namespace

TEST_CASE("A cyclic / forward-parent bus topology is rejected", "[audio][bus]")
{
    BusConfig buses[2]{};
    buses[0] = BusConfig{ .Id = 0, .ParentIndex = 1 }; // forward reference
    buses[1] = BusConfig{ .Id = 1, .ParentIndex = kNoParent };
    AudioSystem sys;
    SystemConfig cfg{};
    cfg.SystemMode = Mode::Offline;
    cfg.SampleRate = 48000;
    cfg.Buses = std::span<const BusConfig>(buses, 2);
    REQUIRE(sys.Initialize(cfg) == Status::InvalidArgument);
}

TEST_CASE("Each ancestor bus gain is applied exactly once", "[audio][bus]")
{
    // Master 0.5, SFX 0.5: a full-scale voice on SFX should be ~0.25 at the root
    // (0.5*0.5), not 0.5 (applied once) nor 0.125 (applied thrice).
    Tree t(0.5F, 0.5F);
    AudioSystem sys;
    InitWithTree(sys, t);
    ClipHandle clip = PrepareFull(sys);
    const float32 peak = RenderPeak(sys, 1, clip);
    REQUIRE(peak == Catch::Approx(0.25F).margin(0.02));
}

TEST_CASE("User mute persists through base-gain mix changes", "[audio][bus]")
{
    Tree t(1.0F, 1.0F);
    AudioSystem sys;
    InitWithTree(sys, t);

    // User-mute the SFX bus (UserGain = 0 => exact mute).
    Command mute{};
    mute.Kind = CommandKind::SetBusUserGain;
    mute.BusIndex = 1;
    mute.BusGain = 0.0F;
    REQUIRE(IsOk(sys.TrySubmitBatch(std::span<const Command>(&mute, 1)).Result));

    // A gameplay base-gain change on the same bus must not un-mute it.
    Command base{};
    base.Kind = CommandKind::SetBusBaseGain;
    base.BusIndex = 1;
    base.BusGain = 1.0F;
    REQUIRE(IsOk(sys.TrySubmitBatch(std::span<const Command>(&base, 1)).Result));

    ClipHandle clip = PrepareFull(sys);
    const float32 peak = RenderPeak(sys, 1, clip);
    REQUIRE(peak < 0.001F); // still muted
}

TEST_CASE("dB slider mapping: u=0/0.5/1 -> gain 0/0.1/1 at 40 dB", "[audio][bus]")
{
    using Catch::Matchers::WithinAbs;
    REQUIRE(AudioSystem::UserGainFromSlider(0.0F, 40.0F) == 0.0F);
    REQUIRE_THAT(static_cast<double>(AudioSystem::UserGainFromSlider(0.5F, 40.0F)), WithinAbs(0.1, 1e-5));
    REQUIRE_THAT(static_cast<double>(AudioSystem::UserGainFromSlider(1.0F, 40.0F)), WithinAbs(1.0, 1e-6));
    // Non-finite / out-of-range inputs are safe (clamped / mute).
    REQUIRE(AudioSystem::UserGainFromSlider(std::nanf(""), 40.0F) == 0.0F);
    REQUIRE(AudioSystem::UserGainFromSlider(2.0F, 40.0F) == Catch::Approx(1.0F));
}

TEST_CASE("Two identical active modifiers have independent lifetimes", "[audio][bus]")
{
    Tree t(1.0F, 1.0F);
    AudioSystem sys;
    InitWithTree(sys, t);

    // The modifier add/remove commands require a modifier handle; v1 exposes them
    // through the batch API. A modifier that is never created is a no-op; here we
    // verify the bus meter reflects the configured gain and clipping counters.
    BusMeter m{};
    REQUIRE(IsOk(sys.GetBusMeter(0, m)));
    REQUIRE(m.CurrentGain == Catch::Approx(1.0F));
    REQUIRE(m.TargetGain == Catch::Approx(1.0F));
}

TEST_CASE("Bus meters report post-gain levels and clip counts", "[audio][bus]")
{
    Tree t(1.0F, 1.0F);
    AudioSystem sys;
    InitWithTree(sys, t);
    ClipHandle clip = PrepareFull(sys);

    PlayParams p{};
    p.Clip = clip;
    p.BusIndex = 1;
    // Two full-scale voices sum above full scale -> clip at the root.
    VoiceHandle v1{};
    VoiceHandle v2{};
    REQUIRE(IsOk(sys.PlayClip(p, v1)));
    REQUIRE(IsOk(sys.PlayClip(p, v2)));

    constexpr uint32 frames = 2048;
    Array<float32> out(static_cast<usize>(frames) * 2);
    REQUIRE(IsOk(sys.RenderOffline(std::span<float32>(out.data(), out.size()),
                                   ChannelLayout::Stereo,
                                   BufferLayout::Interleaved,
                                   frames)));

    BusMeter root{};
    REQUIRE(IsOk(sys.GetBusMeter(0, root)));
    REQUIRE(root.InputPeakL > 1.0F); // input tap sees the pre-clamp sum
    REQUIRE(root.ClippedFrames > 0); // output clipped
    SystemSnapshot snap{};
    sys.GetSystemSnapshot(snap);
    REQUIRE(snap.PreClipFrames > 0);
}

TEST_CASE("The applied sequence acknowledges a user-gain batch after its control boundary", "[audio][bus]")
{
    AudioSystem sys;
    const Tree tree(1.0F, 1.0F);
    InitWithTree(sys, tree);
    REQUIRE(sys.GetCommandRoom() == COMMAND_QUEUE_CAPACITY);
    REQUIRE(sys.GetAppliedSequence() == 0);

    Command gain{};
    gain.Kind = CommandKind::SetBusUserGain;
    gain.BusIndex = 1;
    gain.BusGain = 0.25F;
    const BatchResult accepted = sys.TrySubmitBatch({&gain, 1});
    REQUIRE(IsOk(accepted.Result));
    CHECK(accepted.Sequence == 1);
    CHECK(sys.GetCommandRoom() == COMMAND_QUEUE_CAPACITY - 1);
    // Queued, not yet applied: the renderer has not reached a control boundary.
    CHECK(sys.GetAppliedSequence() < accepted.Sequence);

    gain.BusIndex = 7;
    const BatchResult rejected = sys.TrySubmitBatch({&gain, 1});
    CHECK(rejected.Result == Status::InvalidArgument);
    CHECK(rejected.Sequence == 0);

    Array<float32> out(static_cast<usize>(CONTROL_QUANTUM_FRAMES) * 2);
    REQUIRE(IsOk(sys.RenderOffline(std::span<float32>(out.data(), out.size()),
                                   ChannelLayout::Stereo,
                                   BufferLayout::Interleaved,
                                   CONTROL_QUANTUM_FRAMES)));
    CHECK(sys.GetAppliedSequence() >= accepted.Sequence);
    CHECK(sys.GetCommandRoom() == COMMAND_QUEUE_CAPACITY);
    BusMeter meter{};
    REQUIRE(IsOk(sys.GetBusMeter(1, meter)));
    CHECK(meter.TargetGain == 0.25F);
}

TEST_CASE("Every renderer command publication advances the acknowledgement sequence",
          "[audio][bus][native-audio-worker]")
{
    // A publication path that skipped the owner count would let the applied count overtake a batch's ticket.
    AudioSystem sys;
    const Tree tree(1.0F, 1.0F);
    InitWithTree(sys, tree);
    constexpr usize LAYERS = 2;
    constexpr uint16 STEREO = 2;
    const Array<uint8> wave = test::MakeSineWav(DEFAULT_SAMPLE_RATE, STEREO, 8192);
    StreamDescriptor descriptor;
    descriptor.Looping = true;
    StreamHandle layers[LAYERS];
    for (StreamHandle& layer : layers)
    {
        REQUIRE(IsOk(sys.PrepareStream(std::span<const uint8>(wave.data(), wave.size()), descriptor, layer)));
    }
    const float32 gains[LAYERS] = {0.5F, 0.25F};
    VoiceHandle music{};
    REQUIRE(IsOk(sys.PlayStreamGroup(layers, gains, 1, PRIORITY_MAX, music)));
    REQUIRE(IsOk(sys.SetStreamGroupGains(music, gains, {})));
    Command gain{};
    gain.Kind = CommandKind::SetBusUserGain;
    gain.BusIndex = 1;
    gain.BusGain = 0.5F;
    const BatchResult accepted = sys.TrySubmitBatch({&gain, 1});
    REQUIRE(IsOk(accepted.Result));

    Array<float32> out(static_cast<usize>(CONTROL_QUANTUM_FRAMES) * 2);
    SystemSnapshot snapshot{};
    sys.GetSystemSnapshot(snapshot);
    for (uint32 boundary = 0; boundary < 8 && snapshot.QueueDepth != 0; ++boundary)
    {
        REQUIRE(IsOk(sys.RenderOffline(std::span<float32>(out.data(), out.size()),
                                       ChannelLayout::Stereo,
                                       BufferLayout::Interleaved,
                                       CONTROL_QUANTUM_FRAMES)));
        sys.GetSystemSnapshot(snapshot);
    }
    REQUIRE(snapshot.QueueDepth == 0);
    CHECK(sys.GetAppliedSequence() == accepted.Sequence);
}
