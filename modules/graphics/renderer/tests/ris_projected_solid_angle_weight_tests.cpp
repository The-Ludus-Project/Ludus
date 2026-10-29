#include "../src/internal/ltc_table_data.h"
#include "../src/internal/reference_light_sampling.h"
#include "ris_projected_solid_angle_clipped_receivers.h"
#include "ris_projected_solid_angle_weights.h"
#include "ris_projected_solid_angle_weights_layout.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/shading_normal.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

// Native execution of the ris-projected-solid-angle template's MIS weights and surrogate light density
// (render_graphs/ris_projected_solid_angle/main_shaders/projected_solid_angle_light.slang). The estimator is
// docs/architecture/many-area-light-sampling-reference-review.md; its weights stay unbiased only if they sum to one
// wherever a strategy can sample and are zero where it cannot. Thanks to Christoph Peters, "BRDF Importance Sampling
// for Polygonal Lights", ACM Transactions on Graphics 40(4), Article 140, 2021,
// https://doi.org/10.1145/3450626.3459672, sections 4.1-4.3, for the clamped optimal MIS weights and the empty-clip
// rule checked here, and section 6.1 for the shading normal clipped to the view's hemisphere. RT1's
// src/reference_light_sampling.cpp implements the same frame and techniques step by step in C++, so it is the CPU twin
// of the clipped receivers.
namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace internal = ludus::graphics::renderer::internal;
using rhi::RasterStatus;

constexpr uint64 TIMEOUT_NANOSECONDS = 10 * time::NANOSECONDS_PER_SECOND;
// Per-request readback limit; well below the default 256 KiB transfer profile.
constexpr usize READBACK_CHUNK_BYTES = usize{64} * 1024;
constexpr usize GROUP_COUNT = 64;
constexpr usize INVOCATION_COUNT = GROUP_COUNT * LUDUS_RIS_WEIGHTS_GROUP_SIZE;
constexpr usize WORDS_PER_RECORD = 4;
constexpr usize WEIGHT_RESULT_BYTES =
    INVOCATION_COUNT * LUDUS_RIS_WEIGHTS_RECORDS_PER_INVOCATION * WORDS_PER_RECORD * sizeof(uint32);
constexpr usize CLIPPED_RESULT_BYTES =
    INVOCATION_COUNT * LUDUS_RIS_CLIPPED_RECORDS_PER_INVOCATION * WORDS_PER_RECORD * sizeof(uint32);
constexpr usize LARGEST_RESULT_BYTES =
    WEIGHT_RESULT_BYTES > CLIPPED_RESULT_BYTES ? WEIGHT_RESULT_BYTES : CLIPPED_RESULT_BYTES;
// Weights partition unity up to a few float32 roundings of the two divisions and the blend.
constexpr float32 PARTITION_TOLERANCE = 1e-6F;
// A relative tolerance for float32 quotients that the host recomputes.
constexpr float32 RELATIVE_TOLERANCE = 1e-6F;
// The clipped normal and the albedo luminances: a few roundings of the same arithmetic in two languages.
constexpr float32 ARITHMETIC_TOLERANCE = 1e-5F;
// Integrals and densities, whose clipping and series differ by FP32 rounding (reference_light_sampling_parity_tests).
constexpr float32 GEOMETRY_TOLERANCE = 1e-3F;
// Technique integrals below this are slivers whose FP32 clipping and integration differ between implementations.
constexpr float32 SLIVER_INTEGRAL = 1e-5F;

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

