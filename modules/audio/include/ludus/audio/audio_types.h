#pragma once

// Public vocabulary for the Ludus audio system, per
// .kiro/specs/audio/design.md (sections 3-12) and requirements AU01-AU17.
//
// This header is intentionally lightweight: plain enums over fixed-width base
// types, trivially-copyable records, and pointer+length views. It pulls in no
// container, string, or heavy STL header, and names no miniaudio / Web Audio
// type. Every public fallible call (declared in audio_system.h) returns a
// Status and is noexcept.

#include <ludus/foundation/base/types.h>

#include <span>

namespace ludus::audio
{
using ludus::foundation::float32;
using ludus::foundation::float64;
using ludus::foundation::int32;
using ludus::foundation::uint16;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::uint8;
using ludus::foundation::usize;

// ---------------------------------------------------------------------------
// Default planning limits (requirements "Default planning limits").
// These are engineering starting points, frozen in A0; not performance results.
// Configure capacities at startup within these implementation maxima. Rendering
// never grows beyond them.
// ---------------------------------------------------------------------------
inline constexpr usize LOGICAL_VOICE_CAPACITY = 256; ///< resident logical slots
inline constexpr usize MIXED_VOICE_CAPACITY = 64;    ///< total mixed incl. tails + streams
inline constexpr usize RESIDENT_STEADY_SELECT = 56;  ///< steady-state resident selection ceiling
inline constexpr usize STREAM_CAPACITY = 4;          ///< protected stream instances
inline constexpr usize GROUP_CAPACITY = 16;          ///< resident concurrency groups
inline constexpr usize BUS_CAPACITY = 16;            ///< buses in the static tree
inline constexpr usize MODIFIER_CAPACITY = 8;        ///< active attenuation modifier instances
inline constexpr usize COMMAND_QUEUE_CAPACITY = 512; ///< queued command records (SPSC ring)
inline constexpr usize MAX_BATCH_RECORDS = 32;       ///< records per TrySubmitBatch
inline constexpr usize CONTROL_BOUNDARY_BUDGET = 64; ///< applied commands per 128-frame quantum
inline constexpr usize STREAM_RING_FRAMES = 32768;   ///< stereo frames per stream ring (8 x 4096)
inline constexpr usize STREAM_CHUNK_FRAMES = 4096;   ///< frames per stream chunk
inline constexpr usize STREAM_CHUNK_COUNT = 8;       ///< chunks per stream ring

// Fixed timeline / scheduling constants.
inline constexpr uint32 CONTROL_QUANTUM_FRAMES = 128;       ///< control boundary granularity
inline constexpr uint32 DEFAULT_SAMPLE_RATE = 48000;        ///< typical session rate
inline constexpr uint32 DEFAULT_DEVICE_PERIOD_FRAMES = 256; ///< native request hint
inline constexpr uint32 DEFAULT_RAMP_MS = 5;                ///< default transition duration
inline constexpr uint32 MAX_SCHEDULE_HORIZON_SECONDS = 2;   ///< StartFrame lookahead bound
inline constexpr uint32 COLD_DECODE_UNIT_FRAMES = 4096;     ///< yield granularity for cold worker

// Priority range (0..7, higher is more important).
inline constexpr uint8 PRIORITY_MIN = 0;
inline constexpr uint8 PRIORITY_MAX = 7;

// Playback rate range for resident voices (duration+pitch together).
inline constexpr float32 RATE_MIN = 0.5F;
inline constexpr float32 RATE_MAX = 2.0F;

// Group quota ranges.
inline constexpr uint32 GROUP_MAX_ADMITTED_LIMIT = 256;
inline constexpr uint32 GROUP_MAX_SELECTED_LIMIT = 56;

// Default user-gain slider dB range (section 8 "User volume controls").
inline constexpr float32 DEFAULT_USER_GAIN_DB_RANGE = 40.0F;

// Modifier dB clamp window (section 8).
inline constexpr float32 MODIFIER_DB_MIN = -96.0F;
inline constexpr float32 MODIFIER_DB_MAX = 0.0F;

// Memory accounting ceilings (section 12). Reported, enforced; excluded bytes
// (third-party heaps/stacks) are reported separately.
inline constexpr uint64 RESIDENT_PCM_CAP_BYTES = 64ULL * 1024 * 1024;
inline constexpr uint64 BROWSER_ENCODED_CAP_BYTES = 32ULL * 1024 * 1024;
inline constexpr uint64 STREAM_RING_TOTAL_BYTES = 1ULL * 1024 * 1024;
inline constexpr uint64 CORE_RUNTIME_CAP_BYTES = 2ULL * 1024 * 1024;

// ---------------------------------------------------------------------------
/// @brief Status: the single bounded result domain for public fallible calls.
///
/// Backend/library detail is mapped into these codes; an invalid handle is not
/// overloaded with every failure reason (design section 3).
// ---------------------------------------------------------------------------
enum class Status : uint8
{
    Ok,
    Pending,     // staged/async; never immediately audible
    Disabled,    // system is in Disabled mode
    Suspended,   // device transport frozen (e.g. browser not resumed)
    NotReady,    // asset/system not prepared for this operation
    Unsupported, // capability/format/layout not supported here
    InvalidArgument,
    InvalidHandle,
    QueueFull,
    VoiceCapacity,
    GroupCapacity,
    StreamCapacity,
    AssetCapacity,
    OutOfMemory,
    DecodeError,
    IoError,
    DeviceError,
    SequenceExhausted, // session/generation/epoch space exhausted
    TooLate,           ///< Renderer sealed the transition before the requested edit.
};

[[nodiscard]] constexpr bool IsOk(Status status) noexcept
{
    return status == Status::Ok;
}

/// Stable, bounded, allocation-free diagnostic name. Returns a static string.
[[nodiscard]] const char* ToString(Status status) noexcept;

// ---------------------------------------------------------------------------
// System lifecycle mode and reported state (design sections 3, 10).
// ---------------------------------------------------------------------------
enum class Mode : uint8
{
    Device,   // drive a real output device (native/browser)
    Offline,  // only advances when RenderOffline is called
    Disabled, // no clock, no phantom voices
};

enum class SystemState : uint8
{
    Starting,
    Ready, // not a gesture-unlocked guarantee on the browser
    Suspended,
    Disabled,
    Stopping,
    Failed,
};

// ---------------------------------------------------------------------------
/// @brief Handles.
///
/// Each is distinct and carries Session/Slot/Generation. Zero session or generation is invalid.
/// Fabricated/stale/cross-instance handles cannot index unchecked memory (design section 3).
// ---------------------------------------------------------------------------
struct ClipHandle final
{
    uint32 Session = 0;
    uint32 Slot = 0;
    uint32 Generation = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return Session != 0 && Generation != 0;
    }
    [[nodiscard]] constexpr bool operator==(const ClipHandle&) const noexcept = default;
};

