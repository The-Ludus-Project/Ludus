#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/content/weather_format/weather_format.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/fixed_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/gameplay/weather/climate.h>

/// @file
/// Deterministic fixed-tick weather simulation (Ludus::GameplayWeather).
///
/// The simulation resolves the climate's schedules, feathered regional weights and gameplay overrides at
/// explicit World positions on the session's fixed ticks. Every authoritative quantity is an integer code
/// (Ludus::WeatherFormat), so native and browser builds produce bit-identical samples, events and saves.
/// Rendering and query order never change the state. See docs/modules/gameplay/weather.md and
/// docs/architecture/world-environment.md, "Authored climate and resolved weather".
namespace ludus::gameplay::weather
{
/// Most overrides active at once.
inline constexpr uint32 MAX_ACTIVE_WEATHER_OVERRIDES = 8;
/// Most accepted override commands waiting for their apply tick.
inline constexpr uint32 MAX_PENDING_WEATHER_COMMANDS = 16;
/// Most events one tick can emit: one per schedule source (default region and each region), one per
/// pending command and one phase change per active override.
inline constexpr uint32 MAX_WEATHER_EVENTS_PER_TICK =
    1 + weather_format::MAX_WEATHER_REGIONS + MAX_PENDING_WEATHER_COMMANDS + MAX_ACTIVE_WEATHER_OVERRIDES;
/// Region ID that names the default region in samples and events.
inline constexpr uint32 DEFAULT_WEATHER_REGION_ID = 0;

/// Outcome of a simulation operation. Every failure leaves the simulation and caller outputs unchanged.
enum class WeatherSimulationStatus : uint8
{
    /// The operation completed.
    Success,
    /// An argument is invalid: an empty climate, a zero ID, an unknown channel group or an invalid view.
    InvalidArgument,
    /// The session tick rate differs from the rate the climate was authored for.
    TickRateMismatch,
    /// The sample position is not finite or lies outside the climate domain.
    OutsideDomain,
    /// A command's apply tick is not after the current tick.
    CommandInPast,
    /// A begin command names a preset the climate does not have.
    UnknownPreset,
    /// An override footprint is empty or extends outside the climate domain.
    InvalidFootprint,
    /// Another pending command has the same apply tick, override ID and action.
    DuplicateCommand,
    /// The pending command queue or a caller-owned event output is full.
    CapacityExceeded,
    /// Advancing would move the tick past the largest representable tick.
    TickOverflow,
    /// Saved state was written by an unsupported state layout version.
    UnsupportedStateVersion,
    /// Saved state was written for a different climate, revision or preset revision.
    /// Repair: call ResetToAuthoredBaseline, or load a save that matches the mounted climate.
    IncompatibleClimate,
    /// Saved state is truncated, has trailing bytes or violates a state invariant.
    InvalidState,
    /// Storage for saved state could not be allocated.
    AllocationFailed,
    /// A projected preview names a tick before the current tick. Use an authored preview for earlier ticks.
    PreviewInPast,
};

/// How TryPreview evaluates a preview tick. Neither mode changes the live simulation.
enum class WeatherPreviewMode : uint8
{
    /// The authored schedules and regions at any tick, without overrides or pending commands: what the climate
    /// alone produces then.
    Authored,
    /// The live state carried forward to a tick at or after the current one, applying pending commands and
    /// override phase changes on the way: what the session will see then if no further commands arrive.
    Projected,
};

/// What an override command does at its apply tick.
enum class WeatherOverrideAction : uint8
{
    /// Start an override, or interrupt the active override with the same ID.
    Begin,
    /// Start the return blend of the active override with the command's ID.
    Release,
};

/// A gameplay weather command, applied in fixed-tick order at ApplyTick.
///
/// A Begin for an inactive ID starts a new override that blends in from zero weight. A Begin for an active
/// ID interrupts it: the new blend starts from that override's current weight and channel values, so the
/// resolved weather is continuous at every position. An interruption must keep the priority, channel groups
/// and footprint; use a different ID for a different footprint.
struct WeatherOverrideCommand final
{
    /// What the command does.
    WeatherOverrideAction Action = WeatherOverrideAction::Begin;
    /// Stable nonzero override ID.
    uint32 OverrideId = 0;
    /// Tick at which the command applies; after the current tick when submitted.
    uint64 ApplyTick = 0;
    /// Higher priorities apply first; ties resolve by ascending OverrideId. Begin only.
    int32 Priority = 0;
    /// Target preset ID. Begin only.
    uint32 PresetId = 0;
    /// Bit mask of WeatherChannelGroup values the override replaces. Begin only.
    uint32 ChannelGroups = weather_format::ALL_WEATHER_CHANNEL_GROUPS;
    /// Whether the override covers the whole climate domain. Begin only.
    bool IsGlobal = true;
    /// Footprint when IsGlobal is false, contained in the climate domain. Begin only.
    weather_format::WeatherBoundsWorld FootprintWorld{};
    /// Inward footprint feather distance in millimetres. Begin only.
    uint32 FootprintFeatherMillimetres = 0;
    /// Ticks to blend in. Begin only.
    uint32 BlendInTicks = 0;
    /// Ticks held at full weight before the return blend starts by itself; zero holds until a Release.
    /// Begin only.
    uint32 HoldTicks = 0;
    /// Ticks of the return blend toward the running schedule, at expiry or on Release.
    uint32 BlendOutTicks = 0;
};

/// Phase of an active override.
enum class WeatherOverridePhase : uint8
{
    /// Weight and channel values blend toward the target.
    BlendIn,
    /// Full weight at the target values.
    Hold,
    /// Weight returns to zero; channel values stay frozen.
    BlendOut,
};

/// State of one active override. Exposed for inspection and saved verbatim.
struct WeatherOverrideState final
{
    /// Stable override ID.
    uint32 OverrideId = 0;
    /// Resolution priority.
    int32 Priority = 0;
    /// Target preset ID of the latest Begin.
    uint32 PresetId = 0;
    /// Replaced channel groups.
    uint32 ChannelGroups = 0;
    /// Whether the footprint covers the whole domain.
    bool IsGlobal = true;
    /// Footprint when IsGlobal is false.
    weather_format::WeatherBoundsWorld FootprintWorld{};
    /// Inward footprint feather distance in millimetres.
    uint32 FootprintFeatherMillimetres = 0;
    /// Current phase.
    WeatherOverridePhase Phase = WeatherOverridePhase::BlendIn;
    /// Tick at which the current phase started.
    uint64 PhaseStartTick = 0;
    /// Duration of the current phase; zero in Hold means until released.
    uint32 PhaseTicks = 0;
    /// Hold duration that follows the blend-in; zero means until released.
    uint32 HoldTicks = 0;
    /// Return blend duration at expiry.
    uint32 BlendOutTicks = 0;
    /// Weight fraction code at PhaseStartTick.
    int32 WeightStart = 0;
    /// Channel values at PhaseStartTick.
    weather_format::WeatherChannelCodes SourceStart{};
    /// Channel values the blend-in reaches; the frozen values during Hold and BlendOut.
    weather_format::WeatherChannelCodes SourceTarget{};
};

/// Kind of an authoritative weather event.
enum class WeatherEventKind : uint8
{
    /// A schedule entry started. SourceId is the region; Detail is the entry index within its schedule.
    ScheduleEntryBegan,
    /// A schedule entry started blending toward the next entry. Detail is the blending entry's index.
    ScheduleBlendBegan,
    /// A Begin command started a new override. SourceId is the override; Detail is the preset ID.
    OverrideStarted,
    /// A Begin command interrupted an active override. Detail is the new preset ID.
    OverrideInterrupted,
    /// A Release command started the return blend.
    OverrideReleased,
    /// The hold elapsed and the return blend started.
    OverrideExpired,
    /// The return blend finished and the override was removed.
    OverrideEnded,
    /// A command could not apply at its tick. Detail is a WeatherOverrideRejection value.
    OverrideRejected,
};

/// Reason a command was rejected at its apply tick.
enum class WeatherOverrideRejection : uint8
{
    /// A Release named an override that is not active, or is already returning.
    NotActive = 1,
    /// A Begin found MAX_ACTIVE_WEATHER_OVERRIDES overrides already active.
    CapacityExceeded = 2,
    /// A Begin interruption changed the priority, channel groups or footprint.
    FootprintMismatch = 3,
};

/// One authoritative event. (Tick, Ordinal) is its stable address: ordinals increase by one per event over
/// the whole session and are saved and restored, so replay and deduplication can use them.
struct WeatherEvent final
{
    /// Tick at which the event happened.
    uint64 Tick = 0;
    /// Session-wide event ordinal.
    uint64 Ordinal = 0;
    /// Event kind.
    WeatherEventKind Kind = WeatherEventKind::ScheduleEntryBegan;
    /// Region ID for schedule events, override ID for override events.
    uint32 SourceId = 0;
    /// Kind-specific detail.
    uint32 Detail = 0;

