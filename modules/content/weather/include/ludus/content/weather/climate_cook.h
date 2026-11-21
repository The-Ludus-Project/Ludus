#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/content/weather_format/weather_format.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/vector.hpp>

/// @file
/// Headless climate cooker (Ludus::ContentWeather). It validates an authored climate in natural units,
/// quantizes it to the authoritative codes of Ludus::WeatherFormat and writes one canonical LUWTH blob.
/// Canonical order makes the bytes independent of the order in which presets, schedules and regions
/// were authored. See docs/modules/gameplay/weather.md.
namespace ludus::content::weather
{
using foundation::float64;
using foundation::int32;
using foundation::uint32;
using foundation::uint8;

/// Authored weather preset in natural units. Each value must lie in its WeatherFormat channel range after
/// rounding to the nearest code.
struct WeatherPresetSource final
{
    /// Stable nonzero preset ID.
    uint32 Id = 0;
    /// Nonzero revision. Change it whenever the authored values change, so saved weather can detect it.
    uint32 Revision = 1;
    /// Air temperature at the reference height, in degrees Celsius.
    float64 TemperatureCelsius = 0;
    /// Relative humidity at the reference height, in [0, 1].
    float64 RelativeHumidity = 0;
    /// Ground wind velocity toward the direction the air moves, in World metres per second.
    foundation::math::Vector3d GroundWindWorld{};
    /// Cloud-layer wind velocity toward the direction the air moves, in World metres per second.
    foundation::math::Vector3d CloudWindWorld{};
    /// Horizontal cloud coverage, in [0, 1].
    float64 CloudCoverage = 0;
    /// Cloud type weight from stratiform (0) to cumuliform (1).
    float64 CloudType = 0;
    /// Dimensionless cloud density multiplier, in [0, 8].
    float64 CloudDensityScale = 1;
    /// Excess fog extinction at the height reference plane, in inverse metres, in [0, 1].
    float64 FogExtinctionPerMetre = 0;
    /// Fog scale height above the reference plane, in metres, in [0, 20000].
    float64 FogScaleHeightMetres = 0;
    /// Liquid-water-equivalent precipitation rate, in millimetres per hour, in [0, 500].
    float64 PrecipitationMillimetresPerHour = 0;
};

/// One authored schedule step.
struct WeatherScheduleEntrySource final
{
    /// ID of the held preset.
    uint32 PresetId = 0;
    /// Ticks the preset is held.
    uint32 HoldTicks = 0;
    /// Ticks spent blending toward the following entry. HoldTicks + BlendTicks must be nonzero.
    uint32 BlendTicks = 0;
};

/// One authored looping schedule.
struct WeatherScheduleSource final
{
    /// Stable nonzero schedule ID.
    uint32 Id = 0;
    /// Entries in playback order; borrowed for the cook call only.
    foundation::BorrowedView<const WeatherScheduleEntrySource> Entries;
    /// Entry that follows the last one. Entries before it play once from tick zero.
    uint32 LoopStartEntry = 0;
};

/// Axis-aligned World XZ rectangle in metres.
struct WeatherBoundsMetres final
{
    /// Western edge (minimum X).
    float64 MinimumX = 0;
    /// Northern edge (minimum Z; World north is -Z).
    float64 MinimumZ = 0;
    /// Eastern edge (maximum X).
    float64 MaximumX = 0;
    /// Southern edge (maximum Z).
    float64 MaximumZ = 0;
};

/// One authored feathered region.
struct WeatherRegionSource final
{
    /// Stable nonzero region ID.
    uint32 Id = 0;
    /// Higher priorities claim coverage first; ties resolve by ascending Id.
    int32 Priority = 0;
    /// ID of the region's schedule.
    uint32 ScheduleId = 0;
    /// Outer edge, contained in the climate domain.
    WeatherBoundsMetres BoundsWorld{};
    /// Inward feather distance in metres; zero gives a hard edge.
    float64 FeatherMetres = 0;
};

/// One authored climate. Views are borrowed for the cook call only.
struct ClimateSource final
{
    /// Stable nonzero climate asset ID.
    uint32 Id = 0;
    /// Nonzero authored climate revision.
    uint32 Revision = 1;
    /// Session ticks per second that the durations assume, in [MIN_TICKS_PER_SECOND, MAX_TICKS_PER_SECOND].
    uint32 TicksPerSecond = 0;
    /// Explicit outer boundary of the climate's World domain.
    WeatherBoundsMetres DomainWorld{};
    /// ID of the schedule that drives the default region outside and between authored regions.
    uint32 DefaultScheduleId = 0;
    /// Presets in any order.
    foundation::BorrowedView<const WeatherPresetSource> Presets;
    /// Schedules in any order.
    foundation::BorrowedView<const WeatherScheduleSource> Schedules;
    /// Regions in any order.
    foundation::BorrowedView<const WeatherRegionSource> Regions;
};

/// Outcome of TryCookClimate. Every failure leaves the output bytes unchanged.
enum class ClimateCookStatus : uint8
{
    /// The blob was written.
    Success,
    /// A view is invalid, or an ID, revision, tick rate, duration or loop entry is out of range.
    InvalidArgument,
    /// An authored value is not finite or falls outside its channel range after rounding.
    ValueOutOfRange,
    /// Two presets, schedules or regions share an ID.
    DuplicateId,
    /// A schedule entry, region or the default schedule names an ID that does not exist.
    MissingReference,
    /// Domain or region bounds are empty, out of range or outside the domain.
    InvalidBounds,
    /// The climate exceeds a WeatherFormat count limit.
    CapacityExceeded,
    /// Storage for the blob could not be allocated.
    AllocationFailed,
};

/// Kind of authored record a cook diagnostic names.
enum class ClimateCookSubject : uint8
{
    /// The climate itself: its ID, revision, tick rate, domain or default schedule.
    Climate,
    /// A preset, named by SubjectId.
    Preset,
    /// A schedule, named by SubjectId.
    Schedule,
    /// An entry of the schedule named by SubjectId, at EntryIndex.
    ScheduleEntry,
    /// A region, named by SubjectId.
    Region,
};

/// Result of TryCookClimate, naming the first authored record that failed so an editor can point at it.
struct ClimateCookResult final
{
    /// Overall outcome.
    ClimateCookStatus Status = ClimateCookStatus::Success;
    /// Kind of record that failed; meaningful only on failure.
    ClimateCookSubject Subject = ClimateCookSubject::Climate;
    /// Authored ID of the failing record; zero for the climate itself.
    uint32 SubjectId = 0;
    /// Entry index within the failing schedule, or the failing channel for ValueOutOfRange on a preset.
    uint32 Detail = 0;
};

/// Validate, quantize and serialize a climate as one canonical LUWTH blob.
///
/// Presets and schedules are written in ascending ID order and regions in resolution order (descending
/// priority, then ascending ID), so reordering authored records yields identical bytes. Values round to
/// the nearest WeatherFormat code with ties toward +infinity. The cooker applies the same
/// ValidateClimateRecords rules as the runtime loader before writing.
/// @param source Authored climate; its views are read during the call only.
/// @param outBytes Replaced by the blob only on Success.
/// @return The status and, on failure, the first failing record.
[[nodiscard]] ClimateCookResult TryCookClimate(const ClimateSource& source,
                                               foundation::Array<uint8>& outBytes) noexcept;
} // namespace ludus::content::weather