struct StreamHandle final
{
    uint32 Session = 0;
    uint32 Slot = 0;
    uint32 Generation = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return Session != 0 && Generation != 0;
    }
    [[nodiscard]] constexpr bool operator==(const StreamHandle&) const noexcept = default;
};

struct VoiceHandle final
{
    uint32 Session = 0;
    uint32 Slot = 0;
    uint32 Generation = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return Session != 0 && Generation != 0;
    }
    [[nodiscard]] constexpr bool operator==(const VoiceHandle&) const noexcept = default;
};

struct ModifierHandle final
{
    uint32 Session = 0;
    uint32 Slot = 0;
    uint32 Generation = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return Session != 0 && Generation != 0;
    }
    [[nodiscard]] constexpr bool operator==(const ModifierHandle&) const noexcept = default;
};

// ---------------------------------------------------------------------------
/// @brief Channel / buffer contracts (design section 5).
///
/// Channel meaning, layout, frame count and writable sample capacity are named explicitly. A frame holds one sample
/// per channel; SampleValues = Frames * Channels (checked usize).
// ---------------------------------------------------------------------------
enum class ChannelLayout : uint8
{
    Mono,
    Stereo,
};

enum class BufferLayout : uint8
{
    Interleaved, // L R L R ... (native/offline)
    Planar,      // per-channel contiguous (worklet)
};

[[nodiscard]] constexpr uint32 ChannelCount(ChannelLayout layout) noexcept
{
    return layout == ChannelLayout::Stereo ? 2U : 1U;
}

/// @brief Supported prepared source formats (design section 9).
///
/// Everything else is rejected with Status::Unsupported.
enum class SourceFormat : uint8
{
    Wav,
    Flac,
};

// ---------------------------------------------------------------------------
/// @brief Spatial vocabulary (design section 6).
///
/// Coordinates are meters; the default pose is +X right, +Y up, Forward = -Z.
// ---------------------------------------------------------------------------
struct AudioVec3 final
{
    float32 X = 0.0F;
    float32 Y = 0.0F;
    float32 Z = 0.0F;
};

