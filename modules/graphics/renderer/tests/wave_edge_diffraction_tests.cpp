#include "../src/internal/wave_edge_diffraction.h"
#include "wave_fresnel_reference.h"
#include <catch2/catch_test_macros.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

// Independent checks of the UTD edge operator. The references below integrate Fresnel integrals by composite
// Simpson quadrature with an asymptotic tail; they share no code or series with the production evaluation.
// Geometry follows the knife-edge, right-angle wedge and double-slit scenes in tests/scenes/diffraction.
namespace
{
using namespace ludus::foundation;
namespace math = ludus::foundation::math;
namespace internal = ludus::graphics::renderer::internal;
namespace rhi = ludus::graphics::rhi;
using ludus::graphics::renderer::WaveField;
using wave_test::EvaluateFresnelEdge;
using wave_test::IntegrateFresnelTail;

constexpr float64 DESIGN_WAVELENGTH_WORLD = 0.005;
constexpr math::Vector3d CENTRED_SOURCE_WORLD{0, 0, 0.5};
constexpr float64 RECEIVER_PLANE_WORLD = -1;
/// Distances from the source to the screen and from the screen to the receiver plane, along z.
constexpr float64 SOURCE_TO_SCREEN_WORLD = 0.5;
constexpr float64 SCREEN_TO_RECEIVER_WORLD = 1;
/// Absolute agreement of G with the reference: both are accurate far below this.
constexpr float64 TRANSITION_TOLERANCE = 1e-8;
/// Knife-edge sweep: Fresnel numbers v over which incident-term UTD and Fresnel-Kirchhoff agree within the
/// relative power tolerance (deep shadow and the first lit fringes).
constexpr float64 KNIFE_EDGE_MINIMUM_V = -1.2;
constexpr float64 KNIFE_EDGE_MAXIMUM_V = 3.5;
constexpr float64 KNIFE_EDGE_RELATIVE_TOLERANCE = 0.015;
constexpr float64 RECEIVER_STEP_WORLD = 0.01;
constexpr int32 RECEIVER_STEPS = 30;
/// Half power at the shadow boundary, with both GO and diffraction evaluated just either side of it.
constexpr float64 BOUNDARY_POWER = 0.25;
constexpr float64 BOUNDARY_OFFSET_WORLD = 1e-7;
constexpr float64 BOUNDARY_TOLERANCE = 1e-3;
/// Reciprocity holds to rounding: the coefficient is symmetric in the source and receiver angles.
constexpr float64 RECIPROCITY_TOLERANCE = 1e-12;

WaveField AddFields(WaveField first, WaveField second)
{
    return {first.Real + second.Real, first.Imaginary + second.Imaginary};
}
float64 MeasurePower(WaveField field)
{
    return field.Real * field.Real + field.Imaginary * field.Imaginary;
}
/// Fresnel-Kirchhoff field ratio through an opening between Fresnel numbers lowV and highV.
WaveField EvaluateFresnelOpening(float64 lowV, float64 highV)
{
    const WaveField low = EvaluateFresnelEdge(lowV);
    const WaveField high = EvaluateFresnelEdge(highV);
    return {low.Real - high.Real, low.Imaginary - high.Imaginary};
}
/// A receiver on the sweep and where its straight line from the centred source crosses the screen.
struct SweepPoint final
{
    float64 LineX = 0;
    float64 ReceiverX = 0;
};
/// Fresnel number of an edge at transverse offset `edgeWorld` from the straight line of `point`, using that line's
/// two path lengths.
float64 ComputeFresnelNumber(float64 edgeWorld, const SweepPoint& point)
{
    const float64 lineX = point.LineX;
    const float64 sourceToScreen = math::Sqrt(lineX * lineX + SOURCE_TO_SCREEN_WORLD * SOURCE_TO_SCREEN_WORLD);
    const float64 receiverOffset = point.ReceiverX - lineX;
    const float64 screenToReceiver =
        math::Sqrt(receiverOffset * receiverOffset + SCREEN_TO_RECEIVER_WORLD * SCREEN_TO_RECEIVER_WORLD);
    return edgeWorld * math::Sqrt(2 * (sourceToScreen + screenToReceiver) /
                                  (DESIGN_WAVELENGTH_WORLD * sourceToScreen * screenToReceiver));
}
/// Where the straight line from the centred source to (x, 0, receiver plane) crosses the screen.
float64 ProjectToScreen(float64 receiverX)
{
    return receiverX * SOURCE_TO_SCREEN_WORLD / (SOURCE_TO_SCREEN_WORLD + SCREEN_TO_RECEIVER_WORLD);
}
/// A straight edge along +y through x at z = 0, bounding a half-plane that extends along faceDirectionWorld.
internal::UtdEdge MakeScreenEdge(float64 x, math::Vector3d faceDirectionWorld)
{
    internal::UtdEdge edge;
    edge.BeginWorld = {x, -1, 0};
    edge.EndWorld = {x, 1, 0};
    edge.FaceDirectionsWorld[0] = faceDirectionWorld;
    edge.FaceCount = 1;
    return edge;
}
void AddTriangle(internal::ReferenceSceneData& inoutScene,
                 math::Vector3 first,
                 math::Vector3 second,
                 math::Vector3 third)
{
    internal::ReferenceTriangle triangle;
    triangle.Positions[0] = first;
    triangle.Positions[1] = second;
    triangle.Positions[2] = third;
    triangle.Area = 1;
    triangle.GeometricNormal = {0, 0, 1};
    REQUIRE(inoutScene.Triangles.TryAdd(triangle) == true);
}
/// A screen rectangle at z = 0 between two x values, as two triangles sharing a flat diagonal.
void AddScreenRectangle(internal::ReferenceSceneData& inoutScene, float32 left, float32 right)
{
    constexpr float32 HALF_HEIGHT_WORLD = 2;
    AddTriangle(inoutScene,
                {left, -HALF_HEIGHT_WORLD, 0},
                {right, -HALF_HEIGHT_WORLD, 0},
                {right, HALF_HEIGHT_WORLD, 0});
    AddTriangle(inoutScene, {left, -HALF_HEIGHT_WORLD, 0}, {right, HALF_HEIGHT_WORLD, 0}, {left, HALF_HEIGHT_WORLD, 0});
}
/// UTD connection from the centred source to (x, 0, receiver plane).
WaveField EvaluateSceneConnection(const internal::ReferenceSceneData& scene, float64 receiverX)
{
    const math::Vector3d receiverWorld{receiverX, 0, RECEIVER_PLANE_WORLD};
    const math::Vector3d deltaWorld = receiverWorld - CENTRED_SOURCE_WORLD;
    const float64 distanceWorld = math::Sqrt(math::Dot(deltaWorld, deltaWorld));
    WaveField field;
    REQUIRE(internal::EvaluateUtdConnectionToReceiver(scene,
                                                      CENTRED_SOURCE_WORLD,
                                                      receiverWorld,
                                                      deltaWorld * (1 / distanceWorld),
                                                      distanceWorld,
                                                      DESIGN_WAVELENGTH_WORLD,
                                                      field) == rhi::RasterStatus::Ready);
    return field;
}
} // namespace

