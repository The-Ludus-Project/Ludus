#include "internal/wave_edge_diffraction.h"
#include "../shaders/wave_connection_layout.h"
#include <ludus/foundation/math/enclosure.hpp>

// Thanks to Shlomi Steinberg and Matt Pharr, "Wave Tracing: Generalizing The Path Integral To Wave Optics",
// Computer Graphics Forum 45(2), e70322 (2026), Section 5.2.2 ("UTD-based diffraction"),
// https://doi.org/10.1111/cgf.70322 : a connection's interaction regions supply the edges, and each edge
// joins the regions before and after it through its diffraction point, summed coherently with the free-space
// term. Thanks to Robert G. Kouyoumjian and Prabhakar H. Pathak, "A Uniform Geometrical Theory of
// Diffraction for an Edge in a Perfectly Conducting Surface", Proceedings of the IEEE 62(11), 1448-1461
// (1974), https://doi.org/10.1109/PROC.1974.9651 (NASA CR-139956, https://ntrs.nasa.gov/citations/19750029381),
// for the wedge coefficient (their Eq. 25), its transition function F (Eq. 26), the spherical-wave distance
// parameter L and spreading factor, and the shadow-boundary limit used at grazing alignment.
//
// Departures, each declared in render-graph-authoring.md ("Edge diffraction"): this scalar operator keeps the
// coefficient's incident shadow-boundary terms only. Ludus surfaces scatter through their classical BSDFs, so
// there is no coherent reflected field for the reflection shadow-boundary terms to complete; the remaining
// terms are identical for both polarizations and behave as an absorbing (Kirchhoff) edge, which matches
// Fresnel knife-edge diffraction near the shadow boundary. Connections take one diffraction; corner diffraction
// at edge endpoints and multiple diffraction are omitted. The coefficient is factored so that the incident
// grazing angle and the shadow-boundary singularity cancel analytically, and the path difference is computed
// from the unfolded geometry, so float32 GPU evaluation keeps its phase near the boundaries.
// G is evaluated with its Taylor series for small arguments and Laplace's continued fraction for the
// complementary error function otherwise; this implementation is original.
namespace ludus::graphics::renderer::internal
{
namespace
{
constexpr float64 FRAME_HELPER_COSINE = 0.9;
constexpr float64 MIN_WAVELENGTH_WORLD =
    static_cast<float64>(static_cast<float32>(LUDUS_WAVE_CONNECTION_MIN_WAVELENGTH_WORLD));
constexpr float64 UNIT_DIRECTION_TOLERANCE = 1e-6;
constexpr float64 MAX_WAVELENGTH_TO_DISTANCE = 1.0 / 32;
/// pi / 4, the phase of G(0) and of the coefficient's leading factor.
constexpr float64 EIGHTH_TURN_RADIANS = math::kPi / 4;
/// Sixth: the second Taylor coefficient of sin(x) / x.
constexpr float64 SINC_SECOND_TERM = 1.0 / 6;
constexpr uint32 TRIANGLE_CORNERS = 3;
/// FindCorner's result when no corner matches.
constexpr uint32 NO_CORNER = TRIANGLE_CORNERS;
/// 0 + 1 + 2: subtracting two corner indices from it gives the third.
constexpr uint32 CORNER_INDEX_SUM = 3;

WaveField AddFields(WaveField first, WaveField second) noexcept
{
    return {first.Real + second.Real, first.Imaginary + second.Imaginary};
}
WaveField MultiplyFields(WaveField first, WaveField second) noexcept
{
    return {first.Real * second.Real - first.Imaginary * second.Imaginary,
            first.Real * second.Imaginary + first.Imaginary * second.Real};
}
WaveField ScaleField(WaveField field, float64 scale) noexcept
{
    return {field.Real * scale, field.Imaginary * scale};
}
WaveField DivideFields(WaveField numerator, WaveField denominator) noexcept
{
    const float64 power = denominator.Real * denominator.Real + denominator.Imaginary * denominator.Imaginary;
    return {(numerator.Real * denominator.Real + numerator.Imaginary * denominator.Imaginary) / power,
            (numerator.Imaginary * denominator.Real - numerator.Real * denominator.Imaginary) / power};
}
WaveField MakePhase(float64 phase) noexcept
{
    return {math::Cos(phase), math::Sin(phase)};
}
float64 EvaluateSinc(float64 value) noexcept
{
    return math::Abs(value) < LUDUS_WAVE_UTD_SINC_LIMIT ? 1 - value * value * SINC_SECOND_TERM
                                                        : math::Sin(value) / value;
}
math::Vector3d WidenPosition(math::Vector3 value) noexcept
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}
/// Angle in [0, 2 pi) that rotates the unit `reference` to the unit `direction` about the unit `axis`; both
/// directions are perpendicular to the axis.
float64 MeasureAngleAbout(math::Vector3d axis, math::Vector3d reference, math::Vector3d direction) noexcept
{
    const float64 angle =
        math::Atan2(math::Dot(math::Cross(reference, direction), axis), math::Dot(reference, direction));
    return angle < 0 ? angle + 2 * math::kPi : angle;
}
/// One incident shadow-boundary term, cot((pi + sign beta) / 2n) F(kL a_sign(beta)), divided by sqrt(kL).
/// epsilon is the term's distance from its shadow boundary; the factor sqrt(2) n sgn(epsilon) cos(epsilon/2n)
/// sinc(epsilon/2) / sinc(epsilon/2n) equals cot(.) sqrt(a), which stays finite where cot diverges.
/// The wedge and distance that every incident term of one edge shares.
struct UtdTermScale final
{
    /// n: the exterior wedge angle divided by pi.
    float64 WedgeIndex = 2;
    /// kL: the wavenumber times Kouyoumjian and Pathak's distance parameter.
    float64 DistanceWavenumber = 0;
};
WaveField EvaluateIncidentTerm(float64 epsilon, const UtdTermScale& scale) noexcept
{
    const float64 wedgeIndex = scale.WedgeIndex;
    const float64 distanceWavenumber = scale.DistanceWavenumber;
    const float64 halfEpsilon = epsilon * 0.5;
    const float64 sign = epsilon < 0 ? -1.0 : 1.0;
    const float64 factor = math::Sqrt(2.0) * wedgeIndex * sign * math::Cos(halfEpsilon / wedgeIndex) *
                           EvaluateSinc(halfEpsilon) / EvaluateSinc(halfEpsilon / wedgeIndex);
    const float64 sine = math::Sin(halfEpsilon);
    return ScaleField(EvaluateUtdTransitionRatio(distanceWavenumber * 2 * sine * sine), factor);
}
/// Coverage of `triangle` at barycentric `weights` of its corners, as ordinary visibility sees it.
bool IsTriangleCovered(const ReferenceSceneData& scene,
                       const ReferenceTriangle& triangle,
                       const StaticArray<float64, TRIANGLE_CORNERS>& weights) noexcept
{
    const float64 u = weights[1];
    const float64 v = weights[2];
    // Geometry-only fixtures omit instances. Retained renderer scenes always supply the material owner.
    if (scene.Instances.IsEmpty() == true)
    {
        return true;
    }
    if (triangle.Instance >= scene.Instances.GetSize())
    {
        return false;
    }
    const float32 narrowU = static_cast<float32>(u);
    const float32 narrowV = static_cast<float32>(v);
    return ReferenceOpacity(scene.Instances[triangle.Instance],
                            InterpolateReferenceUv(triangle, narrowU, narrowV),
                            InterpolateReferenceColor(triangle, narrowU, narrowV).W);
}
/// Corner of `triangle` whose position equals `position` bit for bit, or NO_CORNER when none does.
uint32 FindCorner(const ReferenceTriangle& triangle, math::Vector3 position) noexcept
{
    for (uint32 corner = 0; corner < TRIANGLE_CORNERS; ++corner)
    {
        const math::Vector3& candidate = triangle.Positions[corner];
        if (candidate.X == position.X && candidate.Y == position.Y && candidate.Z == position.Z)
        {
            return corner;
        }
    }
    return NO_CORNER;
}
/// A face of an edge found in a region: the triangle, and its corners at the edge's begin and end.
struct RegionEdgeFace final
{
    uint32 TriangleIndex = 0;
    uint32 BeginCorner = 0;
    uint32 EndCorner = 0;
};
/// The connection whose edges a region contributes.
struct UtdConnection final
{
    math::Vector3d SourceWorld{};
    math::Vector3d ReceiverWorld{};
    math::Vector3d DirectionWorld{};
    float64 WavelengthWorld = 0;
    float64 EnvelopeRadiusWorld = 0;
    float64 EndpointEscapeWorld = 0;
};
/// Whether a ballistic segment between two points is clear, ignoring `excluded`. The segment starts
/// nearWorld from `beginWorld` and stops farMarginWorld short of `endWorld`.
rhi::RasterStatus IsSegmentClear(const ReferenceSceneData& scene,
                                 math::Vector3d beginWorld,
                                 math::Vector3d endWorld,
                                 float64 nearWorld,
                                 float64 farMarginWorld,
                                 const WaveExcludedTriangles& excluded,
                                 bool& outIsClear) noexcept
{
    const math::Vector3d deltaWorld = endWorld - beginWorld;
    const float64 lengthWorld = math::Sqrt(math::Dot(deltaWorld, deltaWorld));
    if (lengthWorld - farMarginWorld <= nearWorld)
    {
        outIsClear = true;
        return rhi::RasterStatus::Ready;
    }
    math::EllipticalCone ray;
    ray.OriginWorld = beginWorld;
    ray.DirectionWorld = deltaWorld * (1 / lengthWorld);
    const math::Vector3d helperWorld =
        math::Abs(ray.DirectionWorld.Y) < FRAME_HELPER_COSINE ? math::Vector3d{0, 1, 0} : math::Vector3d{1, 0, 0};
    const math::Vector3d crossWorld = math::Cross(helperWorld, ray.DirectionWorld);
    ray.AxisXWorld = crossWorld * (1 / math::Sqrt(math::Dot(crossWorld, crossWorld)));
    ray.NearWorld = nearWorld;
    ray.FarWorld = lengthWorld - farMarginWorld;
    bool isBlocked = false;
    const rhi::RasterStatus status = TraceWaveBallistic(scene, ray, excluded, isBlocked);
    if (status == rhi::RasterStatus::Ready)
    {
        outIsClear = isBlocked == false;
    }
    return status;
}
/// Add the diffracted fields of the edges in `region` whose diffraction points lie in its axial slab and
/// inside the connection envelope. Each edge is visited from the lowest region slot among its faces.
rhi::RasterStatus AddRegionEdgeFields(const ReferenceSceneData& scene,
                                      const UtdConnection& connection,
                                      const WaveInteractionRegion& region,
                                      WaveField& inoutField) noexcept
{
    for (uint32 slot = 0; slot < region.TriangleCount; ++slot)
    {
        const ReferenceTriangle& triangle = scene.Triangles[region.Triangles[slot]];
        for (uint32 corner = 0; corner < TRIANGLE_CORNERS; ++corner)
        {
            const uint32 endCorner = (corner + 1) % TRIANGLE_CORNERS;
            const math::Vector3 begin = triangle.Positions[corner];
            const math::Vector3 end = triangle.Positions[endCorner];
            if (begin.X == end.X && begin.Y == end.Y && begin.Z == end.Z)
            {
                continue;
            }
            StaticArray<RegionEdgeFace, UTD_MAX_EDGE_FACES> faces{};
            faces[0] = { .TriangleIndex = region.Triangles[slot], .BeginCorner = corner, .EndCorner = endCorner };
            uint32 faceCount = 1;
            bool isVisitedElsewhere = false;
            for (uint32 other = 0; other < region.TriangleCount; ++other)
            {
                if (other == slot)
                {
                    continue;
                }
                const ReferenceTriangle& candidate = scene.Triangles[region.Triangles[other]];
                const uint32 candidateBegin = FindCorner(candidate, begin);
                const uint32 candidateEnd = FindCorner(candidate, end);
                if (candidateBegin == NO_CORNER || candidateEnd == NO_CORNER)
                {
                    continue;
                }
                if (other < slot || faceCount == UTD_MAX_EDGE_FACES)
                {
                    // An earlier face visits this edge; three or more faces make a non-manifold edge, which
                    // has no single exterior and is not diffracted.
                    isVisitedElsewhere = true;
                    break;
                }
                faces[faceCount++] =
                {
                    .TriangleIndex = region.Triangles[other],
                    .BeginCorner = candidateBegin,
                    .EndCorner = candidateEnd,
                };
            }
            if (isVisitedElsewhere == true)
            {
                continue;
            }
            const math::Vector3d beginWorld = WidenPosition(begin);
            const math::Vector3d edgeWorld = WidenPosition(end) - beginWorld;
            const float64 lengthWorld = math::Sqrt(math::Dot(edgeWorld, edgeWorld));
            const math::Vector3d tangent = edgeWorld * (1 / lengthWorld);
            UtdEdge edge;
            edge.BeginWorld = beginWorld;
            edge.EndWorld = WidenPosition(end);
            // Diffraction points depend only on the edge line, so coverage is tested after locating one.
            StaticArray<math::Vector3d, UTD_MAX_EDGE_FACES> directions{};
            bool isDegenerate = false;
            for (uint32 face = 0; face < faceCount; ++face)
            {
                const ReferenceTriangle& owner = scene.Triangles[faces[face].TriangleIndex];
                const uint32 third = CORNER_INDEX_SUM - faces[face].BeginCorner - faces[face].EndCorner;
                const math::Vector3d offsetWorld = WidenPosition(owner.Positions[third]) - beginWorld;
                const math::Vector3d inFaceWorld = offsetWorld - tangent * math::Dot(offsetWorld, tangent);
                const float64 inFaceLength = math::Sqrt(math::Dot(inFaceWorld, inFaceWorld));
                if (inFaceLength <= 0 || math::IsFinite(inFaceLength) == false)
                {
                    isDegenerate = true;
                    break;
                }
                directions[face] = inFaceWorld * (1 / inFaceLength);
            }
            if (isDegenerate == true)
            {
                continue;
            }
            edge.FaceCount = faceCount;
            edge.FaceDirectionsWorld = directions;
            UtdEdgeField diffracted = EvaluateUtdEdgeField(edge,
                                                           connection.SourceWorld,
                                                           connection.ReceiverWorld,
                                                           connection.WavelengthWorld);
            if (diffracted.IsDiffracting == false)
            {
                continue;
            }
            const math::Vector3d pointOffsetWorld = diffracted.PointWorld - connection.SourceWorld;
            const float64 axialWorld = math::Dot(pointOffsetWorld, connection.DirectionWorld);
            const math::Vector3d radialWorld = pointOffsetWorld - connection.DirectionWorld * axialWorld;
            if (axialWorld < region.Frustum.NearWorld || axialWorld > region.Frustum.FarWorld ||
                math::Dot(radialWorld, radialWorld) > connection.EnvelopeRadiusWorld * connection.EnvelopeRadiusWorld)
            {
                continue;
            }
            // Faces that are transparent at the diffraction point do not bound the edge there.
            const float64 edgeCoordinate = math::Dot(diffracted.PointWorld - beginWorld, tangent) / lengthWorld;
            UtdEdge covered = edge;
            covered.FaceCount = 0;
            WaveExcludedTriangles excluded;
            for (uint32 face = 0; face < faceCount; ++face)
            {
                excluded.Triangles[excluded.Count++] = faces[face].TriangleIndex;
                StaticArray<float64, TRIANGLE_CORNERS> weights{};
                weights[faces[face].BeginCorner] = 1 - edgeCoordinate;
                weights[faces[face].EndCorner] = edgeCoordinate;
                if (IsTriangleCovered(scene, scene.Triangles[faces[face].TriangleIndex], weights) == true)
                {
                    covered.FaceDirectionsWorld[covered.FaceCount++] = edge.FaceDirectionsWorld[face];
                }
            }
            if (covered.FaceCount == 0)
            {
                continue;
            }
            if (covered.FaceCount != edge.FaceCount)
            {
                diffracted = EvaluateUtdEdgeField(covered,
                                                  connection.SourceWorld,
                                                  connection.ReceiverWorld,
                                                  connection.WavelengthWorld);
                if (diffracted.IsDiffracting == false)
                {
                    continue;
                }
            }
            bool isIncidentClear = false;
            rhi::RasterStatus status = IsSegmentClear(scene,
                                                      connection.SourceWorld,
                                                      diffracted.PointWorld,
                                                      connection.EndpointEscapeWorld,
                                                      0,
                                                      excluded,
                                                      isIncidentClear);
            if (status != rhi::RasterStatus::Ready)
            {
                return status;
            }
            if (isIncidentClear == false)
            {
                continue;
            }
            bool isDiffractedClear = false;
            status = IsSegmentClear(scene,
                                    diffracted.PointWorld,
                                    connection.ReceiverWorld,
                                    0,
                                    connection.EndpointEscapeWorld,
                                    excluded,
                                    isDiffractedClear);
            if (status != rhi::RasterStatus::Ready)
            {
                return status;
            }
            if (isDiffractedClear == true)
            {
                inoutField = AddFields(inoutField, diffracted.Field);
            }
        }
    }
    return rhi::RasterStatus::Ready;
}
} // namespace

