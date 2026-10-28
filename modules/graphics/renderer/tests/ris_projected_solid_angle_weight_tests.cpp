#include "ris_projected_solid_angle_weights.h"
#include "ris_projected_solid_angle_weights_layout.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
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
// rule checked here.
namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
using rhi::RasterStatus;

constexpr uint64 TIMEOUT_NANOSECONDS = 10 * time::NANOSECONDS_PER_SECOND;
// Per-request readback limit; well below the default 256 KiB transfer profile.
constexpr usize READBACK_CHUNK_BYTES = usize{64} * 1024;
constexpr usize GROUP_COUNT = 64;
constexpr usize INVOCATION_COUNT = GROUP_COUNT * LUDUS_RIS_WEIGHTS_GROUP_SIZE;
constexpr usize WORDS_PER_RECORD = 4;
constexpr usize RESULT_BYTES =
    INVOCATION_COUNT * LUDUS_RIS_WEIGHTS_RECORDS_PER_INVOCATION * WORDS_PER_RECORD * sizeof(uint32);
// Weights partition unity up to a few float32 roundings of the two divisions and the blend.
constexpr float32 PARTITION_TOLERANCE = 1e-6F;
// A relative tolerance for float32 quotients that the host recomputes.
constexpr float32 RELATIVE_TOLERANCE = 1e-6F;

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

/// Run the weights kernel once and read back every record. Returns false without a native GPU adapter.
bool RunWeightKernel(Array<uint8>& outBytes)
{
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    description.RequiredProfile.Compute.MinStorageRange = RESULT_BYTES;
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
    const rhi::RasterShaderDescription shader = ludus::shaders::ris_projected_solid_angle_weights::Compute();
    rhi::RasterShaderHandle shaderHandle;
    rhi::BindingLayoutHandle layout;
    REQUIRE(IsAccepted(rhi::CreateRasterShader(device, shader, shaderHandle)) == true);
    REQUIRE(IsAccepted(rhi::CreateBindingLayout(device, shader.Bindings, layout)) == true);
    WaitReady(device, results);
    WaitReady(device, shaderHandle);
    WaitReady(device, layout);
    const rhi::RasterBindingResource resource{ .Binding = 0, .Buffer = results, .Size = RESULT_BYTES };
    rhi::BindingSetHandle set;
    rhi::ComputePipelineHandle pipeline;
    REQUIRE(IsAccepted(rhi::CreateBindingSet(device, layout, {&resource, 1}, set)) == true);
    REQUIRE(IsAccepted(rhi::CreateComputePipeline(device, { .Shader = shaderHandle, .Layout = layout }, pipeline)) ==
            true);
    WaitReady(device, set);
    WaitReady(device, pipeline);
    rhi::OrderedGraph graph;
    REQUIRE(rhi::CreateOrderedGraph(device, graph) == RasterStatus::Ready);
    rhi::GraphVersion original;
    rhi::GraphVersion version;
    REQUIRE(rhi::ImportGraphBuffer(device, graph, results, original) == RasterStatus::Ready);
    REQUIRE(rhi::NextGraphVersion(device, graph, original, version) == RasterStatus::Ready);
    const rhi::GraphUse use{version,
                            rhi::GraphAccessMode::StorageReadWrite,
                            0,
                            RESULT_BYTES,
                            rhi::RasterVisibility::Compute};
    const rhi::ComputeDispatch dispatch
    {
        .Pipeline = pipeline,
        .Bindings = set,
        .Groups = {static_cast<uint32>(GROUP_COUNT), 1, 1},
    };
    rhi::GraphPassDescription pass;
    pass.Name = "ris projected solid angle weights";
    pass.Uses = &use;
    pass.UseCount = 1;
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

WeightRecord ReadRecord(const Array<uint8>& bytes, usize invocation, usize record)
{
    WeightRecord result;
    const usize offset =
        ((invocation * LUDUS_RIS_WEIGHTS_RECORDS_PER_INVOCATION) + record) * WORDS_PER_RECORD * sizeof(uint32);
    std::memcpy(result.Words.GetData(), bytes.GetData() + offset, sizeof(uint32) * WORDS_PER_RECORD);
    return result;
}

bool IsNear(float32 value, float32 expected) noexcept
{
    return math::Abs(value - expected) <= RELATIVE_TOLERANCE * math::Max(math::Abs(expected), 1.0F);
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
    if (RunWeightKernel(bytes) == false)
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
