#pragma once

// Module-owned audio configuration schema and its first Live owner. See
// docs/architecture/engine-configuration.md#audio-configuration for the
// apply, acknowledgement and retirement contract.
#include <ludus/foundation/base/types.h>

#include <ludus/audio/audio_system.h>
#include <ludus/audio/audio_types.h>
#include <ludus/foundation/config/config.hpp>

#include <span>
#include <string_view>

namespace ludus::audio::configuration
{
using foundation::float32;
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;

/// Identity of the audio schema; bump it for incompatible names, types, ranges or semantics.
inline constexpr std::string_view AUDIO_SCHEMA = "ludus.audio.v1";
/// Live apply group holding the user mix volumes.
inline constexpr uint32 AUDIO_MIX_GROUP = 1;
/// RestartSession apply group holding audio session construction settings.
inline constexpr uint32 AUDIO_SESSION_GROUP = 2;
/// Lowest session sample rate the schema admits, in hertz.
inline constexpr uint32 MIN_SESSION_SAMPLE_RATE = 8000;
/// Highest session sample rate the schema admits, in hertz.
inline constexpr uint32 MAX_SESSION_SAMPLE_RATE = 192000;
/// Marks a mix role that the application's bus tree does not provide.
inline constexpr uint32 NO_BUS = kNoParent;

/// @brief The audio descriptors: `audio.volume.master`, `audio.volume.music` and `audio.volume.effects` (Live,
/// AUDIO_MIX_GROUP) and `audio.session.sample_rate` (RestartSession, AUDIO_SESSION_GROUP).
///
/// Volumes are float32 user gain amplitudes in [0, 1]; zero mutes exactly. Every setting may be persisted as a
/// preference. The returned span and its strings are static and outlive every Context.
[[nodiscard]] std::span<const foundation::config::Descriptor> AudioSchema() noexcept;

/// Plain user mix volumes copied out of a configuration context.
struct AudioMixOptions final
{
    float32 MasterVolume = 1.0F;  ///< User gain amplitude for the master role, [0, 1].
    float32 MusicVolume = 1.0F;   ///< User gain amplitude for the music role, [0, 1].
    float32 EffectsVolume = 1.0F; ///< User gain amplitude for the effects role, [0, 1].
    /// Exact per-role comparison; the owner publishes only when a volume differs.
    friend bool operator==(const AudioMixOptions&, const AudioMixOptions&) noexcept = default;
};

/// Plain options needed to construct an audio session.
struct AudioSessionOptions final
{
    uint32 SampleRate = DEFAULT_SAMPLE_RATE; ///< Requested session sample rate, in hertz.
};

/// @brief Copy the active mix volumes from a context initialized with AudioSchema.
///
/// @return Ok, or InvalidSchema when the context does not hold the audio descriptors. Preserves `outOptions` on
/// failure.
[[nodiscard]] foundation::config::Status ReadAudioMixOptions(const foundation::config::Context& context,
                                                             AudioMixOptions& outOptions) noexcept;

/// @brief Copy the active session options from a context initialized with AudioSchema.
///
/// Pending restart values stay requested until a fresh context is built for the next session, so this returns the
/// values the current session was constructed with.
/// @return Ok, or InvalidSchema when the context does not hold the audio descriptors. Preserves `outOptions` on
/// failure.
[[nodiscard]] foundation::config::Status ReadAudioSessionOptions(const foundation::config::Context& context,
                                                                 AudioSessionOptions& outOptions) noexcept;

/// Which configured bus receives each mix role's user gain; NO_BUS leaves a role unapplied.
struct AudioMixBuses final
{
    uint32 Master = NO_BUS;  ///< Index into SystemConfig::Buses for `audio.volume.master`.
    uint32 Music = NO_BUS;   ///< Index into SystemConfig::Buses for `audio.volume.music`.
    uint32 Effects = NO_BUS; ///< Index into SystemConfig::Buses for `audio.volume.effects`.
};

/// Outcome of an AudioMixOwner operation.
enum class MixStatus : uint8
{
    Ok,                      ///< Committed; any changed mix was published and awaits acknowledgement.
    PendingRestart,          ///< Committed restart-only values; the running session is unchanged.
    InvalidState,            ///< Owner not initialized, already initialized, or no prepared transaction.
    InvalidBus,              ///< A mapped bus index is not part of the audio session.
    AudioNotReady,           ///< The audio system is not Ready; the candidate was discarded.
    QueueFull,               ///< No renderer command room; the candidate was discarded.
    AwaitingAcknowledgement, ///< The previous mix is not acknowledged yet; the candidate was discarded.
    ConfigurationFailed,     ///< The context rejected the transaction; see the diagnostic.
};

/// Stable display name for a MixStatus value.
[[nodiscard]] std::string_view MixStatusName(MixStatus status) noexcept;

/// @brief What the owner has published and what the renderer has acknowledged.
///
/// Generations are the context's AUDIO_MIX_GROUP generation for each snapshot. Applied is meaningful only when
/// HasAcknowledgedMix is true.
struct AudioMixReport final
{
    AudioMixOptions Applied{};              ///< Mix the renderer has acknowledged.
    uint64 AppliedGeneration = 0;           ///< Mix group generation of Applied.
    AudioMixOptions Published{};            ///< Newest mix handed to the renderer.
    uint64 PublishedGeneration = 0;         ///< Mix group generation of Published.
    bool HasAcknowledgedMix = false;        ///< True once the renderer acknowledged any snapshot.
    bool IsAwaitingAcknowledgement = false; ///< True while Published has not been acknowledged.
};

/// @brief Live owner of AUDIO_MIX_GROUP for one AudioSystem session.
///
/// Apply runs at an owner-selected safe point: it reads the context's prepared mix, reserves renderer command
/// room, commits the context and publishes the user gains as one batch. The previous snapshot stays the applied
/// mix until Service observes the renderer's acknowledgement, then it is retired. One publication may await
/// acknowledgement at a time, which bounds retained snapshots to two. Every failure before publication discards
/// the context candidate and preserves the context, the applied mix and the renderer's bus gains.
///
/// Single owner thread, shared with the AudioSystem control owner. The context and system are borrowed and must
/// outlive the owner. Allocates nothing.
class AudioMixOwner final
{
public:
    /// An uninitialized owner; call Initialize before Apply or Service.
    AudioMixOwner() noexcept = default;
    /// Not copyable: the owner borrows one context and session and tracks one in-flight publication.
    AudioMixOwner(const AudioMixOwner&) = delete;
    /// Not copy-assignable, for the same single-ownership reason as the deleted copy constructor.
    AudioMixOwner& operator=(const AudioMixOwner&) = delete;

