#include "slice_sampling_luminaire_estimators.h"
#include "slice_sampling_luminaire_estimators_layout.h"

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

// Unbiasedness and variance of the slice-sampling-luminaire template's proposals on raw linear irradiance. Thanks to
// Shlomi Steinberg, "Slice Sampling: Importance sampling with an auxiliary map" (draft, University of Waterloo):
// Proposition 1 makes uniform-seed slice sampling with the branch probabilities omega / W an unbiased estimator with
// the subdensity of Eq. (13), and Eq. (14) makes W an unbiased estimate of the normalizer. The kernel
// (slice_sampling_luminaire_estimators.slang) runs the template's own slice_sampling.slang; uniform panel-area
// sampling there is evaluated in the area measure, with no slope Jacobian, so agreement checks the seed map's
// Jacobian, the branch weights and the slope-to-solid-angle conversion together. Section 6.4 reports the variance
// advantage over area sampling that the last checks require.
namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
using rhi::RasterStatus;

constexpr uint64 TIMEOUT_NANOSECONDS = 30 * time::NANOSECONDS_PER_SECOND;
// Per-request readback limit; well below the default 256 KiB transfer profile.
constexpr usize READBACK_CHUNK_BYTES = usize{64} * 1024;
constexpr usize CASE_COUNT = LUDUS_SLICE_SAMPLING_ESTIMATORS_CASE_COUNT;
constexpr usize RECEIVER_COUNT = LUDUS_SLICE_SAMPLING_ESTIMATORS_RECEIVER_COUNT;
constexpr usize INVOCATIONS_PER_CASE = LUDUS_SLICE_SAMPLING_ESTIMATORS_INVOCATIONS_PER_CASE;
constexpr usize INVOCATION_COUNT = CASE_COUNT * INVOCATIONS_PER_CASE;
constexpr usize GROUP_COUNT = INVOCATION_COUNT / LUDUS_SLICE_SAMPLING_ESTIMATORS_GROUP_SIZE;
static_assert(INVOCATION_COUNT % LUDUS_SLICE_SAMPLING_ESTIMATORS_GROUP_SIZE == 0, "Cases fill whole groups");
constexpr usize WORDS_PER_RECORD = 4;
constexpr usize RESULT_BYTES =
    INVOCATION_COUNT * LUDUS_SLICE_SAMPLING_ESTIMATORS_RECORDS_PER_INVOCATION * WORDS_PER_RECORD * sizeof(uint32);
