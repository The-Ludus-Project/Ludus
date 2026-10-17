#include "presentation.h"
#include "wav_fixture.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <ludus/audio/content/loader.h>
#include <time.h>
#include <unistd.h>

using namespace ludus::foundation;
namespace content = ludus::content;
namespace audio = ludus::audio;
namespace
{
struct Directory final
{
    char Path[64] = "/tmp/ludus-audio-content-XXXXXX";
    Directory()
    {
        REQUIRE(mkdtemp(Path) != nullptr);
    }
    // Best-effort temporary-directory cleanup: only std::filesystem::path allocation
    // can throw, and bad_alloc terminating the test process is acceptable here.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~Directory()
    {
        std::error_code error;
        std::filesystem::remove_all(Path, error);
    }
};
audio::content::Sound Definition()
{
    audio::content::Sound sound;
    REQUIRE(sound.Id.Set("sound/hit"));
    REQUIRE(sound.Bus.Set("master"));
    REQUIRE(sound.Group.Set("default"));
    REQUIRE(sound.Variations[0].Set("source/hit"));
    sound.VariationCount = 1;
    return sound;
}
content::Status Poll(audio::content::Loader& loader, audio::content::Lease& lease)
{
    const std::string_view buses[] = {"master"}, groups[] = {"default"};
    const audio::content::Routing routing{buses, groups};
    content::Diagnostic diagnostic;
    for (usize i = 0; i < 1000; ++i)
    {
        const auto status = loader.Poll(routing, lease, diagnostic);
        if (status != content::Status::Pending)
        {
            return status;
        }
        const timespec wait{0, 1000000};
        nanosleep(&wait, nullptr);
    }
    return content::Status::Pending;
}
} // namespace
TEST_CASE("Audio definition codecs preserve old outputs on malformed input", "[content][audio]")
{
    const auto sound = Definition();
    content::Bytes bytes;
    REQUIRE(audio::content::WriteSound(sound, bytes) == content::Status::Ok);
    audio::content::Sound parsed;
    content::Diagnostic diagnostic;
    REQUIRE(audio::content::ReadSound(bytes.String(), parsed, diagnostic) == content::Status::Ok);
    content::Bytes canonical;
    REQUIRE(audio::content::WriteSound(parsed, canonical) == content::Status::Ok);
    REQUIRE(bytes.String() == canonical.String());
    for (const std::string_view bad : {"{}", "{\"version\":2}", "{\"version\":1,\"version\":1}", "{\"gain\":NaN}"})
    {
        REQUIRE(audio::content::ReadSound(bad, parsed, diagnostic) != content::Status::Ok);
        REQUIRE(parsed.Id.View() == sound.Id.View());
    }
    auto invalid = sound;
    invalid.VariationCount = 17;
    REQUIRE(audio::content::WriteSound(invalid, canonical) != content::Status::Ok);
    REQUIRE(canonical.String() == bytes.String());
}
TEST_CASE("Asynchronous leases share clips, retain old playback and cancel acquisition",
          "[content][audio][concurrency]")
{
    Directory directory;
    content::Catalog catalog;
    content::Resource source, soundResource;
    REQUIRE(source.Id.Set("source/hit"));
    REQUIRE(source.Path.Set("hit.wav"));
    REQUIRE(catalog.Put(source) == content::Status::Ok);
    REQUIRE(soundResource.Id.Set("sound/hit"));
    REQUIRE(soundResource.Path.Set("hit.json"));
    soundResource.Type = content::Kind::Sound;
    REQUIRE(catalog.Put(soundResource) == content::Status::Ok);
    content::Bytes bytes;
    REQUIRE(catalog.Write(bytes) == content::Status::Ok);
    REQUIRE(content::SaveFile(directory.Path, "catalog.json", bytes.Data(), nullptr) == content::Status::Ok);
    auto sound = Definition();
    REQUIRE(audio::content::WriteSound(sound, bytes) == content::Status::Ok);
    REQUIRE(content::SaveFile(directory.Path, "hit.json", bytes.Data(), nullptr) == content::Status::Ok);
    const auto wav = audio::test::MakeSineWav(44100, 1, 44100);
    REQUIRE(content::SaveFile(directory.Path, "hit.wav", wav, nullptr) == content::Status::Ok);
    audio::AudioSystem system;
    audio::SystemConfig config;
    REQUIRE(system.Initialize(config) == audio::Status::Ok);
    audio::content::Loader loader(system);
    audio::content::Lease first, second;
    REQUIRE(loader.Begin(directory.Path, "sound/hit") == content::Status::Pending);
    REQUIRE(Poll(loader, first) == content::Status::Ok);
    REQUIRE(loader.Begin(directory.Path, "sound/hit") == content::Status::Pending);
    REQUIRE(Poll(loader, second) == content::Status::Ok);
    audio::app::EventDescriptor firstEvent, secondEvent;
    REQUIRE(loader.GetSound(first, sound, firstEvent) == content::Status::Ok);
    REQUIRE(loader.GetSound(second, sound, secondEvent) == content::Status::Ok);
    REQUIRE(firstEvent.Variations[0] == secondEvent.Variations[0]);
    audio::VoiceHandle voice;
    audio::PlayParams play;
    play.Clip = firstEvent.Variations[0];
    REQUIRE(system.PlayClip(play, voice) == audio::Status::Ok);
    REQUIRE(loader.Release(first) == content::Status::Ok);
    const auto oldDigest = content::Hash(wav);
    const uint8 garbage[] = {0, 1, 2};
    REQUIRE(content::SaveFile(directory.Path, "hit.wav", garbage, &oldDigest) == content::Status::Ok);
    audio::content::Lease rejected;
    REQUIRE(loader.Begin(directory.Path, "sound/hit") == content::Status::Pending);
    REQUIRE(Poll(loader, rejected) != content::Status::Ok);
    REQUIRE(system.IsClipReady(secondEvent.Variations[0]));
    float32 output[512];
    REQUIRE(system.RenderOffline(output, audio::ChannelLayout::Stereo, audio::BufferLayout::Interleaved, 256) ==
            audio::Status::Ok);
    REQUIRE(loader.Begin(directory.Path, "sound/hit") == content::Status::Pending);
    loader.Cancel();
    REQUIRE(Poll(loader, rejected) == content::Status::NotFound);
    const auto brokenDigest = content::Hash(garbage);
    REQUIRE(content::SaveFile(directory.Path, "hit.wav", wav, &brokenDigest) == content::Status::Ok);
    REQUIRE(loader.Begin(directory.Path, "sound/hit") == content::Status::Pending);
    REQUIRE(system.BeginShutdown() == audio::Status::Ok);
    REQUIRE(Poll(loader, rejected) == content::Status::Cancelled);
    REQUIRE(loader.GetSound(second, sound, secondEvent) != content::Status::Ok);
    REQUIRE(loader.Release(second) == content::Status::Ok);
    system.StopAll();
}