    /// @brief Bind the mix settings, validate `buses` against the session and publish the context's active mix.
    ///
    /// Call after the startup layers are committed and before Context::SealStartup.
    /// @param inoutContext Context initialized with AudioSchema; borrowed for the owner's lifetime.
    /// @param inoutSystem Ready AudioSystem whose bus tree `buses` indexes; borrowed for the owner's lifetime.
    /// @param buses Bus index per mix role; at least one role must be mapped.
    /// @return Ok with the startup mix awaiting acknowledgement; InvalidState when already initialized or the
    /// context lacks the audio schema; InvalidBus; AudioNotReady; or QueueFull. Failure leaves the owner
    /// uninitialized.
    [[nodiscard]] MixStatus Initialize(foundation::config::Context& inoutContext,
                                       AudioSystem& inoutSystem,
                                       const AudioMixBuses& buses) noexcept;

    /// @brief Commit the context's prepared transaction at this safe point and publish a changed mix.
    ///
    /// The caller prepares the transaction with Context::Prepare. A transaction that leaves the active mix
    /// unchanged commits without publication; restart-only values return PendingRestart.
    /// @param expectedRevision Context revision captured before Prepare.
    /// @param outError Receives the context diagnostic when ConfigurationFailed is returned.
    /// @return Ok, PendingRestart, or a failure status. Every failure discards the candidate; the context, the
    /// published and applied mix and the renderer's gains are unchanged.
    [[nodiscard]] MixStatus Apply(uint64 expectedRevision, foundation::config::Diagnostic& outError) noexcept;

    /// @brief Observe the renderer's acknowledgement and retire the superseded snapshot.
    ///
    /// Call from the owner's service loop after AudioSystem::Service. Never blocks or allocates.
    void Service() noexcept;

    /// @brief Snapshot of the published and acknowledged mix; Applied reports only what the renderer acknowledged.
    [[nodiscard]] AudioMixReport GetReport() const noexcept;

    /// True after a successful Initialize.
    [[nodiscard]] bool IsInitialized() const noexcept
    {
        return mContextOrNull != nullptr;
    }

private:
    /// One retained mix: the volumes handed to the renderer and the context generation they came from.
    struct Snapshot final
    {
        AudioMixOptions Options{}; ///< Volumes copied out of the context at publication.
        uint64 Generation = 0;     ///< AUDIO_MIX_GROUP generation those volumes were committed under.
    };
    [[nodiscard]] MixStatus readCandidate(AudioMixOptions& outOptions) const noexcept;
    [[nodiscard]] MixStatus checkPublication(uint32 records) const noexcept;
    [[nodiscard]] uint32 buildCommands(const AudioMixOptions& options, Command* outCommands) const noexcept;
    [[nodiscard]] MixStatus reject(MixStatus status) noexcept;
    [[nodiscard]] bool submit(const AudioMixOptions& options) noexcept;

    foundation::config::Context* mContextOrNull = nullptr;
    AudioSystem* mSystemOrNull = nullptr;
    AudioMixBuses mBuses{};
    foundation::config::Binding mMaster{};
    foundation::config::Binding mMusic{};
    foundation::config::Binding mEffects{};
    Snapshot mApplied{};
    Snapshot mPublished{};
    uint64 mPublishedSequence = 0;
    bool mHasAcknowledgedMix = false;
    bool mIsAwaitingAcknowledgement = false;
    bool mShouldRetrySubmission = false;
};
} // namespace ludus::audio::configuration
