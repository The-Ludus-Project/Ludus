#include "wav_fixture.h"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ludus/audio/audio_source.h>
#include <ludus/audio/audio_system.h>
#include <new>
#include <thread>

using namespace ludus::audio;
using namespace ludus::foundation;
TEST_CASE("Streaming instances refill concurrently, replay and retire independently",
          "[audio][stream][concurrency][native-audio-worker]")
{
    AudioSystem system;
    SystemConfig config;
    config.SystemMode = Mode::Offline;
    REQUIRE(system.Initialize(config) == Status::Ok);
    const auto wav = test::MakeSineWav(44100, 2, 44100, 440, 0.4);
    StreamDescriptor descriptor;
    descriptor.Looping = true;
    descriptor.SourceLoopBegin = 4410;
    descriptor.SourceLoopEnd = 22050;
    StreamHandle streams[4];
    VoiceHandle voices[4];
    for (usize i = 0; i < 4; ++i)
    {
        REQUIRE(system.PrepareStream(wav, descriptor, streams[i]) == Status::Ok);
        REQUIRE(system.PlayStream(streams[i], 0, 6, 0.2F, voices[i]) == Status::Ok);
    }
    StreamHandle overflow;
    REQUIRE(system.PrepareStream(wav, descriptor, overflow) == Status::StreamCapacity);
    VoiceHandle replay;
    REQUIRE(system.PlayStream(streams[0], 0, 6, 0.2F, replay) == Status::NotReady);
    float32 output[1024];
    float32 peak = 0;
    for (uint32 i = 0; i < 200; ++i)
    {
        REQUIRE(system.RenderOffline(output, ChannelLayout::Stereo, BufferLayout::Interleaved, 512) == Status::Ok);
        for (const auto sample : output)
        {
            REQUIRE(std::isfinite(sample));
            const auto level = std::fabs(sample);
            if (level > peak)
            {
                peak = level;
            }
        }
        if (i == 20)
        {
            REQUIRE(system.RetireStream(streams[0]) == Status::Ok);
            REQUIRE(system.Stop(voices[0]) == Status::Ok);
        }
        system.Service();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(peak > 0.1F);
    StreamHandle replacement;
    REQUIRE(system.PrepareStream(wav, descriptor, replacement) == Status::Ok);
    REQUIRE(replacement != streams[0]);
    REQUIRE(system.RetireStream(replacement) == Status::Ok);
    system.StopAll();
    REQUIRE(system.BeginShutdown() == Status::Ok);
    PlayParams params;
    REQUIRE(system.PlayClip(params, replay) == Status::NotReady);
}
TEST_CASE("Source metadata and direct loop conversion reject invalid bounds", "[audio][source]")
{
    const auto wav = test::MakeSineWav(44100, 1, 4410, 440, 0.4);
    SourceInfo info;
    float32 peaks[64];
    REQUIRE(InspectSource(wav, SourceFormat::Wav, info, peaks) == Status::Ok);
    REQUIRE(info.SampleRate == 44100);
    REQUIRE(info.Frames == 4410);
    REQUIRE(info.Channels == 1);
    REQUIRE(info.Peak > 0.3F);
    REQUIRE(InspectSource(wav, SourceFormat::Flac, info) == Status::DecodeError);
    REQUIRE(InspectSource(wav, static_cast<SourceFormat>(255), info) == Status::Unsupported);
    AudioSystem system;
    SystemConfig config;
    REQUIRE(system.Initialize(config) == Status::Ok);
    ClipDescriptor clip;
    clip.SourceLoopBegin = 100;
    clip.SourceLoopEnd = 4411;
    ClipHandle handle;
    REQUIRE(system.PrepareClip(wav, clip, handle) == Status::InvalidArgument);
    clip.SourceLoopBegin = 441;
    clip.SourceLoopEnd = 2205;
    REQUIRE(system.PrepareClip(wav, clip, handle) == Status::Ok);
    StreamDescriptor stream;
    stream.Looping = true;
    stream.SourceLoopEnd = 4411;
    StreamHandle source;
    REQUIRE(system.PrepareStream(wav, stream, source) == Status::InvalidArgument);
    stream.SourceLoopEnd = 4410;
    stream.Looping = false;
    REQUIRE(system.PrepareStream(wav, stream, source) == Status::InvalidArgument);
}