TEST_CASE("Copied presentation requests cannot revive removed entity generations", "[content][presentation]")
{
    audio::AudioSystem system;
    audio::SystemConfig config;
    config.LogicalVoiceCapacity = 1;
    config.MixedVoiceCapacity = 1;
    REQUIRE(system.Initialize(config) == audio::Status::Ok);
    const auto wav = audio::test::MakeSineWav(48000, 1, 48000);
    audio::ClipDescriptor clip;
    audio::ClipHandle handle;
    REQUIRE(system.PrepareClip(wav, clip, handle) == audio::Status::Ok);
    audio::app::EventDescriptor event;
    event.VariationCount = 1;
    event.Variations[0] = handle;
    event.DefaultGain = 0.5F;
    event.Looping = true;
    auto sound = Definition();
    sample::Presentation presentation(system);
    presentation.ChangeWorld(1);
    sample::Request request;
    request.Sequence = 1;
    request.Entity = {1, 0, 1};
    request.SoundId = sound.Id;
    request.Required = true;
    REQUIRE(presentation.Deliver(request, 0, sound, event) == audio::Status::Ok);
    REQUIRE(presentation.Deliver(request, 0, sound, event) == audio::Status::Ok);
    REQUIRE(presentation.DeliveredSequence() == 1);
    request.Sequence = 2;
    request.Entity.Slot = 1;
    REQUIRE(presentation.Deliver(request, 1, sound, event) == audio::Status::VoiceCapacity);
    REQUIRE(presentation.DeliveredSequence() == 1); // Required request remains retryable.
    float32 output[2048];
    REQUIRE(system.RenderOffline(output, audio::ChannelLayout::Stereo, audio::BufferLayout::Interleaved, 1024) ==
            audio::Status::Ok);
    system.StopAll();
    REQUIRE(system.RenderOffline(output, audio::ChannelLayout::Stereo, audio::BufferLayout::Interleaved, 1024) ==
            audio::Status::Ok);
    presentation.Service(1);
    REQUIRE(presentation.Deliver(request, 1, sound, event) == audio::Status::Ok);
    REQUIRE(presentation.DeliveredSequence() == 2);
    request.Sequence = 3;
    request.Remove = true;
    REQUIRE(presentation.Deliver(request, 2, sound, event) == audio::Status::Ok);
    REQUIRE(system.RenderOffline(output, audio::ChannelLayout::Stereo, audio::BufferLayout::Interleaved, 1024) ==
            audio::Status::Ok);
    presentation.Service(2);
    request.Sequence = 4;
    request.Remove = false;
    REQUIRE(presentation.Deliver(request, 3, sound, event) == audio::Status::Ok);
    audio::SystemSnapshot snapshot;
    system.GetSystemSnapshot(snapshot);
    REQUIRE(snapshot.PendingVoices + snapshot.MixedVoices + snapshot.VirtualVoices + snapshot.FadingVoices ==
            0); // Tombstone suppresses a delayed old-generation trigger.
    request.Sequence = 5;
    request.Entity.Generation = 2;
    REQUIRE(presentation.Deliver(request, 4, sound, event) == audio::Status::Ok);
    presentation.ChangeWorld(2);
    REQUIRE(system.RenderOffline(output, audio::ChannelLayout::Stereo, audio::BufferLayout::Interleaved, 1024) ==
            audio::Status::Ok);
    presentation.Service(4);
    system.GetSystemSnapshot(snapshot);
    REQUIRE(snapshot.PendingVoices + snapshot.MixedVoices + snapshot.VirtualVoices + snapshot.FadingVoices == 0);
    request.Sequence = 6;
    request.Entity = {2, 64, 1};
    request.Required = false;
    REQUIRE(presentation.Deliver(request, 5, sound, event) == audio::Status::VoiceCapacity);
    REQUIRE(presentation.DeliveredSequence() == 6); // Optional overflow cannot block the next request.
    request.Sequence = 7;
    request.Entity.Slot = 0;
    request.Required = true;
    REQUIRE(presentation.Deliver(request, 5, sound, event) == audio::Status::Ok);
}