constexpr float64 SAMPLES_PER_CASE =
    static_cast<float64>(INVOCATIONS_PER_CASE) * LUDUS_SLICE_SAMPLING_ESTIMATORS_SAMPLES_PER_INVOCATION;

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
    description.RequiredProfile.Raster.MinBufferSize = RESULT_BYTES;
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
    const rhi::RasterShaderDescription shader = ludus::shaders::slice_sampling_luminaire_estimators::Compute();
    rhi::RasterShaderHandle shaderHandle;
    rhi::BindingLayoutHandle layout;
    REQUIRE(IsAccepted(rhi::CreateRasterShader(device, shader, shaderHandle)) == true);
    REQUIRE(IsAccepted(rhi::CreateBindingLayout(device, shader.Bindings, layout)) == true);
    WaitReady(device, results);
    WaitReady(device, shaderHandle);
    WaitReady(device, layout);
    constexpr usize BINDING_COUNT = 1;
    const StaticArray<rhi::RasterBindingResource, BINDING_COUNT> resources{{
        { .Binding = LUDUS_SLICE_SAMPLING_ESTIMATORS_RESULTS_BINDING, .Buffer = results, .Size = RESULT_BYTES },
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
    REQUIRE(rhi::ImportGraphBuffer(device, graph, results, original) == RasterStatus::Ready);
    REQUIRE(rhi::NextGraphVersion(device, graph, original, version) == RasterStatus::Ready);
    const StaticArray<rhi::GraphUse, BINDING_COUNT> uses{{
        {version, rhi::GraphAccessMode::StorageReadWrite, 0, RESULT_BYTES, rhi::RasterVisibility::Compute},
    }};
    const rhi::ComputeDispatch dispatch
    {
        .Pipeline = pipeline,
        .Bindings = set,
        .Groups = {static_cast<uint32>(GROUP_COUNT), 1, 1},
    };
    rhi::GraphPassDescription pass;
    pass.Name = "slice sampling luminaire estimators";
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

/// Float stored as bits in word of record of an invocation.
float32 ReadFloat(const Array<uint8>& bytes, usize invocation, usize record, usize word)
{
    const usize offset =
        ((invocation * LUDUS_SLICE_SAMPLING_ESTIMATORS_RECORDS_PER_INVOCATION + record) * WORDS_PER_RECORD + word) *
        sizeof(uint32);
    float32 value = 0;
    std::memcpy(&value, bytes.GetData() + offset, sizeof(value));
    return value;
}

/// Sample mean and its standard error from a case's accumulated sum and sum of squares.
struct MeanEstimate final
{
    float64 Mean = 0;
    float64 Variance = 0;
    float64 StandardError = 0;

    /// Variance divided by the squared mean; 0 for a zero mean.
    [[nodiscard]] float64 GetRelativeVariance() const
    {
        return Mean > 0 ? Variance / (Mean * Mean) : 0;
    }
};

MeanEstimate MakeMeanEstimate(float64 sum, float64 squares)
{
    MeanEstimate result;
    result.Mean = sum / SAMPLES_PER_CASE;
    result.Variance = math::Max(squares / SAMPLES_PER_CASE - result.Mean * result.Mean, 0.0);
    result.StandardError = math::Sqrt(result.Variance / SAMPLES_PER_CASE);
    return result;
}

/// Every statistic of one case, summed in float64 over its invocations.
struct CaseStatistics final
{
    MeanEstimate Defensive;
    MeanEstimate Slice;
    MeanEstimate Area;
    MeanEstimate SeedWeight;
    MeanEstimate AreaNormalizer;
    float64 NullFraction = 0;
    float64 ReplayMismatchFraction = 0;
};

CaseStatistics ReadCase(const Array<uint8>& bytes, usize caseIndex)
{
    constexpr usize SUM_COUNT = 12;
    StaticArray<float64, SUM_COUNT> sums{};
    for (usize invocation = caseIndex * INVOCATIONS_PER_CASE; invocation < (caseIndex + 1) * INVOCATIONS_PER_CASE;
         ++invocation)
    {
        for (usize sum = 0; sum < SUM_COUNT; ++sum)
        {
            sums[sum] +=
                static_cast<float64>(ReadFloat(bytes, invocation, sum / WORDS_PER_RECORD, sum % WORDS_PER_RECORD));
        }
    }
    CaseStatistics result;
    result.Defensive = MakeMeanEstimate(sums[0], sums[1]);
    result.Slice = MakeMeanEstimate(sums[2], sums[3]);
    result.Area = MakeMeanEstimate(sums[4], sums[5]);
    result.SeedWeight = MakeMeanEstimate(sums[6], sums[7]);
    result.AreaNormalizer = MakeMeanEstimate(sums[8], sums[9]);
    result.NullFraction = sums[10] / SAMPLES_PER_CASE;
    result.ReplayMismatchFraction = sums[11] / SAMPLES_PER_CASE;
    return result;
}

/// Two independent unbiased estimates of one integral agree within five combined standard errors. absoluteTolerance
/// covers receivers that only far Gaussian tails reach, whose integrals are below float precision of the samples.
void CheckEstimatesAgree(const MeanEstimate& estimate, const MeanEstimate& reference, float64 absoluteTolerance)
{
    constexpr float64 STANDARD_ERROR_COUNT = 5;
    const float64 combined =
        math::Sqrt(estimate.StandardError * estimate.StandardError + reference.StandardError * reference.StandardError);
    CHECK(math::Abs(estimate.Mean - reference.Mean) <= STANDARD_ERROR_COUNT * combined + absoluteTolerance);
}
} // namespace

TEST_CASE("Slice sampling of the template's luminaire is unbiased and far less noisy than uniform panel area",
          "[renderer][gpu][slice-sampling-luminaire]")
{
    Array<uint8> bytes;
    if (RunEstimatorKernel(bytes) == false)
    {
        SKIP("No GPU adapter");
    }
    // Irradiance above this has the panel's footprint on the receiver; most receivers of the table are lit.
    constexpr float64 LIT_IRRADIANCE = .5;
    constexpr float64 DARK_TOLERANCE = 1.0e-4;
    usize litCaseCount = 0;
    for (usize caseIndex = 0; caseIndex < CASE_COUNT; ++caseIndex)
    {
        const CaseStatistics statistics = ReadCase(bytes, caseIndex);
        INFO("case " << caseIndex << " (receiver " << caseIndex % RECEIVER_COUNT << ", profile "
                     << caseIndex / RECEIVER_COUNT << "): defensive " << statistics.Defensive.Mean << " +- "
                     << statistics.Defensive.StandardError << ", slice " << statistics.Slice.Mean << " +- "
                     << statistics.Slice.StandardError << ", area " << statistics.Area.Mean << " +- "
                     << statistics.Area.StandardError << ", W " << statistics.SeedWeight.Mean << " +- "
                     << statistics.SeedWeight.StandardError << ", f / b " << statistics.AreaNormalizer.Mean << " +- "
                     << statistics.AreaNormalizer.StandardError << ", null " << statistics.NullFraction
                     << ", replay mismatches " << statistics.ReplayMismatchFraction << ", relative variance slice "
                     << statistics.Slice.GetRelativeVariance() << " defensive "
                     << statistics.Defensive.GetRelativeVariance() << " area "
                     << statistics.Area.GetRelativeVariance());
        // Proposition 1 and Eq. (15): both proposals estimate the irradiance that area sampling estimates.
        CheckEstimatesAgree(statistics.Slice, statistics.Area, DARK_TOLERANCE);
        CheckEstimatesAgree(statistics.Defensive, statistics.Area, DARK_TOLERANCE);
        // Eq. (14): W under uniform seeds and f / b under uniform panel area both estimate Z = integral of f.
        CheckEstimatesAgree(statistics.SeedWeight, statistics.AreaNormalizer, DARK_TOLERANCE);
        // Rounding may move a replayed root across a tile edge, but only for a negligible share of the samples.
        constexpr float64 MAXIMUM_REPLAY_MISMATCH_FRACTION = 1.0e-3;
        CHECK(statistics.ReplayMismatchFraction <= MAXIMUM_REPLAY_MISMATCH_FRACTION);
        if (statistics.Area.Mean <= LIT_IRRADIANCE)
        {
            continue;
        }
        ++litCaseCount;
        // Section 6.4 measures slice sampling's relative variance one to three orders of magnitude below fixed
        // tabulations; uniform area is far worse than those. The mixture keeps a tenth of area sampling's draws but
        // divides them by its full density, so it stays well below area sampling too.
        constexpr float64 DEFENSIVE_VARIANCE_REDUCTION = 10;
        CHECK(statistics.Defensive.GetRelativeVariance() * DEFENSIVE_VARIANCE_REDUCTION <=
              statistics.Area.GetRelativeVariance());
        // Near a footprint's edge many seeds have no root (section 3: the attainable seeds U* are a subset), and the
        // null outcomes alone add a relative variance of about null / (1 - null). Inside a footprint, where the
        // default profile's receivers lie, almost every seed has a root and the reduction is far larger.
        constexpr float64 MAXIMUM_INTERIOR_NULL_FRACTION = .1;
        const bool isInterior = statistics.NullFraction <= MAXIMUM_INTERIOR_NULL_FRACTION;
        const float64 sliceVarianceReduction = isInterior == true ? 50 : 10;
        CHECK(statistics.Slice.GetRelativeVariance() * sliceVarianceReduction <= statistics.Area.GetRelativeVariance());
        if (caseIndex < RECEIVER_COUNT)
        {
            CHECK(isInterior == true);
        }
    }
    constexpr usize MINIMUM_LIT_CASES = 8;
    CHECK(litCaseCount >= MINIMUM_LIT_CASES);
}
