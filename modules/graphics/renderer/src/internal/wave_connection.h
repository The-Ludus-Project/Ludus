#pragma once
#include <ludus/foundation/base/core.h>

#include "../../shaders/wave_connection_layout.h"
#include "wave_scene_queries.h"
#include <ludus/graphics/renderer/wave_tracing.hpp>

namespace ludus::graphics::renderer::internal
{
/// Most triangles a ballistic segment may ignore: the two faces of a diffracting wedge.
inline constexpr usize WAVE_MAX_EXCLUDED_TRIANGLES = 2;
/// Triangles a ballistic segment ignores, such as the faces whose shared edge the segment starts or ends on.
struct WaveExcludedTriangles final
{
    StaticArray<uint32, WAVE_MAX_EXCLUDED_TRIANGLES> Triangles{};
    uint32 Count = 0;
    /// Whether `triangleIndex` is one of the first Count entries.
    [[nodiscard]] LUDUS_INLINE bool Contains(uint32 triangleIndex) const noexcept
    {
        for (uint32 slot = 0; slot < Count; ++slot)
        {
            if (Triangles[slot] == triangleIndex)
            {
                return true;
            }
        }
        return false;
    }
};
/// Coordinates in the interaction frustum's transverse basis, in world-distance units.
struct ProjectedPointFrustum final
{
    float64 X = 0;
    float64 Y = 0;
};
/// Whether any triangle of `region`, projected along its frustum direction onto the transverse plane through the
/// frustum origin, covers `positionFrustum` with nonzero opacity. Degenerate projections cover nothing.
[[nodiscard]] bool IsProjectedPointBlocked(const ReferenceSceneData& scene,
                                           const WaveInteractionRegion& region,
                                           const ProjectedPointFrustum& positionFrustum) noexcept;
/// Outcome of escaping a connection endpoint past nearby geometry.
struct WaveOriginEscape final
{
    /// Distance from the cone origin at which the envelope is first clear; valid when neither flag is set.
    float64 DistanceWorld = 0;
    /// The centre ray was blocked inside the escaped span.
    bool IsBlocked = false;
    /// The escape cap was reached; the rest of the connection is treated as ballistic.
    bool IsEntirelyBallistic = false;
};
/// Trace the zero-radius centre ray of rayCone over [NearWorld, FarWorld], ignoring `excluded`. A covered
/// triangle blocks the ray. Ready publishes outIsBlocked; a malformed hierarchy returns InvalidDescription.
[[nodiscard]] rhi::RasterStatus TraceWaveBallistic(const ReferenceSceneData& scene,
                                                   math::EllipticalCone rayCone,
                                                   const WaveExcludedTriangles& excluded,
                                                   bool& outIsBlocked) noexcept;
/// Grow the distance from cone.NearWorld by LUDUS_WAVE_CONNECTION_BALLISTIC_SCALE until the envelope over the
/// next cone.NearWorld is clear of geometry, up to LUDUS_WAVE_CONNECTION_MAX_BALLISTIC_WAVELENGTHS wavelengths.
/// shouldTraceBallistic also tests the centre ray across the skipped span and stops when it is blocked.
/// Returns Unsupported after LUDUS_WAVE_CONNECTION_ESCAPE_ATTEMPTS attempts.
[[nodiscard]] rhi::RasterStatus EscapeWaveOrigin(const ReferenceSceneData& scene,
                                                 const math::EllipticalCone& cone,
                                                 float64 wavelengthWorld,
                                                 bool shouldTraceBallistic,
                                                 WaveOriginEscape& outEscape) noexcept;
/// Coherent scalar Gaussian-mode connection through the current scene. Source emits a Gaussian waist
/// with Sigma = distance / wavenumber; detector samples the field on the mean axis. Geometry in each
/// interaction frustum is projected along propagation onto its midpoint screen. Sequential angular-spectrum
/// propagation accounts for interference across all open cells and successive regions; surfaces are opaque
/// according to their fixed-mip coverage. Returns field divided by the unobstructed analytic Gaussian field.
/// No reflection or polarization is performed here. Caller supplies a finite unit direction, positive metres,
/// receiverWorld retains the independently offset receiver for ballistic visibility; direction/distance define
/// the propagation axis. wavelength/distance <= 1/32 and valid scene data. Failure preserves outField. No
/// allocation/shared state.
[[nodiscard]] rhi::RasterStatus EvaluateWaveConnectionToReceiver(const ReferenceSceneData& scene,
                                                                 math::Vector3d sourceWorld,
                                                                 math::Vector3d receiverWorld,
                                                                 math::Vector3d directionWorld,
                                                                 float64 distanceWorld,
                                                                 float64 wavelengthWorld,
                                                                 WaveField& outField) noexcept;

/// Most straight legs of one relayed connection: the light leg and one after each specular interface event.
inline constexpr usize WAVE_MAX_CONNECTION_LEGS = LUDUS_WAVE_CONNECTION_LEGS;
/// One straight leg of a relayed connection, ordered from the light towards the receiver.
struct WaveConnectionLeg final
{
    /// Offset start: the light endpoint or the interface event that begins this leg.
    math::Vector3d BeginWorld{};
    /// Offset end: the next interface event or the receiver. The far endpoint escape starts here.
    math::Vector3d EndWorld{};
    /// Unit propagation direction of the leg.
    math::Vector3d DirectionWorld{};
    /// Positive geometric length along DirectionWorld, in metres.
    float64 LengthWorld = 0;
    /// Positive real refractive index of the leg's medium. Paraxial propagation uses LengthWorld / RefractiveIndex.
    float64 RefractiveIndex = 1;
    /// The interface event that begins this leg reflects; otherwise it transmits. Ignored for the first leg.
    bool IsReflectedEntry = false;
};
/// A light-to-receiver connection through zero or more specular interface events, as a chain of straight legs.
struct WaveConnectionChain final
{
    StaticArray<WaveConnectionLeg, WAVE_MAX_CONNECTION_LEGS> Legs{};
    uint32 LegCount = 0;
};
/// Append the leg from beginWorld to endWorld. Returns false, leaving the chain unchanged, when it is full, the
/// endpoints coincide or are not finite, or refractiveIndex is not finite and positive.
[[nodiscard]] bool TryAppendWaveConnectionLeg(WaveConnectionChain& inoutChain,
                                              math::Vector3d beginWorld,
                                              math::Vector3d endWorld,
                                              float64 refractiveIndex,
                                              bool isReflectedEntry) noexcept;
/// Unit transverse frame of every leg of a chain, carried across its interface events.
struct WaveConnectionFrames final
{
    /// First transverse axis of each leg; the second is Handedness * Cross(DirectionWorld, AxisXWorld).
    StaticArray<math::Vector3d, WAVE_MAX_CONNECTION_LEGS> AxisXWorld{};
    /// +1 or -1: each reflection mirrors the transverse plane, so the carried frame changes handedness.
    StaticArray<float64, WAVE_MAX_CONNECTION_LEGS> Handedness{};
};
/// Carry the first leg's transverse frame through the chain: a reflection applies the Householder mirror of its
/// interface (normal along the difference of the two directions), a transmission the rotation taking one direction
/// to the next. Returns false for a chain with no legs, a reflection that does not turn the direction or a
/// transmission that reverses it.
[[nodiscard]] bool TryMakeWaveConnectionFrames(const WaveConnectionChain& chain,
                                               WaveConnectionFrames& outFrames) noexcept;
/// Coherent angular-spectrum connection relayed through the chain's interface events. The paraxial Gaussian mode of
/// the reduced length (the sum of each leg's LengthWorld / RefractiveIndex) propagates along the legs in its carried
/// transverse frame, through the interaction regions of every leg, and is observed on the axis at the receiver.
/// The field is divided by the unobstructed field of the same chain, so interface reflectance and transmittance stay
/// with the caller's classical throughput. A one-leg chain is EvaluateWaveConnectionToReceiver. Legs shorter than
/// twice the endpoint escape propagate without interaction regions; the caller's own path traced them clear.
/// Failure preserves outField. No allocation or shared state.
[[nodiscard]] rhi::RasterStatus EvaluateWaveChainConnection(const ReferenceSceneData& scene,
                                                            const WaveConnectionChain& chain,
                                                            float64 wavelengthWorld,
                                                            WaveField& outField) noexcept;

[[nodiscard]] rhi::RasterStatus EvaluateWaveConnection(const ReferenceSceneData& scene,
                                                       math::Vector3d sourceWorld,
                                                       math::Vector3d directionWorld,
                                                       float64 distanceWorld,
                                                       float64 wavelengthWorld,
                                                       WaveField& outField) noexcept;
/// Start a specular chain whose first camera segment begins at the receiver `originWorld`: a non-specular vertex
/// or the camera, where coherence ends.
void BeginWaveRelay(ReferenceOpticalRelay& outRelay, math::Vector3 originWorld) noexcept;
/// Record a glass event at the end of the current camera segment and begin the next one at the event's surface
/// position `nextOriginWorld` (not the offset continuation origin, which lies beyond a transmitting interface). The
/// event infers the medium on both of its sides from the dielectric's convention: a solid dielectric (isSolid) of index
/// eta fills the viewer's side when isViewerInside, and a reflection stays on that side while a transmission crosses.
/// A thin dielectric has air on both sides. A segment that two events infer differently, or a chain longer than
/// LUDUS_WAVE_CONNECTION_LEGS, sets HasConflict.
void AppendWaveRelayEvent(ReferenceOpticalRelay& inoutRelay,
                          bool isSolid,
                          float32 eta,
                          bool isViewerInside,
                          bool isReflected,
                          math::Vector3 nextOriginWorld) noexcept;
/// The light-ordered chain from `sourceWorld` (the emitter the last camera segment reached) back through the relay's
/// glass events to its receiver. Returns false, leaving outChain unspecified, for a conflicting relay, one with no
/// glass event or degenerate legs.
[[nodiscard]] bool TryMakeWaveRelayChain(const ReferenceOpticalRelay& relay,
                                         math::Vector3 sourceWorld,
                                         WaveConnectionChain& outChain) noexcept;
} // namespace ludus::graphics::renderer::internal