TEST_CASE("File-backed music survives source replacement and lease release", "[content][audio][stream][concurrency]")
{
    Directory directory;
    content::Catalog catalog;
    content::Resource source, resource;
    REQUIRE(source.Id.Set("source/music"));
    REQUIRE(source.Path.Set("music.wav"));
    REQUIRE(catalog.Put(source) == content::Status::Ok);
    REQUIRE(resource.Id.Set("music/track"));
    REQUIRE(resource.Path.Set("music.json"));
    resource.Type = content::Kind::Music;
    REQUIRE(catalog.Put(resource) == content::Status::Ok);
    audio::content::Music music;
    music.Id = resource.Id;
    music.Source = source.Id;
    REQUIRE(music.Bus.Set("master"));
    music.Region = { .Enabled = true, .Begin = 4410, .End = 22050 };
    content::Bytes bytes;
    REQUIRE(catalog.Write(bytes) == content::Status::Ok);
    REQUIRE(content::SaveFile(directory.Path, "catalog.json", bytes.Data(), nullptr) == content::Status::Ok);
    REQUIRE(audio::content::WriteMusic(music, bytes) == content::Status::Ok);
    REQUIRE(content::SaveFile(directory.Path, "music.json", bytes.Data(), nullptr) == content::Status::Ok);
    const auto wav = audio::test::MakeSineWav(44100, 1, 44100);
    REQUIRE(content::SaveFile(directory.Path, "music.wav", wav, nullptr) == content::Status::Ok);
    audio::AudioSystem system;
    REQUIRE(system.Initialize({}) == audio::Status::Ok);
    audio::content::Loader loader(system);
    audio::content::Lease lease;
    REQUIRE(loader.Begin(directory.Path, "music/track") == content::Status::Pending);
    REQUIRE(Poll(loader, lease) == content::Status::Ok);
    audio::VoiceHandle voice;
    REQUIRE(loader.PlayMusic(lease, voice) == audio::Status::Ok);
    const auto digest = content::Hash(wav);
    const uint8 broken[] = {0, 1, 2};
    REQUIRE(content::SaveFile(directory.Path, "music.wav", broken, &digest) == content::Status::Ok);
    // A retained reader also starts an independent stream after atomic replacement.
    audio::VoiceHandle second;
    REQUIRE(loader.PlayMusic(lease, second) == audio::Status::Ok);
    REQUIRE(loader.Release(lease) == content::Status::Ok);
    REQUIRE(loader.PlayMusic(lease, second) == audio::Status::InvalidHandle);
    float32 output[1024];
    float32 peak = 0;
    for (uint32 i = 0; i < 160; ++i)
    {
        REQUIRE(system.RenderOffline(output, audio::ChannelLayout::Stereo, audio::BufferLayout::Interleaved, 512) ==
                audio::Status::Ok);
        for (const auto sample : output)
        {
            const auto magnitude = sample < 0 ? -sample : sample;
            REQUIRE(magnitude <= 1.0F);
            if (magnitude > peak)
            {
                peak = magnitude;
            }
        }
        loader.Service();
        const timespec delay{0, 1000000};
        nanosleep(&delay, nullptr);
    }
    REQUIRE(peak > 0.1F);
    audio::VoiceInfo info;
    REQUIRE(system.GetVoiceInfo(voice, info) == audio::Status::Ok);
    REQUIRE(info.CursorFrame > 32768);
    REQUIRE(info.State == audio::VoiceState::Mixed);
    audio::content::Lease rejected{63, 123};
    REQUIRE(loader.Begin(directory.Path, "music/track") == content::Status::Pending);
    REQUIRE(Poll(loader, rejected) != content::Status::Ok);
    REQUIRE(rejected == audio::content::Lease{63, 123});
    REQUIRE(system.BeginShutdown() == audio::Status::Ok);
    loader.Service();
}