TEST_CASE("The UTD transition ratio matches an independent quadrature and its limits", "[renderer][wave-utd]")
{
    constexpr usize ARGUMENT_COUNT = 12;
    constexpr StaticArray<float64, ARGUMENT_COUNT> ARGUMENTS{{0, 1e-6, 0.01, 0.1, 0.5, 1, 2.2, 2.25, 2.3, 5, 30, 1000}};
    for (const float64 argument : ARGUMENTS)
    {
        CAPTURE(argument);
        // G(X) = 2i exp(iX) times the integral of exp(-i t^2) from sqrt(X) to infinity.
        const WaveField integral = IntegrateFresnelTail(math::Sqrt(argument));
        const WaveField rotated{math::Cos(argument) * integral.Real - math::Sin(argument) * integral.Imaginary,
                                math::Cos(argument) * integral.Imaginary + math::Sin(argument) * integral.Real};
        const WaveField expected{-2 * rotated.Imaginary, 2 * rotated.Real};
        const WaveField actual = internal::EvaluateUtdTransitionRatio(argument);
        CHECK(math::Abs(actual.Real - expected.Real) < TRANSITION_TOLERANCE);
        CHECK(math::Abs(actual.Imaginary - expected.Imaginary) < TRANSITION_TOLERANCE);
    }
    const WaveField origin = internal::EvaluateUtdTransitionRatio(0);
    CHECK(math::Abs(origin.Real - math::Sqrt(math::kPi * 0.5)) < TRANSITION_TOLERANCE);
    CHECK(math::Abs(origin.Imaginary - math::Sqrt(math::kPi * 0.5)) < TRANSITION_TOLERANCE);
    // F(X) = sqrt(X) G(X) tends to 1 + i / (2X).
    constexpr float64 LARGE_ARGUMENT = 1e6;
    const WaveField large = internal::EvaluateUtdTransitionRatio(LARGE_ARGUMENT);
    CHECK(math::Abs(large.Real * math::Sqrt(LARGE_ARGUMENT) - 1) < TRANSITION_TOLERANCE);
    CHECK(math::Abs(large.Imaginary * math::Sqrt(LARGE_ARGUMENT) - 0.5 / LARGE_ARGUMENT) < TRANSITION_TOLERANCE);
}

