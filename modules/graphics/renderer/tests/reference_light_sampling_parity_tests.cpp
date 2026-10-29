#include "../src/internal/ltc_table_data.h"
#include "../src/internal/reference_data.h"
#include "../src/internal/reference_light_sampling.h"
#include "reference_light_sampling_parity.h"
#include "reference_light_sampling_parity_layout.h"

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

// Native execution of the GPU-compute reference's scene-independent ResampledProjectedSolidAngle pieces
// (shaders/reference_light_sampling.slang) against their C++ counterparts in src/reference_light_sampling.cpp: the
// technique weights partition unity, Z-hat = 0 applies the inclusion bound, an empty LTC-space clip disables the
// specular technique, and the receiver frame, polygons, densities and plane intersection agree in both
// implementations. The estimator is docs/architecture/many-area-light-sampling-reference-review.md. Thanks to
// Christoph Peters, "BRDF Importance Sampling for Polygonal Lights", ACM Transactions on Graphics 40(4), Article 140,
// 2021, https://doi.org/10.1145/3450626.3459672, sections 4.1-4.3, for the weights and the empty-clip rule.
namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace rr = ludus::graphics::renderer;
namespace internal = ludus::graphics::renderer::internal;
using rhi::RasterStatus;

constexpr uint64 TIMEOUT_NANOSECONDS = 10 * time::NANOSECONDS_PER_SECOND;
// Per-request readback limit; well below the default 256 KiB transfer profile.
constexpr usize READBACK_CHUNK_BYTES = usize{64} * 1024;
constexpr usize GROUP_COUNT = 64;
constexpr usize INVOCATION_COUNT = GROUP_COUNT * LUDUS_REFERENCE_LIGHT_PARITY_GROUP_SIZE;
constexpr usize WORDS_PER_RECORD = 4;
constexpr usize RESULT_BYTES =
    INVOCATION_COUNT * LUDUS_REFERENCE_LIGHT_PARITY_RECORDS_PER_INVOCATION * WORDS_PER_RECORD * sizeof(uint32);
// Weights partition unity up to a few float32 roundings of the two divisions and the blend.
constexpr float32 PARTITION_TOLERANCE = 1e-6F;
// Relative agreement of quantities that both implementations compute with the same float32 operations.
constexpr float32 ARITHMETIC_TOLERANCE = 1e-5F;
// Relative agreement after the table lookup, polygon clipping and projected-solid-angle integration, whose
// transcendental functions differ by a few ulps between the host and the device.
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

/// Run the parity kernel once and read back every record. Returns false without a native GPU adapter.
bool RunParityKernel(Array<uint8>& outBytes)
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
    // The kernel's receiver frames look up the renderer's LTC table, which the renderer binds as a storage buffer.
    rhi::BufferHandle table;
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         { .Role = rhi::BufferRole::Storage, .Size = TABLE_BYTES },
                                         {reinterpret_cast<const uint8*>(internal::LTC_TABLE.GetData()), TABLE_BYTES},
                                         table)) == true);
    const rhi::RasterShaderDescription shader = ludus::shaders::reference_light_sampling_parity::Compute();
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
        { .Binding = 0, .Buffer = results, .Size = RESULT_BYTES },
        { .Binding = LUDUS_REFERENCE_LIGHT_PARITY_LTC_TABLE_BINDING, .Buffer = table, .Size = TABLE_BYTES },
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
    pass.Name = "reference light sampling parity";
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
struct ParityRecord final
{
    StaticArray<uint32, WORDS_PER_RECORD> Words{};

    [[nodiscard]] float32 GetFloat(usize word) const
    {
        float32 value = 0;
        std::memcpy(&value, &Words[word], sizeof(value));
        return value;
    }
};

ParityRecord ReadRecord(const Array<uint8>& bytes, usize invocation, usize record)
{
    ParityRecord result;
    const usize offset = ((invocation * LUDUS_REFERENCE_LIGHT_PARITY_RECORDS_PER_INVOCATION) + record) *
                         WORDS_PER_RECORD * sizeof(uint32);
    std::memcpy(result.Words.GetData(), bytes.GetData() + offset, sizeof(uint32) * WORDS_PER_RECORD);
    return result;
}

bool IsNear(float32 value, float32 expected, float32 tolerance) noexcept
{
    return math::Abs(value - expected) <= tolerance * math::Max(math::Abs(expected), 1.0F);
}
math::Vector3 ReadVector(const ParityRecord& record) noexcept
{
    return {record.GetFloat(0), record.GetFloat(1), record.GetFloat(2)};
}
} // namespace

