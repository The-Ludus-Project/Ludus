#include "../src/internal/ltc_table.h"
#include "../src/internal/ltc_table_data.h"
#include "linearly_transformed_cosine_parity.h"
#include "linearly_transformed_cosine_parity_layout.h"
#include "ltc_table_parity.h"
#include "ltc_table_parity_layout.h"
#include "projected_solid_angle_parity.h"
#include "projected_solid_angle_parity_layout.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/math/linearly_transformed_cosine.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/projected_solid_angle.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <limits>
#include <span>

// GPU evidence for FoundationMath's shader ports of projected solid angle sampling
// (modules/foundation/math/shaders/projected_solid_angle.slang) and of linearly transformed cosines over
// polygons (shaders/linearly_transformed_cosine.slang), and for the renderer's shader lookup of its generated LTC table
// (shaders/ltc_table.slang). The C++ implementation is the
// reference; its accuracy against an exact float64 oracle is established by the FoundationMath
// tests. GPU transcendental functions differ by a few ulps, which the paper's single-precision
// analysis (Peters 2021, section 3.6) amplifies only for small projected solid angles and near the
// ends of a polygon, so the comparison uses percentiles.
namespace
{
using namespace ludus::foundation;
namespace math = ludus::foundation::math;
namespace rhi = ludus::graphics::rhi;
namespace internal = ludus::graphics::renderer::internal;
using S = rhi::RasterStatus;

constexpr uint64 TIMEOUT_NANOSECONDS = 10 * time::NANOSECONDS_PER_SECOND;
// Per-request readback limit; well below the default 256 KiB transfer profile.
constexpr usize READBACK_CHUNK_BYTES = usize{64} * 1024;
constexpr uint32 CASE_COUNT = 64 * LUDUS_PROJECTED_SOLID_ANGLE_PARITY_GROUP_SIZE;
constexpr uint64 RANDOM_SEED = 0x50E7E125ULL;
constexpr usize FLOATS_PER_WORD = 4;

template <typename Handle>
void WaitReady(rhi::DeviceHandle device, Handle handle)
{
    const uint64 start = time::NowTicks();
    S status = rhi::GetStatus(device, handle);
    while (status == S::Pending && time::NowTicks() - start < TIMEOUT_NANOSECONDS)
    {
        REQUIRE(rhi::PollLifetime(device) == S::Ready);
        status = rhi::GetStatus(device, handle);
    }
    REQUIRE(status == S::Ready);
}

bool IsAccepted(S status) noexcept
{
    return status == S::Ready || status == S::Pending;
}

float32 Draw(math::RandomStream& inoutRandom, float32 low, float32 high) noexcept
{
    return low + (high - low) * inoutRandom.NextFloat01();
}

/// A random convex polygon: points on an ellipse in a random plane, possibly crossing the horizon,
/// containing the zenith or wound counterclockwise.
math::ProjectedPolygonInput MakeRandomPolygon(math::RandomStream& inoutRandom) noexcept
{
    math::ProjectedPolygonInput result;
    uint32 count = 0;
    static_cast<void>(inoutRandom.TryNextBounded(math::MAX_PROJECTED_POLYGON_INPUT_VERTICES - 2, count));
    result.VertexCount = count + 3;
    const float32 cosine = Draw(inoutRandom, -0.4F, 1.0F);
    const float32 azimuth = Draw(inoutRandom, 0.0F, math::kTauF);
    const float32 sine = math::Sqrt(1.0F - cosine * cosine);
    const float32 distance = Draw(inoutRandom, 0.1F, 10.0F);
    const math::Vector3 center{sine * math::Cos(azimuth) * distance,
                               sine * math::Sin(azimuth) * distance,
                               cosine * distance};
    math::Vector3 normal{Draw(inoutRandom, -1.0F, 1.0F),
                         Draw(inoutRandom, -1.0F, 1.0F),
                         Draw(inoutRandom, -1.0F, 1.0F)};
    REQUIRE(math::TryNormalize(normal, normal) == math::MathStatus::Success);
    const math::Vector3 helper = math::Abs(normal.X) < 0.9F ? math::Vector3{1, 0, 0} : math::Vector3{0, 1, 0};
    math::Vector3 tangent;
    REQUIRE(math::TryNormalize(math::Cross(normal, helper), tangent) == math::MathStatus::Success);
    const math::Vector3 bitangent = math::Cross(normal, tangent);
    const float32 radius = distance * Draw(inoutRandom, 0.02F, 2.0F);
    const float32 secondRadius = radius * Draw(inoutRandom, 0.3F, 1.0F);
    const float32 phase = Draw(inoutRandom, 0.0F, math::kTauF);
    const bool isReversed = inoutRandom.NextFloat01() < 0.5F;
    for (uint32 i = 0; i < result.VertexCount; ++i)
    {
        const float32 angle = phase + (static_cast<float32>(i) + Draw(inoutRandom, 0.0F, 0.8F)) * math::kTauF /
                                          static_cast<float32>(result.VertexCount);
        const uint32 index = isReversed == true ? result.VertexCount - 1 - i : i;
        result.PositionsTangent[index] =
            center + tangent * (math::Cos(angle) * radius) + bitangent * (math::Sin(angle) * secondRadius);
    }
    return result;
}

/// Run a parity kernel over host-supplied cases on a native compute device and read back its results. Cases are bound
/// at slot 0 and results at slot 1; a nonempty table is bound read-only at slot 2.
/// @return false when no native GPU adapter is available; other failures fail the test.
// The dispatch's group count and the result buffer's byte size are distinct quantities.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool RunParityKernel(const rhi::RasterShaderDescription& shader,
                     const Array<float32>& cases,
                     uint32 groupCount,
                     usize resultBytes,
                     Array<uint8>& outResults,
                     std::span<const uint8> table = {})
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    constexpr usize MAX_RESOURCES = 3;
    const usize caseBytes = cases.GetSize() * sizeof(float32);
    const usize largestBytes = math::Max(math::Max(caseBytes, resultBytes), table.size());

    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    description.RequiredProfile.Raster.MinBufferSize = largestBytes;
    description.RequiredProfile.Compute.MinStorageRange = largestBytes;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        return false;
    }
    REQUIRE(started == rhi::DeviceStatus::Ready);

    rhi::BufferHandle caseBuffer;
    rhi::BufferHandle resultBuffer;
    rhi::BufferHandle tableBuffer;
    const bool hasTable = table.empty() == false;
    if (hasTable == true)
    {
        REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                             { .Role = rhi::BufferRole::Storage, .Size = table.size() },
                                             table,
                                             tableBuffer)) == true);
        WaitReady(device, tableBuffer);
    }
    REQUIRE(outResults.TryResize(resultBytes, uint8{0}) == true);
    const std::span<const uint8> caseSource{reinterpret_cast<const uint8*>(cases.GetData()), caseBytes};
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         {
                                             .Role = rhi::BufferRole::Storage,
                                             .Size = caseBytes,
                                         },
                                         caseSource,
                                         caseBuffer)) == true);
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         { .Role = rhi::BufferRole::Storage, .Size = resultBytes },
                                         std::span<const uint8>{outResults.GetData(), resultBytes},
                                         resultBuffer)) == true);
    rhi::RasterShaderHandle shaderHandle;
    rhi::BindingLayoutHandle layout;
    REQUIRE(IsAccepted(rhi::CreateRasterShader(device, shader, shaderHandle)) == true);
    REQUIRE(IsAccepted(rhi::CreateBindingLayout(device, shader.Bindings, layout)) == true);
    WaitReady(device, caseBuffer);
    WaitReady(device, resultBuffer);
    WaitReady(device, shaderHandle);
    WaitReady(device, layout);
    const StaticArray<rhi::RasterBindingResource, MAX_RESOURCES> resources{{
        { .Binding = 0, .Buffer = caseBuffer, .Size = caseBytes },
        { .Binding = 1, .Buffer = resultBuffer, .Size = resultBytes },
        { .Binding = 2, .Buffer = tableBuffer, .Size = table.size() },
    }};
    const usize resourceCount = hasTable == true ? MAX_RESOURCES : MAX_RESOURCES - 1;
    rhi::BindingSetHandle set;
    rhi::ComputePipelineHandle pipeline;
    REQUIRE(IsAccepted(
                rhi::CreateBindingSet(device,
                                      layout,
                                      std::span<const rhi::RasterBindingResource>{resources.GetData(), resourceCount},
                                      set)) == true);
    REQUIRE(IsAccepted(rhi::CreateComputePipeline(device, { .Shader = shaderHandle, .Layout = layout }, pipeline)) ==
            true);
    WaitReady(device, set);
    WaitReady(device, pipeline);

    rhi::OrderedGraph graph;
    REQUIRE(rhi::CreateOrderedGraph(device, graph) == S::Ready);
    rhi::GraphVersion caseVersion;
    rhi::GraphVersion initialResults;
    rhi::GraphVersion results;
    REQUIRE(rhi::ImportGraphBuffer(device, graph, caseBuffer, caseVersion) == S::Ready);
    REQUIRE(rhi::ImportGraphBuffer(device, graph, resultBuffer, initialResults) == S::Ready);
    REQUIRE(rhi::NextGraphVersion(device, graph, initialResults, results) == S::Ready);
    rhi::GraphVersion tableVersion;
    if (hasTable == true)
    {
        REQUIRE(rhi::ImportGraphBuffer(device, graph, tableBuffer, tableVersion) == S::Ready);
    }
    const StaticArray<rhi::GraphUse, MAX_RESOURCES> uses{{
        {caseVersion, rhi::GraphAccessMode::StorageRead, 0, caseBytes, rhi::RasterVisibility::Compute},
        {results, rhi::GraphAccessMode::StorageReadWrite, 0, resultBytes, rhi::RasterVisibility::Compute},
        {tableVersion, rhi::GraphAccessMode::StorageRead, 0, table.size(), rhi::RasterVisibility::Compute},
    }};
    const rhi::ComputeDispatch dispatch
    {
        .Pipeline = pipeline,
        .Bindings = set,
        .Groups = {groupCount, 1, 1},
    };
    rhi::GraphPassDescription pass;
    pass.Name = "FoundationMath parity";
    pass.Uses = uses.GetData();
    pass.UseCount = static_cast<uint32>(resourceCount);
    pass.SideEffect = true;
    pass.Dispatch = &dispatch;
    REQUIRE(rhi::AddGraphPass(device, graph, pass) == S::Ready);
    rhi::GraphReport report;
    REQUIRE(rhi::CompileOrderedGraph(device, graph, report) == S::Ready);
    REQUIRE(rhi::SetFrameTarget(device, surface, {16, 16, 0, 0, 0, 1}) == rhi::DeviceStatus::Ready);
    rhi::SubmissionToken completion;
    const uint64 start = time::NowTicks();
    S executed = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    while (executed == S::NotReady && time::NowTicks() - start < TIMEOUT_NANOSECONDS)
    {
        executed = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    }
    REQUIRE(executed == S::Ready);
    WaitReady(device, completion);
    for (usize offset = 0; offset < resultBytes; offset += READBACK_CHUNK_BYTES)
    {
        const usize size = resultBytes - offset < READBACK_CHUNK_BYTES ? resultBytes - offset : READBACK_CHUNK_BYTES;
        rhi::ReadbackTicket ticket;
        S readback = rhi::RequestBufferReadback(device, resultBuffer, offset, size, ticket);
        const uint64 readbackStart = time::NowTicks();
        while (readback == S::Pending && time::NowTicks() - readbackStart < TIMEOUT_NANOSECONDS)
        {
            REQUIRE(rhi::PollLifetime(device) == S::Ready);
            readback = rhi::GetStatus(device, ticket);
        }
        REQUIRE(readback == S::Ready);
        REQUIRE(rhi::CopyReadback(device, ticket, outResults.GetData() + offset, size) == S::Ready);
        REQUIRE(rhi::Release(device, ticket) == S::Ready);
    }
    return true;
}