TEST_CASE("A UTD knife edge matches Fresnel-Kirchhoff diffraction and halves the field at its shadow boundary",
          "[renderer][wave-utd]")
{
    // The screen covers x <= 0 at z = 0; the receiver is lit for x > 0.
    const internal::UtdEdge edge = MakeScreenEdge(0, {-1, 0, 0});
    for (int32 step = -RECEIVER_STEPS; step <= RECEIVER_STEPS; ++step)
    {
        const float64 receiverX = static_cast<float64>(step) * RECEIVER_STEP_WORLD;
        const float64 lineX = ProjectToScreen(receiverX);
        const float64 v = ComputeFresnelNumber(-lineX, {lineX, receiverX});
        if (v < KNIFE_EDGE_MINIMUM_V || v > KNIFE_EDGE_MAXIMUM_V || step == 0)
        {
            continue;
        }
        const internal::UtdEdgeField diffracted = internal::EvaluateUtdEdgeField(edge,
                                                                                 CENTRED_SOURCE_WORLD,
                                                                                 {receiverX, 0, RECEIVER_PLANE_WORLD},
                                                                                 DESIGN_WAVELENGTH_WORLD);
        REQUIRE(diffracted.IsDiffracting == true);
        const WaveField ballistic{lineX > 0 ? 1.0 : 0.0, 0};
        const float64 actual = MeasurePower(AddFields(ballistic, diffracted.Field));
        const float64 expected = MeasurePower(EvaluateFresnelEdge(v));
        CAPTURE(receiverX, v, actual, expected);
        CHECK(math::Abs(actual - expected) < KNIFE_EDGE_RELATIVE_TOLERANCE * expected);
        // The diffraction point lies on the edge where the straight line crosses its y.
        CHECK(math::Abs(diffracted.PointWorld.X) < RECIPROCITY_TOLERANCE);
        CHECK(math::Abs(diffracted.PointWorld.Y) < RECIPROCITY_TOLERANCE);
    }
    for (const float64 side : StaticArray<float64, 2>{{-1, 1}})
    {
        const float64 receiverX = side * BOUNDARY_OFFSET_WORLD;
        const internal::UtdEdgeField diffracted = internal::EvaluateUtdEdgeField(edge,
                                                                                 CENTRED_SOURCE_WORLD,
                                                                                 {receiverX, 0, RECEIVER_PLANE_WORLD},
                                                                                 DESIGN_WAVELENGTH_WORLD);
        const WaveField ballistic{side > 0 ? 1.0 : 0.0, 0};
        CHECK(math::Abs(MeasurePower(AddFields(ballistic, diffracted.Field)) - BOUNDARY_POWER) < BOUNDARY_TOLERANCE);
    }
}

