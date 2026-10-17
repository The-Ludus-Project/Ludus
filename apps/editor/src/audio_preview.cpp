#include "internal/audio_preview.h"
#include "internal/content_import.h"
#include <ludus/audio/audio_source.h>
#include <ludus/audio/content/loader.h>

#include <QByteArray>
#include <QMutex>
#include <QMutexLocker>

#include <atomic>
#include <new>
#include <pthread.h>
#include <time.h>

namespace ludus::editor
{
using namespace foundation;
struct AudioPreview::Impl final
{
    explicit Impl(AudioPreview* preview) : Preview(preview) {}
    AudioPreview* Preview;
    pthread_t Thread{};
    bool Started = false;
    std::atomic<uint64> Epoch{0};
    std::atomic<bool> Closing{false};
    std::atomic<bool> Done{true};
    QMutex Mutex;
    bool HasCommand = false;
    bool StopCommand = false;
    bool MusicCommand = false;
    bool ImportCommand = false;
    QString File;
    QString Id;
    QString Root;
    audio::content::Sound Sound;
    audio::content::Music Music;
    void ImportSource(const ContentImportSource& source, uint64 epoch)
    {
        struct Context final
        {
            Impl* Owner;
            uint64 Epoch;
        } context{this, epoch};
        ContentImportGate gate;
        ContentImportHooks hooks;
        hooks.Context = &context;
        hooks.Cancelled = [](void* value) noexcept {
            const auto* input = static_cast<Context*>(value);
            return input->Owner->Closing.load(std::memory_order_acquire) ||
                   input->Owner->Epoch.load(std::memory_order_acquire) != input->Epoch;
        };
        const auto result = ImportAudioSource(source, gate, nullptr, hooks);
        Q_EMIT Preview->Message(result.Message);
        if (result.Status == ludus::content::Status::Ok)
        {
            Q_EMIT Preview->Imported(source.Root);
        }
    }
    void Run()
    {
        audio::BusConfig buses[3];
        buses[0].ParentIndex = audio::kNoParent;
        buses[1].ParentIndex = 0;
        buses[2].ParentIndex = 0;
        audio::GroupConfig groups[2];
        const std::string_view busNames[] = {"master", "sfx", "music"}, groupNames[] = {"default", "impacts"};
        const audio::content::Routing routing{busNames, groupNames};
        audio::AudioSystem system;
        audio::SystemConfig config;
        config.SystemMode = audio::Mode::Device;
        config.Buses = buses;
        config.Groups = groups;
        const auto init = system.Initialize(config);
        if (init != audio::Status::Ok)
        {
            Q_EMIT Preview->Message(QStringLiteral("Audio device: ") + QString::fromUtf8(audio::ToString(init)));
        }
        audio::content::Loader loader(system);
        audio::app::EventAdapter adapter(system);
        audio::content::Lease lease{};
        bool hasLease = false, pending = false;
        audio::VoiceHandle voice{};
        uint64 tick = 0;
        while (!Closing.load(std::memory_order_acquire))
        {
            bool command = false, stop = false, isMusic = false, import = false;
            QString root, file, importId;
            uint64 epoch = 0;
            audio::content::Sound sound;
            audio::content::Music music;
            {
                const QMutexLocker locker(&Mutex);
                command = HasCommand;
                epoch = Epoch.load(std::memory_order_relaxed);
                if (command)
                {
                    stop = StopCommand;
                    isMusic = MusicCommand;
                    import = ImportCommand;
                    root = Root;
                    file = File;
                    importId = Id;
                    sound = Sound;
                    music = Music;
                    HasCommand = false;
                }
            }
            if (command)
            {
                loader.Cancel();
                pending = false;
                if (stop)
                {
                    system.StopAll();
                }
                else if (import)
                {
                    ImportSource({ .Root = root, .File = file, .Id = importId }, epoch);
                }
                else
                {
                    if (init != audio::Status::Ok)
                    {
                        Q_EMIT Preview->Message(QStringLiteral("Preview device unavailable: ") +
                                                QString::fromUtf8(audio::ToString(init)));
                        continue;
                    }
                    const auto encodedRoot = root.toUtf8();
                    const auto id = isMusic ? music.Id.View() : sound.Id.View();
                    // Analysis uses the same decoder on this control worker, never
                    // on the GUI/device thread, and has a separate bounded buffer.
                    ludus::content::Catalog catalog;
                    ludus::content::Bytes bytes;
                    ludus::content::Diagnostic diagnostic;
                    if (ludus::content::ReadFile(encodedRoot.constData(),
                                                 "catalog.json",
                                                 ludus::content::MAX_DOCUMENT_BYTES,
                                                 bytes) == ludus::content::Status::Ok &&
                        catalog.Read(bytes.String(), diagnostic) == ludus::content::Status::Ok)
                    {
                        const auto source =
                            isMusic ? music.Source.View()
                                    : (sound.VariationCount == 0 ? std::string_view{} : sound.Variations[0].View());
                        const auto* entry = catalog.Find(source);
                        if (entry != nullptr && ludus::content::ReadFile(encodedRoot.constData(),
                                                                         entry->Path.View(),
                                                                         audio::BROWSER_ENCODED_CAP_BYTES,
                                                                         bytes) == ludus::content::Status::Ok)
                        {
                            QVector<float32> peaks(512);
                            audio::SourceInfo info;
                            const auto format = entry->Path.View().ends_with(".flac") ? audio::SourceFormat::Flac
                                                                                      : audio::SourceFormat::Wav;
                            if (audio::InspectSource(bytes.Data(),
                                                     format,
                                                     info,
                                                     {peaks.data(), static_cast<usize>(peaks.size())}) ==
                                audio::Status::Ok)
                            {
                                if (Epoch.load(std::memory_order_acquire) == epoch)
                                {
                                    Q_EMIT Preview->Waveform(root, peaks, info.Frames);
                                }
                            }
                        }
                    }
                    if (Epoch.load(std::memory_order_acquire) != epoch)
                    {
                        continue;
                    }
                    const auto result = loader.Begin(encodedRoot.constData(),
                                                     id,
                                                     isMusic ? nullptr : &sound,
                                                     isMusic ? &music : nullptr);
                    pending = result == ludus::content::Status::Pending;
                    Q_EMIT Preview->Message(pending ? QStringLiteral("Preparing draft preview...")
                                                    : QStringLiteral("Draft preparation rejected."));
                }
            }
            if (pending)
            {
                ludus::content::Diagnostic diagnostic;
                audio::content::Lease candidate;
                const auto result = loader.Poll(routing, candidate, diagnostic);
                if (result != ludus::content::Status::Pending)
                {
                    pending = false;
                    if (result == ludus::content::Status::Ok)
                    {
                        audio::content::Music candidateMusic;
                        audio::content::Sound candidateSound;
                        audio::app::EventDescriptor descriptor;
                        const auto previousVoice = voice;
                        audio::Status status = audio::Status::InvalidArgument;
                        if (loader.GetMusic(candidate, candidateMusic) == ludus::content::Status::Ok)
                        {
                            status = loader.PlayMusic(candidate, voice);
                        }
                        else if (loader.GetSound(candidate, candidateSound, descriptor) == ludus::content::Status::Ok)
                        {
                            const auto triggered = adapter.TriggerDescriptor(
                                descriptor,
                                1,
                                (static_cast<uint64>(candidateSound.CooldownMs) * 60 + 999) / 1000,
                                1,
                                tick,
                                candidateSound.SuppressWhileActive);
                            status = triggered.EngineStatus;
                            voice = triggered.Voice;
                        }
                        if (status == audio::Status::Ok)
                        {
                            if (previousVoice.IsValid())
                            {
                                (void)system.Stop(previousVoice);
                            }
                            if (hasLease)
                            {
                                (void)loader.Release(lease);
                            }
                            lease = candidate;
                            hasLease = true;
                            Q_EMIT Preview->Message(QStringLiteral("Playing draft preview."));
                        }
                        else
                        {
                            (void)loader.Release(candidate);
                            Q_EMIT Preview->Message(QStringLiteral("Preview: ") +
                                                    QString::fromUtf8(audio::ToString(status)));
                        }
                    }
                    else
                    {
                        Q_EMIT Preview->Message(
                            QStringLiteral("Preparation failed; previous valid preview retained. ") +
                            QString::fromUtf8(diagnostic.Field));
                    }
                }
            }
            loader.Service();
            adapter.Update(tick++);
            if (tick % 6 == 0)
            {
                audio::BusMeter meter;
                audio::SystemSnapshot snapshot;
                system.GetSystemSnapshot(snapshot);
                if (system.GetBusMeter(0, meter) == audio::Status::Ok)
                {
                    Q_EMIT Preview->Meter(meter.PostPeakL > meter.PostPeakR ? meter.PostPeakL : meter.PostPeakR,
                                          snapshot.StreamStarvations);
                }
            }
            const timespec delay{0, 16666667};
            nanosleep(&delay, nullptr);
        }
        loader.Cancel();
        system.StopAll();
        (void)system.BeginShutdown();
    }
};
AudioPreview::AudioPreview(QObject* parent) : QObject(parent), Impl_(new (std::nothrow) Impl(this)) {}
bool AudioPreview::Start()
{
    if (Impl_ == nullptr || Impl_->Closing.load(std::memory_order_acquire))
    {
        return false;
    }
    if (Impl_->Started)
    {
        return true;
    }
    Impl_->Done.store(false, std::memory_order_release);
    const int result = pthread_create(
        &Impl_->Thread,
        nullptr,
        [](void* input) -> void* {
            auto* impl = static_cast<Impl*>(input);
            impl->Run();
            impl->Done.store(true, std::memory_order_release);
            return nullptr;
        },
        Impl_);
    Impl_->Started = result == 0;
    if (!Impl_->Started)
    {
        Impl_->Done.store(true, std::memory_order_release);
    }
    return Impl_->Started;
}
AudioPreview::~AudioPreview()
{
    Shutdown();
    if (Impl_ != nullptr)
    {
        if (Impl_->Started)
        {
            pthread_join(Impl_->Thread, nullptr);
        }
        delete Impl_;
    }
}
void AudioPreview::Play(const QString& root, const audio::content::Sound& sound)
{
    if (!Start())
    {
        Q_EMIT Message(QStringLiteral("Preview worker unavailable."));
        return;
    }
    const QMutexLocker locker(&Impl_->Mutex);
    Impl_->Root = root;
    Impl_->Sound = sound;
    Impl_->ImportCommand = false;
    Impl_->MusicCommand = false;
    Impl_->StopCommand = false;
    Impl_->Epoch.fetch_add(1, std::memory_order_release);
    Impl_->HasCommand = true;
}
void AudioPreview::Play(const QString& root, const audio::content::Music& music)
{
    if (!Start())
    {
        Q_EMIT Message(QStringLiteral("Preview worker unavailable."));
        return;
    }
    const QMutexLocker locker(&Impl_->Mutex);
    Impl_->Root = root;
    Impl_->Music = music;
    Impl_->ImportCommand = false;
    Impl_->MusicCommand = true;
    Impl_->StopCommand = false;
    Impl_->Epoch.fetch_add(1, std::memory_order_release);
    Impl_->HasCommand = true;
}
void AudioPreview::Import(const ContentImportSource& source)
{
    if (!Start())
    {
        return;
    }
    const QMutexLocker locker(&Impl_->Mutex);
    Impl_->Root = source.Root;
    Impl_->File = source.File;
    Impl_->Id = source.Id;
    Impl_->ImportCommand = true;
    Impl_->StopCommand = false;
    Impl_->Epoch.fetch_add(1, std::memory_order_release);
    Impl_->HasCommand = true;
}
void AudioPreview::Stop()
{
    if (Impl_ != nullptr)
    {
        const QMutexLocker locker(&Impl_->Mutex);
        Impl_->ImportCommand = false;
        Impl_->StopCommand = true;
        Impl_->Epoch.fetch_add(1, std::memory_order_release);
        Impl_->HasCommand = true;
    }
}
void AudioPreview::Shutdown()
{
    if (Impl_ != nullptr)
    {
        Impl_->Closing.store(true, std::memory_order_release);
    }
}
bool AudioPreview::Finished() const noexcept
{
    return Impl_ == nullptr || Impl_->Done.load(std::memory_order_acquire);
}
} // namespace ludus::editor

bool ludus::editor::AudioPreview::Reset()
{
    if (!Finished())
    {
        return false;
    }
    if (Impl_ != nullptr && Impl_->Started)
    {
        pthread_join(Impl_->Thread, nullptr);
    }
    delete Impl_;
    Impl_ = new (std::nothrow) Impl(this);
    return Impl_ != nullptr;
}