/// Run one of the weights kernels once and read back its resultBytes of records. Returns false without a
/// native GPU adapter.
bool RunWeightKernel(const rhi::RasterShaderDescription& shader, usize resultBytes, Array<uint8>& outBytes)
{
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    constexpr usize TABLE_BYTES = sizeof(internal::LTC_TABLE);
    constexpr usize LARGEST_BYTES = LARGEST_RESULT_BYTES > TABLE_BYTES ? LARGEST_RESULT_BYTES : TABLE_BYTES;
    description.RequiredProfile.Raster.MinBufferSize = LARGEST_BYTES;
    description.RequiredProfile.Compute.MinStorageRange = LARGEST_BYTES;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        return false;
    }
    REQUIRE(started == rhi::DeviceStatus::Ready);
    REQUIRE(outBytes.TryResize(resultBytes, uint8{0}) == true);
    rhi::BufferHandle results;
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         { .Role = rhi::BufferRole::Storage, .Size = resultBytes },
                                         {outBytes.GetData(), outBytes.GetSize()},
                                         results)) == true);
    // The kernel's receiver frames look up the renderer's LTC table, which the renderer binds as a storage buffer.
    rhi::BufferHandle table;
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         { .Role = rhi::BufferRole::Storage, .Size = TABLE_BYTES },
                                         {reinterpret_cast<const uint8*>(internal::LTC_TABLE.GetData()), TABLE_BYTES},
                                         table)) == true);
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
        { .Binding = 0, .Buffer = results, .Size = resultBytes },
        { .Binding = LUDUS_RIS_WEIGHTS_LTC_TABLE_BINDING, .Buffer = table, .Size = TABLE_BYTES },
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
        {version, rhi::GraphAccessMode::StorageReadWrite, 0, resultBytes, rhi::RasterVisibility::Compute},
        {tableVersion, rhi::GraphAccessMode::StorageRead, 0, TABLE_BYTES, rhi::RasterVisibility::Compute},
    }};
    const rhi::ComputeDispatch dispatch
    {
        .Pipeline = pipeline,
        .Bindings = set,
        .Groups = {static_cast<uint32>(GROUP_COUNT), 1, 1},
    };
    rhi::GraphPassDescription pass;
    pass.Name = "ris projected solid angle weights";
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
    for (usize offset = 0; offset < resultBytes; offset += READBACK_CHUNK_BYTES)
    {
        const usize size = resultBytes - offset < READBACK_CHUNK_BYTES ? resultBytes - offset : READBACK_CHUNK_BYTES;
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
struct WeightRecord final
{
    StaticArray<uint32, WORDS_PER_RECORD> Words{};

    [[nodiscard]] float32 GetFloat(usize word) const
    {
        float32 value = 0;
        std::memcpy(&value, &Words[word], sizeof(value));
        return value;
    }
};

/// Record `record` of `invocation` in a buffer of recordsPerInvocation records per invocation.
WeightRecord ReadRecordOf(const Array<uint8>& bytes, usize invocation, usize record, usize recordsPerInvocation)
{
    WeightRecord result;
    const usize offset = ((invocation * recordsPerInvocation) + record) * WORDS_PER_RECORD * sizeof(uint32);
    std::memcpy(result.Words.GetData(), bytes.GetData() + offset, sizeof(uint32) * WORDS_PER_RECORD);
    return result;
}

WeightRecord ReadRecord(const Array<uint8>& bytes, usize invocation, usize record)
{
    return ReadRecordOf(bytes, invocation, record, LUDUS_RIS_WEIGHTS_RECORDS_PER_INVOCATION);
}

/// A record of the clipped-receivers kernel's results.
WeightRecord ReadClippedRecord(const Array<uint8>& bytes, usize invocation, usize record)
{
    return ReadRecordOf(bytes, invocation, record, LUDUS_RIS_CLIPPED_RECORDS_PER_INVOCATION);
}

bool IsNear(float32 value, float32 expected) noexcept
{
    return math::Abs(value - expected) <= RELATIVE_TOLERANCE * math::Max(math::Abs(expected), 1.0F);
}

bool IsNear(float32 value, float32 expected, float32 tolerance) noexcept
{
    return math::Abs(value - expected) <= tolerance * math::Max(math::Abs(expected), 1.0F);
}

math::Vector3 GetVector(const WeightRecord& record) noexcept
{
    return {record.GetFloat(0), record.GetFloat(1), record.GetFloat(2)};
}

/// How often the clipped receivers reached each branch, and the relative differences of their sampled LTC densities.
struct ClippedReceiverStatistics final
{
    usize Lit = 0;
    usize Sampled = 0;
    usize Sliver = 0;
    Array<float32> SpecularDensityErrors;
};

/// The relative difference of two quantities that should agree, against the larger of their magnitudes.
float32 GetRelativeDifference(float32 value, float32 expected) noexcept
{
    const float32 scale = math::Max(math::Abs(value), math::Abs(expected));
    return scale > 0.0F ? math::Abs(value - expected) / scale : 0.0F;
}

float32 GetPercentile(Array<float32>& inoutValues, float32 fraction) noexcept
{
    Sort(inoutValues.GetData(), inoutValues.GetSize(), [](float32 a, float32 b) noexcept { return a < b; });
    return inoutValues[static_cast<usize>(fraction * static_cast<float32>(inoutValues.GetSize() - 1))];
}

/// Section 6.1's receiver of one invocation: its clipped normal matches FoundationMath's C++ clip, its frame and
/// polygons match RT1's CPU twin, and its sampled direction's weights partition unity.
void CheckClippedReceiver(const Array<uint8>& bytes, usize invocation, ClippedReceiverStatistics& inoutStatistics)
{
    const WeightRecord viewRecord = ReadClippedRecord(bytes, invocation, LUDUS_RIS_CLIPPED_VIEW_RECORD);
    const WeightRecord material = ReadClippedRecord(bytes, invocation, LUDUS_RIS_CLIPPED_MATERIAL_RECORD);
    const WeightRecord vertex0 = ReadClippedRecord(bytes, invocation, LUDUS_RIS_CLIPPED_TRIANGLE_RECORD);
    const WeightRecord vertex1 = ReadClippedRecord(bytes, invocation, LUDUS_RIS_CLIPPED_TRIANGLE_RECORD + 1);
    const WeightRecord vertex2 = ReadClippedRecord(bytes, invocation, LUDUS_RIS_CLIPPED_TRIANGLE_RECORD + 2);
    const WeightRecord normalRecord = ReadClippedRecord(bytes, invocation, LUDUS_RIS_CLIPPED_NORMAL_RECORD);
    const WeightRecord polygon = ReadClippedRecord(bytes, invocation, LUDUS_RIS_CLIPPED_POLYGON_RECORD);
    const WeightRecord direction = ReadClippedRecord(bytes, invocation, LUDUS_RIS_CLIPPED_DIRECTION_RECORD);
    const math::Vector3 view = GetVector(viewRecord);
    const math::Vector3 normal = GetVector(normalRecord);
    const uint32 flags = vertex0.Words[3];
    CAPTURE(view.X, view.Y, view.Z, viewRecord.GetFloat(3), flags);
    REQUIRE((flags & LUDUS_RIS_CLIPPED_PREPARED) != 0);
    // Every view lies below the margin, so the normal is clipped into the view's hemisphere.
    REQUIRE(view.Z < math::SHADING_NORMAL_MINIMUM_OUTGOING_COSINE);
    math::Vector3 hostNormal;
    REQUIRE(math::TryClipShadingNormalToOutgoingHemisphere({0, 0, 1}, view, hostNormal) == math::MathStatus::Success);
    CHECK(math::Abs(normal.X - hostNormal.X) <= ARITHMETIC_TOLERANCE);
    CHECK(math::Abs(normal.Y - hostNormal.Y) <= ARITHMETIC_TOLERANCE);
    CHECK(math::Abs(normal.Z - hostNormal.Z) <= ARITHMETIC_TOLERANCE);
    const float32 minimumCosine =
        math::SHADING_NORMAL_MINIMUM_OUTGOING_COSINE /
        math::Sqrt(1.0F + math::SHADING_NORMAL_MINIMUM_OUTGOING_COSINE * math::SHADING_NORMAL_MINIMUM_OUTGOING_COSINE);
    CHECK(math::Dot(normal, view) >= minimumCosine - ARITHMETIC_TOLERANCE);

    // RT1's CPU frame and polygons from the same clipped normal.
    const ludus::graphics::renderer::ReferenceBsdf bsdf{GetVector(material),
                                                        material.GetFloat(3),
                                                        viewRecord.GetFloat(3)};
    const internal::ReferenceReceiverFrame frame =
        internal::MakeReferenceReceiverFrame({0, 0, 0}, hostNormal, view, bsdf);
    internal::ReferenceTriangleLight light;
    REQUIRE(internal::TryPrepareReferenceTriangleLight(frame,
                                                       GetVector(vertex0),
                                                       GetVector(vertex1),
                                                       GetVector(vertex2),
                                                       light) == true);
    CHECK(((flags & LUDUS_RIS_CLIPPED_HAS_SPECULAR) != 0) == frame.HasSpecular);
    CHECK(IsNear(vertex1.GetFloat(3), frame.DiffuseAlbedoLuminance, ARITHMETIC_TOLERANCE));
    CHECK(IsNear(vertex2.GetFloat(3), frame.SpecularAlbedoLuminance, GEOMETRY_TOLERANCE));
    // Either side may see a sliver where the other's clip is empty: at the minimum roughness and a grazing view, the
    // GPU can find LTC integrals of 1e-13 to 1e-10 where the CPU clip is empty
    // (polygonal_light_sampling_parity_tests.cpp).
    CAPTURE(polygon.GetFloat(0), light.DiffuseIntegral, polygon.GetFloat(1), light.SpecularIntegral);
    const float32 largerDiffuseIntegral = math::Max(polygon.GetFloat(0), light.DiffuseIntegral);
    const float32 largerSpecularIntegral = math::Max(polygon.GetFloat(1), light.SpecularIntegral);
    const bool isDiffuseResolved = largerDiffuseIntegral >= SLIVER_INTEGRAL;
    const bool isSpecularResolved = largerSpecularIntegral >= SLIVER_INTEGRAL;
    const bool isSliver = (largerDiffuseIntegral > 0 && isDiffuseResolved == false) ||
                          (largerSpecularIntegral > 0 && isSpecularResolved == false);
    inoutStatistics.Sliver += isSliver == true ? 1 : 0;
    if (isDiffuseResolved == true)
    {
        CHECK(IsNear(polygon.GetFloat(0), light.DiffuseIntegral, GEOMETRY_TOLERANCE));
    }
    if (isSpecularResolved == true)
    {
        CHECK(IsNear(polygon.GetFloat(1), light.SpecularIntegral, GEOMETRY_TOLERANCE));
    }
    inoutStatistics.Lit += light.DiffuseIntegral > 0 ? 1 : 0;

    // The sampled direction: CPU densities agree, and the GPU weights partition unity where a technique samples.
    const float32 weightSum = normalRecord.GetFloat(3);
    const float32 diffuseDensity = polygon.GetFloat(2);
    const float32 specularDensity = polygon.GetFloat(3);
    if (diffuseDensity > 0 || specularDensity > 0)
    {
        CHECK(math::Abs(weightSum - 1) <= PARTITION_TOLERANCE);
    }
    else
    {
        CHECK(weightSum == 0);
    }
    if ((flags & LUDUS_RIS_CLIPPED_SAMPLED) == 0)
    {
        return;
    }
    ++inoutStatistics.Sampled;
    const math::Vector2 hostDensities =
        internal::EvaluateReferenceLightTechniqueDensities(frame, light, GetVector(direction));
    CAPTURE(diffuseDensity, hostDensities.X, specularDensity, hostDensities.Y);
    if (isDiffuseResolved == true)
    {
        CHECK(IsNear(diffuseDensity, hostDensities.X, GEOMETRY_TOLERANCE));
    }
    // The LTC density of a minimum-roughness lobe reaches 1e6 per steradian and changes by its own magnitude within a
    // degree, so one direction's density is compared statistically below.
    if (isSpecularResolved == true)
    {
        REQUIRE(inoutStatistics.SpecularDensityErrors.TryAdd(GetRelativeDifference(specularDensity, hostDensities.Y)) ==
                true);
    }
}
} // namespace

