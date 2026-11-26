#pragma once
#include <ludus/foundation/base/core.h>

#include "wave_connection.h"

// Uniform Theory of Diffraction (UTD) connection operator of the wave-tracing profile. See
// wave_edge_diffraction.cpp for the declared field, attribution and limits.
namespace ludus::graphics::renderer::internal
{
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