float32 GetPercentile(Array<float32>& inoutValues, float32 fraction) noexcept
{
    Sort(inoutValues.GetData(), inoutValues.GetSize(), [](float32 a, float32 b) noexcept { return a < b; });
    return inoutValues[static_cast<usize>(fraction * static_cast<float32>(inoutValues.GetSize() - 1))];
}
} // namespace

TEST_CASE("GPU projected solid angle sampling matches the C++ implementation", "[renderer][gpu][polygon-sampling]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;

    // Boundary configurations first (paper supplement A.2 and Fig. 12h), then a random corpus.
    Array<math::ProjectedPolygonInput> polygons;
    Array<math::Vector2> randomNumbers;
    REQUIRE(polygons.TryResize(CASE_COUNT) == true);
    REQUIRE(randomNumbers.TryResize(CASE_COUNT) == true);
    const math::Vector3 fixed[][4] = {
        {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},
        {{0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {}},
        {{1, 0, 1}, {2, 0, 1}, {2, 1, 1}, {1, 1, 1}},
        {{1, -1, 0.5F}, {1, -1, 2}, {1, 1, 2}, {1, 1, 0.5F}},
        {{1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}},
        {{1, 0, -1}, {1, 1, -1}, {0, 1, -2}, {}},
        {{0, 0, 0}, {1, 0, 1}, {0, 1, 1}, {}},
        {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}, {}},
        {{-1, -1.0e-5F, 1}, {1, -1.0e-5F, 1}, {0, 1.0e-5F, 1}, {}},
    };
    constexpr uint32 FIXED_VERTEX_COUNTS[] = {4, 3, 4, 4, 4, 3, 3, 3, 3};
    constexpr uint32 FIXED_COUNT = sizeof(FIXED_VERTEX_COUNTS) / sizeof(FIXED_VERTEX_COUNTS[0]);
    math::RandomStream random;
    REQUIRE(random.TryReseed(RANDOM_SEED, 4) == true);
    for (uint32 i = 0; i < CASE_COUNT; ++i)
    {
        if (i < FIXED_COUNT * 16)
        {
            math::ProjectedPolygonInput& polygon = polygons[i];
            polygon.VertexCount = FIXED_VERTEX_COUNTS[i / 16];
            for (uint32 j = 0; j < polygon.VertexCount; ++j)
            {
                polygon.PositionsTangent[j] = fixed[i / 16][j];
            }
        }
        else
        {
            polygons[i] = MakeRandomPolygon(random);
        }
        randomNumbers[i] = math::Vector2{random.NextFloat01(), random.NextFloat01()};
    }

    Array<float32> cases;
    REQUIRE(cases.TryResize(usize{CASE_COUNT} * LUDUS_PROJECTED_SOLID_ANGLE_PARITY_WORDS_PER_CASE * FLOATS_PER_WORD,
                            0.0F) == true);
    for (uint32 i = 0; i < CASE_COUNT; ++i)
    {
        float32* words =
            cases.GetData() + usize{i} * LUDUS_PROJECTED_SOLID_ANGLE_PARITY_WORDS_PER_CASE * FLOATS_PER_WORD;
        for (uint32 j = 0; j < math::MAX_PROJECTED_POLYGON_INPUT_VERTICES; ++j)
        {
            words[j * FLOATS_PER_WORD] = polygons[i].PositionsTangent[j].X;
            words[j * FLOATS_PER_WORD + 1] = polygons[i].PositionsTangent[j].Y;
            words[j * FLOATS_PER_WORD + 2] = polygons[i].PositionsTangent[j].Z;
        }
        float32* parameters = words + math::MAX_PROJECTED_POLYGON_INPUT_VERTICES * FLOATS_PER_WORD;
        parameters[0] = static_cast<float32>(polygons[i].VertexCount);
        parameters[1] = randomNumbers[i].X;
        parameters[2] = randomNumbers[i].Y;
    }
    const usize resultBytes =
        usize{CASE_COUNT} * LUDUS_PROJECTED_SOLID_ANGLE_PARITY_WORDS_PER_RESULT * FLOATS_PER_WORD * sizeof(float32);
    Array<uint8> resultBytesStorage;
    if (RunParityKernel(ludus::shaders::projected_solid_angle_parity::Compute(),
                        cases,
                        CASE_COUNT / LUDUS_PROJECTED_SOLID_ANGLE_PARITY_GROUP_SIZE,
                        resultBytes,
                        resultBytesStorage) == false)
    {
        SKIP("No native GPU adapter");
    }

    Array<float32> areaErrors;
    Array<float32> directionErrors;
    uint32 sampledCount = 0;
    uint32 centralCount = 0;
    for (uint32 i = 0; i < CASE_COUNT; ++i)
    {
        float32 words[LUDUS_PROJECTED_SOLID_ANGLE_PARITY_WORDS_PER_RESULT * FLOATS_PER_WORD];
        std::memcpy(words, resultBytesStorage.GetData() + usize{i} * sizeof(words), sizeof(words));
        INFO("case " << i << ", vertices " << polygons[i].VertexCount);
        math::ProjectedSolidAnglePolygon expected;
        REQUIRE(math::TryPrepareProjectedSolidAnglePolygon(polygons[i], expected) == math::MathStatus::Success);
        REQUIRE(words[0] == 1.0F);
        REQUIRE(words[2] == (expected.IsMirrored == true ? 1.0F : 0.0F));
        REQUIRE(words[3] == static_cast<float32>(expected.VertexCount));
        const float32 projectedSolidAngle = words[1];
        REQUIRE(math::Abs(projectedSolidAngle - expected.ProjectedSolidAngle) <=
                1.0e-6F + 1.0e-3F * expected.ProjectedSolidAngle);
        // The raster integral must agree with the independent CPU sampling preparation,
        // including reversed winding, horizon clips and thin/distant polygons.
        REQUIRE(math::Abs(words[10] - expected.ProjectedSolidAngle) <=
                1.0e-6F + 1.0e-3F * expected.ProjectedSolidAngle);
        REQUIRE(math::Abs(words[11] - expected.ProjectedSolidAngle) <=
                1.0e-6F + 1.0e-3F * expected.ProjectedSolidAngle);
        math::Vector3 expectedDirection;
        const bool isSampled =
            math::TrySampleProjectedSolidAnglePolygon(expected, randomNumbers[i], expectedDirection) ==
            math::MathStatus::Success;
        REQUIRE(words[4] == (isSampled == true ? 1.0F : 0.0F));
        if (isSampled == false)
        {
            continue;
        }
        ++sampledCount;
        REQUIRE(words[9] == (expected.IsCentralCase == true ? 1.0F : 0.0F));
        centralCount += expected.IsCentralCase == true ? 1 : 0;
        const math::Vector3 direction{words[5], words[6], words[7]};
        REQUIRE(math::Abs(math::Length(direction) - 1.0F) <= 1.0e-5F);
        REQUIRE(areaErrors.TryAdd(math::Abs(projectedSolidAngle - expected.ProjectedSolidAngle) /
                                  expected.ProjectedSolidAngle) == true);
        REQUIRE(directionErrors.TryAdd(math::Length(direction - expectedDirection)) == true);
        REQUIRE(math::Abs(words[8] - direction.Z / projectedSolidAngle) <= 1.0e-6F * words[8]);
    }
    REQUIRE(sampledCount > CASE_COUNT / 2);
    REQUIRE(centralCount > 0);
    REQUIRE(centralCount < sampledCount);
    REQUIRE(GetPercentile(areaErrors, 0.99F) <= 1.0e-4F);
    REQUIRE(GetPercentile(directionErrors, 0.99F) <= 1.0e-4F);
    REQUIRE(GetPercentile(directionErrors, 1.0F) <= 5.0e-2F);
}

