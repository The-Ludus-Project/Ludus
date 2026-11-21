#pragma once
#include <ludus/audio/content/definitions.h>

#include <ludus/foundation/containers/static_array.hpp>

namespace ludus::audio::content
{
/// Maximum distinct loopable sections in a version-two score.
inline constexpr usize MAX_MUSIC_SECTIONS = 16;
/// Maximum gameplay state names mapped by a cue.
inline constexpr usize MAX_MUSIC_STATES = 32;
/// Maximum authored directed edges between sections.
inline constexpr usize MAX_MUSIC_TRANSITIONS = 64;
/// Maximum resident stinger definitions in one cue.
inline constexpr usize MAX_MUSIC_STINGERS = 8;
/// Maximum ordered intensity/gain points per stem.
inline constexpr usize MAX_LAYER_POINTS = 8;

/// Absolute musical boundary used for a state change or stinger.
enum class MusicBoundary : uint8
{
    Beat, ///< One meter-unit note.
    Bar,  ///< One complete measure.
};

/// Linear gain at normalized gameplay intensity.
struct LayerPoint final
{
    float32 Intensity = 0; ///< Horizontal coordinate in [0,1].
    float32 Gain = 1;      ///< Linear amplitude in [0,1].
};
/// One aligned stream and its linearly interpolated gain envelope.
struct MusicLayer final
{
    ResourceId Source; ///< Catalog audio-source ID.
    ludus::foundation::StaticArray<LayerPoint, MAX_LAYER_POINTS>
        Points;           ///< Ascending intensity including both endpoints.
    usize PointCount = 0; ///< Valid prefix of Points.
};
/// A section starts at source frame zero and repeats its half-open loop interval.
struct MusicSection final
{
    ResourceId Id;                                                      ///< Cue-local section name.
    uint64 LoopBegin = 0;                                               ///< Inclusive source frame.
    uint64 LoopEnd = 0;                                                 ///< Exclusive source frame.
    ludus::foundation::StaticArray<MusicLayer, STREAM_CAPACITY> Layers; ///< Simultaneously consumed stems.
    usize LayerCount = 0;                                               ///< Valid prefix of Layers.
};
/// Mapping from a gameplay name to a local section.
struct MusicState final
{
    ResourceId Id;      ///< Unique gameplay state name.
    ResourceId Section; ///< Destination section ID.
};
/// A permitted directed section change.
struct MusicTransition final
{
    ResourceId From;                              ///< Outgoing section.
    ResourceId To;                                ///< Incoming section.
    MusicBoundary Quantize = MusicBoundary::Beat; ///< Eligible musical boundaries.
    uint32 FadeFrames = 0;                        ///< Crossfade duration in session frames, zero for a cut.
};
/// A resident one-shot eligible on a musical boundary.
struct MusicStinger final
{
    ResourceId Id;                                ///< Local trigger name.
    ResourceId Sound;                             ///< Catalog sound ID.
    MusicBoundary Quantize = MusicBoundary::Beat; ///< Eligible start boundary.
};
/// Version-two cue definition. Source metadata and routing require validation at acquisition.
struct MusicScore final
{
    ResourceId Id;               ///< Catalog cue ID.
    ResourceId Bus;              ///< Output bus name.
    ResourceId InitialState;     ///< Gameplay state at cue start.
    uint8 Priority = 0;          ///< Admission priority.
    float32 Gain = 1;            ///< Cue-wide linear gain.
    uint32 TempoNumerator = 0;   ///< Quarter notes per minute times TempoDenominator.
    uint32 TempoDenominator = 1; ///< Positive rational tempo divisor.
    uint32 MeterBeats = 0;       ///< Meter-unit notes per bar.
    uint32 MeterUnit = 0;        ///< Note denominator, e.g. 8 for eighth notes.
    uint32 LayerRampFrames = 0;  ///< Duration of intensity changes in session frames.
    uint32 MaxStingers = 0;      ///< Maximum simultaneously active resident stingers.
    ludus::foundation::StaticArray<MusicSection, MAX_MUSIC_SECTIONS> Sections;          ///< Authored loopable sections.
    usize SectionCount = 0;                                                             ///< Valid prefix of Sections.
    ludus::foundation::StaticArray<MusicState, MAX_MUSIC_STATES> States;                ///< Unique gameplay names.
    usize StateCount = 0;                                                               ///< Valid prefix of States.
    ludus::foundation::StaticArray<MusicTransition, MAX_MUSIC_TRANSITIONS> Transitions; ///< Allowed directed edges.
    usize TransitionCount = 0;                                                 ///< Valid prefix of Transitions.
    ludus::foundation::StaticArray<MusicStinger, MAX_MUSIC_STINGERS> Stingers; ///< Resident accent definitions.
    usize StingerCount = 0;                                                    ///< Valid prefix of Stingers.
};

/// Inspect only the JSON version tag for dispatch after v1 parsing fails. Full validation uses ReadMusicScore.
[[nodiscard]] bool IsDynamicMusicDocument(std::string_view json) noexcept;

/// Parse and validate a complete v2 score, including edge stream capacity. On failure
/// preserve the previous score and identify the rejected field in the diagnostic.
[[nodiscard]] ContentStatus
ReadMusicScore(std::string_view json, MusicScore& outScore, Diagnostic& outDiagnostic) noexcept;

/// Compute the first absolute session frame at or after requestedFrame on the selected
/// beat/bar grid. origin is the cue's first frame. Checked overflow preserves outFrame.
[[nodiscard]] bool QuantizeMusicFrame(const MusicScore& score,
                                      uint32 sampleRate,
                                      MusicBoundary boundary,
                                      uint64 origin,
                                      uint64 requestedFrame,
                                      uint64& outFrame) noexcept;
} // namespace ludus::audio::content
