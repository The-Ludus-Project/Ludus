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
#include <span>

// GPU evidence for FoundationMath's shader port of projected solid angle sampling
// (modules/foundation/math/shaders/projected_solid_angle.slang). The C++ implementation is the
// reference; its accuracy against an exact float64 oracle is established by the FoundationMath
// tests. GPU transcendental functions differ by a few ulps, which the paper's single-precision
// analysis (Peters 2021, section 3.6) amplifies only for small projected solid angles and near the
// ends of a polygon, so the comparison uses percentiles.
namespace
{
using namespace ludus::foundation;
namespace math = ludus::foundation::math;
namespace rhi = ludus::graphics::rhi;
using S = rhi::RasterStatus;

constexpr uint64 TIMEOUT_NANOSECONDS = 10 * time::NANOSECONDS_PER_SECOND;
// Per-request readback limit; well below the default 256 KiB transfer profile.
constexpr usize READBACK_CHUNK_BYTES = usize{64} * 1024;
constexpr uint32 CASE_COUNT = 64 * LUDUS_PROJECTED_SOLID_ANGLE_PARITY_GROUP_SIZE;
constexpr uint64 RANDOM_SEED = 0x50E7E125ULL;
constexpr usize FLOATS_PER_WORD = 4;
constexpr usize MATRIX_ROWS = 3;
constexpr float32 MINIMUM_DETERMINANT_FACTOR = 0.02F;
// LTC integrals below this contribute nothing measurable; sampling may succeed on only one side.
constexpr float32 NEGLIGIBLE_LTC_INTEGRAL = 1.0e-7F;
// Relative statistics exclude tiny lights, whose sector areas cancel in single precision (Peters section 3.6); every
// case still meets the absolute bound.
constexpr float32 MINIMUM_STATISTICS_SOLID_ANGLE = 1.0e-3F;

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
    Array<math::Matrix3> inverseTransforms;
    REQUIRE(inverseTransforms.TryResize(CASE_COUNT) == true);
    const math::Vector3 fixed[][4] = {
        {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},
        {{0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {}},
        {{1, 0, 1}, {2, 0, 1}, {2, 1, 1}, {1, 1, 1}},
        {{1, -1, 0.5F}, {1, -1, 2}, {1, 1, 2}, {1, 1, 0.5F}},
        {{1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}},
        {{1, 0, -1}, {1, 1, -1}, {0, 1, -2}, {}},
    };
    constexpr uint32 FIXED_VERTEX_COUNTS[] = {4, 3, 4, 4, 4, 3};
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
        // An isotropic LTC, M = [[a, 0, b], [0, c, 0], [d, 0, 1]] (Heitz et al. 2016, Equation 6).
        const float32 a = Draw(random, 0.05F, 1.5F);
        const float32 b = Draw(random, -0.6F, 0.6F);
        const float32 c = Draw(random, 0.05F, 1.5F);
        const float32 skew = Draw(random, -0.6F, 0.6F);
        // Keep det(M) = c (a - b d) clearly positive.
        const float32 d = a - b * skew > MINIMUM_DETERMINANT_FACTOR ? skew : 0.0F;
        const math::Matrix3 transform{{math::Vector3{a, 0, d}, math::Vector3{0, c, 0}, math::Vector3{b, 0, 1}}};
        REQUIRE(math::TryInverse(transform, inverseTransforms[i]) == math::MathStatus::Success);
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
        float32* rows = words + LUDUS_PROJECTED_SOLID_ANGLE_PARITY_LTC_WORD * FLOATS_PER_WORD;
        for (usize row = 0; row < MATRIX_ROWS; ++row)
        {
            for (usize column = 0; column < MATRIX_ROWS; ++column)
            {
                rows[row * FLOATS_PER_WORD + column] = inverseTransforms[i].At(row, column);
            }
        }
    }
    const usize caseBytes = cases.GetSize() * sizeof(float32);
    const usize resultBytes =
        usize{CASE_COUNT} * LUDUS_PROJECTED_SOLID_ANGLE_PARITY_WORDS_PER_RESULT * FLOATS_PER_WORD * sizeof(float32);

    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    description.RequiredProfile.Raster.MinBufferSize = caseBytes;
    description.RequiredProfile.Compute.MinStorageRange = caseBytes;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No native GPU adapter");
    }
    REQUIRE(started == rhi::DeviceStatus::Ready);

    rhi::BufferHandle caseBuffer;
    rhi::BufferHandle resultBuffer;
    Array<uint8> resultBytesStorage;
    REQUIRE(resultBytesStorage.TryResize(resultBytes, uint8{0}) == true);
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
                                         std::span<const uint8>{resultBytesStorage.GetData(), resultBytes},
                                         resultBuffer)) == true);
    const rhi::RasterShaderDescription shader = ludus::shaders::projected_solid_angle_parity::Compute();
    rhi::RasterShaderHandle shaderHandle;
    rhi::BindingLayoutHandle layout;
    REQUIRE(IsAccepted(rhi::CreateRasterShader(device, shader, shaderHandle)) == true);
    REQUIRE(IsAccepted(rhi::CreateBindingLayout(device, shader.Bindings, layout)) == true);
    WaitReady(device, caseBuffer);
    WaitReady(device, resultBuffer);
    WaitReady(device, shaderHandle);
    WaitReady(device, layout);
    const rhi::RasterBindingResource resources[] = {
        { .Binding = 0, .Buffer = caseBuffer, .Size = caseBytes },
        { .Binding = 1, .Buffer = resultBuffer, .Size = resultBytes },
    };
    rhi::BindingSetHandle set;
    rhi::ComputePipelineHandle pipeline;
    REQUIRE(IsAccepted(rhi::CreateBindingSet(device, layout, resources, set)) == true);
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
    const rhi::GraphUse uses[] = {
        {caseVersion, rhi::GraphAccessMode::StorageRead, 0, caseBytes, rhi::RasterVisibility::Compute},
        {results, rhi::GraphAccessMode::StorageReadWrite, 0, resultBytes, rhi::RasterVisibility::Compute},
    };
    const rhi::ComputeDispatch dispatch
    {
        .Pipeline = pipeline,
        .Bindings = set,
        .Groups = {CASE_COUNT / LUDUS_PROJECTED_SOLID_ANGLE_PARITY_GROUP_SIZE, 1, 1},
    };
    rhi::GraphPassDescription pass;
    pass.Name = "projected solid angle parity";
    pass.Uses = uses;
    pass.UseCount = 2;
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
        REQUIRE(rhi::CopyReadback(device, ticket, resultBytesStorage.GetData() + offset, size) == S::Ready);
        REQUIRE(rhi::Release(device, ticket) == S::Ready);
    }

    Array<float32> areaErrors;
    Array<float32> directionErrors;
    uint32 sampledCount = 0;
    uint32 centralCount = 0;
    Array<float32> ltcIntegralErrors;
    Array<float32> ltcDirectionErrors;
    Array<float32> ltcDensityErrors;
    uint32 ltcSampledCount = 0;
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
        math::Vector3 expectedDirection;
        const bool isSampled =
            math::TrySampleProjectedSolidAnglePolygon(expected, randomNumbers[i], expectedDirection) ==
            math::MathStatus::Success;
        REQUIRE(words[4] == (isSampled == true ? 1.0F : 0.0F));
        if (isSampled == true)
        {
            ++sampledCount;
            REQUIRE(words[9] == (expected.IsCentralCase == true ? 1.0F : 0.0F));
            centralCount += expected.IsCentralCase == true ? 1 : 0;
            const math::Vector3 direction{words[5], words[6], words[7]};
            REQUIRE(math::Abs(math::Length(direction) - 1.0F) <= 1.0e-5F);
            if (expected.ProjectedSolidAngle >= MINIMUM_STATISTICS_SOLID_ANGLE)
            {
                REQUIRE(areaErrors.TryAdd(math::Abs(projectedSolidAngle - expected.ProjectedSolidAngle) /
                                          expected.ProjectedSolidAngle) == true);
                REQUIRE(directionErrors.TryAdd(math::Length(direction - expectedDirection)) == true);
            }
            REQUIRE(math::Abs(words[8] - direction.Z / projectedSolidAngle) <= 1.0e-6F * words[8]);
        }

        // The LTC technique of Peters section 4.1 on the same polygon.
        const float32* ltcWords = words + 3 * FLOATS_PER_WORD;
        math::LtcPolygon expectedLtc;
        REQUIRE(math::TryPrepareLtcPolygon(polygons[i], inverseTransforms[i], expectedLtc) ==
                math::MathStatus::Success);
        REQUIRE(ltcWords[0] == 1.0F);
        const float32 expectedIntegral = math::GetLtcPolygonIntegral(expectedLtc);
        REQUIRE(math::Abs(ltcWords[1] - expectedIntegral) <= 1.0e-6F + 1.0e-3F * expectedIntegral);
        math::Vector3 expectedLtcDirection;
        const math::MathStatus ltcStatus =
            math::TrySampleLtcPolygon(expectedLtc, randomNumbers[i], expectedLtcDirection);
        const bool isLtcSampled = ltcStatus == math::MathStatus::Success;
        INFO("LTC status " << static_cast<int>(ltcStatus) << ", integral " << expectedIntegral << " vs " << ltcWords[1]
                           << ", clipped " << expectedLtc.Polygon.VertexCount);
        if (isLtcSampled == true && ltcWords[2] == 1.0F)
        {
            ++ltcSampledCount;
            const math::Vector3 ltcDirection{ltcWords[4], ltcWords[5], ltcWords[6]};
            REQUIRE(math::Abs(math::Length(ltcDirection) - 1.0F) <= 1.0e-5F);
            if (expectedLtc.Polygon.ProjectedSolidAngle >= MINIMUM_STATISTICS_SOLID_ANGLE)
            {
                REQUIRE(ltcIntegralErrors.TryAdd(math::Abs(ltcWords[1] - expectedIntegral) / expectedIntegral) == true);
                REQUIRE(ltcDirectionErrors.TryAdd(math::Length(ltcDirection - expectedLtcDirection)) == true);
                const float32 density = math::EvaluateLtcPolygonDensity(expectedLtc, ltcDirection);
                REQUIRE(ltcDensityErrors.TryAdd(math::Abs(ltcWords[3] - density) / density) == true);
            }
        }
        else if (expectedIntegral > NEGLIGIBLE_LTC_INTEGRAL || ltcWords[1] > NEGLIGIBLE_LTC_INTEGRAL)
        {
            // Only a transformed polygon with no measurable integral may disable the technique on one side: an
            // edge-on polygon rounds to zero area in one precision and to about 1e-11 in the other.
            REQUIRE(ltcWords[2] == (isLtcSampled == true ? 1.0F : 0.0F));
        }
    }
    REQUIRE(sampledCount > CASE_COUNT / 2);
    REQUIRE(centralCount > 0);
    REQUIRE(centralCount < sampledCount);
    REQUIRE(GetPercentile(areaErrors, 0.99F) <= 1.0e-4F);
    REQUIRE(GetPercentile(directionErrors, 0.99F) <= 1.0e-4F);
    REQUIRE(GetPercentile(directionErrors, 1.0F) <= 5.0e-2F);
    // The GPU inverts M^-1 in single precision, C++ in double precision.
    REQUIRE(ltcSampledCount > CASE_COUNT / 2);
    REQUIRE(GetPercentile(ltcIntegralErrors, 0.99F) <= 1.0e-4F);
    REQUIRE(GetPercentile(ltcDirectionErrors, 0.99F) <= 1.0e-4F);
    REQUIRE(GetPercentile(ltcDirectionErrors, 1.0F) <= 5.0e-2F);
    REQUIRE(GetPercentile(ltcDensityErrors, 0.99F) <= 1.0e-4F);
}