    /// Events compare equal field by field.
    [[nodiscard]] friend constexpr bool operator==(const WeatherEvent&, const WeatherEvent&) noexcept = default;
};

/// Weight of one region at a sample position.
struct WeatherRegionWeight final
{
    /// Region ID; DEFAULT_WEATHER_REGION_ID for the default region.
    uint32 RegionId = 0;
    /// Weight as a fraction code. The weights of one sample sum to FRACTION_ONE.
    int32 Weight = 0;
};

/// Contribution of one active override at a sample position, before lower-priority overrides.
struct WeatherOverrideContribution final
{
    /// Override ID.
    uint32 OverrideId = 0;
    /// Footprint coverage times the override's current weight, as a fraction code.
    int32 Weight = 0;
};

/// Resolved weather at one position and tick.
struct WeatherSample final
{
    /// Tick the sample describes.
    uint64 Tick = 0;
    /// Authoritative channel codes.
    weather_format::WeatherChannelCodes Channels{};
    /// Nonzero region weights in resolution order, the default region last.
    foundation::FixedArray<WeatherRegionWeight, weather_format::MAX_WEATHER_REGIONS + 1> RegionWeights;
    /// Nonzero override contributions in resolution order.
    foundation::FixedArray<WeatherOverrideContribution, MAX_ACTIVE_WEATHER_OVERRIDES> Overrides;
};

/// Channel codes converted to natural units for presentation consumers.
struct WeatherConditions final
{
    /// Degrees Celsius.
    float64 TemperatureCelsius = 0;
    /// Fraction in [0, 1].
    float64 RelativeHumidity = 0;
    /// World metres per second toward the direction the air moves.
    foundation::math::Vector3d GroundWindWorld{};
    /// World metres per second toward the direction the air moves.
    foundation::math::Vector3d CloudWindWorld{};
    /// Fraction in [0, 1].
    float64 CloudCoverage = 0;
    /// Stratiform (0) to cumuliform (1).
    float64 CloudType = 0;
    /// Dimensionless multiplier.
    float64 CloudDensityScale = 0;
    /// Inverse metres.
    float64 FogExtinctionPerMetre = 0;
    /// Metres.
    float64 FogScaleHeightMetres = 0;
    /// Millimetres of liquid water per hour.
    float64 PrecipitationMillimetresPerHour = 0;
};

/// Convert authoritative codes to natural units. Conversion is exact up to binary64 division rounding and
/// is for presentation only; gameplay thresholds compare codes.
[[nodiscard]] WeatherConditions ConvertWeatherChannels(const weather_format::WeatherChannelCodes& channels) noexcept;

/// Mutable simulation state: everything save and load carry besides the climate identity.
struct WeatherSimulationState final
{
    /// Current tick; zero is the authored baseline.
    uint64 Tick = 0;
    /// Ordinal the next event receives.
    uint64 NextEventOrdinal = 0;
    /// Active overrides in ascending override ID order.
    foundation::FixedArray<WeatherOverrideState, MAX_ACTIVE_WEATHER_OVERRIDES> ActiveOverrides;
    /// Accepted commands in ascending (ApplyTick, OverrideId, Action) order.
    foundation::FixedArray<WeatherOverrideCommand, MAX_PENDING_WEATHER_COMMANDS> PendingCommands;
};

/// Deterministic fixed-tick weather for one mounted climate.
///
/// Thread affinity: not synchronized. Mutate from the session's simulation thread; const queries may run
/// concurrently with each other but not with a mutation. Nothing allocates except TrySaveState.
class WeatherSimulation final
{
public:
    /// Create an empty simulation with no climate. Mount one with TryMount before use.
    WeatherSimulation() noexcept = default;

