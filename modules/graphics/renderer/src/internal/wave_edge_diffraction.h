#pragma once
#include <ludus/foundation/base/core.h>

#include "wave_connection.h"

// Edge-diffraction connections of the wave-tracing profile: the shared connection driver and the Uniform Theory
// of Diffraction (UTD) operator. See wave_edge_diffraction.cpp for the UTD field, attribution and limits, and
// wave_fraunhofer_diffraction.h for the Fraunhofer edge operator.
namespace ludus::graphics::renderer::internal
{
/// Corners of a triangle.
inline constexpr uint32 WAVE_TRIANGLE_CORNERS = 3;
/// FindWaveTriangleCorner's result when no corner matches.
inline constexpr uint32 WAVE_NO_TRIANGLE_CORNER = WAVE_TRIANGLE_CORNERS;
/// Inputs that an edge operator shares across the interaction regions of one connection.
struct WaveEdgeConnection final
{
    math::Vector3d SourceWorld{};
    math::Vector3d ReceiverWorld{};
    /// Unit propagation axis from the source towards the receiver.
    math::Vector3d DirectionWorld{};
    /// Unit transverse axis of the connection frame; the other is Cross(DirectionWorld, AxisXWorld).
    math::Vector3d AxisXWorld{};
    float64 DistanceWorld = 0;
    float64 WavelengthWorld = 0;
    /// Sigma^2 = distance / wavenumber of the connection's Gaussian beam, in square metres.
    float64 SigmaWorldSquared = 0;
    /// Radius around the axis beyond which the Gaussian envelope ignores geometry.
    float64 EnvelopeRadiusWorld = 0;
    /// Span at each endpoint whose own surface geometry is not diffracted.
    float64 EndpointEscapeWorld = 0;
    /// Coordinate rounding scale of the connection, for offsets that must stay resolvable.
    float64 RoundoffWorld = 0;
    /// The centre ray between the escaped endpoints is blocked.
    bool IsBallisticBlocked = false;
};
/// Adds one operator's diffracted fields for the edges of `region` to inoutField; failure may leave it partial.
using AddWaveEdgeRegionFields = rhi::RasterStatus (*)(const ReferenceSceneData& scene,
                                                      const WaveEdgeConnection& connection,
                                                      const WaveInteractionRegion& region,
                                                      WaveField& inoutField) noexcept;
/// Shared edge-connection driver: the ballistic field (one when the centre ray between the escaped endpoints is
/// clear, otherwise zero) plus the fields `addRegionFields` adds for each interaction region along the connection.
/// Endpoint escapes are excluded, and geometry continuing straight past a region is skipped as the angular-spectrum
/// operator skips it. Accepts and rejects the same inputs as EvaluateWaveConnectionToReceiver; too many regions
/// return Unsupported. Failure preserves outField. No allocation or shared state.
[[nodiscard]] rhi::RasterStatus EvaluateWaveEdgeConnection(const ReferenceSceneData& scene,
                                                           math::Vector3d sourceWorld,
                                                           math::Vector3d receiverWorld,
                                                           math::Vector3d directionWorld,
                                                           float64 distanceWorld,
                                                           float64 wavelengthWorld,
                                                           AddWaveEdgeRegionFields addRegionFields,
                                                           WaveField& outField) noexcept;
/// Whether the segment from nearWorld past `beginWorld` to farMarginWorld before `endWorld` is clear of covered
/// triangles other than `excluded`. Ready publishes outIsClear; a malformed hierarchy returns InvalidDescription.
[[nodiscard]] rhi::RasterStatus IsWaveSegmentClear(const ReferenceSceneData& scene,
                                                   math::Vector3d beginWorld,
                                                   math::Vector3d endWorld,
                                                   float64 nearWorld,
                                                   float64 farMarginWorld,
                                                   const WaveExcludedTriangles& excluded,
                                                   bool& outIsClear) noexcept;
/// Corner of `triangle` whose position equals `position` bit for bit, or WAVE_NO_TRIANGLE_CORNER.
[[nodiscard]] uint32 FindWaveTriangleCorner(const ReferenceTriangle& triangle, math::Vector3 position) noexcept;
/// Coverage of `triangle` at barycentric `weights` of its corners, as ordinary visibility sees it. Geometry-only
/// scenes without instances are opaque.
[[nodiscard]] bool IsWaveTriangleCovered(const ReferenceSceneData& scene,
                                         const ReferenceTriangle& triangle,
                                         const StaticArray<float64, WAVE_TRIANGLE_CORNERS>& weights) noexcept;
/// Most faces that meet at a diffracting edge: one for a half-plane, two for a wedge.
inline constexpr usize UTD_MAX_EDGE_FACES = 2;
/// A straight edge segment and the faces that meet along it.
struct UtdEdge final
{
    math::Vector3d BeginWorld{};
    math::Vector3d EndWorld{};
    /// Unit directions perpendicular to the edge, each lying in one face and pointing into it.
    StaticArray<math::Vector3d, UTD_MAX_EDGE_FACES> FaceDirectionsWorld{};
    /// One face is a thin half-plane; two faces bound a wedge.
    uint32 FaceCount = 0;
};
/// One edge's diffracted contribution to a source/receiver connection.
struct UtdEdgeField final
{
    /// Diffracted field divided by the unobstructed free-space field at the receiver.
    WaveField Field{};
    /// Diffraction point on the edge, where incident and diffracted rays make equal angles with the edge.
    math::Vector3d PointWorld{};
    /// The edge diffracts this connection: the point lies inside the segment, source and receiver share the
    /// exterior, and the edge is a silhouette seen from one of them.
    bool IsDiffracting = false;
};
/// G(X) = F(X) / sqrt(X) of Kouyoumjian and Pathak's transition function F, for X >= 0. G(0) is
/// sqrt(pi) exp(i pi / 4); G(X) approaches 1 / sqrt(X) for large X.
[[nodiscard]] WaveField EvaluateUtdTransitionRatio(float64 argument) noexcept;
/// Diffracted field of `edge` for a point source and a receiver, using the incident shadow-boundary terms of
/// the Kouyoumjian-Pathak wedge coefficient. Geometry is in metres. Returns IsDiffracting false, with a zero
/// field, when the edge does not diffract this pair or the geometry is degenerate.
[[nodiscard]] UtdEdgeField EvaluateUtdEdgeField(const UtdEdge& edge,
                                                math::Vector3d sourceWorld,
                                                math::Vector3d receiverWorld,
                                                float64 wavelengthWorld) noexcept;
/// UTD connection through the current scene: the ballistic field (one when the straight segment is clear,
/// otherwise zero) plus the coherent single-diffraction fields of scene edges whose diffraction points lie in
/// the connection's Gaussian envelope. Fields are relative to the unobstructed free-space field. Accepts the
/// same inputs as EvaluateWaveConnectionToReceiver and fails the same way: invalid input returns
/// InvalidDescription, a full interaction region CapacityExceeded and too many regions or escape attempts
/// Unsupported. Failure preserves outField. No allocation or shared state.
[[nodiscard]] rhi::RasterStatus EvaluateUtdConnectionToReceiver(const ReferenceSceneData& scene,
                                                                math::Vector3d sourceWorld,
                                                                math::Vector3d receiverWorld,
                                                                math::Vector3d directionWorld,
                                                                float64 distanceWorld,
                                                                float64 wavelengthWorld,
                                                                WaveField& outField) noexcept;
} // namespace ludus::graphics::renderer::internal