TEST_CASE("A UTD wedge is continuous across its shadow boundary and reciprocal", "[renderer][wave-utd]")
{
    // The right-angle-wedge scene: faces along -z and -x from the +y edge, a 270 degree exterior (n = 3/2).
    internal::UtdEdge edge;
    edge.BeginWorld = {0, -1, 0};
    edge.EndWorld = {0, 1, 0};
    edge.FaceDirectionsWorld[0] = {0, 0, -1};
    edge.FaceDirectionsWorld[1] = {-1, 0, 0};
    edge.FaceCount = 2;
    constexpr math::Vector3d WEDGE_SOURCE_WORLD{-0.5, 0, 0.5};
    // The source centre's direct shadow boundary crosses the receiver plane at x = 1.
    constexpr float64 SHADOW_BOUNDARY_X = 1;
    StaticArray<float64, 2> powers{};
    for (usize side = 0; side < 2; ++side)
    {
        const float64 receiverX = SHADOW_BOUNDARY_X + (side == 0 ? -BOUNDARY_OFFSET_WORLD : BOUNDARY_OFFSET_WORLD);
        const math::Vector3d receiverWorld{receiverX, 0, RECEIVER_PLANE_WORLD};
        const internal::UtdEdgeField forward =
            internal::EvaluateUtdEdgeField(edge, WEDGE_SOURCE_WORLD, receiverWorld, DESIGN_WAVELENGTH_WORLD);
        const internal::UtdEdgeField backward =
            internal::EvaluateUtdEdgeField(edge, receiverWorld, WEDGE_SOURCE_WORLD, DESIGN_WAVELENGTH_WORLD);
        REQUIRE(forward.IsDiffracting == true);
        REQUIRE(backward.IsDiffracting == true);
        CHECK(math::Abs(forward.Field.Real - backward.Field.Real) < RECIPROCITY_TOLERANCE);
        CHECK(math::Abs(forward.Field.Imaginary - backward.Field.Imaginary) < RECIPROCITY_TOLERANCE);
        powers[side] = MeasurePower(AddFields({side == 0 ? 0.0 : 1.0, 0}, forward.Field));
    }
    // The singular term supplies exactly half the ballistic field on either side; the wedge's second incident term
    // adds a small smooth field, so the boundary power is near, not exactly, one quarter.
    constexpr float64 WEDGE_BOUNDARY_TOLERANCE = 0.01;
    CAPTURE(powers[0], powers[1]);
    CHECK(math::Abs(powers[0] - powers[1]) < BOUNDARY_TOLERANCE);
    CHECK(math::Abs(powers[0] - BOUNDARY_POWER) < WEDGE_BOUNDARY_TOLERANCE);
}

TEST_CASE("UTD edges diffract only as exterior silhouettes inside their segment", "[renderer][wave-utd]")
{
    const math::Vector3d receiverWorld{0.1, 0, RECEIVER_PLANE_WORLD};
    internal::UtdEdge edge;
    edge.BeginWorld = {0, -1, 0};
    edge.EndWorld = {0, 1, 0};
    edge.FaceCount = 2;
    SECTION("A flat edge between coplanar faces")
    {
        edge.FaceDirectionsWorld[0] = {-1, 0, 0};
        edge.FaceDirectionsWorld[1] = {1, 0, 0};
        CHECK(internal::EvaluateUtdEdgeField(edge, CENTRED_SOURCE_WORLD, receiverWorld, DESIGN_WAVELENGTH_WORLD)
                  .IsDiffracting == false);
    }
    SECTION("A concave edge")
    {
        // Faces along -z and +x leave a 90 degree exterior that holds the source side.
        edge.FaceDirectionsWorld[0] = {0, 0, 1};
        edge.FaceDirectionsWorld[1] = {1, 0, 0};
        CHECK(internal::EvaluateUtdEdgeField(edge, {0.5, 0, 0.5}, {0.5, 0, 0.25}, DESIGN_WAVELENGTH_WORLD)
                  .IsDiffracting == false);
    }
    SECTION("A receiver inside the wedge")
    {
        // The right-angle wedge occupies x <= 0, z <= 0; its exterior holds the source.
        edge.FaceDirectionsWorld[0] = {0, 0, -1};
        edge.FaceDirectionsWorld[1] = {-1, 0, 0};
        constexpr math::Vector3d WEDGE_SOURCE_WORLD{-0.5, 0, 0.5};
        CHECK(internal::EvaluateUtdEdgeField(edge,
                                             WEDGE_SOURCE_WORLD,
                                             {1, 0, RECEIVER_PLANE_WORLD},
                                             DESIGN_WAVELENGTH_WORLD)
                  .IsDiffracting == true);
        CHECK(internal::EvaluateUtdEdgeField(edge, WEDGE_SOURCE_WORLD, {-0.2, 0, -0.5}, DESIGN_WAVELENGTH_WORLD)
                  .IsDiffracting == false);
    }
    SECTION("A diffraction point past the segment end")
    {
        edge = MakeScreenEdge(0, {-1, 0, 0});
        CHECK(internal::EvaluateUtdEdgeField(edge, {0, 3, 0.5}, {0.1, 3, RECEIVER_PLANE_WORLD}, DESIGN_WAVELENGTH_WORLD)
                  .IsDiffracting == false);
    }
}