namespace
{
constexpr uint32 LTC_CASE_COUNT = 64 * LUDUS_LINEARLY_TRANSFORMED_COSINE_PARITY_GROUP_SIZE;
constexpr uint32 LTC_MATRIX_COLUMN_COUNT = 3;
// The sharpest specular lobe the renderer admits: perceptual roughness 0.045, alpha = 0.045^2.
constexpr float32 MINIMUM_ALPHA = 0.045F * 0.045F;
// LTC integrals below this may sample inaccurately in single precision (Peters 2021, section 3.7); their samples are
// not compared.
constexpr float32 NEGLIGIBLE_INTEGRAL = 1.0e-6F;
// Smaller integrals belong to slivers whose relative error is dominated by rounding of the transformed vertices
// (compare Peters 2021, section 3.6); they are held to the absolute bound only.
constexpr float32 MINIMUM_STATISTICS_INTEGRAL = 1.0e-4F;
// The sampler lifts a projected-disk point to z = sqrt(1 - x^2 - y^2) (Peters 2021, Equation 2), so near the horizon
// it resolves z only in steps of about sqrt(2^-24) = 2.4e-4. A sliver hugging the transformed polygon's horizon can be
// thinner than one such step, and a GPU sample one ulp away from the C++ sample then rounds onto or just past the
// horizon, where the density is zero. Such samples are accepted below MINIMUM_STATISTICS_INTEGRAL if they lie within
// sqrt(8 * 2^-24) of the horizon in cosine space.
constexpr float32 HORIZON_LIFT_RESOLUTION = 6.9053e-4F;
// At most one sliver sample in this many may land on the horizon.
constexpr uint32 SLIVER_SAMPLES_PER_HORIZON_SAMPLE = 100;

/// An isotropic BRDF fit's inverse transform (the planar form of Heitz et al. 2016, Equation 6), rotated by a random
/// azimuth about the normal.
math::Matrix3 MakeRandomInverseTransform(math::RandomStream& inoutRandom) noexcept
{
    const float32 logInverseMinimum = static_cast<float32>(-math::Log(static_cast<float64>(MINIMUM_ALPHA)));
    const float32 inverseAlphaX =
        static_cast<float32>(math::Exp(static_cast<float64>(Draw(inoutRandom, 0.0F, logInverseMinimum))));
    const float32 inverseAlphaY =
        static_cast<float32>(math::Exp(static_cast<float64>(Draw(inoutRandom, 0.0F, logInverseMinimum))));
    const math::Matrix3 canonical{{
        math::Vector3{inverseAlphaX, 0.0F, Draw(inoutRandom, -0.7F, 0.7F)},
        math::Vector3{0.0F, inverseAlphaY, 0.0F},
        math::Vector3{Draw(inoutRandom, -0.7F, 0.7F) * inverseAlphaX, 0.0F, 1.0F},
    }};
    const float32 azimuth = Draw(inoutRandom, 0.0F, math::kTauF);
    const float32 cosine = math::Cos(azimuth);
    const float32 sine = math::Sin(azimuth);
    const math::Matrix3 rotationTransposed{{
        math::Vector3{cosine, -sine, 0.0F},
        math::Vector3{sine, cosine, 0.0F},
        math::Vector3{0.0F, 0.0F, 1.0F},
    }};
    return canonical * rotationTransposed;
}
} // namespace

