#include <ludus/foundation/base/config.h>

#include <ludus/audio/audio_system.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>

#include "wav_fixture.h"

#include <cstdlib>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::audio;
namespace threading = ludus::foundation::threading;
namespace timing = ludus::foundation::time;

TEST_CASE("Native device renders resident and streaming voices and joins on shutdown", "[audio][.device]")
{
    // Hosted CI may have no output device. Local opt-in makes absence a failure.
    const bool requireDevice = std::getenv("LUDUS_TEST_AUDIO_DEVICE") != nullptr;
    for (uint32 iteration = 0; iteration < 3; ++iteration)
    {
        AudioSystem system;
        BusConfig bus;
        bus.UserGain = 1.0F;
        SystemConfig config;
        config.SystemMode = Mode::Device;
        config.SampleRate = iteration == 1 ? 44100 : 48000;
        config.DevicePeriodFrames = 256;
        config.Buses = {&bus, 1};
        const auto result = system.Initialize(config);
        if (result == Status::DeviceError && requireDevice == false)
        {
            REQUIRE_FALSE(system.IsValid());
            REQUIRE(system.GetState() == SystemState::Disabled);
            config.SystemMode = Mode::Offline;
            REQUIRE(system.Initialize(config) == Status::Ok);
            REQUIRE(system.BeginShutdown() == Status::Ok);
            SKIP("No native output device; failure cleanup and explicit offline retry passed");
        }
        REQUIRE(result == Status::Ok);
        REQUIRE(system.GetMode() == Mode::Device);
        REQUIRE(system.GetState() == SystemState::Ready);
        REQUIRE(system.GetSampleRate() == config.SampleRate);
        REQUIRE(system.Initialize(config) == Status::InvalidArgument);

        const auto wav = test::MakeSineWav(44100, 1, 44100, 440, 0.05);
        ClipHandle clip;
        REQUIRE(system.PrepareClip(wav, {}, clip) == Status::Ok);
        PlayParams play;
        play.Clip = clip;
        play.Looping = true;
        play.Gain = 0.1F; // Brief, low-level output above the mixer audibility gate.
        VoiceHandle resident;
        REQUIRE(system.PlayClip(play, resident) == Status::Ok);
        StreamDescriptor descriptor;
        descriptor.Looping = true;
        StreamHandle stream;
        VoiceHandle streaming;
        REQUIRE(system.PrepareStream(wav, descriptor, stream) == Status::Ok);
        REQUIRE(system.PlayStream(stream, 0, 6, 0.1F, streaming) == Status::Ok);
        float32 output[2]{};
        REQUIRE(system.RenderOffline(output, ChannelLayout::Stereo, BufferLayout::Interleaved, 1) ==
                Status::InvalidArgument);

        bool progressed = false;
        for (uint32 poll = 0; poll < 1000; ++poll)
        {
            system.Service();
            REQUIRE(system.GetState() == SystemState::Ready);
            VoiceInfo residentInfo, streamInfo;
            REQUIRE(system.GetVoiceInfo(resident, residentInfo) == Status::Ok);
            REQUIRE(system.GetVoiceInfo(streaming, streamInfo) == Status::Ok);
            if (residentInfo.CursorFrame > 4096 && streamInfo.CursorFrame > 4096)
            {
                progressed = true;
                break;
            }
            threading::SleepFor(timing::Milliseconds(1));
        }
        REQUIRE(progressed);
        REQUIRE(system.RetireStream(stream) == Status::Ok);
        REQUIRE(system.Stop(streaming) == Status::Ok);
        REQUIRE(system.BeginShutdown() == Status::Ok);
        REQUIRE(system.GetShutdownState() == ShutdownState::Complete);
        REQUIRE(system.BeginShutdown() == Status::Ok);
        REQUIRE(system.GetState() == SystemState::Disabled);
        REQUIRE(system.PlayClip(play, resident) == Status::NotReady);
    }
}

TEST_CASE("Native callback acknowledges a streamed group transition", "[audio][music][.device]")
{
    using namespace ludus::foundation;
    AudioSystem system;
    SystemConfig config;
    config.SystemMode = Mode::Device;
    const Status initialized = system.Initialize(config);
    if (initialized == Status::DeviceError && std::getenv("LUDUS_TEST_AUDIO_DEVICE") == nullptr)
    {
        SKIP("Set LUDUS_TEST_AUDIO_DEVICE on a host with a native output device");
    }
    REQUIRE(initialized == Status::Ok);
    const Array<uint8> wave = test::MakeSineWav(system.GetSampleRate(), 2, system.GetSampleRate(), 440, 0.05);
    StreamDescriptor descriptor;
    descriptor.Looping = true;
    StaticArray<StreamHandle, 2> initial{};
    StaticArray<StreamHandle, 2> incoming{};
    for (usize i = 0; i < initial.GetSize(); ++i)
    {
        REQUIRE(system.PrepareStream({wave.GetData(), wave.GetSize()}, descriptor, initial[i]) == Status::Ok);
        REQUIRE(system.PrepareStream({wave.GetData(), wave.GetSize()}, descriptor, incoming[i]) == Status::Ok);
    }
    const StaticArray<float32, 2> gains{1, 0};
    VoiceHandle active;
    VoiceHandle candidate;
    REQUIRE(system.PlayStreamGroup(initial, gains, 0, PRIORITY_MAX, active) == Status::Ok);
    VoiceInfo info;
    for (uint32 i = 0; i < 2000 && info.CursorFrame == 0; ++i)
    {
        system.Service();
        REQUIRE(system.GetVoiceInfo(active, info) == Status::Ok);
        threading::SleepFor(timing::Milliseconds(1));
    }
    REQUIRE(info.CursorFrame > 0);
    SystemSnapshot snapshot;
    system.GetSystemSnapshot(snapshot);
    const uint64 boundary = snapshot.RenderFrame + system.GetSampleRate() / 4 + 1;
    REQUIRE(system.ScheduleStreamGroup(incoming,
                                       gains,
                                       active,
                                       { .StartFrame = boundary, .DurationFrames = CONTROL_QUANTUM_FRAMES },
                                       candidate) == Status::Ok);
    const uint64 revised = boundary + CONTROL_QUANTUM_FRAMES;
    REQUIRE(system.ReviseStreamTransition(candidate, revised) == Status::Ok);
    for (uint32 i = 0; i < 2000; ++i)
    {
        system.Service();
        REQUIRE(system.GetVoiceInfo(candidate, info) == Status::Ok);
        if (info.CursorFrame > CONTROL_QUANTUM_FRAMES)
        {
            break;
        }
        threading::SleepFor(timing::Milliseconds(1));
    }
    REQUIRE(info.CursorFrame > CONTROL_QUANTUM_FRAMES);
    REQUIRE(info.StartFrame == revised);
    REQUIRE(system.GetVoiceInfo(active, info) == Status::Ok);
    REQUIRE(info.Terminal == TerminalReason::Stopped);
    REQUIRE(system.BeginShutdown() == Status::Ok);
}