WaveField EvaluateUtdTransitionRatio(float64 argument) noexcept
{
    const float64 root = math::Sqrt(argument);
    if (argument < LUDUS_WAVE_UTD_SERIES_LIMIT)
    {
        // G = 2i exp(iX) (sqrt(pi)/2 exp(-i pi/4) - sum_m (-i)^m w^(2m+1) / (m! (2m+1))), w = sqrt(X).
        WaveField sum{};
        WaveField rotation{1, 0};
        float64 magnitude = root;
        for (uint32 term = 0; term < LUDUS_WAVE_UTD_SERIES_TERMS; ++term)
        {
            sum = AddFields(sum, ScaleField(rotation, magnitude / static_cast<float64>(2 * term + 1)));
            rotation = {rotation.Imaginary, -rotation.Real};
            magnitude *= argument / static_cast<float64>(term + 1);
        }
        const float64 halfRootPi = math::Sqrt(math::kPi) * 0.5;
        const WaveField tail = {halfRootPi * math::Cos(EIGHTH_TURN_RADIANS) - sum.Real,
                                -halfRootPi * math::Sin(EIGHTH_TURN_RADIANS) - sum.Imaginary};
        const WaveField rotated = MultiplyFields(MakePhase(argument), tail);
        return {-2 * rotated.Imaginary, 2 * rotated.Real};
    }
    // G = exp(i pi/4) / (z + (1/2) / (z + 1 / (z + (3/2) / ...))), z = exp(i pi/4) sqrt(X), evaluated bottom up.
    const WaveField rotation = MakePhase(EIGHTH_TURN_RADIANS);
    const WaveField z = ScaleField(rotation, root);
    WaveField denominator = z;
    for (uint32 level = LUDUS_WAVE_UTD_FRACTION_DEPTH; level > 0; --level)
    {
        denominator = AddFields(z, DivideFields({static_cast<float64>(level) * 0.5, 0}, denominator));
    }
    return DivideFields(rotation, denominator);
}

