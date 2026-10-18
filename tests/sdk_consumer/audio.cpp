#include <ludus/foundation/base/byte_order.hpp>

#include <ludus/audio/audio_system.h>
#include <ludus/audio/content/loader.h>

#include <cstdlib>
#include <span>
#include <time.h>
#include <unistd.h>

int ExerciseInstalledAudio() noexcept
{
    using namespace ludus::audio;
    using namespace ludus::foundation;
    // A PCM16 mono fixture through public byte codecs; no engine-private headers.
    constexpr uint32 frames = 32768;
    uint8 wav[44 + frames * 2] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' '};
    std::span<uint8> bytes(wav);
    if (TryWriteLittleEndian(uint32{36 + frames * 2}, bytes.subspan(4)) == false ||
        TryWriteLittleEndian(uint32{16}, bytes.subspan(16)) == false ||
        TryWriteLittleEndian(uint16{1}, bytes.subspan(20)) == false ||
        TryWriteLittleEndian(uint16{1}, bytes.subspan(22)) == false ||
        TryWriteLittleEndian(uint32{48000}, bytes.subspan(24)) == false ||
        TryWriteLittleEndian(uint32{96000}, bytes.subspan(28)) == false ||
        TryWriteLittleEndian(uint16{2}, bytes.subspan(32)) == false ||
        TryWriteLittleEndian(uint16{16}, bytes.subspan(34)) == false ||
        TryWriteLittleEndian(uint32{frames * 2}, bytes.subspan(40)) == false)
    {
        return 30;
    }
    wav[36] = 'd';
    wav[37] = 'a';
    wav[38] = 't';
    wav[39] = 'a';
    for (usize i = 44; i < bytes.size(); i += 2)
    {
        if (TryWriteLittleEndian(uint16{3276}, bytes.subspan(i)) == false)
        {
            return 31;
        }
    }
    AudioSystem audio;
    if (audio.Initialize({}) != Status::Ok)
    {
        return 32;
    }
    StreamHandle stream;
    VoiceHandle voice;
    if (audio.PrepareStream(bytes, {}, stream) != Status::Ok ||
        audio.PlayStream(stream, 0, 6, 0.5F, voice) != Status::Ok)
    {
        return 33;
    }
    float32 output[1024];
    bool completed = false;
    for (uint32 i = 0; i < 200; ++i)
    {
        if (audio.RenderOffline(output, ChannelLayout::Stereo, BufferLayout::Interleaved, 512) != Status::Ok)
        {
            return 34;
        }
        audio.Service();
        VoiceInfo info;
        if (audio.GetVoiceInfo(voice, info) != Status::Ok)
        {
            return 35;
        }
        if (info.State == VoiceState::Terminal)
        {
            completed = info.Terminal == TerminalReason::Completed && info.CursorFrame == frames;
            break;
        }
        const timespec wait{0, 1000000};
        nanosleep(&wait, nullptr);
    }
    if (completed == false || audio.RetireStream(stream) != Status::Ok)
    {
        return 36;
    }
    char root[] = "/tmp/ludus-sdk-audio-XXXXXX";
    if (mkdtemp(root) == nullptr)
    {
        return 37;
    }
    struct Cleanup final
    {
        const char* Root;
        ~Cleanup() noexcept
        {
            (void)rmdir(Root);
        }
    } cleanup{root};
    content::Loader loader(audio);
    if (loader.Begin(root, "sound/missing") != ludus::content::Status::Pending)
    {
        return 38;
    }
    content::Lease lease{63, 17, 29};
    ludus::content::Diagnostic diagnostic;
    auto result = ludus::content::Status::Pending;
    for (uint32 i = 0; i < 1000 && result == ludus::content::Status::Pending; ++i)
    {
        result = loader.Poll({}, lease, diagnostic);
        const timespec wait{0, 1000000};
        nanosleep(&wait, nullptr);
    }
    if (result != ludus::content::Status::NotFound || lease != content::Lease{63, 17, 29})
    {
        return 39;
    }
    if (loader.Begin(root, "sound/missing") != ludus::content::Status::Pending)
    {
        return 40;
    }
    loader.RequestCancel();
    result = ludus::content::Status::Pending;
    for (uint32 i = 0; i < 1000 && result == ludus::content::Status::Pending; ++i)
    {
        result = loader.Poll({}, lease, diagnostic);
        const timespec wait{0, 1000000};
        nanosleep(&wait, nullptr);
    }
    if (result != ludus::content::Status::Cancelled || lease != content::Lease{63, 17, 29} ||
        audio.BeginShutdown() != Status::Ok)
    {
        return 41;
    }
    return 0;
}

#if defined(LUDUS_SDK_AUDIO_STANDALONE)
int main()
{
    return ExerciseInstalledAudio();
}
#endif
