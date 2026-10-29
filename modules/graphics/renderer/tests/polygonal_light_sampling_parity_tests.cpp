#include "../shaders/renderer_gpu_preview_layout.h"
#include "../src/internal/ltc_table_data.h"
#include "polygonal_light_sampling_estimators.h"
#include "polygonal_light_sampling_estimators_layout.h"
#include "polygonal_light_sampling_oracle.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

// CPU and GPU parity of the polygonal-light-sampling template's estimators on raw linear radiance. The kernel
// (polygonal_light_sampling_estimators.slang) runs the template's own polygonal_light.slang on drawn receivers,
// materials and triangles; the CPU oracle (polygonal_light_sampling_oracle.cpp) implements the same estimators with
// FoundationMath's C++ primitives, the CPU LTC table lookup and the RT1 reference BSDF. Thanks to Christoph Peters,
// "BRDF Importance Sampling for Polygonal Lights", ACM Transactions on Graphics 40(4), Article 140, 2021,
// https://doi.org/10.1145/3450626.3459672, sections 3 and 4.1-4.3, for the estimators compared here.
namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace internal = ludus::graphics::renderer::internal;
namespace testing = ludus::graphics::renderer::testing;
using rhi::RasterStatus;

constexpr uint64 TIMEOUT_NANOSECONDS = 10 * time::NANOSECONDS_PER_SECOND;
// Per-request readback limit; well below the default 256 KiB transfer profile.
constexpr usize READBACK_CHUNK_BYTES = usize{64} * 1024;
constexpr usize GROUP_COUNT = 64;
constexpr usize INVOCATION_COUNT = GROUP_COUNT * LUDUS_POLYGONAL_LIGHT_ESTIMATORS_GROUP_SIZE;
static_assert(LUDUS_POLYGONAL_LIGHT_ESTIMATORS_LTC_TABLE_BINDING == LUDUS_GRAPH_BUFFER0_BINDING + 1,
              "The LTC table directly follows the results, so Metal and Vulkan number it alike");
constexpr usize WORDS_PER_RECORD = 4;
// Integrals below this belong to slivers whose relative error is dominated by rounding of the transformed vertices, and
// whose LTC samples may land inaccurately in single precision (Peters 2021, sections 3.6 and 3.7); the threshold is
// FoundationMath's LTC parity test's (projected_solid_angle_parity_tests.cpp). Their integrals are not compared
// relatively, and the LTC estimator, which divides by such a density alone, is not compared on them.
constexpr float32 MINIMUM_STATISTICS_INTEGRAL = 1.0e-4F;
constexpr usize RESULT_BYTES =
    INVOCATION_COUNT * LUDUS_POLYGONAL_LIGHT_ESTIMATORS_RECORDS_PER_INVOCATION * WORDS_PER_RECORD * sizeof(uint32);

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