TEST_CASE("RIS projected-solid-angle MIS weights partition unity and handle Z-hat = 0 and empty LTC clips",
          "[renderer][gpu][ris-projected-solid-angle]")
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
    if (RunWeightKernel(ludus::shaders::ris_projected_solid_angle_weights::Compute(), WEIGHT_RESULT_BYTES, bytes) ==
        false)
    {
        SKIP("No native GPU adapter");
    }
    usize zeroNormalizationCount = 0;
    usize receiverSampleCount = 0;
    usize emptyClipSampleCount = 0;
    for (usize invocation = 0; invocation < INVOCATION_COUNT; ++invocation)
    {
        INFO("invocation " << invocation);
        // Clamped optimal MIS: zero weight where a technique cannot sample, and a partition of unity elsewhere.
        const WeightRecord clamped = ReadRecord(bytes, invocation, LUDUS_RIS_WEIGHTS_CLAMPED_RECORD);
        const float32 diffuseWeight = clamped.GetFloat(0);
        const float32 specularWeight = clamped.GetFloat(1);
        const float32 diffuseDensity = clamped.GetFloat(2);
        const float32 specularDensity = clamped.GetFloat(3);
        CHECK(diffuseWeight >= 0);
        CHECK(specularWeight >= 0);
        CHECK((diffuseDensity > 0 || diffuseWeight == 0));
        CHECK((specularDensity > 0 || specularWeight == 0));
        const float32 clampedSum = diffuseWeight + specularWeight;
        if (diffuseDensity > 0 || specularDensity > 0)
        {
            CHECK(math::Abs(clampedSum - 1) <= PARTITION_TOLERANCE);
        }
        else
        {
            CHECK(clampedSum == 0);
        }

        // The power heuristic splits each pair of positive densities between the light group and BSDF sampling.
        const WeightRecord power = ReadRecord(bytes, invocation, LUDUS_RIS_WEIGHTS_POWER_RECORD);
        const float32 lightWeight = power.GetFloat(0);
        const float32 bsdfWeight = power.GetFloat(1);
        const float32 lightDensity = power.GetFloat(2);
        const float32 bsdfDensity = power.GetFloat(3);
        if (lightDensity > 0 || bsdfDensity > 0)
        {
            CHECK(math::Abs(lightWeight + bsdfWeight - 1) <= PARTITION_TOLERANCE);
        }
        CHECK((lightDensity > 0 || lightWeight == 0));
        CHECK((bsdfDensity > 0 || bsdfWeight == 0));

        // P-tilde = P_E min(p-hat / Z-hat, min(1, M p_src)); p-hat / Z-hat is +infinity when Z-hat is zero.
        const WeightRecord surrogateInput = ReadRecord(bytes, invocation, LUDUS_RIS_WEIGHTS_SURROGATE_INPUT_RECORD);
        const WeightRecord surrogate = ReadRecord(bytes, invocation, LUDUS_RIS_WEIGHTS_SURROGATE_RECORD);
        const float32 categoryProbability = surrogateInput.GetFloat(0);
        const float32 target = surrogateInput.GetFloat(1);
        const float32 sourcePmf = surrogateInput.GetFloat(2);
        const float32 normalization = surrogateInput.GetFloat(3);
        const float32 inclusionBound = math::Min(1.0F, static_cast<float32>(surrogate.Words[1]) * sourcePmf);
        float32 expected = 0;
        if (categoryProbability > 0 && target > 0 && sourcePmf > 0)
        {
            const bool isBounded = normalization == 0 || target >= inclusionBound * normalization;
            expected = categoryProbability * (isBounded == true ? inclusionBound : target / normalization);
        }
        zeroNormalizationCount += normalization == 0 && target > 0 ? 1 : 0;
        CHECK(IsNear(surrogate.GetFloat(0), expected) == true);

        // An LTC whose clip is empty disables the specular technique; the diffuse technique takes all weight.
        const WeightRecord emptyClip = ReadRecord(bytes, invocation, LUDUS_RIS_WEIGHTS_EMPTY_CLIP_RECORD);
        const WeightRecord emptyClipWeights = ReadRecord(bytes, invocation, LUDUS_RIS_WEIGHTS_EMPTY_CLIP_WEIGHT_RECORD);
        CHECK(emptyClip.Words[0] == 1);
        CHECK(emptyClip.GetFloat(1) > 0);
        CHECK(emptyClip.GetFloat(2) == 0);
        CHECK(emptyClip.GetFloat(3) == 0);
        if (emptyClipWeights.Words[2] == 1)
        {
            ++emptyClipSampleCount;
            CHECK(emptyClipWeights.GetFloat(3) > 0);
            // v * 1 + (1 - v) * 1 may round to one ulp below one.
            CHECK(math::Abs(emptyClipWeights.GetFloat(0) - 1) <= PARTITION_TOLERANCE);
            CHECK(emptyClipWeights.GetFloat(1) == 0);
        }

        // Real receivers, LTCs and triangles: a sampled direction's weights partition unity.
        const WeightRecord receiver = ReadRecord(bytes, invocation, LUDUS_RIS_WEIGHTS_RECEIVER_RECORD);
        const float32 receiverSum = receiver.GetFloat(0);
        if (receiver.GetFloat(1) > 0 || receiver.GetFloat(2) > 0)
        {
            CHECK(math::Abs(receiverSum - 1) <= PARTITION_TOLERANCE);
        }
        else
        {
            CHECK(receiverSum == 0);
        }
        receiverSampleCount += receiver.Words[3] == 1 ? 1 : 0;
    }
    // Every branch was exercised: Z-hat = 0 with a positive target, empty clips and sampled receivers.
    CHECK(zeroNormalizationCount > INVOCATION_COUNT / 4);
    CHECK(emptyClipSampleCount == INVOCATION_COUNT);
    CHECK(receiverSampleCount > INVOCATION_COUNT / 4);
}

