// Native startup sound worker. The browser editor does not build this file: its Audio
// device needs a user gesture before it may start, which a launch sound cannot provide.

#include "internal/startup_sound.h"

#include <ludus/audio/audio_system.h>
#include <ludus/foundation/base/assert.hpp>
#include <ludus/foundation/logging/category.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/threading/worker_queue.hpp>

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QThread>

#include <atomic>
#include <new>
#include <string_view>

namespace ludus::editor
{
namespace
{
using foundation::uint32;
using foundation::uint8;
using foundation::usize;

LUDUS_DEFINE_LOG_CATEGORY(LOG_STARTUP_SOUND, "Editor.StartupSound");

/// The worker checks for the end of playback, or for Stop, about once per 60 Hz frame.
constexpr uint32 POLL_INTERVAL_MICROSECONDS = 16667;
/// Polls allowed for the stop fade before the system shuts down anyway: about a quarter second.
constexpr uint32 STOP_FADE_POLLS = 15;
} // namespace

struct StartupSoundPlayer::Impl final
{
    explicit Impl(const QByteArray& encoded) : Encoded(encoded) {}
    QByteArray Encoded;
    foundation::threading::WorkerQueue Worker;
    std::atomic<bool> IsStopRequested{false};
    std::atomic<bool> IsDone{false};

    void Run() noexcept
    {
        audio::AudioSystem system;
        audio::SystemConfig config;
        config.SystemMode = audio::Mode::Device;
        const audio::Status initialized = system.Initialize(config);
        if (initialized != audio::Status::Ok)
        {
            LUDUS_LOG_INFO(LOG_STARTUP_SOUND,
                           "Startup sound skipped: no audio device ({})",
                           std::string_view(audio::ToString(initialized)));
            return;
        }
        const std::span<const uint8> encoded(reinterpret_cast<const uint8*>(Encoded.constData()),
                                             static_cast<usize>(Encoded.size()));
        audio::VoiceHandle voice;
        const audio::Status started = StartStartupSound(system, encoded, voice);
        if (started != audio::Status::Ok)
        {
            LUDUS_LOG_WARN(LOG_STARTUP_SOUND,
                           "Startup sound could not play ({})",
                           std::string_view(audio::ToString(started)));
            static_cast<void>(system.BeginShutdown());
            return;
        }
        bool isStopping = false;
        uint32 stopPolls = 0;
        while (isStopping == false || stopPolls < STOP_FADE_POLLS)
        {
            system.Service();
            audio::VoiceInfo info;
            if (system.GetVoiceInfo(voice, info) != audio::Status::Ok || info.State == audio::VoiceState::Terminal)
            {
                break;
            }
            if (isStopping == true)
            {
                ++stopPolls;
            }
            else if (IsStopRequested.load(std::memory_order_acquire) == true)
            {
                // Stop fades the voice out instead of cutting it, so closing the editor never clicks.
                static_cast<void>(system.Stop(voice));
                isStopping = true;
            }
            QThread::usleep(POLL_INTERVAL_MICROSECONDS);
        }
        static_cast<void>(system.BeginShutdown());
    }
};

StartupSoundPlayer::~StartupSoundPlayer()
{
    if (mImplOrNull != nullptr)
    {
        Stop();
        static_cast<void>(mImplOrNull->Worker.Shutdown());
        delete mImplOrNull;
    }
}

bool StartupSoundPlayer::Play(const QByteArray& encoded)
{
    if (mImplOrNull != nullptr)
    {
        return false;
    }
    Impl* implOrNull = new (std::nothrow) Impl(encoded);
    if (implOrNull == nullptr)
    {
        return false;
    }
    if (implOrNull->Worker.Initialize() != foundation::threading::WorkerQueueStatus::Ready ||
        implOrNull->Worker.Submit(
            [](void* context) noexcept {
                LUDUS_ASSERT(context != nullptr);
                Impl* implementation = static_cast<Impl*>(context);
                implementation->Run();
                implementation->IsDone.store(true, std::memory_order_release);
            },
            [](void*) noexcept {},
            implOrNull) != foundation::threading::WorkerQueueStatus::Ready)
    {
        delete implOrNull;
        return false;
    }
    mImplOrNull = implOrNull;
    return true;
}

void StartupSoundPlayer::Stop() noexcept
{
    if (mImplOrNull != nullptr)
    {
        mImplOrNull->IsStopRequested.store(true, std::memory_order_release);
    }
}

bool StartupSoundPlayer::IsFinished() const noexcept
{
    return mImplOrNull == nullptr || mImplOrNull->IsDone.load(std::memory_order_acquire) == true;
}

void PlayStartupSoundIfEnabled(StartupSoundPlayer& inoutPlayer)
{
    if (IsStartupSoundEnabled() == false)
    {
        return;
    }
    QFile file(QString::fromLatin1(STARTUP_SOUND_RESOURCE));
    if (file.open(QIODevice::ReadOnly) == false)
    {
        LUDUS_LOG_TEXT(LOG_STARTUP_SOUND, Warning, "Startup sound resource is missing from this editor build.");
        return;
    }
    if (inoutPlayer.Play(file.readAll()) == false)
    {
        LUDUS_LOG_TEXT(LOG_STARTUP_SOUND, Warning, "Startup sound worker could not start.");
    }
}
} // namespace ludus::editor