/// Run the estimator kernel once and read back every record. Returns false without a native GPU adapter.
bool RunEstimatorKernel(Array<uint8>& outBytes)
{
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    constexpr usize TABLE_BYTES = sizeof(internal::LTC_TABLE);
    constexpr usize LARGEST_BYTES = RESULT_BYTES > TABLE_BYTES ? RESULT_BYTES : TABLE_BYTES;
    description.RequiredProfile.Raster.MinBufferSize = LARGEST_BYTES;
    description.RequiredProfile.Compute.MinStorageRange = LARGEST_BYTES;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        return false;
    }
    REQUIRE(started == rhi::DeviceStatus::Ready);
    REQUIRE(outBytes.TryResize(RESULT_BYTES, uint8{0}) == true);
    rhi::BufferHandle results;
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         { .Role = rhi::BufferRole::Storage, .Size = RESULT_BYTES },
                                         {outBytes.GetData(), outBytes.GetSize()},
                                         results)) == true);
    rhi::BufferHandle table;
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         { .Role = rhi::BufferRole::Storage, .Size = TABLE_BYTES },
                                         {reinterpret_cast<const uint8*>(internal::LTC_TABLE.GetData()), TABLE_BYTES},
                                         table)) == true);
    const rhi::RasterShaderDescription shader = ludus::shaders::polygonal_light_sampling_estimators::Compute();
    rhi::RasterShaderHandle shaderHandle;
    rhi::BindingLayoutHandle layout;
    REQUIRE(IsAccepted(rhi::CreateRasterShader(device, shader, shaderHandle)) == true);
    REQUIRE(IsAccepted(rhi::CreateBindingLayout(device, shader.Bindings, layout)) == true);
    WaitReady(device, results);
    WaitReady(device, table);
    WaitReady(device, shaderHandle);
    WaitReady(device, layout);
    constexpr usize BINDING_COUNT = 2;
    const StaticArray<rhi::RasterBindingResource, BINDING_COUNT> resources{{
        { .Binding = LUDUS_GRAPH_BUFFER0_BINDING, .Buffer = results, .Size = RESULT_BYTES },
        { .Binding = LUDUS_POLYGONAL_LIGHT_ESTIMATORS_LTC_TABLE_BINDING, .Buffer = table, .Size = TABLE_BYTES },
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
    rhi::GraphVersion original;
    rhi::GraphVersion version;
    rhi::GraphVersion tableVersion;
    REQUIRE(rhi::ImportGraphBuffer(device, graph, results, original) == RasterStatus::Ready);
    REQUIRE(rhi::NextGraphVersion(device, graph, original, version) == RasterStatus::Ready);
    REQUIRE(rhi::ImportGraphBuffer(device, graph, table, tableVersion) == RasterStatus::Ready);
    const StaticArray<rhi::GraphUse, BINDING_COUNT> uses{{
        {version, rhi::GraphAccessMode::StorageReadWrite, 0, RESULT_BYTES, rhi::RasterVisibility::Compute},
        {tableVersion, rhi::GraphAccessMode::StorageRead, 0, TABLE_BYTES, rhi::RasterVisibility::Compute},
    }};
    const rhi::ComputeDispatch dispatch
    {
        .Pipeline = pipeline,
        .Bindings = set,
        .Groups = {static_cast<uint32>(GROUP_COUNT), 1, 1},
    };
    rhi::GraphPassDescription pass;
    pass.Name = "polygonal light sampling estimators";
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
    for (usize offset = 0; offset < RESULT_BYTES; offset += READBACK_CHUNK_BYTES)
    {
        const usize size = RESULT_BYTES - offset < READBACK_CHUNK_BYTES ? RESULT_BYTES - offset : READBACK_CHUNK_BYTES;
        rhi::ReadbackTicket ticket;
        RasterStatus status = rhi::RequestBufferReadback(device, results, offset, size, ticket);
        const uint64 start = time::NowTicks();
        while (status == RasterStatus::Pending && time::NowTicks() - start < TIMEOUT_NANOSECONDS)
        {
            REQUIRE(rhi::PollLifetime(device) == RasterStatus::Ready);
            status = rhi::GetStatus(device, ticket);
        }
        REQUIRE(status == RasterStatus::Ready);
        REQUIRE(rhi::CopyReadback(device, ticket, outBytes.GetData() + offset, size) == RasterStatus::Ready);
        REQUIRE(rhi::Release(device, ticket) == RasterStatus::Ready);
    }
    return true;
}

/// One uint4 record of an invocation, with float accessors for the words that store float bits.
struct EstimatorRecord final
{
    StaticArray<uint32, WORDS_PER_RECORD> Words{};

    [[nodiscard]] float32 GetFloat(usize word) const
    {
        float32 value = 0;
        std::memcpy(&value, &Words[word], sizeof(value));
        return value;
    }
    [[nodiscard]] math::Vector3 GetVector() const
    {
        return {GetFloat(0), GetFloat(1), GetFloat(2)};
    }
};

EstimatorRecord ReadRecord(const Array<uint8>& bytes, usize invocation, usize record)
{
    EstimatorRecord result;
    const usize offset = ((invocation * LUDUS_POLYGONAL_LIGHT_ESTIMATORS_RECORDS_PER_INVOCATION) + record) *
                         WORDS_PER_RECORD * sizeof(uint32);
    std::memcpy(result.Words.GetData(), bytes.GetData() + offset, sizeof(uint32) * WORDS_PER_RECORD);
    return result;
}

/// The drawn inputs of one invocation, as the CPU oracle takes them.
struct DrawnCase final
{
    testing::PolygonalLightCase Case;
    math::Vector2 ProjectedSolidAngleSample;
    math::Vector2 LtcSample;
};

DrawnCase ReadCase(const Array<uint8>& bytes, usize invocation)
{
    const EstimatorRecord normal = ReadRecord(bytes, invocation, LUDUS_POLYGONAL_LIGHT_ESTIMATORS_NORMAL_RECORD);
    const EstimatorRecord view = ReadRecord(bytes, invocation, LUDUS_POLYGONAL_LIGHT_ESTIMATORS_VIEW_RECORD);
    const EstimatorRecord material = ReadRecord(bytes, invocation, LUDUS_POLYGONAL_LIGHT_ESTIMATORS_MATERIAL_RECORD);
    const EstimatorRecord vertex0 = ReadRecord(bytes, invocation, LUDUS_POLYGONAL_LIGHT_ESTIMATORS_VERTEX0_RECORD);
    const EstimatorRecord vertex1 = ReadRecord(bytes, invocation, LUDUS_POLYGONAL_LIGHT_ESTIMATORS_VERTEX1_RECORD);
    const EstimatorRecord vertex2 = ReadRecord(bytes, invocation, LUDUS_POLYGONAL_LIGHT_ESTIMATORS_VERTEX2_RECORD);
    const EstimatorRecord emission = ReadRecord(bytes, invocation, LUDUS_POLYGONAL_LIGHT_ESTIMATORS_EMISSION_RECORD);
    const EstimatorRecord integral = ReadRecord(bytes, invocation, LUDUS_POLYGONAL_LIGHT_ESTIMATORS_INTEGRAL_RECORD);
    DrawnCase result;
    result.Case.NormalWorld = normal.GetVector();
    result.Case.Metallic = normal.GetFloat(3);
    result.Case.ViewDirectionWorld = view.GetVector();
    result.Case.Roughness = view.GetFloat(3);
    result.Case.BaseColor = material.GetVector();
    result.Case.Visibility = material.GetFloat(3);
    result.Case.PositionsWorld = {{vertex0.GetVector(), vertex1.GetVector(), vertex2.GetVector()}};
    result.Case.IsEmissionTwoSided = vertex0.GetFloat(3) == 1.0F;
    result.Case.Emission = emission.GetVector();
    result.ProjectedSolidAngleSample = {vertex1.GetFloat(3), vertex2.GetFloat(3)};
    result.LtcSample = {emission.GetFloat(3), integral.GetFloat(0)};
    return result;
}

float32 GetPercentile(Array<float32>& inoutValues, float32 fraction) noexcept
{
    Sort(inoutValues.GetData(), inoutValues.GetSize(), [](float32 a, float32 b) noexcept { return a < b; });
    return inoutValues[static_cast<usize>(fraction * static_cast<float32>(inoutValues.GetSize() - 1))];
}

float32 SumChannels(math::Vector3 value) noexcept
{
    return math::Abs(value.X) + math::Abs(value.Y) + math::Abs(value.Z);
}

/// The relative difference of two quantities that should agree, against the larger of their magnitudes.
float32 GetRelativeDifference(math::Vector3 value, math::Vector3 expected) noexcept
{
    const float32 scale = math::Max(SumChannels(value), SumChannels(expected));
    return scale > 0.0F ? SumChannels(value - expected) / scale : 0.0F;
}

float32 GetRelativeDifference(float32 value, float32 expected) noexcept
{
    const float32 scale = math::Max(math::Abs(value), math::Abs(expected));
    return scale > 0.0F ? math::Abs(value - expected) / scale : 0.0F;
}
} // namespace

