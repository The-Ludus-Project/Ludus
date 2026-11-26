#include "../src/internal/wave_fraunhofer_diffraction.h"
#include "wave_fresnel_reference.h"
#include <catch2/catch_test_macros.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

// Checks of optical connections relayed through specular interface events (Steinberg and Pharr, "Wave Tracing",
// Sections 4.2 and 5.2). A mirror relay is an isometry: it must equal the straight connection to the light's mirror
// image, with the far leg's geometry mirrored back. A glass leg propagates paraxially over its reduced length l / n:
// it must equal the straight connection in air with that leg shortened, and a knife edge seen through glass must
// match Fresnel-Kirchhoff diffraction at reduced distances. Classical surface scattering, which squares the field at
// every vertex, sees only the ballistic step there.
namespace
{
using namespace ludus::foundation;
namespace math = ludus::foundation::math;
namespace internal = ludus::graphics::renderer::internal;
namespace rhi = ludus::graphics::rhi;
using ludus::graphics::renderer::WaveField;

/// The lattice operator's test wavelength (as in the existing connection checks) and the edge operators' 5 mm design
/// wavelength.
constexpr float64 LATTICE_WAVELENGTH_WORLD = 0.01;
constexpr float64 EDGE_WAVELENGTH_WORLD = 0.005;
/// Isometric unfolding reproduces the straight connection to rounding.
constexpr float64 UNFOLDING_TOLERANCE = 1e-9;
/// UTD evaluates exact (non-paraxial) geometry around the virtual endpoints of a glass leg, so its reduced-distance
/// equivalence holds to second order in the diffraction angle (below 0.03 here).
constexpr float64 UTD_REDUCED_TOLERANCE = 2e-3;
/// Relative power agreement of UTD with Fresnel-Kirchhoff on the admitted knife-edge Fresnel numbers, as for the
/// straight knife edge.
constexpr float64 KNIFE_EDGE_RELATIVE_TOLERANCE = 0.015;
constexpr float64 KNIFE_EDGE_MINIMUM_V = -1.2;
constexpr float64 KNIFE_EDGE_MAXIMUM_V = 3.5;
/// Coherent relays must differ measurably from the classical step and from relays that ignore the glass index.
constexpr float64 MEASURABLE_POWER_DIFFERENCE = 0.02;
/// Half extent of the obstacle rectangles, outside every test envelope.
constexpr float64 FAR_EDGE_WORLD = 2;

enum class Operator : uint8
{
    AngularSpectrum,
    UtdEdgeDiffraction,
    FraunhoferEdgeDiffraction,
};
constexpr StaticArray<Operator, 3> OPERATORS{
    {Operator::AngularSpectrum, Operator::UtdEdgeDiffraction, Operator::FraunhoferEdgeDiffraction}};

float64 GetWavelength(Operator connection)
{
    return connection == Operator::AngularSpectrum ? LATTICE_WAVELENGTH_WORLD : EDGE_WAVELENGTH_WORLD;
}
float64 MeasurePower(WaveField field)
{
    return field.Real * field.Real + field.Imaginary * field.Imaginary;
}
math::Vector3 NarrowVector(math::Vector3d value)
{
    return {static_cast<float32>(value.X), static_cast<float32>(value.Y), static_cast<float32>(value.Z)};
}
constexpr usize TRIANGLE_CORNERS = 3;
using TriangleCorners = StaticArray<math::Vector3d, TRIANGLE_CORNERS>;
void AddTriangle(internal::ReferenceSceneData& inoutScene, const TriangleCorners& cornersWorld)
{
    internal::ReferenceTriangle triangle;
    for (usize corner = 0; corner < TRIANGLE_CORNERS; ++corner)
    {
        triangle.Positions[corner] = NarrowVector(cornersWorld[corner]);
    }
    triangle.Area = 1;
    triangle.GeometricNormal = {0, 0, 1};
    REQUIRE(inoutScene.Triangles.TryAdd(triangle) == true);
}
/// Maps the frame of a straight connection along +z to the world.
using PlaceFrame = math::Vector3d (*)(math::Vector3d positionFrame);
math::Vector3d PlaceIdentity(math::Vector3d positionFrame)
{
    return positionFrame;
}
/// An axis-aligned rectangle in the frame's plane z, between (left, bottom) and (right, top), placed in the world.
void AddRectangle(internal::ReferenceSceneData& inoutScene,
                  float64 left,
                  float64 right,
                  float64 bottom,
                  float64 top,
                  float64 z,
                  PlaceFrame place)
{
    AddTriangle(inoutScene, {{place({left, bottom, z}), place({right, bottom, z}), place({right, top, z})}});
    AddTriangle(inoutScene, {{place({left, bottom, z}), place({right, top, z}), place({left, top, z})}});
}
/// A half plane covering x >= edge (frame coordinates) at depth z.
void AddKnifeEdge(internal::ReferenceSceneData& inoutScene, float64 edge, float64 z, PlaceFrame place)
{
    AddRectangle(inoutScene, edge, FAR_EDGE_WORLD, -FAR_EDGE_WORLD, FAR_EDGE_WORLD, z, place);
}
WaveField EvaluateChain(const internal::ReferenceSceneData& scene,
                        const internal::WaveConnectionChain& chain,
                        Operator connection)
{
    WaveField field{7, 8};
    rhi::RasterStatus status = rhi::RasterStatus::Failed;
    switch (connection)
    {
        case Operator::AngularSpectrum:
            status = internal::EvaluateWaveChainConnection(scene, chain, GetWavelength(connection), field);
            break;
        case Operator::UtdEdgeDiffraction:
            status = internal::EvaluateUtdChainConnection(scene, chain, GetWavelength(connection), field);
            break;
        case Operator::FraunhoferEdgeDiffraction:
            status = internal::EvaluateFraunhoferChainConnection(scene, chain, GetWavelength(connection), field);
            break;
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
    return field;
}
/// The straight connection from the origin to (0, 0, distance), through the production one-leg entry points.
WaveField EvaluateStraight(const internal::ReferenceSceneData& scene, float64 distanceWorld, Operator connection)
{
    const math::Vector3d sourceWorld{0, 0, 0};
    const math::Vector3d receiverWorld{0, 0, distanceWorld};
    const math::Vector3d directionWorld{0, 0, 1};
    const float64 wavelengthWorld = GetWavelength(connection);
    WaveField field{7, 8};
    rhi::RasterStatus status = rhi::RasterStatus::Failed;
    switch (connection)
    {
        case Operator::AngularSpectrum:
            status = internal::EvaluateWaveConnectionToReceiver(scene,
                                                                sourceWorld,
                                                                receiverWorld,
                                                                directionWorld,
                                                                distanceWorld,
                                                                wavelengthWorld,
                                                                field);
            break;
        case Operator::UtdEdgeDiffraction:
            status = internal::EvaluateUtdConnectionToReceiver(scene,
                                                               sourceWorld,
                                                               receiverWorld,
                                                               directionWorld,
                                                               distanceWorld,
                                                               wavelengthWorld,
                                                               field);
            break;
        case Operator::FraunhoferEdgeDiffraction:
            status = internal::EvaluateFraunhoferConnectionToReceiver(scene,
                                                                      sourceWorld,
                                                                      receiverWorld,
                                                                      directionWorld,
                                                                      distanceWorld,
                                                                      wavelengthWorld,
                                                                      field);
            break;
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
    return field;
}
void CheckFieldsEqual(WaveField actual, WaveField expected, float64 tolerance)
{
    CAPTURE(actual.Real, actual.Imaginary, expected.Real, expected.Imaginary);
    CHECK(math::Abs(actual.Real - expected.Real) < tolerance);
    CHECK(math::Abs(actual.Imaginary - expected.Imaginary) < tolerance);
}

// Mirror relay: the light at the origin shines along +z onto a mirror at (0, 0, 2) whose normal is (-1, 0, 1) / sqrt 2,
// which turns the beam to +x towards the receiver at (2, 0, 2). Unfolded, the receiver is the mirror image (0, 0, 4).
constexpr float64 MIRROR_DEPTH_WORLD = 2;
constexpr float64 MIRROR_LEG_WORLD = 2;
constexpr float64 UNFOLDED_DISTANCE_WORLD = MIRROR_DEPTH_WORLD + MIRROR_LEG_WORLD;
constexpr float64 NEAR_SCREEN_DEPTH_WORLD = 1;
constexpr float64 FAR_SCREEN_DEPTH_WORLD = 3;
/// Reflection of an unfolded point beyond the mirror back into the world.
math::Vector3d PlaceBeyondMirror(math::Vector3d positionFrame)
{
    // With n = (-1, 0, 1) / sqrt 2 through (0, 0, 2): p - 2 ((p - m) . n) n = (2 - z + x... ) reduces to this map.
    return {positionFrame.Z - MIRROR_DEPTH_WORLD, positionFrame.Y, MIRROR_DEPTH_WORLD + positionFrame.X};
}
internal::WaveConnectionChain MakeMirrorChain()
{
    internal::WaveConnectionChain chain;
    REQUIRE(internal::TryAppendWaveConnectionLeg(chain, {0, 0, 0}, {0, 0, MIRROR_DEPTH_WORLD}, 1, false) == true);
    REQUIRE(internal::TryAppendWaveConnectionLeg(chain,
                                                 {0, 0, MIRROR_DEPTH_WORLD},
                                                 {MIRROR_LEG_WORLD, 0, MIRROR_DEPTH_WORLD},
                                                 1,
                                                 true) == true);
    return chain;
}

// Glass slab: a 2 m slab of index 1.5 between z = 0.5 and z = 2.5 at normal incidence, with the receiver at z = 3.
// Reduced lengths 0.5, 4/3 and 0.5 give a 7/3 m equivalent straight connection.
constexpr float64 GLASS_INDEX = 1.5;
constexpr float64 GLASS_FRONT_WORLD = 0.5;
constexpr float64 GLASS_BACK_WORLD = 2.5;
constexpr float64 GLASS_RECEIVER_WORLD = 3;
/// A screen inside the glass, halfway through it: reduced distances 7/6 m on either side.
constexpr float64 GLASS_INSIDE_DEPTH_WORLD = 1.5;
/// Scene vertices are binary32, so a screen's depth moves by up to 5e-8 m between the relayed and reduced scenes.
constexpr float64 GEOMETRY_ROUNDING_TOLERANCE = 1e-6;
constexpr float64 GLASS_REDUCED_DISTANCE_WORLD =
    GLASS_FRONT_WORLD + (GLASS_BACK_WORLD - GLASS_FRONT_WORLD) / GLASS_INDEX + GLASS_RECEIVER_WORLD - GLASS_BACK_WORLD;
internal::WaveConnectionChain MakeSlabChain(float64 refractiveIndex)
{
    internal::WaveConnectionChain chain;
    REQUIRE(internal::TryAppendWaveConnectionLeg(chain, {0, 0, 0}, {0, 0, GLASS_FRONT_WORLD}, 1, false) == true);
    REQUIRE(internal::TryAppendWaveConnectionLeg(chain,
                                                 {0, 0, GLASS_FRONT_WORLD},
                                                 {0, 0, GLASS_BACK_WORLD},
                                                 refractiveIndex,
                                                 false) == true);
    REQUIRE(
        internal::TryAppendWaveConnectionLeg(chain, {0, 0, GLASS_BACK_WORLD}, {0, 0, GLASS_RECEIVER_WORLD}, 1, false) ==
        true);
    return chain;
}
/// Reduced depth of a world depth along the slab chain.
float64 ReduceSlabDepth(float64 depthWorld)
{
    if (depthWorld <= GLASS_FRONT_WORLD)
    {
        return depthWorld;
    }
    if (depthWorld <= GLASS_BACK_WORLD)
    {
        return GLASS_FRONT_WORLD + (depthWorld - GLASS_FRONT_WORLD) / GLASS_INDEX;
    }
    return depthWorld - (GLASS_BACK_WORLD - GLASS_FRONT_WORLD) * (1 - 1 / GLASS_INDEX);
}
/// Fresnel numbers per metre of edge offset for the two (reduced) distances.
float64 ComputeFresnelScale(float64 sourceToEdgeWorld, float64 edgeToReceiverWorld)
{
    return math::Sqrt(2 * (sourceToEdgeWorld + edgeToReceiverWorld) /
                      (EDGE_WAVELENGTH_WORLD * sourceToEdgeWorld * edgeToReceiverWorld));
}
/// Fresnel-Kirchhoff field ratio on the axis behind a half plane covering x >= edge, at reduced distances.
WaveField EvaluateKnifeEdgeReference(float64 edge, float64 sourceToEdgeWorld, float64 edgeToReceiverWorld)
{
    // Mirroring x, the screen covers Fresnel numbers below -edge scaled by the Fresnel zone of the two distances.
    return wave_test::EvaluateFresnelEdge(-edge * ComputeFresnelScale(sourceToEdgeWorld, edgeToReceiverWorld));
}
} // namespace

TEST_CASE("Relayed connection frames mirror at reflections and rotate at refractions", "[renderer][wave-relay]")
{
    const internal::WaveConnectionChain mirror = MakeMirrorChain();
    internal::WaveConnectionFrames frames;
    REQUIRE(internal::TryMakeWaveConnectionFrames(mirror, frames) == true);
    CHECK(frames.Handedness[0] == 1);
    CHECK(frames.Handedness[1] == -1);
    // The first leg's frame axis +x reflects to +z, perpendicular to the reflected direction +x.
    CHECK(math::Abs(frames.AxisXWorld[0].X - 1) < UNFOLDING_TOLERANCE);
    CHECK(math::Abs(frames.AxisXWorld[1].Z - 1) < UNFOLDING_TOLERANCE);
    CHECK(math::Abs(math::Dot(frames.AxisXWorld[1], mirror.Legs[1].DirectionWorld)) < UNFOLDING_TOLERANCE);
    // A refraction in the x-z plane rotates the in-plane axis with the direction and keeps the handedness.
    internal::WaveConnectionChain refraction;
    REQUIRE(internal::TryAppendWaveConnectionLeg(refraction, {0, 0, 0}, {0, 0, 1}, 1, false) == true);
    REQUIRE(internal::TryAppendWaveConnectionLeg(refraction, {0, 0, 1}, {1, 0, 2}, GLASS_INDEX, false) == true);
    REQUIRE(internal::TryMakeWaveConnectionFrames(refraction, frames) == true);
    CHECK(frames.Handedness[1] == 1);
    const float64 halfSqrt = math::Sqrt(0.5);
    CHECK(math::Abs(frames.AxisXWorld[1].X - halfSqrt) < UNFOLDING_TOLERANCE);
    CHECK(math::Abs(frames.AxisXWorld[1].Z + halfSqrt) < UNFOLDING_TOLERANCE);
    // A transmission cannot reverse the direction, and a reflection must turn it.
    internal::WaveConnectionChain reversed;
    REQUIRE(internal::TryAppendWaveConnectionLeg(reversed, {0, 0, 0}, {0, 0, 1}, 1, false) == true);
    REQUIRE(internal::TryAppendWaveConnectionLeg(reversed, {0, 0, 1}, {0, 0, 0}, 1, false) == true);
    CHECK(internal::TryMakeWaveConnectionFrames(reversed, frames) == false);
    internal::WaveConnectionChain unturned;
    REQUIRE(internal::TryAppendWaveConnectionLeg(unturned, {0, 0, 0}, {0, 0, 1}, 1, false) == true);
    REQUIRE(internal::TryAppendWaveConnectionLeg(unturned, {0, 0, 1}, {0, 0, 2}, 1, true) == true);
    CHECK(internal::TryMakeWaveConnectionFrames(unturned, frames) == false);
    // Legs reject coincident endpoints and nonpositive indices without changing the chain.
    CHECK(internal::TryAppendWaveConnectionLeg(unturned, {0, 0, 2}, {0, 0, 2}, 1, false) == false);
    CHECK(internal::TryAppendWaveConnectionLeg(unturned, {0, 0, 2}, {0, 0, 3}, 0, false) == false);
    CHECK(unturned.LegCount == 2);
}

TEST_CASE("Specular relays infer each leg's medium and reject disagreeing glass", "[renderer][wave-relay]")
{
    constexpr float32 ETA = 1.5F;
    constexpr float32 OTHER_ETA = 1.33F;
    internal::ReferenceOpticalRelay relay;
    // A camera ray from the receiver at z = 0 enters a slab, leaves it and reaches an emitter at z = 3.
    internal::BeginWaveRelay(relay, {0, 0, 0});
    internal::AppendWaveRelayEvent(relay, true, ETA, false, false, {0, 0, 1});
    internal::AppendWaveRelayEvent(relay, true, ETA, true, false, {0, 0, 1.6F});
    CHECK(relay.HasConflict == false);
    REQUIRE(relay.SegmentCount == 3);
    CHECK(relay.RefractiveIndices[0] == 1);
    CHECK(relay.RefractiveIndices[1] == ETA);
    CHECK(relay.RefractiveIndices[2] == 1);
    internal::WaveConnectionChain chain;
    REQUIRE(internal::TryMakeWaveRelayChain(relay, {0, 0, 3}, chain) == true);
    // Light order: the emitter leg first, ending at the last event, and the receiver last.
    REQUIRE(chain.LegCount == 3);
    CHECK(chain.Legs[0].BeginWorld.Z == 3);
    CHECK(math::Abs(chain.Legs[0].EndWorld.Z - 1.6) < 1e-6);
    CHECK(chain.Legs[1].RefractiveIndex == static_cast<float64>(ETA));
    CHECK(chain.Legs[2].EndWorld.Z == 0);
    CHECK(chain.Legs[1].IsReflectedEntry == false);
    SECTION("Total internal reflection stays inside the glass")
    {
        internal::BeginWaveRelay(relay, {0, 0, 0});
        internal::AppendWaveRelayEvent(relay, true, ETA, false, false, {0, 0, 1});
        internal::AppendWaveRelayEvent(relay, true, ETA, true, true, {0, 0.5F, 1.5F});
        CHECK(relay.HasConflict == false);
        CHECK(relay.RefractiveIndices[2] == ETA);
        REQUIRE(internal::TryMakeWaveRelayChain(relay, {0, 1, 1}, chain) == true);
        CHECK(chain.Legs[1].IsReflectedEntry == true);
    }
    SECTION("A thin dielectric has air on both sides")
    {
        internal::BeginWaveRelay(relay, {0, 0, 0});
        internal::AppendWaveRelayEvent(relay, false, ETA, false, false, {0, 0, 1});
        CHECK(relay.RefractiveIndices[0] == 1);
        CHECK(relay.RefractiveIndices[1] == 1);
    }
    SECTION("Touching glass of different indices")
    {
        internal::BeginWaveRelay(relay, {0, 0, 0});
        internal::AppendWaveRelayEvent(relay, true, ETA, false, false, {0, 0, 1});
        // The next surface is the outside of another dielectric, which reports air on the shared segment.
        internal::AppendWaveRelayEvent(relay, true, OTHER_ETA, false, false, {0, 0, 1.6F});
        CHECK(relay.HasConflict == true);
        CHECK(internal::TryMakeWaveRelayChain(relay, {0, 0, 3}, chain) == false);
    }
    SECTION("More events than legs")
    {
        internal::BeginWaveRelay(relay, {0, 0, 0});
        for (uint32 event = 0; event < internal::WAVE_MAX_CONNECTION_LEGS; ++event)
        {
            internal::AppendWaveRelayEvent(relay, false, ETA, false, false, {0, 0, static_cast<float32>(event + 1)});
        }
        CHECK(relay.HasConflict == true);
    }
    SECTION("No event")
    {
        internal::BeginWaveRelay(relay, {0, 0, 0});
        CHECK(internal::TryMakeWaveRelayChain(relay, {0, 0, 3}, chain) == false);
    }
}

TEST_CASE("A mirror relay equals the straight connection to the light's image", "[renderer][wave-relay]")
{
    const internal::WaveConnectionChain chain = MakeMirrorChain();
    for (const Operator connection : OPERATORS)
    {
        CAPTURE(static_cast<uint32>(connection));
        SECTION("Open")
        {
            const internal::ReferenceSceneData empty;
            CheckFieldsEqual(EvaluateChain(empty, chain, connection), {1, 0}, UNFOLDING_TOLERANCE);
        }
        SECTION("A slit formed by an edge before the mirror and an edge after it")
        {
            // Unfolded, the edges at x = 0.05 (depth 1) and x = -0.05 (depth 3) bound a slit whose two edge waves
            // interfere at the receiver. Neither edge's diffracted ray meets the other screen.
            constexpr float64 SLIT_EDGE_WORLD = 0.05;
            internal::ReferenceSceneData relayed;
            AddKnifeEdge(relayed, SLIT_EDGE_WORLD, NEAR_SCREEN_DEPTH_WORLD, PlaceIdentity);
            AddRectangle(relayed,
                         -FAR_EDGE_WORLD,
                         -SLIT_EDGE_WORLD,
                         -FAR_EDGE_WORLD,
                         FAR_EDGE_WORLD,
                         FAR_SCREEN_DEPTH_WORLD,
                         PlaceBeyondMirror);
            REQUIRE(internal::BuildReferenceBvh(relayed) == rhi::RasterStatus::Ready);
            internal::ReferenceSceneData unfolded;
            AddKnifeEdge(unfolded, SLIT_EDGE_WORLD, NEAR_SCREEN_DEPTH_WORLD, PlaceIdentity);
            AddRectangle(unfolded,
                         -FAR_EDGE_WORLD,
                         -SLIT_EDGE_WORLD,
                         -FAR_EDGE_WORLD,
                         FAR_EDGE_WORLD,
                         FAR_SCREEN_DEPTH_WORLD,
                         PlaceIdentity);
            REQUIRE(internal::BuildReferenceBvh(unfolded) == rhi::RasterStatus::Ready);
            const WaveField relayedField = EvaluateChain(relayed, chain, connection);
            CheckFieldsEqual(relayedField,
                             EvaluateStraight(unfolded, UNFOLDED_DISTANCE_WORLD, connection),
                             UNFOLDING_TOLERANCE);
            // Classical scattering at the mirror sees the open ballistic step through the slit.
            CHECK(math::Abs(MeasurePower(relayedField) - 1) > MEASURABLE_POWER_DIFFERENCE);
        }
    }
}

TEST_CASE("A mirror relay carries the lattice in a mirrored frame", "[renderer][wave-relay]")
{
    // Two off-axis squares, one on each leg, overlap once unfolded. Evaluated with the wrong handedness, the far square
    // would land on the opposite side of the axis and block more of the beam.
    constexpr float64 SQUARE_LEFT_WORLD = 0.02;
    constexpr float64 SQUARE_RIGHT_WORLD = 0.2;
    constexpr float64 SQUARE_BOTTOM_WORLD = 0.03;
    constexpr float64 SQUARE_TOP_WORLD = 0.2;
    const auto addSquares = [&](internal::ReferenceSceneData& inoutScene, float64 farSign, PlaceFrame place) {
        AddRectangle(inoutScene,
                     SQUARE_LEFT_WORLD,
                     SQUARE_RIGHT_WORLD,
                     SQUARE_BOTTOM_WORLD,
                     SQUARE_TOP_WORLD,
                     NEAR_SCREEN_DEPTH_WORLD,
                     PlaceIdentity);
        AddRectangle(inoutScene,
                     SQUARE_LEFT_WORLD,
                     SQUARE_RIGHT_WORLD,
                     farSign * SQUARE_BOTTOM_WORLD,
                     farSign * SQUARE_TOP_WORLD,
                     FAR_SCREEN_DEPTH_WORLD,
                     place);
        REQUIRE(internal::BuildReferenceBvh(inoutScene) == rhi::RasterStatus::Ready);
    };
    internal::ReferenceSceneData relayed;
    addSquares(relayed, 1, PlaceBeyondMirror);
    internal::ReferenceSceneData overlapping;
    addSquares(overlapping, 1, PlaceIdentity);
    internal::ReferenceSceneData opposite;
    addSquares(opposite, -1, PlaceIdentity);
    const WaveField relayedField = EvaluateChain(relayed, MakeMirrorChain(), Operator::AngularSpectrum);
    const WaveField overlappingField =
        EvaluateStraight(overlapping, UNFOLDED_DISTANCE_WORLD, Operator::AngularSpectrum);
    const WaveField oppositeField = EvaluateStraight(opposite, UNFOLDED_DISTANCE_WORLD, Operator::AngularSpectrum);
    CheckFieldsEqual(relayedField, overlappingField, UNFOLDING_TOLERANCE);
    CAPTURE(MeasurePower(overlappingField), MeasurePower(oppositeField));
    CHECK(math::Abs(MeasurePower(overlappingField) - MeasurePower(oppositeField)) > MEASURABLE_POWER_DIFFERENCE);
}

TEST_CASE("A mirror relays a knife edge's Fresnel diffraction at the unfolded distance", "[renderer][wave-relay]")
{
    // A specular reflection that relays a diffracted field: the knife edge lies before the mirror, the receiver after
    // it. Classical scattering at the mirror would see only the ballistic step (zero or one).
    constexpr StaticArray<float64, 6> EDGES_WORLD{{-0.04, -0.02, -0.01, 0.01, 0.02, 0.04}};
    const internal::WaveConnectionChain chain = MakeMirrorChain();
    for (const float64 edgeWorld : EDGES_WORLD)
    {
        internal::ReferenceSceneData scene;
        AddKnifeEdge(scene, edgeWorld, NEAR_SCREEN_DEPTH_WORLD, PlaceIdentity);
        REQUIRE(internal::BuildReferenceBvh(scene) == rhi::RasterStatus::Ready);
        const float64 actual = MeasurePower(EvaluateChain(scene, chain, Operator::UtdEdgeDiffraction));
        const float64 expected =
            MeasurePower(EvaluateKnifeEdgeReference(edgeWorld,
                                                    NEAR_SCREEN_DEPTH_WORLD,
                                                    UNFOLDED_DISTANCE_WORLD - NEAR_SCREEN_DEPTH_WORLD));
        const float64 v = -edgeWorld * ComputeFresnelScale(NEAR_SCREEN_DEPTH_WORLD,
                                                           UNFOLDED_DISTANCE_WORLD - NEAR_SCREEN_DEPTH_WORLD);
        REQUIRE(v >= KNIFE_EDGE_MINIMUM_V);
        REQUIRE(v <= KNIFE_EDGE_MAXIMUM_V);
        const float64 classical = edgeWorld > 0 ? 1.0 : 0.0;
        CAPTURE(edgeWorld, actual, expected);
        CHECK(math::Abs(actual - expected) < KNIFE_EDGE_RELATIVE_TOLERANCE * expected);
        CHECK(math::Abs(actual - classical) > MEASURABLE_POWER_DIFFERENCE);
    }
}

TEST_CASE("A glass leg propagates over its reduced length", "[renderer][wave-relay]")
{
    // A knife edge before, inside and behind the slab. Each relayed connection equals the straight connection in air
    // with the slab shortened to its reduced thickness. Inside it, it differs from a relay that ignores the index.
    constexpr StaticArray<float64, 3> EDGE_DEPTHS_WORLD{{0.25, GLASS_INSIDE_DEPTH_WORLD, 2.75}};
    // The lattice's longer wavelength takes a wider offset for similar Fresnel numbers.
    constexpr float64 EDGE_WORLD = 0.02;
    constexpr float64 LATTICE_EDGE_WORLD = 0.04;
    const internal::WaveConnectionChain glass = MakeSlabChain(GLASS_INDEX);
    const internal::WaveConnectionChain air = MakeSlabChain(1);
    for (const Operator connection : OPERATORS)
    {
        for (const float64 depthWorld : EDGE_DEPTHS_WORLD)
        {
            CAPTURE(static_cast<uint32>(connection), depthWorld);
            const float64 edgeWorld = connection == Operator::AngularSpectrum ? LATTICE_EDGE_WORLD : EDGE_WORLD;
            internal::ReferenceSceneData relayed;
            AddKnifeEdge(relayed, edgeWorld, depthWorld, PlaceIdentity);
            REQUIRE(internal::BuildReferenceBvh(relayed) == rhi::RasterStatus::Ready);
            internal::ReferenceSceneData reduced;
            AddKnifeEdge(reduced, edgeWorld, ReduceSlabDepth(depthWorld), PlaceIdentity);
            REQUIRE(internal::BuildReferenceBvh(reduced) == rhi::RasterStatus::Ready);
            const WaveField relayedField = EvaluateChain(relayed, glass, connection);
            const WaveField expected = EvaluateStraight(reduced, GLASS_REDUCED_DISTANCE_WORLD, connection);
            CheckFieldsEqual(relayedField,
                             expected,
                             connection == Operator::UtdEdgeDiffraction ? UTD_REDUCED_TOLERANCE
                                                                        : GEOMETRY_ROUNDING_TOLERANCE);
            const float64 airPower = MeasurePower(EvaluateChain(relayed, air, connection));
            CAPTURE(MeasurePower(relayedField), airPower);
            // The 16 x 16 lattice resolves this change of Fresnel number only to 0.007 in power; its reduced-length
            // equivalence above is the check that applies to it.
            if (depthWorld == GLASS_INSIDE_DEPTH_WORLD && connection != Operator::AngularSpectrum)
            {
                // Inside the glass, the index shrinks both reduced distances to the edge, widening its Fresnel number.
                CHECK(math::Abs(MeasurePower(relayedField) - airPower) > MEASURABLE_POWER_DIFFERENCE);
            }
        }
    }
}

TEST_CASE("A knife edge seen through glass matches Fresnel diffraction at reduced distances", "[renderer][wave-relay]")
{
    constexpr StaticArray<float64, 6> EDGES_WORLD{{-0.03, -0.015, -0.005, 0.005, 0.015, 0.03}};
    // The screen inside the glass: UTD sees the source and receiver at their apparent depths, at the wavelength in
    // the glass.
    constexpr float64 EDGE_DEPTH_WORLD = GLASS_INSIDE_DEPTH_WORLD;
    const internal::WaveConnectionChain glass = MakeSlabChain(GLASS_INDEX);
    const float64 sourceToEdgeWorld = ReduceSlabDepth(EDGE_DEPTH_WORLD);
    for (const float64 edgeWorld : EDGES_WORLD)
    {
        internal::ReferenceSceneData scene;
        AddKnifeEdge(scene, edgeWorld, EDGE_DEPTH_WORLD, PlaceIdentity);
        REQUIRE(internal::BuildReferenceBvh(scene) == rhi::RasterStatus::Ready);
        const float64 actual = MeasurePower(EvaluateChain(scene, glass, Operator::UtdEdgeDiffraction));
        const float64 expected = MeasurePower(
            EvaluateKnifeEdgeReference(edgeWorld, sourceToEdgeWorld, GLASS_REDUCED_DISTANCE_WORLD - sourceToEdgeWorld));
        const float64 v =
            -edgeWorld * ComputeFresnelScale(sourceToEdgeWorld, GLASS_REDUCED_DISTANCE_WORLD - sourceToEdgeWorld);
        REQUIRE(v >= KNIFE_EDGE_MINIMUM_V);
        REQUIRE(v <= KNIFE_EDGE_MAXIMUM_V);
        CAPTURE(edgeWorld, actual, expected);
        CHECK(math::Abs(actual - expected) < KNIFE_EDGE_RELATIVE_TOLERANCE * expected);
    }
}

TEST_CASE("Relayed connections reject invalid chains and preserve the field", "[renderer][wave-relay]")
{
    const internal::ReferenceSceneData empty;
    internal::WaveConnectionChain chain;
    WaveField field{7, 8};
    for (const Operator connection : OPERATORS)
    {
        rhi::RasterStatus status = rhi::RasterStatus::Ready;
        if (connection == Operator::AngularSpectrum)
        {
            status = internal::EvaluateWaveChainConnection(empty, chain, LATTICE_WAVELENGTH_WORLD, field);
        }
        else if (connection == Operator::UtdEdgeDiffraction)
        {
            status = internal::EvaluateUtdChainConnection(empty, chain, EDGE_WAVELENGTH_WORLD, field);
        }
        else
        {
            status = internal::EvaluateFraunhoferChainConnection(empty, chain, EDGE_WAVELENGTH_WORLD, field);
        }
        CHECK(status == rhi::RasterStatus::InvalidDescription);
    }
    // A chain shorter than 32 wavelengths in reduced length, even with long geometric legs.
    REQUIRE(internal::TryAppendWaveConnectionLeg(chain, {0, 0, 0}, {0, 0, 1}, 4, false) == true);
    CHECK(internal::EvaluateWaveChainConnection(empty, chain, 0.1, field) == rhi::RasterStatus::InvalidDescription);
    CHECK(field.Real == 7);
    CHECK(field.Imaginary == 8);
}
