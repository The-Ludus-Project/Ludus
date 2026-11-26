#include "../src/internal/wave_fraunhofer_diffraction.h"
#include <catch2/catch_test_macros.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

// Independent checks of the Fraunhofer edge operator. The reference is the obstacle's area integral itself: the
// connection's Gaussian beam (waist Sigma^2 = D / k at the source) observed on the axis behind a thin screen gives
// U / U0 = 1 - (beta / 2pi) * integral over the obstacle of exp(-beta rho^2 / 2), beta = 1 / (Sigma^2 + i z / k)
// - i k / (D - z). Rectangles separate into products of one-dimensional integrals, evaluated here by composite
// Simpson quadrature; the reference shares no edge decomposition, segmentation or closed form with the operator.
// Geometry follows the knife-edge and double-slit scenes in tests/scenes/diffraction.
namespace
{
using namespace ludus::foundation;
namespace math = ludus::foundation::math;
namespace internal = ludus::graphics::renderer::internal;
namespace rhi = ludus::graphics::rhi;
using ludus::graphics::renderer::WaveField;

constexpr float64 DESIGN_WAVELENGTH_WORLD = 0.005;
constexpr float64 SOURCE_Z_WORLD = 0.5;
constexpr float64 RECEIVER_Z_WORLD = -1;
/// Source-to-receiver distance and source-to-screen depth of the z = 0 screen.
constexpr float64 DISTANCE_WORLD = SOURCE_Z_WORLD - RECEIVER_Z_WORLD;
constexpr float64 SCREEN_DEPTH_WORLD = SOURCE_Z_WORLD;
/// Simpson intervals over each reference integral, truncated where the envelope falls below exp(-TAIL_EXPONENT).
constexpr usize SIMPSON_INTERVALS = 20000;
constexpr float64 TAIL_EXPONENT = 30;
/// Field agreement with the area integral. On these geometries the largest component difference is 0.0018,
/// from the segments' linear amplitude and phase; it shrinks quadratically with LUDUS_WAVE_FRAUNHOFER_SEGMENT_SCALE.
constexpr float64 FIELD_TOLERANCE = 0.003;
/// Reordering the same edges changes the sum only by rounding.
constexpr float64 ROUNDING_TOLERANCE = 1e-12;
/// Half height of the screen rectangles: in scene vertices, and the same extent in frame coordinates.
constexpr float32 SCREEN_HALF_HEIGHT_WORLD = 2;
constexpr float64 SCREEN_HALF_HEIGHT_FRAME = 2;
/// Upper end of a half plane's open side: the kernel's envelope is below exp(-TAIL_EXPONENT) well before it.
constexpr float64 OPEN_END_WORLD = 1;

struct ComplexValue final
{
    float64 Real = 0;
    float64 Imaginary = 0;
};
ComplexValue Multiply(ComplexValue first, ComplexValue second)
{
    return {first.Real * second.Real - first.Imaginary * second.Imaginary,
            first.Real * second.Imaginary + first.Imaginary * second.Real};
}
/// beta of the beam kernel at the screen depth.
ComplexValue ComputeBeta()
{
    const float64 wavenumber = 2 * math::kPi / DESIGN_WAVELENGTH_WORLD;
    const float64 sigmaWorldSquared = DISTANCE_WORLD / wavenumber;
    const float64 correlation = SCREEN_DEPTH_WORLD / wavenumber;
    const float64 denominator = sigmaWorldSquared * sigmaWorldSquared + correlation * correlation;
    return {sigmaWorldSquared / denominator,
            -correlation / denominator - wavenumber / (DISTANCE_WORLD - SCREEN_DEPTH_WORLD)};
}
internal::FraunhoferBeam MakeBeam()
{
    const float64 wavenumber = 2 * math::kPi / DESIGN_WAVELENGTH_WORLD;
    return
    {
        .Wavenumber = wavenumber,
        .SigmaWorldSquared = DISTANCE_WORLD / wavenumber,
        .DistanceWorld = DISTANCE_WORLD,
    };
}
/// Integral of exp(-beta x^2 / 2) over [begin, end], by Simpson quadrature over the part where it is resolvable.
ComplexValue IntegrateKernel(float64 begin, float64 end, ComplexValue beta)
{
    const float64 limit = math::Sqrt(2 * TAIL_EXPONENT / beta.Real);
    const float64 low = math::Max(begin, -limit);
    const float64 high = math::Min(end, limit);
    if (low >= high)
    {
        return {};
    }
    const float64 step = (high - low) / static_cast<float64>(SIMPSON_INTERVALS);
    ComplexValue sum{};
    for (usize index = 0; index <= SIMPSON_INTERVALS; ++index)
    {
        const float64 x = low + static_cast<float64>(index) * step;
        const float64 weight = index == 0 || index == SIMPSON_INTERVALS ? 1 : (index % 2 == 1 ? 4 : 2);
        const float64 magnitude = math::Exp(-beta.Real * x * x * 0.5);
        const float64 phase = -beta.Imaginary * x * x * 0.5;
        sum.Real += weight * magnitude * math::Cos(phase);
        sum.Imaginary += weight * magnitude * math::Sin(phase);
    }
    return {sum.Real * step / 3, sum.Imaginary * step / 3};
}
/// Principal square root of a complex number.
ComplexValue ComputeSquareRoot(ComplexValue value)
{
    const float64 magnitude = math::Sqrt(math::Sqrt(value.Real * value.Real + value.Imaginary * value.Imaginary));
    const float64 angle = math::Atan2(value.Imaginary, value.Real) * 0.5;
    return {magnitude * math::Cos(angle), magnitude * math::Sin(angle)};
}
/// An axis-aligned obstacle rectangle in the connection frame.
struct FrameRectangle final
{
    float64 Left = 0;
    float64 Right = 0;
    float64 Bottom = 0;
    float64 Top = 0;
};
/// U / U0 behind rectangular obstacles: one minus (beta / 2pi) times their separable area integrals.
WaveField EvaluateRectanglesReference(const FrameRectangle* rectangles, usize count)
{
    const ComplexValue beta = ComputeBeta();
    const ComplexValue scale{beta.Real / (2 * math::kPi), beta.Imaginary / (2 * math::kPi)};
    WaveField field{1, 0};
    for (usize index = 0; index < count; ++index)
    {
        const ComplexValue area = Multiply(IntegrateKernel(rectangles[index].Left, rectangles[index].Right, beta),
                                           IntegrateKernel(rectangles[index].Bottom, rectangles[index].Top, beta));
        const ComplexValue blocked = Multiply(scale, area);
        field.Real -= blocked.Real;
        field.Imaginary -= blocked.Imaginary;
    }
    return field;
}
/// U / U0 behind the half plane x < edge: sqrt(beta / 2pi) times the integral of the open part.
WaveField EvaluateHalfPlaneReference(float64 edgeX)
{
    const ComplexValue beta = ComputeBeta();
    const ComplexValue root = ComputeSquareRoot({beta.Real / (2 * math::kPi), beta.Imaginary / (2 * math::kPi)});
    const ComplexValue open = Multiply(root, IntegrateKernel(edgeX, OPEN_END_WORLD, beta));
    return {open.Real, open.Imaginary};
}
WaveField EvaluateEdgesField(const internal::FraunhoferEdge* edges, usize count, bool isAxisBlocked)
{
    WaveField field{isAxisBlocked == true ? 0.0 : 1.0, 0};
    for (usize index = 0; index < count; ++index)
    {
        WaveField edgeField{};
        REQUIRE(internal::EvaluateFraunhoferEdgeField(edges[index], MakeBeam(), isAxisBlocked, edgeField) ==
                rhi::RasterStatus::Ready);
        field.Real += edgeField.Real;
        field.Imaginary += edgeField.Imaginary;
    }
    return field;
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
/// The x extent of a screen rectangle at z = 0.
struct ScreenSpan final
{
    float32 Left = 0;
    float32 Right = 0;
};
/// A screen rectangle at z = 0 over `span`, as two triangles sharing a flat diagonal; isReversed winds both
/// triangles the other way.
void AddScreenRectangle(internal::ReferenceSceneData& inoutScene, ScreenSpan span, bool isReversed)
{
    const float32 left = span.Left;
    const float32 right = span.Right;
    const math::Vector3 lowLeft{left, -SCREEN_HALF_HEIGHT_WORLD, 0};
    const math::Vector3 lowRight{right, -SCREEN_HALF_HEIGHT_WORLD, 0};
    const math::Vector3 highRight{right, SCREEN_HALF_HEIGHT_WORLD, 0};
    const math::Vector3 highLeft{left, SCREEN_HALF_HEIGHT_WORLD, 0};
    if (isReversed == true)
    {
        AddTriangle(inoutScene, lowLeft, highRight, lowRight);
        AddTriangle(inoutScene, lowLeft, highLeft, highRight);
        return;
    }
    AddTriangle(inoutScene, lowLeft, lowRight, highRight);
    AddTriangle(inoutScene, lowLeft, highRight, highLeft);
}
/// Fraunhofer connection along -z from (x, 0, SOURCE_Z_WORLD) to (x, 0, RECEIVER_Z_WORLD).
WaveField EvaluateSceneConnection(const internal::ReferenceSceneData& scene, float64 axisX)
{
    const math::Vector3d sourceWorld{axisX, 0, SOURCE_Z_WORLD};
    const math::Vector3d receiverWorld{axisX, 0, RECEIVER_Z_WORLD};
    WaveField field;
    REQUIRE(internal::EvaluateFraunhoferConnectionToReceiver(scene,
                                                             sourceWorld,
                                                             receiverWorld,
                                                             {0, 0, -1},
                                                             DISTANCE_WORLD,
                                                             DESIGN_WAVELENGTH_WORLD,
                                                             field) == rhi::RasterStatus::Ready);
    return field;
}
} // namespace

TEST_CASE("A Fraunhofer knife edge matches the Gaussian-beam area integral and halves the field at its boundary",
          "[renderer][wave-fraunhofer]")
{
    // A vertical edge at frame x = edgeX, obstacle to its left (x < edgeX), long enough to leave the envelope.
    constexpr int32 EDGE_STEPS = 16;
    constexpr float64 EDGE_STEP_WORLD = 0.006;
    for (int32 step = -EDGE_STEPS; step <= EDGE_STEPS; ++step)
    {
        const float64 edgeX = static_cast<float64>(step) * EDGE_STEP_WORLD;
        CAPTURE(edgeX);
        const internal::FraunhoferEdge edge
        {
            .BeginFrustum = { .X = edgeX, .Y = -SCREEN_HALF_HEIGHT_FRAME },
            .EndFrustum = { .X = edgeX, .Y = SCREEN_HALF_HEIGHT_FRAME },
            .BeginDepthWorld = SCREEN_DEPTH_WORLD,
            .EndDepthWorld = SCREEN_DEPTH_WORLD,
            .IsObstacleLeft = true,
        };
        const WaveField expected = EvaluateHalfPlaneReference(edgeX);
        // The axis is blocked when it lies inside the obstacle; at edgeX = 0 both choices meet at one half.
        for (const bool isAxisBlocked : StaticArray<bool, 2>{{false, true}})
        {
            if (step != 0 && isAxisBlocked != (edgeX > 0))
            {
                continue;
            }
            const WaveField actual = EvaluateEdgesField(&edge, 1, isAxisBlocked);
            CAPTURE(isAxisBlocked, actual.Real, actual.Imaginary, expected.Real, expected.Imaginary);
            CHECK(math::Abs(actual.Real - expected.Real) < FIELD_TOLERANCE);
            CHECK(math::Abs(actual.Imaginary - expected.Imaginary) < FIELD_TOLERANCE);
            if (step == 0)
            {
                CHECK(math::Abs(actual.Real - 0.5) < ROUNDING_TOLERANCE);
                CHECK(math::Abs(actual.Imaginary) < ROUNDING_TOLERANCE);
            }
        }
    }
}

TEST_CASE("Fraunhofer edge waves reproduce rectangular obstacles and their corners", "[renderer][wave-fraunhofer]")
{
    // Counterclockwise rectangles, so the obstacle lies to the left of every edge.
    constexpr usize RECTANGLE_COUNT = 5;
    constexpr StaticArray<FrameRectangle, RECTANGLE_COUNT> RECTANGLES{{
        { .Left = 0.01, .Right = 0.05, .Bottom = -0.02, .Top = 0.03 },
        { .Left = -0.03, .Right = 0.02, .Bottom = -0.04, .Top = 0.01 },
        { .Left = -0.07, .Right = -0.01, .Bottom = 0.02, .Top = 0.08 },
        { .Left = -0.005, .Right = 0.004, .Bottom = -0.006, .Top = 0.003 },
        { .Left = 0.002, .Right = 0.09, .Bottom = -0.09, .Top = 0.09 },
    }};
    for (const FrameRectangle& rectangle : RECTANGLES)
    {
        CAPTURE(rectangle.Left, rectangle.Right, rectangle.Bottom, rectangle.Top);
        constexpr usize CORNER_COUNT = 4;
        const StaticArray<internal::ProjectedPointFrustum, CORNER_COUNT> corners{{
            { .X = rectangle.Left, .Y = rectangle.Bottom },
            { .X = rectangle.Right, .Y = rectangle.Bottom },
            { .X = rectangle.Right, .Y = rectangle.Top },
            { .X = rectangle.Left, .Y = rectangle.Top },
        }};
        StaticArray<internal::FraunhoferEdge, CORNER_COUNT> edges{};
        for (usize corner = 0; corner < CORNER_COUNT; ++corner)
        {
            edges[corner] =
            {
                .BeginFrustum = corners[corner],
                .EndFrustum = corners[(corner + 1) % CORNER_COUNT],
                .BeginDepthWorld = SCREEN_DEPTH_WORLD,
                .EndDepthWorld = SCREEN_DEPTH_WORLD,
                .IsObstacleLeft = true,
            };
        }
        const bool isAxisBlocked =
            rectangle.Left < 0 && 0 < rectangle.Right && rectangle.Bottom < 0 && 0 < rectangle.Top;
        const WaveField actual = EvaluateEdgesField(edges.GetData(), CORNER_COUNT, isAxisBlocked);
        const WaveField expected = EvaluateRectanglesReference(&rectangle, 1);
        CAPTURE(actual.Real, actual.Imaginary, expected.Real, expected.Imaginary);
        CHECK(math::Abs(actual.Real - expected.Real) < FIELD_TOLERANCE);
        CHECK(math::Abs(actual.Imaginary - expected.Imaginary) < FIELD_TOLERANCE);
    }
}

TEST_CASE("Fraunhofer edges reject invalid beams and depths", "[renderer][wave-fraunhofer]")
{
    const internal::FraunhoferEdge valid
    {
        .BeginFrustum = { .X = 0.01, .Y = -1 },
        .EndFrustum = { .X = 0.01, .Y = 1 },
        .BeginDepthWorld = SCREEN_DEPTH_WORLD,
        .EndDepthWorld = SCREEN_DEPTH_WORLD,
        .IsObstacleLeft = true,
    };
    WaveField field{2, 3};
    internal::FraunhoferEdge edge = valid;
    edge.EndDepthWorld = DISTANCE_WORLD;
    CHECK(internal::EvaluateFraunhoferEdgeField(edge, MakeBeam(), false, field) ==
          rhi::RasterStatus::InvalidDescription);
    edge = valid;
    edge.BeginDepthWorld = 0;
    CHECK(internal::EvaluateFraunhoferEdgeField(edge, MakeBeam(), false, field) ==
          rhi::RasterStatus::InvalidDescription);
    internal::FraunhoferBeam beam = MakeBeam();
    beam.SigmaWorldSquared = 0;
    CHECK(internal::EvaluateFraunhoferEdgeField(valid, beam, false, field) == rhi::RasterStatus::InvalidDescription);
    CHECK(field.Real == 2);
    CHECK(field.Imaginary == 3);
    // An edge seen end on projects to a point and diffracts nothing.
    edge = valid;
    edge.EndFrustum = edge.BeginFrustum;
    edge.EndDepthWorld = SCREEN_DEPTH_WORLD * 0.5;
    REQUIRE(internal::EvaluateFraunhoferEdgeField(edge, MakeBeam(), false, field) == rhi::RasterStatus::Ready);
    CHECK(field.Real == 0);
    CHECK(field.Imaginary == 0);
}

TEST_CASE("Scene Fraunhofer connections diffract at the boundary of the projected screen",
          "[renderer][wave-fraunhofer]")
{
    internal::ReferenceSceneData empty;
    const WaveField open = EvaluateSceneConnection(empty, 0);
    CHECK(open.Real == 1);
    CHECK(open.Imaginary == 0);
    constexpr int32 AXIS_STEPS = 12;
    constexpr float64 AXIS_STEP_WORLD = 0.0075;
    SECTION("Knife edge in either winding")
    {
        internal::ReferenceSceneData scene;
        AddScreenRectangle(scene, { .Left = -2, .Right = 0 }, false);
        REQUIRE(internal::BuildReferenceBvh(scene) == rhi::RasterStatus::Ready);
        internal::ReferenceSceneData reversed;
        AddScreenRectangle(reversed, { .Left = -2, .Right = 0 }, true);
        REQUIRE(internal::BuildReferenceBvh(reversed) == rhi::RasterStatus::Ready);
        for (int32 step = -AXIS_STEPS; step <= AXIS_STEPS; ++step)
        {
            const float64 axisX = (static_cast<float64>(step) + 0.5) * AXIS_STEP_WORLD;
            CAPTURE(axisX);
            const WaveField actual = EvaluateSceneConnection(scene, axisX);
            // The screen covers world x < 0, that is x < -axisX relative to the axis.
            const WaveField expected = EvaluateHalfPlaneReference(-axisX);
            CAPTURE(actual.Real, actual.Imaginary, expected.Real, expected.Imaginary);
            CHECK(math::Abs(actual.Real - expected.Real) < FIELD_TOLERANCE);
            CHECK(math::Abs(actual.Imaginary - expected.Imaginary) < FIELD_TOLERANCE);
            // Back faces bound the projected obstacle exactly as front faces do.
            const WaveField mirrored = EvaluateSceneConnection(reversed, axisX);
            CHECK(math::Abs(mirrored.Real - actual.Real) < ROUNDING_TOLERANCE);
            CHECK(math::Abs(mirrored.Imaginary - actual.Imaginary) < ROUNDING_TOLERANCE);
        }
    }
    SECTION("Double slit")
    {
        // Two 4 cm openings 20 cm apart, as in the double-slit scene; the outer screen edges leave the envelope.
        constexpr usize PART_COUNT = 3;
        constexpr StaticArray<float32, PART_COUNT * 2> PART_EDGES{{-2, -0.12F, -0.08F, 0.08F, 0.12F, 2}};
        internal::ReferenceSceneData scene;
        for (usize part = 0; part < PART_COUNT; ++part)
        {
            AddScreenRectangle(scene, { .Left = PART_EDGES[2 * part], .Right = PART_EDGES[2 * part + 1] }, false);
        }
        REQUIRE(internal::BuildReferenceBvh(scene) == rhi::RasterStatus::Ready);
        for (int32 step = -AXIS_STEPS * 2; step <= AXIS_STEPS * 2; ++step)
        {
            const float64 axisX = (static_cast<float64>(step) + 0.5) * AXIS_STEP_WORLD;
            CAPTURE(axisX);
            StaticArray<FrameRectangle, PART_COUNT> parts{};
            for (usize part = 0; part < PART_COUNT; ++part)
            {
                parts[part] =
                {
                    .Left = static_cast<float64>(PART_EDGES[2 * part]) - axisX,
                    .Right = static_cast<float64>(PART_EDGES[2 * part + 1]) - axisX,
                    .Bottom = -SCREEN_HALF_HEIGHT_FRAME,
                    .Top = SCREEN_HALF_HEIGHT_FRAME,
                };
            }
            const WaveField actual = EvaluateSceneConnection(scene, axisX);
            const WaveField expected = EvaluateRectanglesReference(parts.GetData(), PART_COUNT);
            CAPTURE(actual.Real, actual.Imaginary, expected.Real, expected.Imaginary);
            CHECK(math::Abs(actual.Real - expected.Real) < FIELD_TOLERANCE);
            CHECK(math::Abs(actual.Imaginary - expected.Imaginary) < FIELD_TOLERANCE);
        }
    }
}