TEST_CASE("GPU linearly transformed cosine sampling matches the C++ implementation",
          "[renderer][gpu][polygon-sampling]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;

    // Boundary configurations first: the identity, a shear whose support reaches below the horizon, a shear whose
    // support excludes the light, then rejected matrices (zero, mirror, nonfinite); then a random corpus.
    const float32 notANumber = std::numeric_limits<float32>::quiet_NaN();
    const math::Matrix3 fixedMatrices[] = {
        math::Matrix3::Identity(),
        math::Matrix3{{math::Vector3{1, 0, 0.5F}, math::Vector3{0, 1, 0}, math::Vector3{0, 0, 1}}},
        math::Matrix3{{math::Vector3{1, 0, -4}, math::Vector3{0, 1, 0}, math::Vector3{0, 0, 1}}},
        math::Matrix3::Zero(),
        math::Matrix3{{math::Vector3{1, 0, 0}, math::Vector3{0, 1, 0}, math::Vector3{0, 0, -1}}},
        math::Matrix3{{math::Vector3{1, 0, 0}, math::Vector3{0, notANumber, 0}, math::Vector3{0, 0, 1}}},
    };
    constexpr uint32 FIXED_MATRIX_COUNT = sizeof(fixedMatrices) / sizeof(fixedMatrices[0]);
    constexpr uint32 CASES_PER_FIXED_MATRIX = 16;
    math::ProjectedPolygonInput straddling;
    straddling.VertexCount = 4;
    straddling.PositionsTangent[0] = math::Vector3{1, -0.5F, -0.3F};
    straddling.PositionsTangent[1] = math::Vector3{1, 0.5F, -0.3F};
    straddling.PositionsTangent[2] = math::Vector3{1, 0.5F, 0.3F};
    straddling.PositionsTangent[3] = math::Vector3{1, -0.5F, 0.3F};
    Array<math::ProjectedPolygonInput> polygons;
    Array<math::Matrix3> matrices;
    Array<math::Vector2> randomNumbers;
    REQUIRE(polygons.TryResize(LTC_CASE_COUNT) == true);
    REQUIRE(matrices.TryResize(LTC_CASE_COUNT) == true);
    REQUIRE(randomNumbers.TryResize(LTC_CASE_COUNT) == true);
    math::RandomStream random;
    REQUIRE(random.TryReseed(RANDOM_SEED, 5) == true);
    for (uint32 i = 0; i < LTC_CASE_COUNT; ++i)
    {
        if (i < FIXED_MATRIX_COUNT * CASES_PER_FIXED_MATRIX)
        {
            matrices[i] = fixedMatrices[i / CASES_PER_FIXED_MATRIX];
            polygons[i] = straddling;
        }
        else
        {
            matrices[i] = MakeRandomInverseTransform(random);
            polygons[i] = MakeRandomPolygon(random);
        }
        randomNumbers[i] = math::Vector2{random.NextFloat01(), random.NextFloat01()};
    }

    Array<float32> cases;
    REQUIRE(cases.TryResize(usize{LTC_CASE_COUNT} * LUDUS_LINEARLY_TRANSFORMED_COSINE_PARITY_WORDS_PER_CASE *
                                FLOATS_PER_WORD,
                            0.0F) == true);
    for (uint32 i = 0; i < LTC_CASE_COUNT; ++i)
    {
        float32* words =
            cases.GetData() + usize{i} * LUDUS_LINEARLY_TRANSFORMED_COSINE_PARITY_WORDS_PER_CASE * FLOATS_PER_WORD;
        for (uint32 j = 0; j < math::MAX_PROJECTED_POLYGON_INPUT_VERTICES; ++j)
        {
            words[j * FLOATS_PER_WORD] = polygons[i].PositionsTangent[j].X;
            words[j * FLOATS_PER_WORD + 1] = polygons[i].PositionsTangent[j].Y;
            words[j * FLOATS_PER_WORD + 2] = polygons[i].PositionsTangent[j].Z;
        }
        float32* parameters = words + math::MAX_PROJECTED_POLYGON_INPUT_VERTICES * FLOATS_PER_WORD;
        parameters[0] = static_cast<float32>(polygons[i].VertexCount);
        parameters[1] = randomNumbers[i].X;
        parameters[2] = randomNumbers[i].Y;
        for (uint32 j = 0; j < LTC_MATRIX_COLUMN_COUNT; ++j)
        {
            float32* column = parameters + (j + 1) * FLOATS_PER_WORD;
            column[0] = matrices[i].Columns[j].X;
            column[1] = matrices[i].Columns[j].Y;
            column[2] = matrices[i].Columns[j].Z;
        }
    }
    const usize resultBytes = usize{LTC_CASE_COUNT} * LUDUS_LINEARLY_TRANSFORMED_COSINE_PARITY_WORDS_PER_RESULT *
                              FLOATS_PER_WORD * sizeof(float32);
    Array<uint8> resultBytesStorage;
    if (RunParityKernel(ludus::shaders::linearly_transformed_cosine_parity::Compute(),
                        cases,
                        LTC_CASE_COUNT / LUDUS_LINEARLY_TRANSFORMED_COSINE_PARITY_GROUP_SIZE,
                        resultBytes,
                        resultBytesStorage) == false)
    {
        SKIP("No native GPU adapter");
    }

    Array<float32> integralErrors;
    Array<float32> directionErrors;
    Array<float32> densityErrors;
    uint32 rejectedCount = 0;
    uint32 emptyCount = 0;
    uint32 sampledCount = 0;
    uint32 belowHorizonCount = 0;
    uint32 sliverSampledCount = 0;
    uint32 horizonCount = 0;
    for (uint32 i = 0; i < LTC_CASE_COUNT; ++i)
    {
        float32 words[LUDUS_LINEARLY_TRANSFORMED_COSINE_PARITY_WORDS_PER_RESULT * FLOATS_PER_WORD];
        std::memcpy(words, resultBytesStorage.GetData() + usize{i} * sizeof(words), sizeof(words));
        INFO("case " << i << ", vertices " << polygons[i].VertexCount);
        math::LinearlyTransformedCosine distribution;
        const bool isMade =
            math::TryMakeLinearlyTransformedCosine(matrices[i], distribution) == math::MathStatus::Success;
        REQUIRE(words[0] == (isMade == true ? 1.0F : 0.0F));
        if (isMade == false)
        {
            ++rejectedCount;
            continue;
        }
        REQUIRE(math::Abs(words[10] - distribution.InverseDeterminant) <= 1.0e-5F * distribution.InverseDeterminant);
        math::LinearlyTransformedCosinePolygon expected;
        REQUIRE(math::TryPrepareLinearlyTransformedCosinePolygon(polygons[i], distribution, expected) ==
                math::MathStatus::Success);
        REQUIRE(words[1] == 1.0F);
        REQUIRE(words[3] == static_cast<float32>(expected.TransformedPolygon.VertexCount));
        const float32 integral = math::GetLinearlyTransformedCosinePolygonIntegral(expected);
        REQUIRE(math::Abs(words[11] - integral) <= 1.0e-6F + 1.0e-3F * integral);
        REQUIRE(math::Abs(words[2] - integral) <= 1.0e-6F + 1.0e-3F * integral);
        if (integral < NEGLIGIBLE_INTEGRAL)
        {
            if (integral == 0.0F && words[2] == 0.0F)
            {
                // An empty clip disables the technique on both sides. Rounding may leave one side a sliver.
                ++emptyCount;
                REQUIRE(words[4] == 0.0F);
            }
            continue;
        }
        if (integral >= MINIMUM_STATISTICS_INTEGRAL)
        {
            REQUIRE(integralErrors.TryAdd(math::Abs(words[2] - integral) / integral) == true);
        }
        math::Vector3 expectedDirection;
        REQUIRE(math::TrySampleLinearlyTransformedCosinePolygon(expected, randomNumbers[i], expectedDirection) ==
                math::MathStatus::Success);
        REQUIRE(words[4] == 1.0F);
        ++sampledCount;
        const math::Vector3 direction{words[5], words[6], words[7]};
        belowHorizonCount += direction.Z < 0.0F ? 1 : 0;
        REQUIRE(math::Abs(math::Length(direction) - 1.0F) <= 1.0e-5F);
        if (integral >= MINIMUM_STATISTICS_INTEGRAL)
        {
            REQUIRE(directionErrors.TryAdd(math::Length(direction - expectedDirection)) == true);
        }
        // Evaluate the C++ densities at the GPU's direction to compare the evaluators alone.
        const float32 expectedDensity = math::EvaluateLinearlyTransformedCosinePolygonDensity(expected, direction);
        if (integral < MINIMUM_STATISTICS_INTEGRAL)
        {
            ++sliverSampledCount;
            if (expectedDensity == 0.0F)
            {
                // A horizon sample of a sliver; see HORIZON_LIFT_RESOLUTION. Its densities are zero, not compared.
                const math::Vector3 cosineDirection = distribution.InverseTransform * direction;
                REQUIRE(-cosineDirection.Z <= HORIZON_LIFT_RESOLUTION * math::Length(cosineDirection));
                ++horizonCount;
                continue;
            }
        }
        REQUIRE(expectedDensity > 0.0F);
        if (integral >= MINIMUM_STATISTICS_INTEGRAL)
        {
            REQUIRE(densityErrors.TryAdd(math::Abs(words[8] - expectedDensity) / expectedDensity) == true);
        }
        const float32 expectedLobe = math::EvaluateLinearlyTransformedCosine(distribution, direction);
        REQUIRE(math::Abs(words[9] - expectedLobe) <= 1.0e-4F * expectedLobe);
    }
    CAPTURE(rejectedCount, emptyCount, sampledCount, belowHorizonCount, sliverSampledCount, horizonCount);
    REQUIRE(rejectedCount == 3 * CASES_PER_FIXED_MATRIX);
    REQUIRE(emptyCount >= CASES_PER_FIXED_MATRIX);
    REQUIRE(sampledCount > LTC_CASE_COUNT / 4);
    REQUIRE(belowHorizonCount > 0);
    // Apple M5 measured one horizon sample (cosine-space z of zero) in 889 sliver samples.
    REQUIRE(horizonCount * SLIVER_SAMPLES_PER_HORIZON_SAMPLE <= sliverSampledCount);
    // Apple M5 measured p99 relative integral and density errors of 1.9e-4 (largest 5.2e-4) and a p99 direction
    // error of 1.8e-4 (largest 1.5e-3); the GPU rounds the transformed vertices differently.
    REQUIRE(GetPercentile(integralErrors, 0.99F) <= 5.0e-4F);
    REQUIRE(GetPercentile(integralErrors, 1.0F) <= 2.0e-3F);
    REQUIRE(GetPercentile(densityErrors, 0.99F) <= 5.0e-4F);
    REQUIRE(GetPercentile(directionErrors, 0.99F) <= 5.0e-4F);
    REQUIRE(GetPercentile(directionErrors, 1.0F) <= 5.0e-2F);
}