UtdEdgeField EvaluateUtdEdgeField(const UtdEdge& edge,
                                  math::Vector3d sourceWorld,
                                  math::Vector3d receiverWorld,
                                  float64 wavelengthWorld) noexcept
{
    UtdEdgeField result;
    if (edge.FaceCount == 0 || edge.FaceCount > UTD_MAX_EDGE_FACES || wavelengthWorld <= 0)
    {
        return result;
    }
    const math::Vector3d edgeWorld = edge.EndWorld - edge.BeginWorld;
    const float64 lengthWorld = math::Sqrt(math::Dot(edgeWorld, edgeWorld));
    if (lengthWorld <= 0 || math::IsFinite(lengthWorld) == false)
    {
        return result;
    }
    const math::Vector3d tangent = edgeWorld * (1 / lengthWorld);
    const math::Vector3d sourceOffsetWorld = sourceWorld - edge.BeginWorld;
    const math::Vector3d receiverOffsetWorld = receiverWorld - edge.BeginWorld;
    const float64 sourceAlongWorld = math::Dot(sourceOffsetWorld, tangent);
    const float64 receiverAlongWorld = math::Dot(receiverOffsetWorld, tangent);
    const math::Vector3d sourceRadialWorld = sourceOffsetWorld - tangent * sourceAlongWorld;
    const math::Vector3d receiverRadialWorld = receiverOffsetWorld - tangent * receiverAlongWorld;
    const float64 sourceRadiusWorld = math::Sqrt(math::Dot(sourceRadialWorld, sourceRadialWorld));
    const float64 receiverRadiusWorld = math::Sqrt(math::Dot(receiverRadialWorld, receiverRadialWorld));
    if (sourceRadiusWorld <= 0 || receiverRadiusWorld <= 0)
    {
        return result;
    }
    // Keller's law: unfold the receiver about the edge; the diffraction point divides the axial offset in the
    // ratio of the two radial distances. Endpoints would be corner diffraction, which is omitted.
    const float64 pointAlongWorld = sourceAlongWorld + (receiverAlongWorld - sourceAlongWorld) * sourceRadiusWorld /
                                                           (sourceRadiusWorld + receiverRadiusWorld);
    if (pointAlongWorld <= 0 || pointAlongWorld >= lengthWorld || math::IsFinite(pointAlongWorld) == false)
    {
        return result;
    }
    const math::Vector3d sourceDirection = sourceRadialWorld * (1 / sourceRadiusWorld);
    const math::Vector3d receiverDirection = receiverRadialWorld * (1 / receiverRadiusWorld);
    // Measure both angles from face 0 across the exterior, the sector between the faces that holds the source.
    math::Vector3d firstFace = edge.FaceDirectionsWorld[0];
    float64 exteriorAngle = 2 * math::kPi;
    if (edge.FaceCount == UTD_MAX_EDGE_FACES)
    {
        const float64 between = MeasureAngleAbout(tangent, edge.FaceDirectionsWorld[0], edge.FaceDirectionsWorld[1]);
        const float64 sourceFromFirst = MeasureAngleAbout(tangent, edge.FaceDirectionsWorld[0], sourceDirection);
        if (sourceFromFirst < between)
        {
            exteriorAngle = between;
        }
        else
        {
            firstFace = edge.FaceDirectionsWorld[1];
            exteriorAngle = 2 * math::kPi - between;
        }
    }
    const float64 sourceAngle = MeasureAngleAbout(tangent, firstFace, sourceDirection);
    const float64 receiverAngle = MeasureAngleAbout(tangent, firstFace, receiverDirection);
    if (sourceAngle <= 0 || sourceAngle >= exteriorAngle || receiverAngle <= 0 || receiverAngle >= exteriorAngle)
    {
        return result;
    }
    const float64 wedgeIndex = exteriorAngle / math::kPi;
    // Only silhouette edges, where exactly one face is front-facing, cast an incident shadow boundary. A point
    // sees face 0 below pi and face n above (n - 1) pi; testing both ends keeps the operator reciprocal.
    const bool isSourceSilhouette = (sourceAngle < math::kPi) != (sourceAngle > (wedgeIndex - 1) * math::kPi);
    const bool isReceiverSilhouette = (receiverAngle < math::kPi) != (receiverAngle > (wedgeIndex - 1) * math::kPi);
    if (isSourceSilhouette == false && isReceiverSilhouette == false)
    {
        return result;
    }
    const float64 incidentAlongWorld = pointAlongWorld - sourceAlongWorld;
    const float64 diffractedAlongWorld = receiverAlongWorld - pointAlongWorld;
    const float64 incidentWorld =
        math::Sqrt(sourceRadiusWorld * sourceRadiusWorld + incidentAlongWorld * incidentAlongWorld);
    const float64 diffractedWorld =
        math::Sqrt(receiverRadiusWorld * receiverRadiusWorld + diffractedAlongWorld * diffractedAlongWorld);
    const float64 pathWorld = incidentWorld + diffractedWorld;
    const math::Vector3d directWorld = receiverWorld - sourceWorld;
    const float64 directDistanceWorld = math::Sqrt(math::Dot(directWorld, directWorld));
    const float64 wavenumber = 2 * math::kPi / wavelengthWorld;
    // L = s s' sin^2(beta0) / (s + s'); sin(beta0) = rho' / s'.
    const float64 sineIncidence = sourceRadiusWorld / incidentWorld;
    const float64 distanceParameterWorld = diffractedWorld * incidentWorld * sineIncidence * sineIncidence / pathWorld;
    const float64 distanceWavenumber = wavenumber * distanceParameterWorld;
    const float64 angleDifference = receiverAngle - sourceAngle;
    const float64 period = 2 * math::kPi * wedgeIndex;
    const float64 plusIndex = math::Floor((angleDifference + math::kPi) / period + 0.5);
    const float64 minusIndex = math::Floor((angleDifference - math::kPi) / period + 0.5);
    const UtdTermScale termScale{ .WedgeIndex = wedgeIndex, .DistanceWavenumber = distanceWavenumber };
    const WaveField terms =
        AddFields(EvaluateIncidentTerm(math::kPi + angleDifference - period * plusIndex, termScale),
                  EvaluateIncidentTerm(math::kPi - angleDifference + period * minusIndex, termScale));
    // s + s' - |R - S| = 2 rho rho' (1 + cos(phi - phi')) / (s + s' + |R - S|), with 1 + cos = |u + u'|^2 / 2.
    const math::Vector3d directionSum = sourceDirection + receiverDirection;
    const float64 pathDifferenceWorld = sourceRadiusWorld * receiverRadiusWorld *
                                        math::Dot(directionSum, directionSum) / (pathWorld + directDistanceWorld);
    const float64 cycles = pathDifferenceWorld / wavelengthWorld;
    const WaveField delay = MakePhase(-2 * math::kPi * (cycles - math::Floor(cycles)));
    // -exp(-i pi/4) |R - S| / (s + s') (terms) / (2 n sqrt(2 pi)) exp(-ik(s + s' - |R - S|)).
    const float64 scale = -directDistanceWorld / pathWorld / (2 * wedgeIndex * math::Sqrt(2 * math::kPi));
    const WaveField leading = ScaleField(MakePhase(-EIGHTH_TURN_RADIANS), scale);
    result.Field = MultiplyFields(MultiplyFields(leading, terms), delay);
    result.PointWorld = edge.BeginWorld + tangent * pointAlongWorld;
    result.IsDiffracting = math::IsFinite(result.Field.Real) == true && math::IsFinite(result.Field.Imaginary) == true;
    if (result.IsDiffracting == false)
    {
        result.Field = {};
    }
    return result;
}