TEST_CASE("Polygonal-light-sampling estimators agree with the CPU oracle on raw linear radiance",
          "[renderer][gpu][polygonal-light-sampling]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    Array<uint8> bytes;
    if (RunEstimatorKernel(bytes) == false)
    {
        SKIP("No native GPU adapter");
    }
    StaticArray<Array<float32>, testing::POLYGONAL_LIGHT_ESTIMATOR_COUNT> radianceErrors;
    Array<float32> integralErrors;
    Array<float32> coefficientErrors;
    StaticArray<math::Vector3, testing::POLYGONAL_LIGHT_ESTIMATOR_COUNT> gpuTotals{};
    StaticArray<math::Vector3, testing::POLYGONAL_LIGHT_ESTIMATOR_COUNT> cpuTotals{};
    StaticArray<usize, testing::POLYGONAL_LIGHT_ESTIMATOR_COUNT> nonzeroCounts{};
    usize preparedCount = 0;
    usize emptyLtcClipCount = 0;
    usize partialHorizonCount = 0;
    usize sliverCount = 0;
    for (usize invocation = 0; invocation < INVOCATION_COUNT; ++invocation)
    {
        CAPTURE(invocation);
        const DrawnCase drawn = ReadCase(bytes, invocation);
        const EstimatorRecord integral =
            ReadRecord(bytes, invocation, LUDUS_POLYGONAL_LIGHT_ESTIMATORS_INTEGRAL_RECORD);
        const EstimatorRecord diffuseCoefficient =
            ReadRecord(bytes, invocation, LUDUS_POLYGONAL_LIGHT_ESTIMATORS_DIFFUSE_COEFFICIENT_RECORD);
        const EstimatorRecord specularCoefficient =
            ReadRecord(bytes, invocation, LUDUS_POLYGONAL_LIGHT_ESTIMATORS_SPECULAR_COEFFICIENT_RECORD);
        testing::PolygonalLightOracle oracle;
        const bool isPrepared = testing::TryPreparePolygonalLightOracle(drawn.Case, oracle);
        // Drawn triangles are finite and nondegenerate, so both preparations succeed.
        REQUIRE(isPrepared == true);
        REQUIRE(integral.Words[3] == 1);
        ++preparedCount;
        REQUIRE((diffuseCoefficient.GetFloat(3) == 1.0F) == oracle.HasSpecular);
        emptyLtcClipCount += oracle.HasSpecular == true && oracle.SpecularIntegral == 0.0F ? 1 : 0;
        partialHorizonCount += oracle.DiffuseIntegral > 0.0F && oracle.Diffuse.VertexCount != 3 ? 1 : 0;
        // Relative statistics skip slivers, as FoundationMath's LTC parity test does.
        const bool isDiffuseCompared = oracle.DiffuseIntegral >= MINIMUM_STATISTICS_INTEGRAL;
        const bool isSpecularSliver =
            oracle.SpecularIntegral > 0.0F && oracle.SpecularIntegral < MINIMUM_STATISTICS_INTEGRAL;
        sliverCount += isSpecularSliver == true ? 1 : 0;
        if (isDiffuseCompared == true)
        {
            REQUIRE(integralErrors.TryAdd(GetRelativeDifference(integral.GetFloat(1), oracle.DiffuseIntegral)) == true);
            REQUIRE(coefficientErrors.TryAdd(
                        GetRelativeDifference(diffuseCoefficient.GetVector(), oracle.DiffuseCoefficient)) == true);
        }
        if (oracle.SpecularIntegral >= MINIMUM_STATISTICS_INTEGRAL)
        {
            REQUIRE(integralErrors.TryAdd(GetRelativeDifference(integral.GetFloat(2), oracle.SpecularIntegral)) ==
                    true);
            REQUIRE(coefficientErrors.TryAdd(
                        GetRelativeDifference(specularCoefficient.GetVector(), oracle.SpecularCoefficient)) == true);
        }
        StaticArray<math::Vector3, testing::POLYGONAL_LIGHT_ESTIMATOR_COUNT> expected;
        REQUIRE(
            testing::TryEstimatePolygonalLight(oracle, drawn.ProjectedSolidAngleSample, drawn.LtcSample, expected) ==
            true);
        for (usize estimator = 0; estimator < testing::POLYGONAL_LIGHT_ESTIMATOR_COUNT; ++estimator)
        {
            const math::Vector3 radiance =
                ReadRecord(bytes, invocation, LUDUS_POLYGONAL_LIGHT_ESTIMATORS_FIRST_RADIANCE_RECORD + estimator)
                    .GetVector();
            REQUIRE(math::IsFinite(radiance) == true);
            REQUIRE(radiance.X >= 0.0F);
            REQUIRE(radiance.Y >= 0.0F);
            REQUIRE(radiance.Z >= 0.0F);
            // A sliver's LTC sample is not reproducible across precisions; the projected solid angle estimator does not
            // use it.
            if (isSpecularSliver == true &&
                estimator != static_cast<usize>(testing::PolygonalLightEstimator::ProjectedSolidAngle))
            {
                continue;
            }
            gpuTotals[estimator] = gpuTotals[estimator] + radiance;
            cpuTotals[estimator] = cpuTotals[estimator] + expected[estimator];
            if (SumChannels(radiance) > 0.0F || SumChannels(expected[estimator]) > 0.0F)
            {
                ++nonzeroCounts[estimator];
                REQUIRE(radianceErrors[estimator].TryAdd(GetRelativeDifference(radiance, expected[estimator])) == true);
            }
        }
    }
    CAPTURE(preparedCount, emptyLtcClipCount, partialHorizonCount, sliverCount);
    // The corpus covers empty LTC clips and triangles that straddle the horizon.
    CHECK(emptyLtcClipCount > 0);
    CHECK(partialHorizonCount > 0);
    const float32 integralP99 = GetPercentile(integralErrors, 0.99F);
    const float32 coefficientP99 = GetPercentile(coefficientErrors, 0.99F);
    CAPTURE(integralP99, coefficientP99);
    // Apple M5 measured p99 relative differences of 5e-5 for both; FoundationMath's LTC parity allows 5e-4.
    CHECK(integralP99 <= 5.0e-4F);
    CHECK(coefficientP99 <= 5.0e-4F);
    // Slivers are common with the corpus's minimum-roughness receivers, but most cases are compared.
    CHECK(sliverCount < INVOCATION_COUNT / 2);
    for (usize estimator = 0; estimator < testing::POLYGONAL_LIGHT_ESTIMATOR_COUNT; ++estimator)
    {
        const float32 radianceP90 = GetPercentile(radianceErrors[estimator], 0.9F);
        const float32 radianceP99 = GetPercentile(radianceErrors[estimator], 0.99F);
        const float32 radianceMaximum = GetPercentile(radianceErrors[estimator], 1.0F);
        const float32 totalError = GetRelativeDifference(gpuTotals[estimator], cpuTotals[estimator]);
        INFO("estimator " << estimator << ": nonzero=" << nonzeroCounts[estimator] << ", p90=" << radianceP90
                          << ", p99=" << radianceP99 << ", maximum=" << radianceMaximum << ", total=" << totalError);
        CHECK(nonzeroCounts[estimator] > INVOCATION_COUNT / 4);
        // Apple M5 measured p90 relative differences of at most 2.8e-5 and p99 of at most 2.8e-3 per invocation, and
        // at most 6.1e-4 on the summed radiance. The largest single differences (up to 0.93) are samples whose
        // directions differ by an ulp at a triangle edge or a minimum-roughness highlight, where the integrand jumps.
        CHECK(radianceP90 <= 1.0e-4F);
        CHECK(radianceP99 <= 5.0e-3F);
        CHECK(totalError <= 2.0e-3F);
    }
}
