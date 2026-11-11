#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/graphics/renderer/lighting.hpp>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace ludus::editor
{
[[nodiscard]] QString GetGiProbeVolumeComponentId();
[[nodiscard]] QString GetGiProbeReceiverComponentId();
/// The component property that stores the layout choice.
[[nodiscard]] QString GetGiProbeLayoutKey();

/// How an authored GI Probe Volume decides its probe counts.
enum class GiProbeLayout : foundation::uint8
{
    /// Probes fill the Transform scale, read as the volume's size in metres, at the authored spacing.
    Volume,
    /// The authored counts and spacing set the size; the Transform scale is not used.
    Counts
};

/// The schema choice word stored for a layout.
[[nodiscard]] QString GetGiProbeLayoutWord(GiProbeLayout layout);
/// Read the authored layout. Volumes saved before the property existed keep their counts.
[[nodiscard]] GiProbeLayout ReadWorldGiProbeLayout(const QJsonObject& components);

/// Probes along each axis of a new volume, including both ends.
inline constexpr foundation::int32 DEFAULT_GI_PROBES_PER_AXIS = 4;
/// Spacing of a new volume, in metres.
inline constexpr foundation::float64 DEFAULT_GI_PROBE_SPACING_METRES = 1;
/// Edge length of a new volume, in metres: the default probes at the default spacing.
inline constexpr foundation::float64 DEFAULT_GI_PROBE_VOLUME_SIZE_METRES =
    (DEFAULT_GI_PROBES_PER_AXIS - 1) * DEFAULT_GI_PROBE_SPACING_METRES;
/// Smallest volume edge the viewport authors, in metres: one minimum spacing.
inline constexpr foundation::float64 MIN_GI_PROBE_VOLUME_SIZE_METRES = graphics::renderer::MIN_GI_PROBE_SPACING_METRES;
/// Largest volume edge the viewport authors, in metres. The built-in Transform scale range admits the same value.
inline constexpr foundation::float64 MAX_GI_PROBE_VOLUME_SIZE_METRES = 1000;

/// Probe counts on the three axes.
using GiProbeCounts = foundation::StaticArray<foundation::uint32, graphics::renderer::GI_PROBE_DIMENSIONS>;

/// An authored volume: its box and the probe grid that fills it, whether or not its bake is enabled.
struct WorldGiProbeVolume final
{
    GiProbeLayout Layout = GiProbeLayout::Volume;
    /// Edge lengths in metres. The Volume layout reads the Transform scale; the Counts layout derives them.
    foundation::math::Vector3 SizeWorld{};
    /// Counts the spacing needs to reach every edge, before the per-axis limit. Equal to Grid.Counts when
    /// the limit is not reached.
    GiProbeCounts RequiredCounts{};
    /// The grid the bake samples: its first probe is the volume's minimum corner.
    graphics::renderer::GiProbeGrid Grid{};
    bool IsEnabled = false;
    /// Whether the per-axis probe limit stops the grid short of an edge.
    [[nodiscard]] bool IsCoverageLimited() const noexcept;
};

/// Whether the entity owns only the volume and its minimum-corner Transform.
[[nodiscard]] bool IsStandaloneWorldGiProbeVolume(const QJsonObject& entity, const QJsonObject& components);

/// Add the built-in components without changing the bundled schema's recovery fingerprint.
void AppendGiProbeComponents(QJsonArray& inoutComponents);

/// Read an attached volume, including one whose bake is disabled. Absent components produce zero counts.
/// Fails with a message naming the repair when the grid cannot be represented.
[[nodiscard]] bool
ReadWorldGiProbeVolume(const QJsonObject& components, WorldGiProbeVolume& outVolume, QString& outError);

/// Read an attached volume's grid. A disabled volume produces zero counts unless requested for authoring.
[[nodiscard]] bool ReadWorldGiProbeGrid(const QJsonObject& components,
                                        graphics::renderer::GiProbeGrid& outGrid,
                                        QString& outError,
                                        bool shouldIncludeDisabled = false);

/// Number of probes an edge of sizeMetres holds at spacingMetres: the minimum corner plus the nearest whole
/// number of spacings, at least two, so the last probe lies within half a spacing of the far face. Not limited
/// to the per-axis maximum.
[[nodiscard]] foundation::uint32 ComputeGiProbeCountForSize(foundation::float64 sizeMetres,
                                                            foundation::float64 spacingMetres) noexcept;

/// The grid that fills a box from its minimum corner at the given spacing, limited to the per-axis maximum.
/// outRequiredCounts receives the counts before the limit.
[[nodiscard]] graphics::renderer::GiProbeGrid ComputeGiProbeGridForVolume(const foundation::math::Vector3& originWorld,
                                                                          const foundation::math::Vector3& sizeWorld,
                                                                          foundation::float32 spacingMetres,
                                                                          GiProbeCounts& outRequiredCounts) noexcept;

/// Smallest spacing, in metres and in whole centimetres where that keeps the count, at which the per-axis
/// maximum of probes spans the longest edge.
[[nodiscard]] foundation::float64
ComputeGiProbeSpacingCoveringSize(const foundation::math::Vector3& sizeWorld) noexcept;

/// The first and last probe positions of a nonempty grid.
[[nodiscard]] foundation::math::Aabb3 ComputeWorldGiProbeBounds(const graphics::renderer::GiProbeGrid& grid) noexcept;

/// The authored box: the minimum corner and its size.
[[nodiscard]] foundation::math::Aabb3 ComputeWorldGiProbeVolumeBounds(const WorldGiProbeVolume& volume) noexcept;

/// A sentence naming the edges the per-axis limit leaves short and the spacing that would reach them, or
/// empty when the grid reaches every edge.
[[nodiscard]] QString DescribeWorldGiProbeCoverage(const WorldGiProbeVolume& volume);

/// Read and write the authored minimum corner in X/Y/Z order, preserving JSON precision.
[[nodiscard]] QJsonArray ReadWorldGiProbeOrigin(const QJsonObject& components);
void WriteWorldGiProbeOrigin(QJsonObject& inoutComponents, const QJsonArray& origin);

/// Write uniform probe spacing in metres. The caller validates the edit with the World document.
void WriteWorldGiProbeSpacing(QJsonObject& inoutComponents, foundation::float64 spacingMetres);

/// Write one edge of a Volume-layout box, in metres, as the Transform scale on that axis.
void WriteWorldGiProbeSize(QJsonObject& inoutComponents, foundation::usize axis, foundation::float64 sizeMetres);

/// Change one axis count, shifting its minimum corner if the maximum face stays fixed.
void ResizeWorldGiProbeAxis(QJsonObject& inoutComponents,
                            foundation::usize axis,
                            foundation::uint32 count,
                            bool isMinimumFace);

/// Change one edge of a Volume-layout box, shifting its minimum corner if the maximum face stays fixed.
void ResizeWorldGiProbeVolumeAxis(QJsonObject& inoutComponents,
                                  foundation::usize axis,
                                  foundation::float64 sizeMetres,
                                  bool isMinimumFace);

/// Preserve the probe layout when switching representations. Counts to Volume uses the probe extent for
/// unchanged scale axes. Volume to Counts writes the counts derived from the scale.
void SynchronizeWorldGiProbeLayout(const QJsonObject& previousComponents, QJsonObject& inoutComponents);
} // namespace ludus::editor
