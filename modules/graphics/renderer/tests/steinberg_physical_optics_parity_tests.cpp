#include "steinberg_physical_optics_oracle.h"
#include "steinberg_physical_optics_parity.h"
#include "steinberg_physical_optics_parity_layout.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

// GPU parity of the steinberg-physical-optics template's scattering core with its CPU oracle. Thanks to Shlomi
// Steinberg, Pradeep Sen and Ling-Qi Yan, "Towards Practical Physical-Optics Rendering", ACM Transactions on Graphics
// 41(4), article 132, SIGGRAPH 2022, https://doi.org/10.1145/3528223.3530119 : the kernel
// (steinberg_physical_optics_parity.slang) runs the template's own wave_optics.slang on random beams and surfaces, and
// every result (the scattered Stokes vector, D of each component, the scattered shape matrices and frame, alpha_direct,
// the envelope density and the coherence length) must match the float64 oracle within float32 tolerances.
namespace
{
using namespace ludus::foundation;
using namespace ludus::graphics::renderer::testing;
namespace rhi = ludus::graphics::rhi;
using rhi::RasterStatus;

constexpr uint64 TIMEOUT_NANOSECONDS = 30 * time::NANOSECONDS_PER_SECOND;
constexpr usize CASE_COUNT = LUDUS_PHYSICAL_OPTICS_PARITY_CASE_COUNT;
constexpr usize WORDS_PER_RECORD = 4;
constexpr usize CASE_WORDS = CASE_COUNT * LUDUS_PHYSICAL_OPTICS_PARITY_CASE_RECORDS * WORDS_PER_RECORD;
constexpr usize RESULT_WORDS = CASE_COUNT * LUDUS_PHYSICAL_OPTICS_PARITY_RESULT_RECORDS * WORDS_PER_RECORD;
constexpr usize CASE_BYTES = CASE_WORDS * sizeof(uint32);
constexpr usize RESULT_BYTES = RESULT_WORDS * sizeof(uint32);
constexpr usize LOBE_COUNT = 4;
static_assert(CASE_COUNT % LUDUS_PHYSICAL_OPTICS_PARITY_GROUP_SIZE == 0, "Cases fill whole groups");
constexpr float64 SUN_COHERENCE = 50e-6;
constexpr float64 RADIATOR_AREA = 1e-6;
constexpr float64 SOLID_ANGLE = 1e-6;

template <typename Handle>
void WaitReady(rhi::DeviceHandle device, Handle handle)
{
    const uint64 start = time::NowTicks();
    RasterStatus status = rhi::GetStatus(device, handle);
    while (status == RasterStatus::Pending && time::NowTicks() - start < TIMEOUT_NANOSECONDS)
    {
        REQUIRE(rhi::PollLifetime(device) == RasterStatus::Ready);
        status = rhi::GetStatus(device, handle);
    }
    REQUIRE(status == RasterStatus::Ready);
}

bool IsAccepted(RasterStatus status) noexcept
{
    return status == RasterStatus::Ready || status == RasterStatus::Pending;
}

float64 Draw(math::RandomStream& inoutRandom, float64 minimum, float64 maximum)
{
    return minimum + (maximum - minimum) * static_cast<float64>(inoutRandom.NextFloat01());
}

/// A unit direction within the upper hemisphere of +Z whose cosine is at least minimumCosine.
math::Vector3d DrawUpperDirection(math::RandomStream& inoutRandom, float64 minimumCosine)
{
    const float64 cosine = Draw(inoutRandom, minimumCosine, 1);
    const float64 sine = math::Sqrt(1 - cosine * cosine);
    const float64 azimuth = Draw(inoutRandom, 0, math::kTau);
    return {sine * math::Cos(azimuth), sine * math::Sin(azimuth), cosine};
}

/// An outgoing direction whose half vector h~ with the incident direction lies within the lobe: the mirror direction
/// for direct and diffuse lobes moved by a Gaussian offset of the lobe's own spread in h~ (the coherence's and, for a
/// rough surface, the correlation length's), around the order -1, 0 or 1 for a grating. Random directions would mostly
/// fall where the narrow lobes underflow.
math::Vector3d DrawOutgoingNearLobe(math::RandomStream& inoutRandom,
                                    const OpticsCase& opticsCase,
                                    math::Vector3d incident,
                                    bool isCentered)
{
    const math::Vector3d mirror{-incident.X, -incident.Y, incident.Z};
    if (opticsCase.Lobe == OpticsLobe::Direct)
    {
        return mirror;
    }
    constexpr float64 NANOMETERS_PER_METER = 1e9;
    const float64 wavenumber = math::kTau * NANOMETERS_PER_METER / opticsCase.WavelengthNanometers;
    const float64 coherence =
        wavenumber * ComputeOpticsCoherenceLength(opticsCase.Incident, opticsCase.WavelengthNanometers);
    float64 variance = 1 / (coherence * coherence);
    float64 centerY = 0;
    if (opticsCase.Lobe == OpticsLobe::Scattered)
    {
        const float64 correlation = wavenumber * opticsCase.CorrelationLengthWorld;
        variance += 1 / (correlation * correlation);
    }
    else if (opticsCase.Lobe == OpticsLobe::Grating)
    {
        constexpr float64 ORDER_COUNT = 3;
        const float64 order = math::Floor(Draw(inoutRandom, 0, ORDER_COUNT)) - 1;
        centerY = order * opticsCase.WavelengthNanometers / (opticsCase.GratingPeriodWorld * NANOMETERS_PER_METER);
    }
    else
    {
        constexpr float64 DIFFUSE_VARIANCE = .1;
        variance = DIFFUSE_VARIANCE;
    }
    // NextFloat01 lies on a 2^-24 grid in [0, 1); half a step moves it into the open interval.
    constexpr float64 HALF_STEP = 0x1p-25;
    // A centred case sits on the lobe's peak, where the narrow longitudinal direction matters most.
    const float64 deviation = isCentered == true ? 0 : math::Sqrt(variance);
    const float64 halfX = deviation * math::ComputeStandardNormalQuantile(Draw(inoutRandom, 0, 1) + HALF_STEP);
    const float64 halfY =
        centerY + deviation * math::ComputeStandardNormalQuantile(Draw(inoutRandom, 0, 1) + HALF_STEP);
    // h~ = r^ + s^ on the surface (normal +Z, tangent +X): the outgoing projection is h~ minus the incident one.
    const float64 x = halfX - incident.X;
    const float64 y = halfY - incident.Y;
    constexpr float64 LARGEST_PROJECTION_SQUARED = .9;
    if (x * x + y * y >= LARGEST_PROJECTION_SQUARED)
    {
        return mirror;
    }
    return {x, y, math::Sqrt(1 - x * x - y * y)};
}

/// Random cases over every lobe: sunlight and radiator beams at a range of distances, partially polarized and
/// anisotropically coherent incident light, dielectric and conductor indices, coherent-limit evaluations.
StaticArray<OpticsCase, CASE_COUNT> MakeCases()
{
    constexpr uint64 SEED = 0x4f505449;
    math::RandomStream random;
    REQUIRE(random.TryReseed(SEED, 0) == true);
    constexpr float64 MINIMUM_COSINE = .2;
    constexpr StaticArray<OpticsComplex, 3> INDICES{{{1.5, 0}, {.2, 3.4}, {1.2, 7}}};
    StaticArray<OpticsCase, CASE_COUNT> result{};
    for (usize index = 0; index < CASE_COUNT; ++index)
    {
        OpticsCase& opticsCase = result[index];
        opticsCase.Lobe = static_cast<OpticsLobe>(index % LOBE_COUNT);
        constexpr float64 SHORTEST = 380;
        constexpr float64 LONGEST = 780;
        opticsCase.WavelengthNanometers = Draw(random, SHORTEST, LONGEST);
        const math::Vector3d incident = DrawUpperDirection(random, MINIMUM_COSINE);
        // Every fourth case is sunlight; the others radiators between 0.5 and 4 m away.
        constexpr usize SUN_PERIOD = 4;
        constexpr float64 NEAREST = .5;
        constexpr float64 FARTHEST = 4;
        opticsCase.Incident =
            (index / LOBE_COUNT) % SUN_PERIOD == 0
                ? MakeOpticsNaturalBeam(-incident, 1, ComputeOpticsSunShape(SUN_COHERENCE), OPTICS_SUN_DISTANCE_WORLD)
                : MakeOpticsNaturalBeam(-incident,
                                        1,
                                        ComputeOpticsRadiatorShape(RADIATOR_AREA, SOLID_ANGLE),
                                        Draw(random, NEAREST, FARTHEST));
        // Partially polarized, anisotropic light that an earlier interaction would have produced.
        constexpr float64 POLARIZATION = .3;
        opticsCase.Incident.Stokes = {1,
                                      Draw(random, -POLARIZATION, POLARIZATION),
                                      Draw(random, -POLARIZATION, POLARIZATION),
                                      Draw(random, -POLARIZATION, POLARIZATION)};
        constexpr float64 ANISOTROPY = .4;
        const OpticsSymmetric shape = opticsCase.Incident.ShapeX;
        opticsCase.Incident.ShapeX = {shape.Xx * Draw(random, 1 - ANISOTROPY, 1 + ANISOTROPY),
                                      shape.Xx * Draw(random, -ANISOTROPY, ANISOTROPY) / 2,
                                      shape.Yy * Draw(random, 1 - ANISOTROPY, 1 + ANISOTROPY)};
        opticsCase.Incident.ShapeY = {shape.Xx * Draw(random, 1 - ANISOTROPY, 1 + ANISOTROPY),
                                      shape.Xx * Draw(random, -ANISOTROPY, ANISOTROPY) / 2,
                                      shape.Yy * Draw(random, 1 - ANISOTROPY, 1 + ANISOTROPY)};
        opticsCase.Eta = INDICES[index % INDICES.GetSize()];
        constexpr float64 HIGHEST = 200e-9;
        opticsCase.RmsHeightWorld = Draw(random, 0, HIGHEST);
        constexpr float64 SHORTEST_CORRELATION = .3e-6;
        constexpr float64 LONGEST_CORRELATION = 5e-6;
        opticsCase.CorrelationLengthWorld = Draw(random, SHORTEST_CORRELATION, LONGEST_CORRELATION);
        opticsCase.OutgoingWorld = DrawOutgoingNearLobe(random, opticsCase, incident, index % 2 == 0);
        constexpr float64 NARROWEST_ENVELOPE = .05;
        constexpr float64 WIDEST_ENVELOPE = 2;
        opticsCase.EnvelopeVariance = Draw(random, NARROWEST_ENVELOPE, WIDEST_ENVELOPE);
        constexpr float64 SMALLEST_SHARE = .2;
        opticsCase.Weight = Draw(random, SMALLEST_SHARE, 1);
        // Every eighth case evaluates the radiometric pass's coherent limit.
        constexpr usize RADIOMETRIC_PERIOD = 8;
        opticsCase.IsWave = index % RADIOMETRIC_PERIOD != RADIOMETRIC_PERIOD - 1;
    }
    return result;
}

void WriteFloat(Array<uint32>& outWords, usize caseIndex, usize record, usize word, float64 value)
{
    const float32 narrowed = static_cast<float32>(value);
    uint32 bits = 0;
    std::memcpy(&bits, &narrowed, sizeof(bits));
    outWords[(caseIndex * LUDUS_PHYSICAL_OPTICS_PARITY_CASE_RECORDS + record) * WORDS_PER_RECORD + word] = bits;
}

void WriteVector(Array<uint32>& outWords, usize caseIndex, usize record, math::Vector3d vector, float64 last)
{
    WriteFloat(outWords, caseIndex, record, 0, vector.X);
    WriteFloat(outWords, caseIndex, record, 1, vector.Y);
    WriteFloat(outWords, caseIndex, record, 2, vector.Z);
    WriteFloat(outWords, caseIndex, record, 3, last);
}

void WriteShape(Array<uint32>& outWords, usize caseIndex, usize record, const OpticsSymmetric& shape, uint32 last)
{
    WriteFloat(outWords, caseIndex, record, 0, shape.Xx);
    WriteFloat(outWords, caseIndex, record, 1, shape.Xy);
    WriteFloat(outWords, caseIndex, record, 2, shape.Yy);
    outWords[(caseIndex * LUDUS_PHYSICAL_OPTICS_PARITY_CASE_RECORDS + record) * WORDS_PER_RECORD + 3] = last;
}

/// The cases as the kernel reads them, every input rounded to float32 as the GPU sees it.
Array<uint32> EncodeCases(const StaticArray<OpticsCase, CASE_COUNT>& cases)
{
    Array<uint32> words;
    REQUIRE(words.TryResize(CASE_WORDS, uint32{0}) == true);
    for (usize index = 0; index < CASE_COUNT; ++index)
    {
        const OpticsCase& opticsCase = cases[index];
        const OpticsBeam& beam = opticsCase.Incident;
        WriteVector(words, index, 0, beam.PropagationWorld, opticsCase.WavelengthNanometers);
        WriteVector(words, index, 1, beam.TransverseXWorld, beam.DistanceWorld);
        WriteFloat(words, index, 2, 0, beam.Stokes.S0);
        WriteFloat(words, index, 2, 1, beam.Stokes.S1);
        WriteFloat(words, index, 2, 2, beam.Stokes.S2);
        WriteFloat(words, index, 2, 3, beam.Stokes.S3);
        WriteShape(words, index, 3, beam.ShapeX, static_cast<uint32>(opticsCase.Lobe));
        WriteShape(words, index, 4, beam.ShapeY, opticsCase.IsWave == true ? 1 : 0);
        WriteVector(words, index, 5, opticsCase.NormalWorld, opticsCase.Eta.Real);
        WriteVector(words, index, 6, opticsCase.TangentWorld, opticsCase.Eta.Imaginary);
        WriteVector(words, index, 7, opticsCase.OutgoingWorld, opticsCase.RmsHeightWorld);
        WriteFloat(words, index, 8, 0, opticsCase.CorrelationLengthWorld);
        WriteFloat(words, index, 8, 1, opticsCase.LongitudinalCoherenceWorld);
        WriteFloat(words, index, 8, 2, opticsCase.GratingPeriodWorld);
        WriteFloat(words, index, 8, 3, opticsCase.GratingHeightWorld);
        words[(index * LUDUS_PHYSICAL_OPTICS_PARITY_CASE_RECORDS + 9) * WORDS_PER_RECORD] =
            opticsCase.GratingMaximumOrder;
        WriteFloat(words, index, 9, 1, opticsCase.EnvelopeVariance);
        WriteFloat(words, index, 9, 2, opticsCase.Weight);
    }
    return words;
}

/// The oracle's input after the round trip through float32, so both sides evaluate the same case.
OpticsCase RoundCase(const OpticsCase& opticsCase)
{
    const auto round = [](float64 value) { return static_cast<float64>(static_cast<float32>(value)); };
    const auto roundVector = [&round](math::Vector3d vector) {
        return math::Vector3d{round(vector.X), round(vector.Y), round(vector.Z)};
    };
    const auto roundShape = [&round](const OpticsSymmetric& shape) {
        return OpticsSymmetric{round(shape.Xx), round(shape.Xy), round(shape.Yy)};
    };
    OpticsCase result = opticsCase;
    result.Incident.PropagationWorld = roundVector(opticsCase.Incident.PropagationWorld);
    result.Incident.TransverseXWorld = roundVector(opticsCase.Incident.TransverseXWorld);
    result.Incident.DistanceWorld = round(opticsCase.Incident.DistanceWorld);
    result.Incident.Stokes = {round(opticsCase.Incident.Stokes.S0),
                              round(opticsCase.Incident.Stokes.S1),
                              round(opticsCase.Incident.Stokes.S2),
                              round(opticsCase.Incident.Stokes.S3)};
    result.Incident.ShapeX = roundShape(opticsCase.Incident.ShapeX);
    result.Incident.ShapeY = roundShape(opticsCase.Incident.ShapeY);
    result.WavelengthNanometers = round(opticsCase.WavelengthNanometers);
    result.OutgoingWorld = roundVector(opticsCase.OutgoingWorld);
    result.Eta = {round(opticsCase.Eta.Real), round(opticsCase.Eta.Imaginary)};
    result.RmsHeightWorld = round(opticsCase.RmsHeightWorld);
    result.CorrelationLengthWorld = round(opticsCase.CorrelationLengthWorld);
    result.LongitudinalCoherenceWorld = round(opticsCase.LongitudinalCoherenceWorld);
    result.GratingPeriodWorld = round(opticsCase.GratingPeriodWorld);
    result.GratingHeightWorld = round(opticsCase.GratingHeightWorld);
    result.EnvelopeVariance = round(opticsCase.EnvelopeVariance);
    result.Weight = round(opticsCase.Weight);
    return result;
}

/// Run the parity kernel once over the encoded cases. Returns false without a native GPU adapter.
bool RunParityKernel(const Array<uint32>& caseWords, Array<uint32>& outResults)
{
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    description.RequiredProfile.Raster.MinBufferSize = CASE_BYTES;
    description.RequiredProfile.Compute.MinStorageRange = CASE_BYTES;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        return false;
    }
    REQUIRE(started == rhi::DeviceStatus::Ready);
    REQUIRE(outResults.TryResize(RESULT_WORDS, uint32{0}) == true);
    rhi::BufferHandle cases;
    rhi::BufferHandle results;
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         { .Role = rhi::BufferRole::Storage, .Size = CASE_BYTES },
                                         {reinterpret_cast<const uint8*>(caseWords.GetData()), CASE_BYTES},
                                         cases)) == true);
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         { .Role = rhi::BufferRole::Storage, .Size = RESULT_BYTES },
                                         {reinterpret_cast<const uint8*>(outResults.GetData()), RESULT_BYTES},
                                         results)) == true);
    const rhi::RasterShaderDescription shader = ludus::shaders::steinberg_physical_optics_parity::Compute();
    rhi::RasterShaderHandle shaderHandle;
    rhi::BindingLayoutHandle layout;
    REQUIRE(IsAccepted(rhi::CreateRasterShader(device, shader, shaderHandle)) == true);
    REQUIRE(IsAccepted(rhi::CreateBindingLayout(device, shader.Bindings, layout)) == true);
    WaitReady(device, cases);
    WaitReady(device, results);
    WaitReady(device, shaderHandle);
    WaitReady(device, layout);
    constexpr usize BINDING_COUNT = 2;
    const StaticArray<rhi::RasterBindingResource, BINDING_COUNT> resources{{
        { .Binding = LUDUS_PHYSICAL_OPTICS_PARITY_CASES_BINDING, .Buffer = cases, .Size = CASE_BYTES },
        { .Binding = LUDUS_PHYSICAL_OPTICS_PARITY_RESULTS_BINDING, .Buffer = results, .Size = RESULT_BYTES },
    }};
    rhi::BindingSetHandle set;
    rhi::ComputePipelineHandle pipeline;
    REQUIRE(IsAccepted(rhi::CreateBindingSet(device, layout, resources.AsSpan(), set)) == true);
    REQUIRE(IsAccepted(rhi::CreateComputePipeline(device, { .Shader = shaderHandle, .Layout = layout }, pipeline)) ==
            true);
    WaitReady(device, set);
    WaitReady(device, pipeline);
    rhi::OrderedGraph graph;
    REQUIRE(rhi::CreateOrderedGraph(device, graph) == RasterStatus::Ready);
    rhi::GraphVersion casesOriginal;
    rhi::GraphVersion casesVersion;
    rhi::GraphVersion resultsOriginal;
    rhi::GraphVersion resultsVersion;
    REQUIRE(rhi::ImportGraphBuffer(device, graph, cases, casesOriginal) == RasterStatus::Ready);
    REQUIRE(rhi::NextGraphVersion(device, graph, casesOriginal, casesVersion) == RasterStatus::Ready);
    REQUIRE(rhi::ImportGraphBuffer(device, graph, results, resultsOriginal) == RasterStatus::Ready);
    REQUIRE(rhi::NextGraphVersion(device, graph, resultsOriginal, resultsVersion) == RasterStatus::Ready);
    const StaticArray<rhi::GraphUse, BINDING_COUNT> uses{{
        {casesVersion, rhi::GraphAccessMode::StorageReadWrite, 0, CASE_BYTES, rhi::RasterVisibility::Compute},
        {resultsVersion, rhi::GraphAccessMode::StorageReadWrite, 0, RESULT_BYTES, rhi::RasterVisibility::Compute},
    }};
    const rhi::ComputeDispatch dispatch
    {
        .Pipeline = pipeline,
        .Bindings = set,
        .Groups = {static_cast<uint32>(CASE_COUNT / LUDUS_PHYSICAL_OPTICS_PARITY_GROUP_SIZE), 1, 1},
    };
    rhi::GraphPassDescription pass;
    pass.Name = "steinberg physical optics parity";
    pass.Uses = uses.GetData();
    pass.UseCount = static_cast<uint32>(uses.GetSize());
    pass.SideEffect = true;
    pass.Dispatch = &dispatch;
    REQUIRE(rhi::AddGraphPass(device, graph, pass) == RasterStatus::Ready);
    rhi::GraphReport graphReport;
    REQUIRE(rhi::CompileOrderedGraph(device, graph, graphReport) == RasterStatus::Ready);
    REQUIRE(rhi::SetFrameTarget(device, surface, {16, 16, 0, 0, 0, 1}) == rhi::DeviceStatus::Ready);
    rhi::SubmissionToken completion;
    RasterStatus executed = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    const uint64 beginning = time::NowTicks();
    while (executed == RasterStatus::NotReady && time::NowTicks() - beginning < TIMEOUT_NANOSECONDS)
    {
        executed = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    }
    REQUIRE(executed == RasterStatus::Ready);
    WaitReady(device, completion);
    rhi::ReadbackTicket ticket;
    RasterStatus status = rhi::RequestBufferReadback(device, results, 0, RESULT_BYTES, ticket);
    const uint64 start = time::NowTicks();
    while (status == RasterStatus::Pending && time::NowTicks() - start < TIMEOUT_NANOSECONDS)
    {
        REQUIRE(rhi::PollLifetime(device) == RasterStatus::Ready);
        status = rhi::GetStatus(device, ticket);
    }
    REQUIRE(status == RasterStatus::Ready);
    REQUIRE(rhi::CopyReadback(device, ticket, reinterpret_cast<uint8*>(outResults.GetData()), RESULT_BYTES) ==
            RasterStatus::Ready);
    REQUIRE(rhi::Release(device, ticket) == RasterStatus::Ready);
    return true;
}