TEST_CASE("GPU LTC table lookups match the C++ lookup", "[renderer][gpu][ltc]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;

    constexpr uint32 LOOKUP_CASE_COUNT = 64 * LUDUS_LTC_TABLE_PARITY_GROUP_SIZE;
    constexpr uint32 INVERSE_COLUMN_COUNT = 3;
    // Roughness outside the table, views below the horizon or along the normal, and F0 outside [0, 1] exercise the
    // clamps: (roughness, view xyz, F0 rgb).
    constexpr usize FIXED_CASE_FLOATS = 7;
    constexpr float32 FIXED_CASES[][FIXED_CASE_FLOATS] = {
        {0.0F, 0.0F, 0.0F, 1.0F, 0.04F, 0.04F, 0.04F},
        {0.045F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F, 1.0F},
        {1.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
        {1.5F, 0.6F, 0.0F, 0.8F, 2.0F, 2.0F, 2.0F},
        {0.3F, 0.0F, 0.6F, -0.8F, -1.0F, 0.5F, 0.2F},
        {0.3F, -0.6F, 0.0F, 0.8F, 0.95F, 0.64F, 0.54F},
    };
    // A random F0 is redrawn when its luminance lies this close to the boundary between two slices, where the GPU's
    // rounding of the luminance could select the other slice.
    constexpr float32 SLICE_BOUNDARY_MARGIN = 1.0e-3F;
    constexpr math::Vector3 LUMINANCE_WEIGHTS{0.2126F, 0.7152F, 0.0722F};
    constexpr uint32 FIXED_CASE_COUNT = sizeof(FIXED_CASES) / sizeof(FIXED_CASES[0]);
    math::RandomStream random;
    REQUIRE(random.TryReseed(RANDOM_SEED, 6) == true);
    Array<float32> cases;
    REQUIRE(cases.TryResize(usize{LOOKUP_CASE_COUNT} * LUDUS_LTC_TABLE_PARITY_WORDS_PER_CASE * FLOATS_PER_WORD, 0.0F) ==
            true);
    for (uint32 i = 0; i < LOOKUP_CASE_COUNT; ++i)
    {
        float32* words = cases.GetData() + usize{i} * LUDUS_LTC_TABLE_PARITY_WORDS_PER_CASE * FLOATS_PER_WORD;
        if (i < FIXED_CASE_COUNT)
        {
            for (usize j = 0; j < FIXED_CASE_FLOATS; ++j)
            {
                words[j] = FIXED_CASES[i][j];
            }
            continue;
        }
        const float32 cosine = random.NextFloat01();
        const float32 sine = math::Sqrt(1.0F - cosine * cosine);
        const float32 azimuth = Draw(random, 0.0F, math::kTauF);
        words[0] = Draw(random, 0.045F, 1.0F);
        words[1] = sine * math::Cos(azimuth);
        words[2] = sine * math::Sin(azimuth);
        words[3] = cosine;
        float32 boundaryDistance = 0.0F;
        while (boundaryDistance < SLICE_BOUNDARY_MARGIN)
        {
            for (uint32 j = 0; j < 3; ++j)
            {
                words[FLOATS_PER_WORD + j] = random.NextFloat01();
            }
            const float32 slice =
                math::Dot(math::Vector3{words[FLOATS_PER_WORD], words[FLOATS_PER_WORD + 1], words[FLOATS_PER_WORD + 2]},
                          LUMINANCE_WEIGHTS) *
                static_cast<float32>(LUDUS_LTC_TABLE_FRESNEL_COUNT - 1);
            boundaryDistance = math::Abs(slice - static_cast<float32>(static_cast<uint32>(slice)) - 0.5F);
        }
    }
    const usize resultBytes =
        usize{LOOKUP_CASE_COUNT} * LUDUS_LTC_TABLE_PARITY_WORDS_PER_RESULT * FLOATS_PER_WORD * sizeof(float32);
    Array<uint8> resultBytesStorage;
    if (RunParityKernel(ludus::shaders::ltc_table_parity::Compute(),
                        cases,
                        LOOKUP_CASE_COUNT / LUDUS_LTC_TABLE_PARITY_GROUP_SIZE,
                        resultBytes,
                        resultBytesStorage,
                        std::span<const uint8>{reinterpret_cast<const uint8*>(internal::LTC_TABLE.GetData()),
                                               sizeof(internal::LTC_TABLE)}) == false)
    {
        SKIP("No native GPU adapter");
    }

    Array<float32> errors;
    for (uint32 i = 0; i < LOOKUP_CASE_COUNT; ++i)
    {
        float32 words[LUDUS_LTC_TABLE_PARITY_WORDS_PER_RESULT * FLOATS_PER_WORD];
        std::memcpy(words, resultBytesStorage.GetData() + usize{i} * sizeof(words), sizeof(words));
        const float32* parameters =
            cases.GetData() + usize{i} * LUDUS_LTC_TABLE_PARITY_WORDS_PER_CASE * FLOATS_PER_WORD;
        const math::Vector3 f0{parameters[FLOATS_PER_WORD],
                               parameters[FLOATS_PER_WORD + 1],
                               parameters[FLOATS_PER_WORD + 2]};
        INFO("case " << i << ", roughness " << parameters[0] << ", view z " << parameters[3] << ", F0 slice "
                     << internal::GetLtcTableFresnelSlice(f0));
        const internal::SpecularLtc expected =
            internal::LookupSpecularLtc(parameters[0], math::Vector3{parameters[1], parameters[2], parameters[3]}, f0);
        // Entries are compared relative to the matrix's largest entry, the scale the LTC is invariant to.
        float32 scale = 0.0F;
        for (uint32 j = 0; j < INVERSE_COLUMN_COUNT; ++j)
        {
            scale = math::Max(scale,
                              math::Max(math::Abs(expected.InverseTransform.Columns[j].X),
                                        math::Max(math::Abs(expected.InverseTransform.Columns[j].Y),
                                                  math::Abs(expected.InverseTransform.Columns[j].Z))));
        }
        float32 error = 0.0F;
        for (uint32 j = 0; j < INVERSE_COLUMN_COUNT; ++j)
        {
            const math::Vector3 column{words[j * FLOATS_PER_WORD],
                                       words[j * FLOATS_PER_WORD + 1],
                                       words[j * FLOATS_PER_WORD + 2]};
            error = math::Max(error, math::Length(column - expected.InverseTransform.Columns[j]) / scale);
        }
        const float32* moments = words + INVERSE_COLUMN_COUNT * FLOATS_PER_WORD;
        error = math::Max(error, math::Abs(moments[0] - expected.Norm));
        error = math::Max(error, math::Abs(moments[1] - expected.FresnelNorm));
        REQUIRE(errors.TryAdd(error) == true);
        if (i < FIXED_CASE_COUNT)
        {
            REQUIRE(error <= 1.0e-5F);
        }
    }
    // Apple M5 measured a p99 of 1.4e-6 and a largest error of 1.2e-5; the GPU's arctangent differs by a few ulps.
    // Lavapipe measured a p99 of about 2e-5; Vulkan bounds atan only to 4096 ulps.
    REQUIRE(GetPercentile(errors, 0.99F) <= 5.0e-5F);
    REQUIRE(GetPercentile(errors, 1.0F) <= 1.0e-4F);
}
