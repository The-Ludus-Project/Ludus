#include "internal/file_input.hpp"
#include <ludus/audio/audio_source.h>
#include <ludus/audio/content/loader.h>
#include <ludus/foundation/base/config.h>

#include <atomic>
#include <new>
#if defined(LUDUS_AUDIO_CONTENT_WORKER_TESTING)
#    include "internal/worker_test_hooks.hpp"
#endif
#if LUDUS_TARGET_OS == LUDUS_OS_LINUX || LUDUS_TARGET_OS == LUDUS_OS_MACOS
#    include <pthread.h>
#endif

namespace ludus::audio::content
{
namespace
{
// Owner provenance is process-local and never reused, including after destruction.
std::atomic<uint64> NEXT_OWNER{1};
uint64 ReserveOwner() noexcept
{
    auto owner = NEXT_OWNER.load(std::memory_order_relaxed);
    while (owner != 0xFFFFFFFFFFFFFFFFULL)
    {
        if (NEXT_OWNER.compare_exchange_weak(owner, owner + 1, std::memory_order_relaxed))
        {
            return owner;
        }
    }
    return 0;
}
#if defined(LUDUS_AUDIO_CONTENT_WORKER_TESTING)
internal::WorkerTestHooks WORKER_TEST_HOOKS;
#endif
SourceFormat Format(std::string_view path) noexcept
{
    return path.ends_with(".flac") ? SourceFormat::Flac : SourceFormat::Wav;
}
struct Acquisition final
{
    Text<4096> Root;
    ResourceId Id;
    Sound Definition;
    Music MusicDefinition;
    bool Draft = false;
    bool MusicDraft = false;
    bool IsMusic = false;
    bool HasSnapshot = false;
    ludus::content::Catalog Catalog;
    Bytes Sources[16];
    ludus::content::Digest Digests[16];
    SourceFormat Formats[16]{};
    internal::FileInput* Input = nullptr;
    ~Acquisition() noexcept
    {
        delete Input;
    }
    PreparedClip Prepared[16];
    uint32 Rate = 0;
    uint32 Session = 0;
    SourceInfo Info[16];
    Diagnostic Error;
    std::atomic<bool> Cancelled{false};
    std::atomic<bool> Done{false};
    ContentStatus Result = ContentStatus::Pending;
#if defined(LUDUS_AUDIO_CONTENT_WORKER_TESTING)
    internal::WorkerTestHooks TestHooks;
#endif
#if LUDUS_TARGET_OS == LUDUS_OS_LINUX || LUDUS_TARGET_OS == LUDUS_OS_MACOS
    pthread_t Thread{};
#endif
    void Run() noexcept
    {
        Bytes bytes;
        if (HasSnapshot == false)
        {
            Result = ludus::content::ReadFile(Root.View(), "catalog.json", ludus::content::MAX_DOCUMENT_BYTES, bytes);
            if (Result != ContentStatus::Ok)
            {
                return;
            }
            Result = Catalog.Read(bytes.String(), Error);
            if (Result != ContentStatus::Ok)
            {
                return;
            }
        }
        const auto* entry = Catalog.Find(Id.View());
        if (entry == nullptr)
        {
            Result = ContentStatus::NotFound;
            return;
        }
        IsMusic = entry->Type == ludus::content::Kind::Music;
        if (entry->Type != ludus::content::Kind::Sound && IsMusic == false)
        {
            Result = ContentStatus::Invalid;
            return;
        }
        if (Draft == false && MusicDraft == false)
        {
            Result =
                ludus::content::ReadFile(Root.View(), entry->Path.View(), ludus::content::MAX_DOCUMENT_BYTES, bytes);
            if (Result != ContentStatus::Ok)
            {
                return;
            }
            Result = IsMusic ? ReadMusic(bytes.String(), MusicDefinition, Error)
                             : ReadSound(bytes.String(), Definition, Error);
            if (Result != ContentStatus::Ok)
            {
                return;
            }
        }
        if ((IsMusic && MusicDefinition.Id.View() != Id.View()) ||
            (IsMusic == false && Definition.Id.View() != Id.View()) || (Draft && IsMusic) ||
            (MusicDraft && IsMusic == false))
        {
            Result = ContentStatus::Invalid;
            return;
        }
        const auto count = IsMusic ? 1 : Definition.VariationCount;
        uint64 total = 0;
        uint32 firstRate = 0;
        uint64 preparedTotal = 0;
        for (usize i = 0; i < count; ++i)
        {
            if (Cancelled.load(std::memory_order_acquire))
            {
                Result = ContentStatus::Cancelled;
                return;
            }
            const auto key = IsMusic ? MusicDefinition.Source.View() : Definition.Variations[i].View();
            const auto* source = Catalog.Find(key);
            if (source == nullptr || source->Type != ludus::content::Kind::AudioSource)
            {
                Result = ContentStatus::NotFound;
                return;
            }
            Formats[i] = Format(source->Path.View());
            if (IsMusic)
            {
                Input = new (std::nothrow) internal::FileInput();
                if (Input == nullptr)
                {
                    Result = ContentStatus::OutOfMemory;
                    return;
                }
                Result = Input->File.Open(Root.View(), source->Path.View());
                if (Result != ContentStatus::Ok || InspectSource(*Input, Formats[i], Info[i]) != Status::Ok)
                {
                    Result = ContentStatus::Invalid;
                    return;
                }
                const auto& loop = MusicDefinition.Region;
                if (loop.Enabled && loop.End > Info[i].Frames)
                {
                    Result = ContentStatus::Invalid;
                    return;
                }
                continue;
            }
            Result = ludus::content::ReadFile(Root.View(), source->Path.View(), BROWSER_ENCODED_CAP_BYTES, Sources[i]);
            if (Result != ContentStatus::Ok)
            {
                return;
            }
            total += Sources[i].Data().size();
            if (total > BROWSER_ENCODED_CAP_BYTES)
            {
                Result = ContentStatus::Limit;
                return;
            }
            Digests[i] = ludus::content::Hash(Sources[i].Data());
            if (InspectSource(Sources[i].Data(), Formats[i], Info[i]) != Status::Ok)
            {
                Result = ContentStatus::Invalid;
                return;
            }
            const auto& loop = IsMusic ? MusicDefinition.Region : Definition.Region;
            if (loop.Enabled && (loop.End > Info[i].Frames || (i != 0 && firstRate != Info[i].SampleRate)))
            {
                Result = ContentStatus::Invalid;
                return;
            }
            ClipDescriptor clipDescriptor;
            clipDescriptor.Format = Formats[i];
            clipDescriptor.SourceLoopBegin = loop.Begin;
            clipDescriptor.SourceLoopEnd = loop.End;
            const auto frames = Info[i].Frames;
            const auto sourceRate = Info[i].SampleRate;
            if (frames / sourceRate >
                RESIDENT_PCM_CAP_BYTES / (static_cast<uint64>(sizeof(float32)) * Info[i].Channels * Rate))
            {
                Result = ContentStatus::Limit;
                return;
            }
            const auto targetFrames =
                (frames / sourceRate) * Rate + ((frames % sourceRate) * Rate + sourceRate - 1) / sourceRate + 2;
            if (targetFrames >
                (RESIDENT_PCM_CAP_BYTES - preparedTotal) / (static_cast<uint64>(sizeof(float32)) * Info[i].Channels))
            {
                Result = ContentStatus::Limit;
                return;
            }
            const auto decoded = Prepared[i].Decode(Sources[i].Data(), clipDescriptor, Rate);
            if (decoded != Status::Ok)
            {
                Result = decoded == Status::OutOfMemory ? ContentStatus::OutOfMemory : ContentStatus::Invalid;
                return;
            }
            preparedTotal += Prepared[i].Bytes();
            if (preparedTotal > RESIDENT_PCM_CAP_BYTES)
            {
                Result = ContentStatus::Limit;
                return;
            }
            Sources[i] = Bytes{};
            firstRate = Info[i].SampleRate;
        }
        Result = ContentStatus::Ok;
    }
};
} // namespace
struct Loader::Impl final
{
    explicit Impl(AudioSystem& audio) noexcept : Audio(audio) {}
    AudioSystem& Audio;
    uint64 Owner = ReserveOwner();
    Acquisition* Pending = nullptr;
    struct Clip final
    {
        bool InUse = false;
        ludus::content::Digest Digest;
        Loop Region;
        ClipHandle Handle;
        uint32 References = 0;
    };
    struct Revision final
    {
        bool InUse = false;
        uint32 Generation = 0;
        uint32 Session = 0;
        bool IsMusic = false;
        Sound Definition;
        Music MusicDefinition;
        app::EventDescriptor Descriptor;
        uint32 Clips[16]{};
        usize Count = 0;
        Bytes Encoded;
        SourceFormat Format = SourceFormat::Wav;
        uint32 MusicBus = 0;
        internal::FileInput* Input = nullptr;
    };
    Clip Clips[64];
    Revision Revisions[64];
    struct Playing final
    {
        bool InUse = false;
        StreamHandle Stream;
        VoiceHandle Voice;
    };
    Playing Streams[4];
    void ReleasePending() noexcept
    {
        delete Pending;
        Pending = nullptr;
    }
    void Drop(uint32 index) noexcept
    {
        auto& clip = Clips[index];
        if (clip.References != 0)
        {
            --clip.References;
        }
        if (clip.References == 0)
        {
            (void)Audio.RetireClip(clip.Handle);
            clip.InUse = false;
        }
    }
};
Loader::Loader(AudioSystem& audio) noexcept : mImpl(new (std::nothrow) Impl(audio)) {}
Loader::~Loader() noexcept
{
    Cancel();
    if (mImpl != nullptr)
    {
        for (uint32 i = 0; i < 64; ++i)
        {
            if (mImpl->Revisions[i].InUse)
            {
                (void)Release({i, mImpl->Revisions[i].Generation, mImpl->Owner});
            }
        }
        for (auto& stream : mImpl->Streams)
        {
            if (stream.InUse)
            {
                (void)mImpl->Audio.Stop(stream.Voice);
                (void)mImpl->Audio.RetireStream(stream.Stream);
            }
        }
        delete mImpl;
    }
}
ContentStatus Loader::Begin(std::string_view root,
                            std::string_view id,
                            const Sound* draft,
                            const Music* musicDraft,
                            const ludus::content::Catalog* snapshot) noexcept
{
    if (mImpl == nullptr)
    {
        return ContentStatus::OutOfMemory;
    }
    if (mImpl->Pending != nullptr)
    {
        return ContentStatus::Pending;
    }
    if (ludus::content::ValidId(id) == false || root.size() > 4096 || (draft != nullptr && musicDraft != nullptr))
    {
        return ContentStatus::Invalid;
    }
    if (mImpl->Owner == 0)
    {
        return ContentStatus::Limit;
    }
    auto* job = new (std::nothrow) Acquisition();
    if (job == nullptr)
    {
        return ContentStatus::OutOfMemory;
    }
    job->Rate = mImpl->Audio.GetSampleRate();
    if (job->Rate == 0)
    {
        delete job;
        return ContentStatus::Invalid;
    }
    job->Session = mImpl->Audio.GetSession();
    (void)job->Root.Set(root);
    (void)job->Id.Set(id);
    if (snapshot != nullptr)
    {
        for (const auto& entry : snapshot->Entries())
        {
            const auto status = job->Catalog.Put(entry);
            if (status != ContentStatus::Ok)
            {
                delete job;
                return status;
            }
        }
        job->HasSnapshot = true;
    }
    Bytes canonical;
    Diagnostic error;
    if (draft != nullptr)
    {
        const auto result = WriteSound(*draft, canonical);
        if (result != ContentStatus::Ok)
        {
            delete job;
            return result;
        }
        if (ReadSound(canonical.String(), job->Definition, error) != ContentStatus::Ok)
        {
            delete job;
            return ContentStatus::Invalid;
        }
        job->Draft = true;
    }
    if (musicDraft != nullptr)
    {
        const auto result = WriteMusic(*musicDraft, canonical);
        if (result != ContentStatus::Ok)
        {
            delete job;
            return result;
        }
        if (ReadMusic(canonical.String(), job->MusicDefinition, error) != ContentStatus::Ok)
        {
            delete job;
            return ContentStatus::Invalid;
        }
        job->MusicDraft = true;
    }
#if LUDUS_TARGET_OS == LUDUS_OS_LINUX || LUDUS_TARGET_OS == LUDUS_OS_MACOS
#    if defined(LUDUS_AUDIO_CONTENT_WORKER_TESTING)
    job->TestHooks = WORKER_TEST_HOOKS;
#    endif
    if (pthread_create(
            &job->Thread,
            nullptr,
            [](void* input) -> void* {
                auto* acquisition = static_cast<Acquisition*>(input);
#    if defined(LUDUS_AUDIO_CONTENT_WORKER_TESTING)
                const auto hooks = acquisition->TestHooks;
                if (hooks.BeforeRun != nullptr)
                {
                    hooks.BeforeRun(hooks.Context);
                }
#    endif
                acquisition->Run();
                // Final access to acquisition: acquire-observing Poll may detach
                // and destroy it before this native thread returns. Never add
                // job access or job-owned cleanup after publication. This is the
                // Stage 1 boundary in docs/architecture/resource-management.md.
                acquisition->Done.store(true, std::memory_order_release);
#    if defined(LUDUS_AUDIO_CONTENT_WORKER_TESTING)
                if (hooks.AfterCompletion != nullptr)
                {
                    hooks.AfterCompletion(hooks.Context);
                }
#    endif
                return nullptr;
            },
            job) != 0)
    {
        delete job;
        return ContentStatus::IoError;
    }
    mImpl->Pending = job;
    return ContentStatus::Pending;
#else
    delete job;
    return ContentStatus::Unsupported;
#endif
}
void Loader::RequestCancel() noexcept
{
    if (mImpl != nullptr && mImpl->Pending != nullptr)
    {
        mImpl->Pending->Cancelled.store(true, std::memory_order_release);
    }
}
void Loader::Cancel() noexcept
{
    if (mImpl == nullptr || mImpl->Pending == nullptr)
    {
        return;
    }
    RequestCancel();
#if LUDUS_TARGET_OS == LUDUS_OS_LINUX || LUDUS_TARGET_OS == LUDUS_OS_MACOS
    pthread_join(mImpl->Pending->Thread, nullptr);
#endif
    mImpl->ReleasePending();
}
ContentStatus Loader::Poll(const Routing& routing, Lease& output, Diagnostic& diagnostic) noexcept
{
    if (mImpl == nullptr)
    {
        return ContentStatus::OutOfMemory;
    }
    auto* job = mImpl->Pending;
    if (job == nullptr)
    {
        return ContentStatus::NotFound;
    }
    if (job->Done.load(std::memory_order_acquire) == false)
    {
        return ContentStatus::Pending;
    }
#if LUDUS_TARGET_OS == LUDUS_OS_LINUX || LUDUS_TARGET_OS == LUDUS_OS_MACOS
    // Detach only completed work. On failure retain ownership for Cancel's join;
    // no candidate installation or lease mutation has happened yet.
    if (pthread_detach(job->Thread) != 0)
    {
        return ContentStatus::IoError;
    }
#endif
    SystemSnapshot audioState;
    mImpl->Audio.GetSystemSnapshot(audioState);
    ContentStatus result = job->Cancelled.load(std::memory_order_acquire) == false &&
                                   job->Session == mImpl->Audio.GetSession() && audioState.State == SystemState::Ready
                               ? job->Result
                               : ContentStatus::Cancelled;
    diagnostic = job->Error;
    uint32 index = 64;
    for (uint32 i = 0; i < 64; ++i)
    {
        if (mImpl->Revisions[i].InUse == false && mImpl->Revisions[i].Generation < 0xFFFFFFFEU)
        {
            index = i;
            break;
        }
    }
    if (result == ContentStatus::Ok && index == 64)
    {
        result = ContentStatus::Limit;
    }
    if (result != ContentStatus::Ok)
    {
        mImpl->ReleasePending();
        return result;
    }
    auto& revision = mImpl->Revisions[index];
    app::EventDescriptor descriptor{};
    uint32 musicBus = 0;
    result = job->IsMusic ? ValidateMusic(job->MusicDefinition, job->Catalog, routing, musicBus, diagnostic)
                          : Resolve(job->Definition, job->Catalog, routing, descriptor, diagnostic);
    if (result != ContentStatus::Ok)
    {
        mImpl->ReleasePending();
        return result;
    }
    revision.Count = 0;
    if (job->IsMusic == false)
    {
        for (usize i = 0; i < job->Definition.VariationCount; ++i)
        {
            uint32 clipIndex = 64;
            const auto& region = job->Definition.Region;
            for (uint32 j = 0; j < 64; ++j)
            {
                const auto& clip = mImpl->Clips[j];
                if (clip.InUse && clip.Digest == job->Digests[i] && clip.Region.Enabled == region.Enabled &&
                    clip.Region.Begin == region.Begin && clip.Region.End == region.End &&
                    clip.Handle.Session == mImpl->Audio.GetSession() && mImpl->Audio.IsClipReady(clip.Handle))
                {
                    clipIndex = j;
                    break;
                }
            }
            if (clipIndex == 64)
            {
                for (uint32 j = 0; j < 64; ++j)
                {
                    if (mImpl->Clips[j].InUse == false)
                    {
                        clipIndex = j;
                        break;
                    }
                }
                if (clipIndex == 64)
                {
                    result = ContentStatus::Limit;
                    break;
                }
                auto& clip = mImpl->Clips[clipIndex];
                const auto status = mImpl->Audio.InstallClip(job->Prepared[i], clip.Handle);
                if (status != Status::Ok)
                {
                    result = status == Status::OutOfMemory ? ContentStatus::OutOfMemory : ContentStatus::Invalid;
                    break;
                }
                clip.InUse = true;
                clip.Digest = job->Digests[i];
                clip.Region = region;
                clip.References = 0;
            }
            auto& clip = mImpl->Clips[clipIndex];
            ++clip.References;
            revision.Clips[revision.Count++] = clipIndex;
            descriptor.Variations[i] = clip.Handle;
        }
        if (result != ContentStatus::Ok)
        {
            for (usize i = 0; i < revision.Count; ++i)
            {
                mImpl->Drop(revision.Clips[i]);
            }
            mImpl->ReleasePending();
            return result;
        }
        descriptor.VariationCount = static_cast<uint32>(revision.Count);
        descriptor.EventId = index + 1;
    }
    revision.InUse = true;
    ++revision.Generation;
    revision.Session = job->Session;
    revision.IsMusic = job->IsMusic;
    revision.Definition = job->Definition;
    revision.MusicDefinition = job->MusicDefinition;
    revision.Descriptor = descriptor;
    revision.MusicBus = musicBus;
    if (job->IsMusic)
    {
        revision.Input = job->Input;
        job->Input = nullptr;
        revision.Format = job->Formats[0];
    }
    output = {index, revision.Generation, mImpl->Owner};
    mImpl->ReleasePending();
    return ContentStatus::Ok;
}
ContentStatus Loader::GetSound(Lease lease, Sound& definition, app::EventDescriptor& descriptor) const noexcept
{
    if (mImpl == nullptr || lease.Slot >= 64 || lease.Owner != mImpl->Owner)
    {
        return ContentStatus::Invalid;
    }
    const auto& revision = mImpl->Revisions[lease.Slot];
    if (revision.InUse == false || revision.Generation != lease.Generation ||
        revision.Session != mImpl->Audio.GetSession() || revision.IsMusic)
    {
        return ContentStatus::Invalid;
    }
    for (usize i = 0; i < revision.Count; ++i)
    {
        if (mImpl->Audio.IsClipReady(revision.Descriptor.Variations[i]) == false)
        {
            return ContentStatus::Cancelled;
        }
    }
    definition = revision.Definition;
    descriptor = revision.Descriptor;
    return ContentStatus::Ok;
}
ContentStatus Loader::GetMusic(Lease lease, Music& definition) const noexcept
{
    if (mImpl == nullptr || lease.Slot >= 64 || lease.Owner != mImpl->Owner)
    {
        return ContentStatus::Invalid;
    }
    const auto& revision = mImpl->Revisions[lease.Slot];
    if (revision.InUse == false || revision.Generation != lease.Generation ||
        revision.Session != mImpl->Audio.GetSession() || revision.IsMusic == false)
    {
        return ContentStatus::Invalid;
    }
    definition = revision.MusicDefinition;
    return ContentStatus::Ok;
}
Status Loader::PlayMusic(Lease lease, VoiceHandle& voice) noexcept
{
    Music definition;
    if (GetMusic(lease, definition) != ContentStatus::Ok)
    {
        return Status::InvalidHandle;
    }
    auto& revision = mImpl->Revisions[lease.Slot];
    Impl::Playing* free = nullptr;
    for (auto& playing : mImpl->Streams)
    {
        if (playing.InUse == false)
        {
            free = &playing;
            break;
        }
    }
    if (free == nullptr)
    {
        return Status::StreamCapacity;
    }
    StreamDescriptor config{};
    config.Format = revision.Format;
    config.Looping = definition.Region.Enabled;
    config.SourceLoopBegin = definition.Region.Begin;
    config.SourceLoopEnd = definition.Region.End;
    StreamInput* input = revision.Input->Clone();
    if (input == nullptr)
    {
        return Status::OutOfMemory;
    }
    StreamHandle stream;
    auto status = mImpl->Audio.PrepareStream(foundation::core::UniquePtr<StreamInput>(input), config, stream);
    if (status != Status::Ok)
    {
        return status;
    }
    status = mImpl->Audio.PlayStream(stream, revision.MusicBus, definition.Priority, definition.Gain, voice);
    if (status != Status::Ok)
    {
        (void)mImpl->Audio.RetireStream(stream);
        return status;
    }
    free->InUse = true;
    free->Stream = stream;
    free->Voice = voice;
    return Status::Ok;
}
ContentStatus Loader::Release(Lease lease) noexcept
{
    if (mImpl == nullptr || lease.Slot >= 64 || lease.Owner != mImpl->Owner)
    {
        return ContentStatus::Invalid;
    }
    auto& revision = mImpl->Revisions[lease.Slot];
    if (revision.InUse == false || revision.Generation != lease.Generation)
    {
        return ContentStatus::Invalid;
    }
    for (usize i = 0; i < revision.Count; ++i)
    {
        mImpl->Drop(revision.Clips[i]);
    }
    revision.Encoded = Bytes{};
    delete revision.Input;
    revision.Input = nullptr;
    revision.InUse = false;
    return ContentStatus::Ok;
}
void Loader::Service() noexcept
{
    if (mImpl == nullptr)
    {
        return;
    }
    mImpl->Audio.Service();
    for (auto& playing : mImpl->Streams)
    {
        if (playing.InUse == false)
        {
            continue;
        }
        VoiceInfo info;
        if (mImpl->Audio.GetVoiceInfo(playing.Voice, info) != Status::Ok || info.State == VoiceState::Terminal)
        {
            (void)mImpl->Audio.RetireStream(playing.Stream);
            playing.InUse = false;
        }
    }
}
#if defined(LUDUS_AUDIO_CONTENT_WORKER_TESTING)
void internal::SetWorkerTestHooks(WorkerTestHooks hooks) noexcept
{
    WORKER_TEST_HOOKS = hooks;
}
#endif
} // namespace ludus::audio::content