float64 ReadResult(const Array<uint32>& results, usize caseIndex, usize record, usize word)
{
    float32 value = 0;
    std::memcpy(&value,
                &results[(caseIndex * LUDUS_PHYSICAL_OPTICS_PARITY_RESULT_RECORDS + record) * WORDS_PER_RECORD + word],
                sizeof(value));
    return static_cast<float64>(value);
}

/// Values below this are radiance no image can show, and binary32 flushes the smallest of them to zero.
constexpr float64 NEGLIGIBLE_VALUE = 1e-30;

/// Whether a GPU value agrees with the oracle's: |gpu - cpu| <= tolerance (|cpu| + scale) + NEGLIGIBLE_VALUE.
bool Agrees(float64 gpu, float64 cpu, float64 tolerance, float64 scale)
{
    return math::Abs(gpu - cpu) <= tolerance * (math::Abs(cpu) + scale) + NEGLIGIBLE_VALUE;
}
} // namespace

TEST_CASE("The Steinberg physical-optics template's scattering core matches its CPU oracle on the GPU",
          "[renderer][gpu][steinberg-physical-optics][parity]")
{
    const StaticArray<OpticsCase, CASE_COUNT> cases = MakeCases();
    const Array<uint32> words = EncodeCases(cases);
    Array<uint32> results;
    if (RunParityKernel(words, results) == false)
    {
        SKIP("No native GPU adapter.");
    }
    // Metal compiles with fast math. The D values are exponentials of quadratic forms of up to a few tens, so their
    // float32 rounding is relative; the shapes pass through finite-difference Hessians with a 0.01 step, which float32
    // evaluates to about 1e-3.
    constexpr float64 RADIANCE_TOLERANCE = 2e-3;
    constexpr float64 SHAPE_TOLERANCE = 2e-2;
    constexpr float64 DIRECTION_TOLERANCE = 1e-4;
    for (usize index = 0; index < CASE_COUNT; ++index)
    {
        const OpticsResult expected = EvaluateOpticsCase(RoundCase(cases[index]));
        INFO("case " << index << ", lobe " << static_cast<uint32>(cases[index].Lobe) << ", wave "
                     << cases[index].IsWave);
        const OpticsStokes& stokes = expected.Scattered.Stokes;
        const float64 stokesScale = math::Abs(stokes.S0);
        INFO("Stokes expected " << stokes.S0 << ' ' << stokes.S1 << ' ' << stokes.S2 << ' ' << stokes.S3 << ", GPU "
                                << ReadResult(results, index, 0, 0) << ' ' << ReadResult(results, index, 0, 1) << ' '
                                << ReadResult(results, index, 0, 2) << ' ' << ReadResult(results, index, 0, 3));
        CHECK(Agrees(ReadResult(results, index, 0, 0), stokes.S0, RADIANCE_TOLERANCE, 0) == true);
        CHECK(Agrees(ReadResult(results, index, 0, 1), stokes.S1, RADIANCE_TOLERANCE, stokesScale) == true);
        CHECK(Agrees(ReadResult(results, index, 0, 2), stokes.S2, RADIANCE_TOLERANCE, stokesScale) == true);
        CHECK(Agrees(ReadResult(results, index, 0, 3), stokes.S3, RADIANCE_TOLERANCE, stokesScale) == true);
        INFO("D expected " << expected.DiffractionX << ' ' << expected.DiffractionY << ' ' << expected.DiffractionHalf
                           << ", GPU " << ReadResult(results, index, 1, 3) << ' ' << ReadResult(results, index, 2, 3)
                           << ' ' << ReadResult(results, index, 3, 0));
        CHECK(Agrees(ReadResult(results, index, 1, 3), expected.DiffractionX, RADIANCE_TOLERANCE, 0) == true);
        CHECK(Agrees(ReadResult(results, index, 2, 3), expected.DiffractionY, RADIANCE_TOLERANCE, 0) == true);
        CHECK(Agrees(ReadResult(results, index, 3, 0), expected.DiffractionHalf, RADIANCE_TOLERANCE, 0) == true);
        CHECK(Agrees(ReadResult(results, index, 3, 1), expected.DirectFraction, RADIANCE_TOLERANCE, 0) == true);
        CHECK(Agrees(ReadResult(results, index, 3, 2), expected.EnvelopeDensity, RADIANCE_TOLERANCE, 0) == true);
        CHECK(Agrees(ReadResult(results, index, 3, 3), expected.CoherenceLengthWorld, RADIANCE_TOLERANCE, 0) == true);
        const OpticsSymmetric& shapeX = expected.Scattered.ShapeX;
        const OpticsSymmetric& shapeY = expected.Scattered.ShapeY;
        const float64 shapeScale =
            math::Max(math::Abs(shapeX.Xx) + math::Abs(shapeX.Yy), math::Abs(shapeY.Xx) + math::Abs(shapeY.Yy));
        INFO("theta_x expected " << shapeX.Xx << ' ' << shapeX.Xy << ' ' << shapeX.Yy << ", GPU "
                                 << ReadResult(results, index, 1, 0) << ' ' << ReadResult(results, index, 1, 1) << ' '
                                 << ReadResult(results, index, 1, 2));
        CHECK(math::Abs(ReadResult(results, index, 1, 0) - shapeX.Xx) <= SHAPE_TOLERANCE * shapeScale);
        CHECK(math::Abs(ReadResult(results, index, 1, 1) - shapeX.Xy) <= SHAPE_TOLERANCE * shapeScale);
        CHECK(math::Abs(ReadResult(results, index, 1, 2) - shapeX.Yy) <= SHAPE_TOLERANCE * shapeScale);
        CHECK(math::Abs(ReadResult(results, index, 2, 0) - shapeY.Xx) <= SHAPE_TOLERANCE * shapeScale);
        CHECK(math::Abs(ReadResult(results, index, 2, 1) - shapeY.Xy) <= SHAPE_TOLERANCE * shapeScale);
        CHECK(math::Abs(ReadResult(results, index, 2, 2) - shapeY.Yy) <= SHAPE_TOLERANCE * shapeScale);
        const math::Vector3d x = expected.Scattered.TransverseXWorld;
        CHECK(math::Abs(ReadResult(results, index, 4, 0) - x.X) <= DIRECTION_TOLERANCE);
        CHECK(math::Abs(ReadResult(results, index, 4, 1) - x.Y) <= DIRECTION_TOLERANCE);
        CHECK(math::Abs(ReadResult(results, index, 4, 2) - x.Z) <= DIRECTION_TOLERANCE);
    }
}