TEST_CASE("Scene UTD connections take screen edges from their envelope", "[renderer][wave-utd]")
{
    internal::ReferenceSceneData empty;
    const WaveField open = EvaluateSceneConnection(empty, 0);
    CHECK(open.Real == 1);
    CHECK(open.Imaginary == 0);
    SECTION("Knife edge")
    {
        internal::ReferenceSceneData scene;
        AddScreenRectangle(scene, -2, 0);
        REQUIRE(internal::BuildReferenceBvh(scene) == rhi::RasterStatus::Ready);
        const internal::UtdEdge edge = MakeScreenEdge(0, {-1, 0, 0});
        for (int32 step = -RECEIVER_STEPS; step <= RECEIVER_STEPS; step += 3)
        {
            const float64 receiverX = static_cast<float64>(step) * RECEIVER_STEP_WORLD + RECEIVER_STEP_WORLD * 0.5;
            CAPTURE(receiverX);
            const WaveField actual = EvaluateSceneConnection(scene, receiverX);
            // Only the straight edge diffracts: the shared diagonal is flat, and the outer edges lie outside the
            // envelope. Its single Keller point matches the synthetic edge exactly.
            const internal::UtdEdgeField diffracted =
                internal::EvaluateUtdEdgeField(edge,
                                               CENTRED_SOURCE_WORLD,
                                               {receiverX, 0, RECEIVER_PLANE_WORLD},
                                               DESIGN_WAVELENGTH_WORLD);
            const WaveField expected = AddFields({receiverX > 0 ? 1.0 : 0.0, 0}, diffracted.Field);
            CHECK(math::Abs(actual.Real - expected.Real) < RECIPROCITY_TOLERANCE);
            CHECK(math::Abs(actual.Imaginary - expected.Imaginary) < RECIPROCITY_TOLERANCE);
        }
    }
    SECTION("Double slit")
    {
        // The double-slit scene: two 4 cm openings with centres 20 cm apart. Near the axis the receiver sees only
        // the four edges' fields, which interfere with a fringe spacing of wavelength times screen distance over
        // the separation (25 mm), in step with the Fresnel-Kirchhoff two-slit pattern.
        constexpr float32 OPENING_INNER_WORLD = 0.08F;
        constexpr float32 OPENING_OUTER_WORLD = 0.12F;
        constexpr float32 SCREEN_EXTENT_WORLD = 2;
        internal::ReferenceSceneData scene;
        AddScreenRectangle(scene, -SCREEN_EXTENT_WORLD, -OPENING_OUTER_WORLD);
        AddScreenRectangle(scene, -OPENING_INNER_WORLD, OPENING_INNER_WORLD);
        AddScreenRectangle(scene, OPENING_OUTER_WORLD, SCREEN_EXTENT_WORLD);
        REQUIRE(internal::BuildReferenceBvh(scene) == rhi::RasterStatus::Ready);
        constexpr float64 FRINGE_SPACING_WORLD = 0.025;
        constexpr float64 PATTERN_TOLERANCE = 0.01;
        constexpr float64 MINIMUM_CONTRAST = 10;
        constexpr int32 FRINGE_HALF_STEPS = 4;
        const float64 centre = MeasurePower(EvaluateSceneConnection(scene, 0));
        const float64 dark = MeasurePower(EvaluateSceneConnection(scene, FRINGE_SPACING_WORLD * 0.5));
        const float64 next = MeasurePower(EvaluateSceneConnection(scene, FRINGE_SPACING_WORLD));
        CAPTURE(centre, dark, next);
        CHECK(centre > MINIMUM_CONTRAST * dark);
        CHECK(next > MINIMUM_CONTRAST * dark);
        const float64 innerWorld = static_cast<float64>(OPENING_INNER_WORLD);
        const float64 outerWorld = static_cast<float64>(OPENING_OUTER_WORLD);
        for (int32 step = -FRINGE_HALF_STEPS; step <= FRINGE_HALF_STEPS; ++step)
        {
            const float64 receiverX = static_cast<float64>(step) * FRINGE_SPACING_WORLD * 0.5;
            const float64 lineX = ProjectToScreen(receiverX);
            const WaveField left =
                EvaluateFresnelOpening(ComputeFresnelNumber(-outerWorld - lineX, {lineX, receiverX}),
                                       ComputeFresnelNumber(-innerWorld - lineX, {lineX, receiverX}));
            const WaveField right =
                EvaluateFresnelOpening(ComputeFresnelNumber(innerWorld - lineX, {lineX, receiverX}),
                                       ComputeFresnelNumber(outerWorld - lineX, {lineX, receiverX}));
            const float64 expected = MeasurePower(AddFields(left, right));
            const float64 actual = MeasurePower(EvaluateSceneConnection(scene, receiverX));
            CAPTURE(receiverX, actual, expected);
            CHECK(math::Abs(actual - expected) < PATTERN_TOLERANCE);
        }
    }
    SECTION("Blocked diffracted ray")
    {
        internal::ReferenceSceneData scene;
        AddScreenRectangle(scene, -2, 0);
        // An opaque card wider than the envelope, between the screen and the receiver, blocks the straight ray and
        // the edge's diffracted ray; its own edges lie outside the envelope.
        constexpr float32 CARD_HALF_WIDTH_WORLD = 1;
        constexpr float32 CARD_DEPTH_WORLD = -0.5F;
        AddTriangle(scene,
                    {-CARD_HALF_WIDTH_WORLD, -CARD_HALF_WIDTH_WORLD, CARD_DEPTH_WORLD},
                    {CARD_HALF_WIDTH_WORLD, -CARD_HALF_WIDTH_WORLD, CARD_DEPTH_WORLD},
                    {0, CARD_HALF_WIDTH_WORLD, CARD_DEPTH_WORLD});
        REQUIRE(internal::BuildReferenceBvh(scene) == rhi::RasterStatus::Ready);
        const WaveField field = EvaluateSceneConnection(scene, RECEIVER_STEP_WORLD);
        CHECK(field.Real == 0);
        CHECK(field.Imaginary == 0);
    }
    SECTION("Invalid input preserves the field")
    {
        WaveField field{7, 8};
        CHECK(internal::EvaluateUtdConnectionToReceiver(empty, {0, 0, 0}, {0, 0, 1}, {0, 0, 1}, 1, 0.5, field) ==
              rhi::RasterStatus::InvalidDescription);
        CHECK(field.Real == 7);
        CHECK(field.Imaginary == 8);
    }
}

