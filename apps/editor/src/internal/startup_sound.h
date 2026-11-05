#pragma once

// Editor startup sound (private editor header, not installed).
//
// The desktop editor plays the Ludus startup theme once when its window first appears.
// Playback uses the engine's own Audio device path on a worker thread, never the GUI
// thread, and a missing or failing output device leaves the editor silent. Tools >
// Options > General turns it off. See docs/development/editor-workspace.md#startup-sound.

#include <ludus/foundation/base/types.h>

#include "internal/theme_preferences.h"

#include <ludus/audio/audio_types.h>

#include <QByteArray>
#include <QtGlobal>

#include <span>

QT_BEGIN_NAMESPACE
class QSettings;
QT_END_NAMESPACE

namespace ludus::audio
{
class AudioSystem;
} // namespace ludus::audio

namespace ludus::editor
{
/// Qt resource holding the startup theme: lossless FLAC, 44.1 kHz stereo, about 4.5 s, peaks at -1.1 dBFS.
inline constexpr const char* STARTUP_SOUND_RESOURCE = ":/ludus/startup-theme.flac";
/// QSettings key that remembers whether the startup sound plays; an absent key means it plays.
inline constexpr const char* STARTUP_SOUND_SETTING_KEY = "editor/playStartupSound";
/// Environment variable that forces the startup sound on ("1", "true", "on") or off ("0", "false", "off").
inline constexpr const char* STARTUP_SOUND_VARIABLE = "LUDUS_STARTUP_SOUND";

/// Whether the startup sound plays: the remembered choice unless the environment override wins.
[[nodiscard]] bool ResolveStartupSoundEnabled(bool isSettingEnabled, EnvironmentOverride soundOverride) noexcept;
/// The remembered choice in settings; true until the person turns the sound off.
[[nodiscard]] bool ReadStartupSoundSetting(const QSettings& settings);
/// Remember whether the startup sound plays.
void WriteStartupSoundSetting(QSettings& inoutSettings, bool isEnabled);
/// ReadStartupSoundSetting from the default QSettings with the LUDUS_STARTUP_SOUND override applied.
[[nodiscard]] bool IsStartupSoundEnabled();

/// Decode encoded FLAC into inoutSystem and start one non-looping voice on its root bus at full gain.
/// @param inoutSystem An initialized Audio system; preparation is cold work for its control owner.
/// @param encoded FLAC bytes, borrowed only during this call.
/// @param outVoice Receives the voice on success; unchanged on failure.
/// @return Ok, or the Audio status that prevented preparation or playback.
[[nodiscard]] audio::Status StartStartupSound(audio::AudioSystem& inoutSystem,
                                              std::span<const foundation::uint8> encoded,
                                              audio::VoiceHandle& outVoice) noexcept;

/// Plays the startup sound once on its own worker thread and output device.
/// Native desktop editors only; the browser editor does not define it.
class StartupSoundPlayer final
{
public:
    StartupSoundPlayer() = default;
    /// Fades out any playback still running and joins the worker.
    ~StartupSoundPlayer();
    StartupSoundPlayer(const StartupSoundPlayer&) = delete;
    StartupSoundPlayer& operator=(const StartupSoundPlayer&) = delete;

    /// Start the worker with its own copy of encoded FLAC bytes. Device or decoding failures
    /// are logged on the worker and leave the editor silent.
    /// @return false when the player was already started or the worker cannot start.
    [[nodiscard]] bool Play(const QByteArray& encoded);
    /// Ask the worker to fade out and finish. Returns at once; the destructor joins.
    void Stop() noexcept;
    /// True when no worker is running: never started, finished playing, or stopped.
    [[nodiscard]] bool IsFinished() const noexcept;

private:
    struct Impl;
    Impl* mImplOrNull = nullptr;
};

/// Read STARTUP_SOUND_RESOURCE and play it on inoutPlayer when IsStartupSoundEnabled() says so.
/// Does nothing when the sound is turned off or the resource is missing.
void PlayStartupSoundIfEnabled(StartupSoundPlayer& inoutPlayer);
} // namespace ludus::editor