TEST_CASE("GPU-compute resampled light pieces match the CPU oracle's",
          "[renderer][gpu][reference-compute][resampled-light]")
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
    if (RunParityKernel(bytes) == false)
    {
        SKIP("No native GPU adapter");
    }
    usize zeroTotalCount = 0;
    usize preparedCount = 0;
    usize geometryMismatchCount = 0;
    usize diffuseResolvedCount = 0;
    usize specularResolvedCount = 0;
    usize sampledCount = 0;
    usize planeHitCount = 0;
    for (usize invocation = 0; invocation < INVOCATION_COUNT; ++invocation)
    {
        INFO("invocation " << invocation);
        // Clamped optimal MIS: a partition of unity where a technique samples, zero weight where it cannot.
        const ParityRecord weightInput =
            ReadRecord(bytes, invocation, LUDUS_REFERENCE_LIGHT_PARITY_WEIGHT_INPUT_RECORD);
        const ParityRecord weight = ReadRecord(bytes, invocation, LUDUS_REFERENCE_LIGHT_PARITY_WEIGHT_RECORD);
        const math::Vector2 coefficients{weightInput.GetFloat(0), weightInput.GetFloat(1)};
        const math::Vector2 densities{weightInput.GetFloat(2), weightInput.GetFloat(3)};
        const math::Vector2 weights{weight.GetFloat(1), weight.GetFloat(2)};
        CHECK((densities.X > 0 || weights.X == 0));
        CHECK((densities.Y > 0 || weights.Y == 0));
        if (densities.X > 0 || densities.Y > 0)
        {
            CHECK(math::Abs(weights.X + weights.Y - 1) <= PARTITION_TOLERANCE);
        }
        const math::Vector2 hostWeights =
            internal::ComputeReferenceLightTechniqueWeights(coefficients, densities, weight.GetFloat(0));
        CHECK(IsNear(weights.X, hostWeights.X, ARITHMETIC_TOLERANCE));
        CHECK(IsNear(weights.Y, hostWeights.Y, ARITHMETIC_TOLERANCE));

        // P-tilde = P_E min(p-hat / Z-hat, min(1, M p_src)); the inclusion bound alone when Z-hat = 0.
        const ParityRecord surrogateInput =
            ReadRecord(bytes, invocation, LUDUS_REFERENCE_LIGHT_PARITY_SURROGATE_INPUT_RECORD);
        const float32 surrogate =
            ReadRecord(bytes, invocation, LUDUS_REFERENCE_LIGHT_PARITY_SURROGATE_RECORD).GetFloat(0);
        const float32 categoryProbability = surrogateInput.GetFloat(0);
        const float32 target = surrogateInput.GetFloat(1);
        const float32 sourcePmf = surrogateInput.GetFloat(2);
        const float32 targetTotal = surrogateInput.GetFloat(3);
        CHECK(IsNear(surrogate,
                     internal::EvaluateReferenceSurrogateProbability(categoryProbability,
                                                                     target,
                                                                     sourcePmf,
                                                                     targetTotal,
                                                                     LUDUS_REFERENCE_RIS_CANDIDATE_COUNT),
                     ARITHMETIC_TOLERANCE));
        if (targetTotal == 0 && categoryProbability > 0 && target > 0 && sourcePmf > 0)
        {
            ++zeroTotalCount;
            const float32 bound =
                math::Min(1.0F, static_cast<float32>(LUDUS_REFERENCE_RIS_CANDIDATE_COUNT) * sourcePmf);
            CHECK(IsNear(surrogate, categoryProbability * bound, ARITHMETIC_TOLERANCE));
        }

        // An empty LTC-space clip disables the specular technique; the diffuse one takes all the weight.
        const ParityRecord emptyClip = ReadRecord(bytes, invocation, LUDUS_REFERENCE_LIGHT_PARITY_EMPTY_CLIP_RECORD);
        CHECK(emptyClip.Words[0] == 1);
        CHECK(emptyClip.GetFloat(1) > 0);
        CHECK(emptyClip.GetFloat(2) == 0);
        CHECK(emptyClip.GetFloat(3) == 0);

        // The receiver chain: frame, polygons, densities at the device's sample and its plane intersection.
        const ParityRecord view = ReadRecord(bytes, invocation, LUDUS_REFERENCE_LIGHT_PARITY_VIEW_RECORD);
        const ParityRecord material = ReadRecord(bytes, invocation, LUDUS_REFERENCE_LIGHT_PARITY_MATERIAL_RECORD);
        rr::ReferenceBsdf bsdf;
        bsdf.BaseColor = ReadVector(material);
        bsdf.Metallic = material.GetFloat(3);
        bsdf.Roughness = view.GetFloat(3);
        const internal::ReferenceReceiverFrame frame =
            internal::MakeReferenceReceiverFrame({}, {0, 0, 1}, ReadVector(view), bsdf);
        StaticArray<math::Vector3, 3> vertices{};
        for (usize corner = 0; corner < 3; ++corner)
        {
            vertices[corner] =
                ReadVector(ReadRecord(bytes, invocation, LUDUS_REFERENCE_LIGHT_PARITY_VERTEX_RECORD + corner));
        }
        const ParityRecord polygon = ReadRecord(bytes, invocation, LUDUS_REFERENCE_LIGHT_PARITY_POLYGON_RECORD);
        const ParityRecord density = ReadRecord(bytes, invocation, LUDUS_REFERENCE_LIGHT_PARITY_DENSITY_RECORD);
        internal::ReferenceTriangleLight light;
        const bool isPrepared =
            internal::TryPrepareReferenceTriangleLight(frame, vertices[0], vertices[1], vertices[2], light);
        CHECK((polygon.Words[0] == 1) == isPrepared);
        CHECK((density.Words[1] == 1) == frame.HasSpecular);
        CHECK(IsNear(polygon.GetFloat(3), frame.DiffuseAlbedoLuminance, ARITHMETIC_TOLERANCE));
        if (isPrepared == false)
        {
            continue;
        }
        ++preparedCount;
        const ParityRecord direction = ReadRecord(bytes, invocation, LUDUS_REFERENCE_LIGHT_PARITY_DIRECTION_RECORD);
        const math::Vector3 directionWorld = ReadVector(direction);
        const math::Vector2 hostDensities =
            internal::EvaluateReferenceLightTechniqueDensities(frame, light, directionWorld);
        // A sliver polygon's integral is only as accurate as single precision allows (projected_solid_angle.hpp,
        // linearly_transformed_cosine.hpp); its technique then carries a negligible coefficient, so each technique's
        // integral and density are compared above that size.
        const bool isDiffuseResolved = light.DiffuseIntegral >= SLIVER_INTEGRAL;
        const bool isSpecularResolved = light.SpecularIntegral >= SLIVER_INTEGRAL;
        const bool isGeometryNear =
            IsNear(density.GetFloat(0), frame.SpecularAlbedoLuminance, GEOMETRY_TOLERANCE) &&
            (isDiffuseResolved == false || (IsNear(polygon.GetFloat(1), light.DiffuseIntegral, GEOMETRY_TOLERANCE) &&
                                            IsNear(density.GetFloat(2), hostDensities.X, GEOMETRY_TOLERANCE))) &&
            (isSpecularResolved == false || (IsNear(polygon.GetFloat(2), light.SpecularIntegral, GEOMETRY_TOLERANCE) &&
                                             IsNear(density.GetFloat(3), hostDensities.Y, GEOMETRY_TOLERANCE)));
        diffuseResolvedCount += isDiffuseResolved == true ? 1U : 0U;
        specularResolvedCount += isSpecularResolved == true ? 1U : 0U;
        geometryMismatchCount += isGeometryNear == true ? 0U : 1U;
        if (direction.Words[3] == 0)
        {
            continue;
        }
        ++sampledCount;
        const ParityRecord plane = ReadRecord(bytes, invocation, LUDUS_REFERENCE_LIGHT_PARITY_PLANE_RECORD);
        float32 distanceWorld = 0;
        math::Vector2 barycentric{};
        const bool isHit = internal::TryIntersectReferenceTrianglePlane({},
                                                                        directionWorld,
                                                                        vertices[0],
                                                                        vertices[1],
                                                                        vertices[2],
                                                                        distanceWorld,
                                                                        barycentric);
        planeHitCount += plane.Words[0] == 1 ? 1U : 0U;
        if (isHit == true && plane.Words[0] == 1)
        {
            CHECK(IsNear(plane.GetFloat(1), distanceWorld, GEOMETRY_TOLERANCE));
            CHECK(math::Abs(plane.GetFloat(2) - barycentric.X) <= GEOMETRY_TOLERANCE);
            CHECK(math::Abs(plane.GetFloat(3) - barycentric.Y) <= GEOMETRY_TOLERANCE);
        }
    }
    CHECK(zeroTotalCount > INVOCATION_COUNT / 4);
    CHECK(preparedCount > INVOCATION_COUNT / 2);
    // Apple M5 Metal: 3 of 4096. Clipping at a vertex near the horizon can amplify an ulp of difference.
    CHECK(geometryMismatchCount <= preparedCount / 500);
    CHECK(diffuseResolvedCount > preparedCount / 2);
    CHECK(specularResolvedCount > preparedCount / 4);
    // A sample of the triangle's own solid angle meets its plane inside it, except at FP32 edges and for LTC sliver
    // samples (Apple M5 Metal: 3505 of 3533).
    CHECK(sampledCount > preparedCount / 2);
    CHECK(planeHitCount * 100 >= sampledCount * 98);
}