namespace
{
/// The envelope cone of a straight connection, from its declaration (Steinberg and Pharr, Section 5.1, Eq. 12):
/// footprint 3 * w0 at the source and slope 3 * w0 / L, with w0 = sqrt(L * wavelength / (2 pi)). Computed here
/// independently of MakeWaveEnvelope.
float64 ComputeConeRadius(float64 distanceWorld, float64 depthWorld)
{
    constexpr float64 TAIL_DEVIATIONS = 3;
    const float64 waistWorld = math::Sqrt(distanceWorld * DESIGN_WAVELENGTH_WORLD / (2 * math::kPi));
    return TAIL_DEVIATIONS * waistWorld * (1 + depthWorld / distanceWorld);
}
/// The cylinder the profile used before: the same number of amplitude radii at the receiver, sqrt(2) * w0.
float64 ComputeCylinderRadius(float64 distanceWorld)
{
    return ComputeConeRadius(distanceWorld, 0) * math::Sqrt(2.0);
}
/// A screen at depth z that covers x from left to right, as two triangles sharing a flat diagonal.
void AddScreenRectangleAtDepth(internal::ReferenceSceneData& inoutScene, float32 left, float32 right, float32 z)
{
    constexpr float32 HALF_HEIGHT_WORLD = 2;
    AddTriangle(inoutScene,
                {left, -HALF_HEIGHT_WORLD, z},
                {right, -HALF_HEIGHT_WORLD, z},
                {right, HALF_HEIGHT_WORLD, z});
    AddTriangle(inoutScene, {left, -HALF_HEIGHT_WORLD, z}, {right, HALF_HEIGHT_WORLD, z}, {left, HALF_HEIGHT_WORLD, z});
}
/// Where the connection from the centred source to receiverWorld crosses depth screenZ, and its length.
struct ConnectionCrossing final
{
    float64 DistanceWorld = 0;
    float64 DepthWorld = 0;
    float64 AxisX = 0;
};
ConnectionCrossing CrossConnection(math::Vector3d receiverWorld, float32 screenZ)
{
    const float64 z = static_cast<float64>(screenZ);
    const math::Vector3d deltaWorld = receiverWorld - CENTRED_SOURCE_WORLD;
    const float64 fraction = (CENTRED_SOURCE_WORLD.Z - z) / (CENTRED_SOURCE_WORLD.Z - receiverWorld.Z);
    const float64 distanceWorld = math::Sqrt(math::Dot(deltaWorld, deltaWorld));
    return
    {
        .DistanceWorld = distanceWorld,
        .DepthWorld = fraction * distanceWorld,
        .AxisX = CENTRED_SOURCE_WORLD.X + fraction * deltaWorld.X,
    };
}
} // namespace