/// Which position drives distance attenuation for a positional voice.
enum class AttenuationOrigin : uint8
{
    Listener,        // the listener's AttenuationPosition
    PanningPosition, // camera-relative exception (footsteps, etc.)
};

struct ListenerPose final
{
    AudioVec3 PanningPosition{};     ///< drives stereo pan
    AudioVec3 AttenuationPosition{}; ///< drives distance attenuation (defaults equal)
    AudioVec3 Forward{0.0F, 0.0F, -1.0F};
    AudioVec3 Up{0.0F, 1.0F, 0.0F};
};

// ---------------------------------------------------------------------------
// Voice policy and lifecycle (design sections 3, 7).
// ---------------------------------------------------------------------------
enum class VirtualPolicy : uint8
{
    AdvanceWhenVirtual, // advance fractional cursor without DSP; loops wrap
    KillWhenInaudible,  // fade existing output then terminate
};

enum class VoiceState : uint8
{
    Pending,      // accepted Play not yet consumed at a control boundary
    Scheduled,    // consumed; waiting for StartFrame
    Mixed,        // physically mixed (includes start/reentry fade-in)
    Virtualizing, // fading toward Virtual (reversible)
    Virtual,      // advancing without DSP
    Stopping,     // irreversible output fade toward Terminal
    Terminal,     // owner may acknowledge and reclaim
};

enum class PreparationState : uint8
{
    Preparing,
    Ready,
    Failed,
    Retiring,
    Reclaimed,
};

/// Numeric cause attached to every voice state change (design sections 3, 11).
enum class StateCause : uint8
{
    None,
    PlayConsumed,
    StartFrameReached,
    SelectedAudible,
    LostSelectionGroupQuota,
    LostSelectionGlobalBudget,
    LostSelectionAudibility,
    ReselectedReversed,
    HigherPriorityDisplaced,
    StopRequested,
    StopAllEpoch,
    NaturalEof,
    Expired, // finite advancing voice reached EOF while virtual
    FadeComplete,
    PreparationFailed,
    Cancelled,
    StreamError,
};

/// Terminal reason recorded in the durable mailbox (design section 4).
enum class TerminalReason : uint8
{
    None,
    Completed, // natural EOF after any stop envelope
    Stopped,   // explicit Stop/StopAll
    Cancelled, // pending play cancelled before start
    Expired,   // finite virtual voice reached EOF
    Killed,    // kill policy / displacement
    DecodeFailed,
    StreamError,
};

/// @brief Independent silence cause flags (design section 11).
///
/// Multiple may coexist, so this is a bitset, not a single enum. The current flags fit in 16 bits; the bitwise
/// operators widen to uint32 for safe shifting.
enum class SilenceCause : uint16
{
    None = 0,
    NotStarted = 1U << 0,
    DistanceZero = 1U << 1,
    UserMuted = 1U << 2,
    BelowThreshold = 1U << 3,
    GroupQuota = 1U << 4,
    GlobalBudget = 1U << 5,
    WaitingForFadeSlot = 1U << 6,
    Stopping = 1U << 7,
    StreamStarved = 1U << 8,
};

[[nodiscard]] constexpr SilenceCause operator|(SilenceCause a, SilenceCause b) noexcept
{
    return static_cast<SilenceCause>(static_cast<uint32>(a) | static_cast<uint32>(b));
}
[[nodiscard]] constexpr SilenceCause operator&(SilenceCause a, SilenceCause b) noexcept
{
    return static_cast<SilenceCause>(static_cast<uint32>(a) & static_cast<uint32>(b));
}
constexpr SilenceCause& operator|=(SilenceCause& a, SilenceCause b) noexcept
{
    a = a | b;
    return a;
}
[[nodiscard]] constexpr bool HasCause(SilenceCause set, SilenceCause flag) noexcept
{
    return static_cast<uint32>(set & flag) != 0;
}

// ---------------------------------------------------------------------------
// Configuration records (design sections 3, 6-10).
// ---------------------------------------------------------------------------

// One bus in the static tree. ParentIndex == kNoParent marks the root.
inline constexpr uint32 kNoParent = 0xFFFFFFFFU;

struct BusConfig final
{
    uint32 Id = 0;                  ///< application-chosen stable id
    uint32 ParentIndex = kNoParent; ///< index into the configured bus array
    float32 BaseGain = 1.0F;        ///< [0,1] amplitude
    float32 UserGain = 1.0F;        ///< [0,1] amplitude, user-owned
};

/// One resident concurrency group. Default group 0 allows 256/56.
struct GroupConfig final
{
    uint32 MaxAdmitted = GROUP_MAX_ADMITTED_LIMIT; ///< 1..256
    uint32 MaxSelected = GROUP_MAX_SELECTED_LIMIT; ///< 1..56
};

