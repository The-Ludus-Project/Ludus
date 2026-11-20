#include <ludus/audio/configuration/audio_configuration.hpp>

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>

// Thanks to Lasse Staff Jensen, "A Generic Tweaker", Game Programming Gems 2,
// sec.1.18, pp.118-126: module-owned descriptors share metadata with tools while
// the subsystem consumes plain options. The owner copies values into renderer
// commands; no writable engine pointer is exposed to configuration.
namespace ludus::audio::configuration
{
namespace
{
using foundation::usize;
using foundation::config::Apply;
using foundation::config::Binding;
using foundation::config::Context;
using foundation::config::Descriptor;
using foundation::config::Diagnostic;
using foundation::config::Explanation;
using foundation::config::Status;
using foundation::config::Type;
using foundation::config::Value;

constexpr std::string_view MASTER_VOLUME = "audio.volume.master";
constexpr std::string_view MUSIC_VOLUME = "audio.volume.music";
constexpr std::string_view EFFECTS_VOLUME = "audio.volume.effects";
constexpr std::string_view SESSION_SAMPLE_RATE = "audio.session.sample_rate";
/// One SetBusUserGain record per mix role.
constexpr usize MIX_ROLE_COUNT = 3;
/// The mix roles plus the session sample rate.
constexpr usize AUDIO_SETTING_COUNT = MIX_ROLE_COUNT + 1;

[[nodiscard]] Descriptor MakeVolume(std::string_view name, std::string_view help) noexcept
{
    return
    {
        .Name = name,
        .Default = Value::FromFloat32(1.0F),
        .Help = help,
        .Units = "amplitude",
        .Choices = {},
        .MinReal = 0.0,
        .MaxReal = 1.0,
        .Group = AUDIO_MIX_GROUP,
        .Application = Apply::Live,
        .Persistent = true,
    };
}

[[nodiscard]] Status ReadActive(const Context& context, std::string_view name, Type kind, Value& outValue) noexcept
{
    Binding binding;
    Explanation explanation;
    if (context.Bind(name, binding) != Status::Ok || context.Explain(binding, explanation) != Status::Ok ||
        explanation.Active.Kind != kind)
    {
        return Status::InvalidSchema;
    }
    outValue = explanation.Active;
    return Status::Ok;
}

[[nodiscard]] bool IsBusUsable(const AudioSystem& system, uint32 bus) noexcept
{
    BusMeter meter;
    return bus == NO_BUS || system.GetBusMeter(bus, meter) == audio::Status::Ok;
}
} // namespace

std::span<const Descriptor> AudioSchema() noexcept
{
    static const foundation::StaticArray<Descriptor, AUDIO_SETTING_COUNT> SCHEMA{
        MakeVolume(MASTER_VOLUME, "User volume for the master bus; zero mutes. Applies live."),
        MakeVolume(MUSIC_VOLUME, "User volume for the music bus; zero mutes. Applies live."),
        MakeVolume(EFFECTS_VOLUME, "User volume for the effects bus; zero mutes. Applies live."),
        Descriptor
        {
            .Name = SESSION_SAMPLE_RATE,
            .Default = Value::FromUint32(DEFAULT_SAMPLE_RATE),
            .Help = "Audio session sample rate. Takes effect when the audio session is recreated.",
            .Units = "Hz",
            .Choices = {},
            .MinUnsigned = MIN_SESSION_SAMPLE_RATE,
            .MaxUnsigned = MAX_SESSION_SAMPLE_RATE,
            .Group = AUDIO_SESSION_GROUP,
            .Application = Apply::RestartSession,
            .Persistent = true,
        },
    };
    return {SCHEMA.GetData(), SCHEMA.GetSize()};
}

Status ReadAudioMixOptions(const Context& context, AudioMixOptions& outOptions) noexcept
{
    Value master;
    Value music;
    Value effects;
    if (ReadActive(context, MASTER_VOLUME, Type::Float32, master) != Status::Ok ||
        ReadActive(context, MUSIC_VOLUME, Type::Float32, music) != Status::Ok ||
        ReadActive(context, EFFECTS_VOLUME, Type::Float32, effects) != Status::Ok)
    {
        return Status::InvalidSchema;
    }
    outOptions =
    {
        .MasterVolume = static_cast<float32>(master.Real),
        .MusicVolume = static_cast<float32>(music.Real),
        .EffectsVolume = static_cast<float32>(effects.Real),
    };
    return Status::Ok;
}

Status ReadAudioSessionOptions(const Context& context, AudioSessionOptions& outOptions) noexcept
{
    Value rate;
    if (ReadActive(context, SESSION_SAMPLE_RATE, Type::Uint32, rate) != Status::Ok)
    {
        return Status::InvalidSchema;
    }
    outOptions = { .SampleRate = static_cast<uint32>(rate.Unsigned) };
    return Status::Ok;
}

std::string_view MixStatusName(MixStatus status) noexcept
{
    switch (status)
    {
        case MixStatus::Ok:
            return "Ok";
        case MixStatus::PendingRestart:
            return "PendingRestart";
        case MixStatus::InvalidState:
            return "InvalidState";
        case MixStatus::InvalidBus:
            return "InvalidBus";
        case MixStatus::AudioNotReady:
            return "AudioNotReady";
        case MixStatus::QueueFull:
            return "QueueFull";
        case MixStatus::AwaitingAcknowledgement:
            return "AwaitingAcknowledgement";
        case MixStatus::ConfigurationFailed:
            return "ConfigurationFailed";
    }
    return "Unknown";
}

MixStatus
AudioMixOwner::Initialize(Context& inoutContext, AudioSystem& inoutSystem, const AudioMixBuses& buses) noexcept
{
    if (mContextOrNull != nullptr)
    {
        return MixStatus::InvalidState;
    }
    Binding master;
    Binding music;
    Binding effects;
    AudioMixOptions active;
    if (inoutContext.Bind(MASTER_VOLUME, master) != Status::Ok ||
        inoutContext.Bind(MUSIC_VOLUME, music) != Status::Ok ||
        inoutContext.Bind(EFFECTS_VOLUME, effects) != Status::Ok ||
        ReadAudioMixOptions(inoutContext, active) != Status::Ok)
    {
        return MixStatus::InvalidState;
    }
    const bool hasRole = buses.Master != NO_BUS || buses.Music != NO_BUS || buses.Effects != NO_BUS;
    if (hasRole == false || IsBusUsable(inoutSystem, buses.Master) == false ||
        IsBusUsable(inoutSystem, buses.Music) == false || IsBusUsable(inoutSystem, buses.Effects) == false)
    {
        return MixStatus::InvalidBus;
    }
    Explanation explanation;
    if (inoutContext.Explain(master, explanation) != Status::Ok)
    {
        return MixStatus::InvalidState;
    }
    mContextOrNull = &inoutContext;
    mSystemOrNull = &inoutSystem;
    mBuses = buses;
    const MixStatus ready = checkPublication(buildCommands(active, nullptr));
    if (ready != MixStatus::Ok || submit(active) == false)
    {
        mContextOrNull = nullptr;
        mSystemOrNull = nullptr;
        return ready != MixStatus::Ok ? ready : MixStatus::QueueFull;
    }
    mMaster = master;
    mMusic = music;
    mEffects = effects;
    mPublished = { .Options = active, .Generation = explanation.GroupGeneration };
    return MixStatus::Ok;
}

MixStatus AudioMixOwner::Apply(uint64 expectedRevision, Diagnostic& outError) noexcept
{
    outError = {};
    if (mContextOrNull == nullptr)
    {
        return MixStatus::InvalidState;
    }
    AudioMixOptions candidate;
    const MixStatus read = readCandidate(candidate);
    if (read != MixStatus::Ok)
    {
        return read;
    }
    const bool isMixChanged = (candidate == mPublished.Options) == false;
    if (isMixChanged == true)
    {
        // Every fallible step precedes Commit, so a failure here leaves all published state intact.
        if (mIsAwaitingAcknowledgement == true || mShouldRetrySubmission == true)
        {
            return reject(MixStatus::AwaitingAcknowledgement);
        }
        const MixStatus ready = checkPublication(buildCommands(candidate, nullptr));
        if (ready != MixStatus::Ok)
        {
            return reject(ready);
        }
    }
    const Status committed = mContextOrNull->Commit(expectedRevision, outError);
    if (committed != Status::Ok && committed != Status::PendingRestart)
    {
        mContextOrNull->Discard();
        return MixStatus::ConfigurationFailed;
    }
    if (isMixChanged == true)
    {
        Explanation explanation;
        const Status explained = mContextOrNull->Explain(mMaster, explanation);
        LUDUS_ASSERT(explained == Status::Ok, "A bound audio setting must remain explainable");
        (void)explained;
        mPublished = { .Options = candidate, .Generation = explanation.GroupGeneration };
        // The room and state were reserved above; the owner thread published nothing in between.
        mShouldRetrySubmission = submit(candidate) == false;
    }
    return committed == Status::PendingRestart ? MixStatus::PendingRestart : MixStatus::Ok;
}

void AudioMixOwner::Service() noexcept
{
    if (mSystemOrNull == nullptr)
    {
        return;
    }
    if (mShouldRetrySubmission == true)
    {
        mShouldRetrySubmission = submit(mPublished.Options) == false;
        return;
    }
    if (mIsAwaitingAcknowledgement == true && mSystemOrNull->GetAppliedSequence() >= mPublishedSequence)
    {
        // The renderer now uses mPublished, so the superseded snapshot retires here.
        mApplied = mPublished;
        mHasAcknowledgedMix = true;
        mIsAwaitingAcknowledgement = false;
    }
}

AudioMixReport AudioMixOwner::GetReport() const noexcept
{
    return
    {
        .Applied = mApplied.Options,
        .AppliedGeneration = mApplied.Generation,
        .Published = mPublished.Options,
        .PublishedGeneration = mPublished.Generation,
        .HasAcknowledgedMix = mHasAcknowledgedMix,
        .IsAwaitingAcknowledgement = mIsAwaitingAcknowledgement || mShouldRetrySubmission,
    };
}

MixStatus AudioMixOwner::readCandidate(AudioMixOptions& outOptions) const noexcept
{
    Value master;
    Value music;
    Value effects;
    if (mContextOrNull->ReadPrepared(mMaster, master) != Status::Ok ||
        mContextOrNull->ReadPrepared(mMusic, music) != Status::Ok ||
        mContextOrNull->ReadPrepared(mEffects, effects) != Status::Ok)
    {
        return MixStatus::InvalidState;
    }
    outOptions =
    {
        .MasterVolume = static_cast<float32>(master.Real),
        .MusicVolume = static_cast<float32>(music.Real),
        .EffectsVolume = static_cast<float32>(effects.Real),
    };
    return MixStatus::Ok;
}

MixStatus AudioMixOwner::checkPublication(uint32 records) const noexcept
{
    if (mSystemOrNull->GetState() != SystemState::Ready)
    {
        return MixStatus::AudioNotReady;
    }
    return mSystemOrNull->GetCommandRoom() < records ? MixStatus::QueueFull : MixStatus::Ok;
}

uint32 AudioMixOwner::buildCommands(const AudioMixOptions& options, Command* outCommands) const noexcept
{
    const uint32 buses[MIX_ROLE_COUNT]{mBuses.Master, mBuses.Music, mBuses.Effects};
    const float32 gains[MIX_ROLE_COUNT]{options.MasterVolume, options.MusicVolume, options.EffectsVolume};
    uint32 count = 0;
    for (usize role = 0; role < MIX_ROLE_COUNT; ++role)
    {
        if (buses[role] == NO_BUS)
        {
            continue;
        }
        if (outCommands != nullptr)
        {
            outCommands[count].Kind = CommandKind::SetBusUserGain;
            outCommands[count].BusIndex = buses[role];
            outCommands[count].BusGain = gains[role];
        }
        ++count;
    }
    return count;
}

MixStatus AudioMixOwner::reject(MixStatus status) noexcept
{
    mContextOrNull->Discard();
    return status;
}

bool AudioMixOwner::submit(const AudioMixOptions& options) noexcept
{
    Command commands[MIX_ROLE_COUNT]{};
    const uint32 count = buildCommands(options, commands);
    const BatchResult result = mSystemOrNull->TrySubmitBatch({commands, count});
    if (result.Result != audio::Status::Ok)
    {
        return false;
    }
    mPublishedSequence = result.Sequence;
    mIsAwaitingAcknowledgement = true;
    return true;
}
} // namespace ludus::audio::configuration