TEST_CASE("UTD connections see edges inside an envelope cone that grows along the connection",
          "[renderer][wave-utd][wave-cone]")
{
    // At 5 mm over 1.5 m the waist is w0 = 3.45 cm: the cone's radius grows from 10.4 cm at the source to 20.7 cm at
    // the receiver, through the old cylinder's 14.7 cm at 41% of the way. Receivers stay within 2 cm of the axis so
    // that each edge's Keller point stays on one side of both radii.
    constexpr int32 RECEIVER_HALF_STEPS = 2;
    // Smallest change of the power that the envelope makes, against the field without the edge: measurable with
    // respect to every tolerance of the analytic fixtures above.
    constexpr float64 MEASURABLE_POWER_CHANGE = 0.02;
    SECTION("An edge near the receiver lies inside the grown cone, outside the cylinder")
    {
        constexpr float32 EDGE_X_WORLD = 0.165F;
        constexpr float32 SCREEN_Z_WORLD = -0.7F;
        internal::ReferenceSceneData scene;
        AddScreenRectangleAtDepth(scene, EDGE_X_WORLD, 2, SCREEN_Z_WORLD);
        REQUIRE(internal::BuildReferenceBvh(scene) == rhi::RasterStatus::Ready);
        internal::UtdEdge edge;
        edge.BeginWorld = {static_cast<float64>(EDGE_X_WORLD), -1, static_cast<float64>(SCREEN_Z_WORLD)};
        edge.EndWorld = {static_cast<float64>(EDGE_X_WORLD), 1, static_cast<float64>(SCREEN_Z_WORLD)};
        edge.FaceDirectionsWorld[0] = {1, 0, 0};
        edge.FaceCount = 1;
        float64 largestChange = 0;
        for (int32 step = -RECEIVER_HALF_STEPS; step <= RECEIVER_HALF_STEPS; ++step)
        {
            const float64 receiverX = static_cast<float64>(step) * RECEIVER_STEP_WORLD;
            const ConnectionCrossing crossing = CrossConnection({receiverX, 0, RECEIVER_PLANE_WORLD}, SCREEN_Z_WORLD);
            const float64 radialWorld = static_cast<float64>(EDGE_X_WORLD) - crossing.AxisX;
            CAPTURE(receiverX, radialWorld);
            REQUIRE(radialWorld > ComputeCylinderRadius(crossing.DistanceWorld));
            REQUIRE(radialWorld < ComputeConeRadius(crossing.DistanceWorld, crossing.DepthWorld));
            const WaveField actual = EvaluateSceneConnection(scene, receiverX);
            const internal::UtdEdgeField diffracted =
                internal::EvaluateUtdEdgeField(edge,
                                               CENTRED_SOURCE_WORLD,
                                               {receiverX, 0, RECEIVER_PLANE_WORLD},
                                               DESIGN_WAVELENGTH_WORLD);
            REQUIRE(diffracted.IsDiffracting == true);
            const WaveField expected = AddFields({1, 0}, diffracted.Field);
            CHECK(math::Abs(actual.Real - expected.Real) < RECIPROCITY_TOLERANCE);
            CHECK(math::Abs(actual.Imaginary - expected.Imaginary) < RECIPROCITY_TOLERANCE);
            largestChange = math::Max(largestChange, math::Abs(MeasurePower(actual) - 1));
        }
        CAPTURE(largestChange);
        CHECK(largestChange > MEASURABLE_POWER_CHANGE);
    }
    SECTION("An edge near the source lies outside the narrow cone, inside the cylinder")
    {
        constexpr float32 EDGE_X_WORLD = -0.135F;
        constexpr float32 SCREEN_Z_WORLD = 0.2F;
        internal::ReferenceSceneData scene;
        AddScreenRectangleAtDepth(scene, -2, EDGE_X_WORLD, SCREEN_Z_WORLD);
        REQUIRE(internal::BuildReferenceBvh(scene) == rhi::RasterStatus::Ready);
        internal::UtdEdge edge;
        edge.BeginWorld = {static_cast<float64>(EDGE_X_WORLD), -1, static_cast<float64>(SCREEN_Z_WORLD)};
        edge.EndWorld = {static_cast<float64>(EDGE_X_WORLD), 1, static_cast<float64>(SCREEN_Z_WORLD)};
        edge.FaceDirectionsWorld[0] = {-1, 0, 0};
        edge.FaceCount = 1;
        float64 largestCylinderChange = 0;
        for (int32 step = -RECEIVER_HALF_STEPS; step <= RECEIVER_HALF_STEPS; ++step)
        {
            const float64 receiverX = static_cast<float64>(step) * RECEIVER_STEP_WORLD;
            const ConnectionCrossing crossing = CrossConnection({receiverX, 0, RECEIVER_PLANE_WORLD}, SCREEN_Z_WORLD);
            const float64 radialWorld = crossing.AxisX - static_cast<float64>(EDGE_X_WORLD);
            CAPTURE(receiverX, radialWorld);
            REQUIRE(radialWorld < ComputeCylinderRadius(crossing.DistanceWorld));
            REQUIRE(radialWorld > ComputeConeRadius(crossing.DistanceWorld, crossing.DepthWorld));
            const WaveField actual = EvaluateSceneConnection(scene, receiverX);
            CHECK(actual.Real == 1);
            CHECK(actual.Imaginary == 0);
            // The field the cylinder admitted for the same edge.
            const internal::UtdEdgeField diffracted =
                internal::EvaluateUtdEdgeField(edge,
                                               CENTRED_SOURCE_WORLD,
                                               {receiverX, 0, RECEIVER_PLANE_WORLD},
                                               DESIGN_WAVELENGTH_WORLD);
            REQUIRE(diffracted.IsDiffracting == true);
            largestCylinderChange =
                math::Max(largestCylinderChange, math::Abs(MeasurePower(AddFields({1, 0}, diffracted.Field)) - 1));
        }
        CAPTURE(largestCylinderChange);
        CHECK(largestCylinderChange > MEASURABLE_POWER_CHANGE);
    }
}