/// Session configuration passed to Initialize.
struct SystemConfig final
{
    Mode SystemMode = Mode::Offline;
    uint32 SampleRate = DEFAULT_SAMPLE_RATE;                  ///< session rate (device may resolve another)
    uint32 DevicePeriodFrames = DEFAULT_DEVICE_PERIOD_FRAMES; ///< hint only

    /// @brief Static bus tree (validated before rendering).
    ///
    /// An empty span installs a single implicit root bus.
    std::span<const BusConfig> Buses;

    /// @brief Resident concurrency groups.
    ///
    /// Index 0 is the default group; an empty span installs one default group.
    std::span<const GroupConfig> Groups;

    /// @brief Capacity overrides (0 = use the planning default).
    ///
    /// Rejected if above the implementation maximum or if a product overflows.
    uint32 LogicalVoiceCapacity = 0;
    uint32 MixedVoiceCapacity = 0;
    uint32 StreamInstanceCapacity = 0;
};

/// Resident clip preparation descriptor (design section 5/6).
struct ClipDescriptor final
{
    SourceFormat Format = SourceFormat::Wav;
    /// @brief Optional source-rate loop metadata; converted to prepared points during preparation.
    ///
    /// A zero LoopEnd means "no loop metadata".
    uint64 SourceLoopBegin = 0;
    uint64 SourceLoopEnd = 0;
    /// Optional debug asset hash (owner-side metadata only).
    uint64 AssetHash = 0;
};

struct StreamDescriptor final
{
    SourceFormat Format = SourceFormat::Wav;
    uint64 SourceLoopBegin = 0;
    uint64 SourceLoopEnd = 0;
    bool Looping = false;
    uint64 AssetHash = 0;
};

/// @brief A single Play command's parameters (design sections 5-7).
///
/// All numeric; no pointers, closures or transient views.
struct PlayParams final
{
    ClipHandle Clip{};
    uint32 BusIndex = 0;   ///< index into the configured bus array
    uint32 GroupIndex = 0; ///< index into the configured group array
    uint8 Priority = PRIORITY_MIN;
    float32 Gain = 1.0F; ///< [0,1] linear
    float32 Rate = 1.0F; ///< [RATE_MIN, RATE_MAX]
    bool Looping = false;
    VirtualPolicy Policy = VirtualPolicy::KillWhenInaudible;

    /// @brief Scheduling.
    ///
    /// StartFrame == 0 means "next applied boundary". Otherwise an absolute session frame no more than
    /// MAX_SCHEDULE_HORIZON_SECONDS ahead.
    uint64 StartFrame = 0;

    /// @brief Spatialization.
    ///
    /// When Positional is false the voice is nonspatial and uses ExplicitPan directly (mono) or a stereo-bed
    /// scalar gain.
    bool Positional = false;
    AudioVec3 EmitterPosition{};
    AttenuationOrigin Origin = AttenuationOrigin::Listener;
    float32 MinDistance = 1.0F;
    float32 MaxDistance = 100.0F;
    float32 ExplicitPan = 0.0F; ///< [-1,1] for nonspatial mono

    /// Owner-side numeric diagnostic tag (never interpreted by the renderer).
    uint32 PolicyTag = 0;
};

/// @brief A command record published in a batch (design section 4).
///
/// The kind selects which payload is meaningful. BatchLocalPlayIndex lets an Update/Stop target a Play reserved
/// earlier in the same batch before handles are finalized.
enum class CommandKind : uint8
{
    Play,
    UpdateVoice, // gain/rate/priority/position update
    StopVoice,
    SetListener,
    SetBusBaseGain, // part of a base snapshot batch
    SetBusUserGain,
    AddModifier,
    UpdateModifier,
    RemoveModifier,
};

struct VoiceUpdate final
{
    VoiceHandle Voice{};
    int32 BatchLocalPlayIndex = -1; ///< >= 0 targets a Play earlier in this batch
    bool SetGain = false;
    float32 Gain = 1.0F;
    bool SetRate = false;
    float32 Rate = 1.0F;
    bool SetPriority = false;
    uint8 Priority = PRIORITY_MIN;
    bool SetPosition = false;
    AudioVec3 EmitterPosition{};
};

struct ModifierValue final
{
    uint32 BusIndex = 0;
    float32 TargetDb = 0.0F; ///< [-96,0]
};

struct Command final
{
    CommandKind Kind = CommandKind::Play;

