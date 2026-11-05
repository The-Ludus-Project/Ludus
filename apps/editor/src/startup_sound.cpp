#include "internal/startup_sound.h"

#include <ludus/audio/audio_system.h>

#include <QSettings>
#include <QString>

namespace ludus::editor
{
bool ResolveStartupSoundEnabled(bool isSettingEnabled, EnvironmentOverride soundOverride) noexcept
{
    return soundOverride == EnvironmentOverride::Unset ? isSettingEnabled : soundOverride == EnvironmentOverride::On;
}

bool ReadStartupSoundSetting(const QSettings& settings)
{
    return settings.value(QString::fromLatin1(STARTUP_SOUND_SETTING_KEY), true).toBool();
}

void WriteStartupSoundSetting(QSettings& inoutSettings, bool isEnabled)
{
    inoutSettings.setValue(QString::fromLatin1(STARTUP_SOUND_SETTING_KEY), isEnabled);
}

bool IsStartupSoundEnabled()
{
    const QSettings settings;
    return ResolveStartupSoundEnabled(ReadStartupSoundSetting(settings),
                                      ParseEnvironmentOverride(qgetenv(STARTUP_SOUND_VARIABLE)));
}

audio::Status StartStartupSound(audio::AudioSystem& inoutSystem,
                                std::span<const foundation::uint8> encoded,
                                audio::VoiceHandle& outVoice) noexcept
{
    audio::ClipDescriptor descriptor;
    descriptor.Format = audio::SourceFormat::Flac;
    audio::ClipHandle clip;
    const audio::Status prepared = inoutSystem.PrepareClip(encoded, descriptor, clip);
    if (prepared != audio::Status::Ok)
    {
        return prepared;
    }
    audio::PlayParams play;
    play.Clip = clip;
    audio::VoiceHandle voice;
    const audio::Status played = inoutSystem.PlayClip(play, voice);
    // The voice keeps its clip alive; retiring now lets Service reclaim it once playback ends.
    static_cast<void>(inoutSystem.RetireClip(clip));
    if (played != audio::Status::Ok)
    {
        return played;
    }
    outVoice = voice;
    return audio::Status::Ok;
}
} // namespace ludus::editor