    /// Mount a climate at its authored baseline (tick zero, no overrides).
    /// @param climate Loaded climate; copied, so it need not outlive the simulation.
    /// @param sessionTicksPerSecond Fixed tick rate of the session; must equal the climate's.
    /// @param seed World seed, saved with the state for replay identity. The authored schedules of this
    ///        format version do not draw from it.
    /// @param outSimulation Replaced only on Success.
    [[nodiscard]] static WeatherSimulationStatus TryMount(const Climate& climate,
                                                          uint32 sessionTicksPerSecond,
                                                          uint64 seed,
                                                          WeatherSimulation& outSimulation) noexcept;

    /// Whether a climate is mounted.
    [[nodiscard]] LUDUS_INLINE bool IsMounted() const noexcept
    {
        return mClimate.IsEmpty() == false;
    }

    /// The mounted climate.
    [[nodiscard]] LUDUS_INLINE const Climate& GetClimate() const noexcept
    {
        return mClimate;
    }

    /// Current tick.
    [[nodiscard]] LUDUS_INLINE uint64 GetTick() const noexcept
    {
        return mState.Tick;
    }

    /// World seed recorded at mount.
    [[nodiscard]] LUDUS_INLINE uint64 GetSeed() const noexcept
    {
        return mSeed;
    }