    // Play
    PlayParams Play{};

    // UpdateVoice
    VoiceUpdate Update{};

    // StopVoice
    VoiceHandle StopTarget{};
    int32 StopBatchLocalPlayIndex = -1;

    // SetListener
    ListenerPose Listener{};

    // SetBusBaseGain / SetBusUserGain
    uint32 BusIndex = 0;
    float32 BusGain = 1.0F;

    // AddModifier / UpdateModifier / RemoveModifier
    ModifierHandle Modifier{};
    float32 ModifierWeight = 1.0F;                 ///< [0,1]
    std::span<const ModifierValue> ModifierValues; ///< owned/copied on submit
};

// ---------------------------------------------------------------------------
// Query result records (design sections 3, 11).
// ---------------------------------------------------------------------------
struct VoiceInfo final
{
    VoiceHandle Voice{};
    VoiceState State = VoiceState::Terminal;
    StateCause LastCause = StateCause::None;
    TerminalReason Terminal = TerminalReason::None;
    SilenceCause Silence = SilenceCause::None;
    bool Stale = false;       ///< cached renderer view may be stale
    uint64 SnapshotFrame = 0; ///< RenderFrame at which the snapshot was taken
    uint64 CursorFrame = 0;   ///< source cursor (prepared-rate)
    uint64 HeldFrames = 0;    ///< Group frames withheld while any stem lacked samples.
    uint64 StartFrame = 0;
    float32 EffectiveGain = 0.0F;
    uint8 Priority = 0;
    uint32 GroupIndex = 0;
    uint32 BusIndex = 0;
    uint32 PolicyTag = 0;
    uint64 VirtualAgeFrames = 0;

    /// @brief Separate selection-score contributions (design section 7.2).
    ///
    /// Selection envelopes are intentionally excluded from the score.
    float32 ScoreVoiceGain = 0.0F;
    float32 ScorePreparedPeak = 0.0F;
    float32 ScoreDistanceGain = 0.0F;
    float32 ScoreBusChainGain = 0.0F;

    /// Spatial inputs retained for debugging (true emitter never rewritten).
    AudioVec3 EmitterPosition{};
    AudioVec3 PanningPosition{};
    AudioVec3 AttenuationPosition{};
    AttenuationOrigin Origin = AttenuationOrigin::Listener;
    float32 Pan = 0.0F;
};

struct GroupInfo final
{
    uint32 MaxAdmitted = 0;
    uint32 MaxSelected = 0;
    uint32 Admitted = 0; ///< charged logical reservations
    uint32 Selected = 0; ///< steady-state selected
    uint32 Fading = 0;   ///< Virtualizing/Stopping tails charged to this group
};

struct BusMeter final
{
    /// Per-channel, before this bus's gain (input tap) and after (post-gain tap).
    float32 InputPeakL = 0.0F;
    float32 InputPeakR = 0.0F;
    float32 InputRmsL = 0.0F;
    float32 InputRmsR = 0.0F;
    float32 PostPeakL = 0.0F;
    float32 PostPeakR = 0.0F;
    float32 PostRmsL = 0.0F;
    float32 PostRmsR = 0.0F;
    float32 CurrentGain = 0.0F;
    float32 TargetGain = 0.0F;
    uint32 ClippedFrames = 0;
};

struct SystemSnapshot final
{
    SystemState State = SystemState::Disabled;
    Mode SystemMode = Mode::Disabled;
    uint32 Session = 0;
    uint32 SampleRate = 0;
    uint64 RenderFrame = 0;

    uint32 QueueDepth = 0;
    uint32 QueueHighWater = 0;
    uint64 AcceptedCommands = 0;
    uint64 RejectedCommands = 0;
    uint64 StaleCommands = 0;

    uint32 PendingVoices = 0;
    uint32 MixedVoices = 0;
    uint32 VirtualVoices = 0;
    uint32 FadingVoices = 0;
    uint32 StreamVoices = 0;
    uint32 LogicalHighWater = 0;
    uint32 UnreclaimedTerminals = 0;

    uint64 StreamStarvations = 0;
    uint64 EofEvents = 0;
    uint64 Errors = 0;

    uint32 ResidentClips = 0;
    uint64 ResidentPcmBytes = 0;
    uint64 StreamEncodedBytes = 0;
    uint64 StreamRingBytes = 0;

    uint32 SnapshotLosses = 0; ///< debug ring publication losses

    uint64 PreClipFrames = 0; ///< frames summed above full scale before clamp
    uint64 NonFiniteFaults = 0;
};

} // namespace ludus::audio
