// Startup sound: the setting defaults to on and an environment override wins either way, the
// bundled theme decodes and plays once through the production Audio mixer (offline, so tests stay
// silent), a bad clip fails without a voice, and Tools > Options edits the choice.

#include "internal/options_dialog.h"
#include "internal/startup_sound.h"

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>

#include <ludus/audio/audio_system.h>

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QCheckBox>
#include <QFile>
#include <QListWidget>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>
#include <QThread>

#include <span>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace audio = ludus::audio;

namespace
{
/// Frames rendered per offline block, about 21 ms at 48 kHz.
constexpr uint32 RENDER_BLOCK_FRAMES = 1024;
constexpr usize RENDER_BLOCK_SAMPLES = usize{RENDER_BLOCK_FRAMES} * 2;
/// The theme is about 4.46 s long; rendering stops after this many seconds if it never ends.
constexpr uint32 MAX_RENDER_SECONDS = 8;
/// Upper bound for a failing worker to finish; it never reaches a device-length playback.
constexpr uint32 MAX_WORKER_WAIT_MILLISECONDS = 5000;

QByteArray ReadStartupTheme()
{
    QFile file(QString::fromLatin1(STARTUP_SOUND_RESOURCE));
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}

std::span<const uint8> GetBytes(const QByteArray& bytes)
{
    return {reinterpret_cast<const uint8*>(bytes.constData()), static_cast<usize>(bytes.size())};
}
} // namespace

TEST_CASE("Startup sound plays unless turned off, and the environment wins either way", "[editor][startup-sound]")
{
    CHECK(ResolveStartupSoundEnabled(true, EnvironmentOverride::Unset) == true);
    CHECK(ResolveStartupSoundEnabled(false, EnvironmentOverride::Unset) == false);
    CHECK(ResolveStartupSoundEnabled(false, EnvironmentOverride::On) == true);
    CHECK(ResolveStartupSoundEnabled(true, EnvironmentOverride::Off) == false);
}

TEST_CASE("Startup sound setting defaults to on and remembers the choice", "[editor][startup-sound]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    CHECK(ReadStartupSoundSetting(settings) == true);
    WriteStartupSoundSetting(settings, false);
    CHECK(ReadStartupSoundSetting(settings) == false);
    WriteStartupSoundSetting(settings, true);
    CHECK(ReadStartupSoundSetting(settings) == true);
}

TEST_CASE("Bundled startup theme plays once through the Audio mixer and ends", "[editor][startup-sound][audio]")
{
    const QByteArray theme = ReadStartupTheme();
    REQUIRE(theme.startsWith("fLaC") == true);

    audio::AudioSystem system;
    audio::SystemConfig config;
    config.SystemMode = audio::Mode::Offline;
    REQUIRE(system.Initialize(config) == audio::Status::Ok);
    audio::VoiceHandle voice;
    REQUIRE(StartStartupSound(system, GetBytes(theme), voice) == audio::Status::Ok);
    REQUIRE(voice.IsValid() == true);

    StaticArray<float32, RENDER_BLOCK_SAMPLES> block{};
    const uint64 maxFrames = uint64{system.GetSampleRate()} * MAX_RENDER_SECONDS;
    uint64 renderedFrames = 0;
    float32 peak = 0.0F;
    bool hasEnded = false;
    while (renderedFrames < maxFrames && hasEnded == false)
    {
        REQUIRE(system.RenderOffline(std::span<float32>(block.GetData(), RENDER_BLOCK_SAMPLES),
                                     audio::ChannelLayout::Stereo,
                                     audio::BufferLayout::Interleaved,
                                     RENDER_BLOCK_FRAMES) == audio::Status::Ok);
        renderedFrames += RENDER_BLOCK_FRAMES;
        for (usize i = 0; i < RENDER_BLOCK_SAMPLES; ++i)
        {
            const float32 magnitude = block[i] < 0.0F ? -block[i] : block[i];
            peak = magnitude > peak ? magnitude : peak;
        }
        system.Service();
        audio::VoiceInfo info;
        hasEnded = system.GetVoiceInfo(voice, info) != audio::Status::Ok || info.State == audio::VoiceState::Terminal;
    }
    CHECK(hasEnded == true);
    // The master is about 4.46 s; the voice ends once its last frame has been mixed, not looped.
    const float64 seconds = static_cast<float64>(renderedFrames) / static_cast<float64>(system.GetSampleRate());
    CHECK(seconds > 4.3);
    CHECK(seconds < 4.8);
    // The master peaks at -1.1 dBFS (0.88); resampling to the session rate may move it slightly.
    CHECK(peak > 0.5F);
    CHECK(peak < 0.95F);
    REQUIRE(system.BeginShutdown() == audio::Status::Ok);
}

TEST_CASE("Startup sound reports a clip that cannot decode and starts no voice", "[editor][startup-sound][audio]")
{
    audio::AudioSystem system;
    audio::SystemConfig config;
    config.SystemMode = audio::Mode::Offline;
    REQUIRE(system.Initialize(config) == audio::Status::Ok);
    const QByteArray damaged("fLaC but not a stream");
    audio::VoiceHandle voice;
    CHECK(StartStartupSound(system, GetBytes(damaged), voice) != audio::Status::Ok);
    CHECK(voice.IsValid() == false);
    REQUIRE(system.BeginShutdown() == audio::Status::Ok);
}

TEST_CASE("Startup sound player finishes on failure and plays at most once", "[editor][startup-sound][audio]")
{
    StartupSoundPlayer player;
    CHECK(player.IsFinished() == true);
    // Undecodable bytes keep the test silent whether or not this host has an output device.
    REQUIRE(player.Play(QByteArray("not audio")) == true);
    CHECK(player.Play(QByteArray("not audio")) == false);
    uint32 waited = 0;
    while (player.IsFinished() == false && waited < MAX_WORKER_WAIT_MILLISECONDS)
    {
        QThread::msleep(1);
        ++waited;
    }
    CHECK(player.IsFinished() == true);
}

TEST_CASE("Options edits whether the startup sound plays", "[editor][startup-sound][options]")
{
    OptionsDialog dialog;
    QListWidget* pages = dialog.findChild<QListWidget*>(QStringLiteral("optionsPages"));
    REQUIRE(pages != nullptr);
    CHECK(pages->item(0)->text() == QStringLiteral("Render Graph"));
    REQUIRE(pages->count() == 2);
    CHECK(pages->item(1)->text() == QStringLiteral("General"));
    QCheckBox* checkBox = dialog.findChild<QCheckBox*>(QStringLiteral("optionsPlayStartupSound"));
    REQUIRE(checkBox != nullptr);

    dialog.SetPlaysStartupSound(true);
    CHECK(checkBox->isChecked() == true);
    CHECK(dialog.ShouldPlayStartupSound() == true);
    checkBox->setChecked(false);
    CHECK(dialog.ShouldPlayStartupSound() == false);
    dialog.SetPlaysStartupSound(true);
    CHECK(dialog.ShouldPlayStartupSound() == true);
}