rhi::RasterStatus EvaluateUtdConnectionToReceiver(const ReferenceSceneData& scene,
                                                  math::Vector3d sourceWorld,
                                                  math::Vector3d receiverWorld,
                                                  math::Vector3d directionWorld,
                                                  float64 distanceWorld,
                                                  float64 wavelengthWorld,
                                                  WaveField& outField) noexcept
{
    if (math::IsFinite(sourceWorld) == false || math::IsFinite(receiverWorld) == false ||
        math::IsFinite(directionWorld) == false || math::IsFinite(distanceWorld) == false || distanceWorld <= 0 ||
        math::IsFinite(wavelengthWorld) == false || wavelengthWorld < MIN_WAVELENGTH_WORLD ||
        wavelengthWorld / distanceWorld > MAX_WAVELENGTH_TO_DISTANCE ||
        math::Abs(math::Dot(directionWorld, directionWorld) - 1) > UNIT_DIRECTION_TOLERANCE)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    const float64 wavenumber = 2 * math::kPi / wavelengthWorld;
    const float64 sigmaWorldSquared = distanceWorld / wavenumber;
    // The same Gaussian envelope as the angular-spectrum operator bounds which edges this connection sees.
    const float64 halfWidthWorld = LUDUS_WAVE_CONNECTION_TAIL_DEVIATIONS * math::Sqrt(2 * sigmaWorldSquared);
    const float64 coordinateScaleWorld = math::Max(
        distanceWorld,
        math::Max(
            math::Max(math::Abs(sourceWorld.X), math::Max(math::Abs(sourceWorld.Y), math::Abs(sourceWorld.Z))),
            math::Max(math::Abs(receiverWorld.X), math::Max(math::Abs(receiverWorld.Y), math::Abs(receiverWorld.Z)))));
    const float64 endpointEscapeWorld = math::Max(LUDUS_WAVE_CONNECTION_BALLISTIC_WAVELENGTHS * wavelengthWorld,
                                                  LUDUS_WAVE_CONNECTION_ENDPOINT_ROUNDOFF_SCALE * coordinateScaleWorld);
    if (2 * endpointEscapeWorld >= distanceWorld)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    math::EllipticalCone cone;
    cone.OriginWorld = sourceWorld;
    cone.DirectionWorld = directionWorld;
    const math::Vector3d helperWorld =
        math::Abs(directionWorld.Y) < FRAME_HELPER_COSINE ? math::Vector3d{0, 1, 0} : math::Vector3d{1, 0, 0};
    const math::Vector3d crossWorld = math::Cross(helperWorld, directionWorld);
    cone.AxisXWorld = crossWorld * (1 / math::Sqrt(math::Dot(crossWorld, crossWorld)));
    cone.RadiusWorld = halfWidthWorld;
    cone.NearWorld = endpointEscapeWorld;
    cone.FarWorld = distanceWorld - endpointEscapeWorld;
    // Ballistic term: the straight segment between the offset endpoints.
    bool isBlocked = false;
    rhi::RasterStatus status = TraceWaveBallistic(scene, cone, {}, isBlocked);
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    WaveField field{isBlocked == true ? 0.0 : 1.0, 0};
    // Edges at the endpoints' own surfaces lie inside their escapes and are not diffracted.
    WaveOriginEscape escape;
    status = EscapeWaveOrigin(scene, cone, wavelengthWorld, false, escape);
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    if (escape.IsEntirelyBallistic == true)
    {
        outField = field;
        return rhi::RasterStatus::Ready;
    }
    const float64 sourceEscapeWorld = escape.DistanceWorld;
    math::EllipticalCone reverse = cone;
    reverse.OriginWorld = receiverWorld;
    reverse.DirectionWorld = -directionWorld;
    status = EscapeWaveOrigin(scene, reverse, wavelengthWorld, false, escape);
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    if (escape.IsEntirelyBallistic == true)
    {
        outField = field;
        return rhi::RasterStatus::Ready;
    }
    const UtdConnection connection
    {
        .SourceWorld = sourceWorld,
        .ReceiverWorld = receiverWorld,
        .DirectionWorld = directionWorld,
        .WavelengthWorld = wavelengthWorld,
        .EnvelopeRadiusWorld = halfWidthWorld,
        .EndpointEscapeWorld = endpointEscapeWorld,
    };
    cone.NearWorld = sourceEscapeWorld;
    cone.FarWorld = math::Max(cone.NearWorld, distanceWorld - escape.DistanceWorld);
    for (uint32 interaction = 0; interaction < LUDUS_WAVE_CONNECTION_REGIONS; ++interaction)
    {
        if (cone.NearWorld >= cone.FarWorld)
        {
            outField = field;
            return rhi::RasterStatus::Ready;
        }
        WaveInteractionRegion region;
        status = FindWaveInteractionRegion(scene, cone, region);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
        if (region.TriangleCount == 0)
        {
            outField = field;
            return rhi::RasterStatus::Ready;
        }
        status = AddRegionEdgeFields(scene, connection, region, field);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
        const float64 nextNearWorld = math::NextFloat64Up(region.Frustum.FarWorld);
        cone.NearWorld = math::Min(cone.FarWorld, nextNearWorld);
        if (cone.FarWorld - cone.NearWorld > LUDUS_WAVE_CONNECTION_BALLISTIC_WAVELENGTHS * wavelengthWorld)
        {
            // Skip geometry that continues straight past the region, as the angular-spectrum operator does.
            math::EllipticalCone skip = cone;
            skip.OriginWorld = sourceWorld + directionWorld * cone.NearWorld;
            skip.FarWorld = cone.FarWorld - cone.NearWorld;
            skip.NearWorld = math::Min(skip.FarWorld, endpointEscapeWorld);
            status = EscapeWaveOrigin(scene, skip, wavelengthWorld, false, escape);
            if (status != rhi::RasterStatus::Ready)
            {
                return status;
            }
            cone.NearWorld = escape.IsEntirelyBallistic == true
                                 ? cone.FarWorld
                                 : math::Min(cone.FarWorld, cone.NearWorld + escape.DistanceWorld);
        }
    }
    return rhi::RasterStatus::Unsupported;
}
} // namespace ludus::graphics::renderer::internal