TEST_CASE("RIS projected-solid-angle receivers viewed from below their shading horizon match RT1's CPU twin",
          "[renderer][gpu][ris-projected-solid-angle]")
{
    // Peters 2021, section 6.1: TryLoadShadingPoint clips the shading normal towards the view. Without the clip every
    // one of these receivers would be black; with it, the clipped frame's techniques must match RT1's C++ functions and
    // their weights must partition unity.
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    Array<uint8> bytes;
    if (RunWeightKernel(ludus::shaders::ris_projected_solid_angle_clipped_receivers::Compute(),
                        CLIPPED_RESULT_BYTES,
                        bytes) == false)
    {
        SKIP("No native GPU adapter");
    }
    ClippedReceiverStatistics statistics;
    for (usize invocation = 0; invocation < INVOCATION_COUNT; ++invocation)
    {
        INFO("invocation " << invocation);
        CheckClippedReceiver(bytes, invocation, statistics);
    }
    REQUIRE(statistics.SpecularDensityErrors.IsEmpty() == false);
    const float32 specularDensityP99 = GetPercentile(statistics.SpecularDensityErrors, 0.99F);
    const float32 specularDensityMaximum = GetPercentile(statistics.SpecularDensityErrors, 1.0F);
    INFO("lit=" << statistics.Lit << ", sampled=" << statistics.Sampled << ", slivers=" << statistics.Sliver
                << ", LTC densities compared=" << statistics.SpecularDensityErrors.GetSize()
                << ", p99=" << specularDensityP99 << ", maximum=" << specularDensityMaximum);
    CHECK(statistics.Lit > INVOCATION_COUNT / 8);
    CHECK(statistics.Sampled > INVOCATION_COUNT / 8);
    // Apple M5 measured 3121 lit and 3055 sampled receivers of 4096, and a p99 relative LTC density difference of
    // 5.4e-4 (largest 2.3e-3) over 1549 directions; 1595 receivers have a sliver integral that is not compared.
    CHECK(specularDensityP99 <= GEOMETRY_TOLERANCE);
}