    /// Mutable state, for inspection.
    [[nodiscard]] LUDUS_INLINE const WeatherSimulationState& GetState() const noexcept
    {
        return mState;
    }

    /// Accept a command for a future tick. Commands apply in (ApplyTick, OverrideId, Action) order, so the
    /// order of submission within one tick does not change the outcome. Conditions that depend on the state
    /// at the apply tick (capacity, interruption compatibility, releasing an inactive override) are reported
    /// then as OverrideRejected events.
    /// @param command Validated against the climate; ApplyTick must be after GetTick().
    [[nodiscard]] WeatherSimulationStatus TrySubmit(const WeatherOverrideCommand& command) noexcept;

    /// Advance by tickCount fixed ticks, applying commands, phase changes and schedule boundaries in tick
    /// order. Advancing N ticks at once and one tick N times yield the same state and events.
    /// @param tickCount Ticks to advance; zero is a no-op.
    /// @param outEvents Receives the emitted events in order. If it is too small, nothing changes and the
    ///        call returns CapacityExceeded; MAX_WEATHER_EVENTS_PER_TICK per tick always suffices.
    /// @param outEventCount Number of events written; written only on Success.
    [[nodiscard]] WeatherSimulationStatus
    TryAdvance(uint32 tickCount, foundation::BorrowedView<WeatherEvent> outEvents, usize& outEventCount) noexcept;

    /// Resolve the weather at a World position for the current tick.
    /// @param positionXWorld World X in metres.
    /// @param positionZWorld World Z in metres.
    /// @param outSample Written only on Success.
    /// @return OutsideDomain for a nonfinite position or one outside the climate domain.
    [[nodiscard]] WeatherSimulationStatus
    TrySample(float64 positionXWorld, float64 positionZWorld, WeatherSample& outSample) const noexcept;

    /// Resolve the weather at a World position for a preview tick, as a scrub control does, without changing
    /// the simulation, its event ordinal or its saved state. Previews emit no events, so replays and saves
    /// are unaffected. A projected preview costs O(pending commands + override phase changes) regardless of
    /// the distance to the preview tick.
    /// @param mode Authored schedules only, or the live state projected forward.
    /// @param previewTick Tick to evaluate; for Projected, at or after GetTick().
    /// @param positionXWorld World X in metres.
    /// @param positionZWorld World Z in metres.
    /// @param outSample Written only on Success; its Tick is previewTick.
    /// @return PreviewInPast for a projected tick before GetTick(); OutsideDomain as for TrySample.
    [[nodiscard]] WeatherSimulationStatus TryPreview(WeatherPreviewMode mode,
                                                     uint64 previewTick,
                                                     float64 positionXWorld,
                                                     float64 positionZWorld,
                                                     WeatherSample& outSample) const noexcept;

    /// Return to the authored baseline: tick zero, no overrides or pending commands, event ordinal zero.
    /// This is the reset path for an incompatible save.
    void ResetToAuthoredBaseline() noexcept;

    /// Serialize the state with the mounted climate's identity.
    /// @param outBytes Replaced only on Success.
    [[nodiscard]] WeatherSimulationStatus TrySaveState(foundation::Array<uint8>& outBytes) const noexcept;

    /// Restore state written by TrySaveState for the same climate blob.
    /// @param bytes Exact saved state, borrowed for the call only.
    /// @return IncompatibleClimate when the save names another climate, climate revision or preset
    ///         revision; the state is unchanged and ResetToAuthoredBaseline is the repair.
    [[nodiscard]] WeatherSimulationStatus TryLoadState(foundation::BorrowedView<const uint8> bytes) noexcept;

private:
    Climate mClimate;
    uint64 mSeed = 0;
    WeatherSimulationState mState;
};
} // namespace ludus::gameplay::weather
