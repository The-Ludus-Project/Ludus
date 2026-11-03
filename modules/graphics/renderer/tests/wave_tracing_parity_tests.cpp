#include "../src/internal/wave_connection.h"
#include "../src/internal/wave_scene_queries.h"
#include "wave_tracing_parity.h"
#include "wave_tracing_parity_layout.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/elliptical_cone.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/wave_tracing.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

namespace
{
using namespace ludus::foundation;
using namespace ludus::graphics::renderer;
namespace math = ludus::foundation::math;
namespace rhi = ludus::graphics::rhi;
using rhi::RasterStatus;
constexpr uint64 TIMEOUT_NANOSECONDS = 30 * time::NANOSECONDS_PER_SECOND;
constexpr usize CASE_COUNT = LUDUS_WAVE_PARITY_CASE_COUNT;
constexpr usize WORDS_PER_RECORD = 4;
constexpr usize CASE_WORDS = CASE_COUNT * LUDUS_WAVE_PARITY_CASE_RECORDS * WORDS_PER_RECORD;
constexpr usize RESULT_WORDS = CASE_COUNT * LUDUS_WAVE_PARITY_RESULT_RECORDS * WORDS_PER_RECORD;
constexpr usize CELL_COUNT = usize{LUDUS_WAVE_PARITY_RESOLUTION} * LUDUS_WAVE_PARITY_RESOLUTION;
constexpr usize CASE_BYTES = CASE_WORDS * sizeof(uint32);
constexpr usize RESULT_BYTES = RESULT_WORDS * sizeof(uint32);
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
    const rhi::RasterShaderDescription shader = ludus::shaders::wave_tracing_parity::Compute();
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
        { .Binding = 0, .Buffer = cases, .Size = CASE_BYTES },
        { .Binding = 1, .Buffer = results, .Size = RESULT_BYTES },
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
        .Groups = {static_cast<uint32>(CASE_COUNT / CASE_COUNT), 1, 1},
    };
    rhi::GraphPassDescription pass;
    pass.Name = "wave tracing parity";
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

void WriteFloat(usize index, Array<uint32>& outWords, float64 value)
{
    const float32 narrowed = static_cast<float32>(value);
    std::memcpy(&outWords[index], &narrowed, sizeof(narrowed));
}
float64 ReadFloat(const Array<uint32>& words, usize index)
{
    float32 value = 0;
    std::memcpy(&value, &words[index], sizeof(value));
    return static_cast<float64>(value);
}
} // namespace

TEST_CASE("Scalar wave transport and beam propagation match the CPU oracle on GPU",
          "[renderer][gpu][wave-tracing][parity]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    Array<uint32> words;
    REQUIRE(words.TryResize(CASE_WORDS, uint32{0}) == true);
    for (usize index = 0; index < CASE_COUNT; ++index)
    {
        const usize base = index * LUDUS_WAVE_PARITY_CASE_RECORDS * WORDS_PER_RECORD;
        WriteFloat(base, words, 0.02 + 0.002 * static_cast<float64>(index));
        WriteFloat(base + 1, words, 0.01);
        WriteFloat(base + 2, words, 0.02);
        WriteFloat(base + 3, words, 0.3);
        WriteFloat(base + 4, words, 0.4);
        words[base + 5] = LUDUS_WAVE_PARITY_RESOLUTION;
        WriteFloat(base + 6, words, 0.02 * static_cast<float64>(index));
        WriteFloat(base + 7, words, -0.01 * static_cast<float64>(index));
        WriteFloat(base + 8, words, 4);
        WriteFloat(base + 9, words, 0.001 * static_cast<float64>(index));
        WriteFloat(base + 10, words, 2);
        for (usize cell = 0; cell < CELL_COUNT; ++cell)
        {
            const usize x = cell % LUDUS_WAVE_PARITY_RESOLUTION;
            const bool isOpen = index % 4 == 0 || (index % 4 == 1 && x < 12) ||
                                (index % 4 == 2 && ((x >= 8 && x < 12) || (x >= 20 && x < 24)));
            WriteFloat(base + LUDUS_WAVE_PARITY_HEADER_RECORDS * WORDS_PER_RECORD + cell,
                       words,
                       isOpen == true ? 1 : 0);
        }
    }
    Array<uint32> results;
    if (RunParityKernel(words, results) == false)
    {
        SKIP("No native GPU adapter");
    }
    for (usize index = 0; index < CASE_COUNT; ++index)
    {
        CAPTURE(index);
        const usize base = index * LUDUS_WAVE_PARITY_CASE_RECORDS * WORDS_PER_RECORD;
        const WaveAperture aperture{ReadFloat(words, base),
                                    ReadFloat(words, base + 1),
                                    ReadFloat(words, base + 2),
                                    ReadFloat(words, base + 3),
                                    ReadFloat(words, base + 4),
                                    LUDUS_WAVE_PARITY_RESOLUTION};
        const math::Vector3d receiverApertureWorld{ReadFloat(words, base + 6),
                                                   ReadFloat(words, base + 7),
                                                   ReadFloat(words, base + 8)};
        Array<float64> transmission;
        REQUIRE(transmission.TryResize(CELL_COUNT) == true);
        for (usize cell = 0; cell < transmission.GetSize(); ++cell)
        {
            transmission[cell] = ReadFloat(words, base + LUDUS_WAVE_PARITY_HEADER_RECORDS * WORDS_PER_RECORD + cell);
        }
        WaveField field;
        REQUIRE(TryEvaluateWaveAperture(aperture,
                                        {transmission.GetData(), transmission.GetSize()},
                                        receiverApertureWorld,
                                        field) == true);
        WaveBeam propagated;
        WaveMoments moments;
        REQUIRE(TryPropagateWaveBeam({aperture.WavelengthWorld,
                                      aperture.SigmaXWorldSquared,
                                      aperture.SigmaYWorldSquared,
                                      ReadFloat(words, base + 9)},
                                     ReadFloat(words, base + 10),
                                     propagated,
                                     moments) == true);
        const usize resultBase = index * LUDUS_WAVE_PARITY_RESULT_RECORDS * WORDS_PER_RECORD;
        CHECK(math::Abs(ReadFloat(results, resultBase) - field.Real) < 1e-5);
        CHECK(math::Abs(ReadFloat(results, resultBase + 1) - field.Imaginary) < 1e-5);
        CHECK(math::Abs(ReadFloat(results, resultBase + 2) -
                        (field.Real * field.Real + field.Imaginary * field.Imaginary)) < 1e-5);
        CHECK(ReadFloat(results, resultBase + 3) == 1);
        CHECK(math::Abs(ReadFloat(results, resultBase + 4) - moments.VarianceXWorldSquared) < 1e-7);
        CHECK(math::Abs(ReadFloat(results, resultBase + 5) - moments.VarianceYWorldSquared) < 1e-7);
        // float32 tau and Metal reciprocal rounding contribute up to three ulps to the covariance.
        CHECK(math::Abs(ReadFloat(results, resultBase + 6) - moments.CovarianceX) < 2e-7);
        CHECK(math::Abs(ReadFloat(results, resultBase + 7) - moments.CovarianceY) < 2e-7);
    }
}

TEST_CASE("Elliptical cone GPU extrema match the independent float64 oracle",
          "[renderer][gpu][elliptical-cone][parity]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    Array<uint32> words;
    REQUIRE(words.TryResize(CASE_WORDS, uint32{0}) == true);
    // Rotate an oblique triangle through the beam footprint, exercising plane and edge extrema,
    // cylindrical and opening envelopes, nonzero origins, clipping and misses.
    for (usize batch = 0; batch < 8; ++batch)
    {
        rhi::Shutdown();
        for (usize index = 0; index < CASE_COUNT; ++index)
        {
            const usize base = index * LUDUS_WAVE_PARITY_CASE_RECORDS * WORDS_PER_RECORD;
            const float64 angle = static_cast<float64>(batch * CASE_COUNT + index) * 0.37;
            WriteFloat(base, words, 2);
            WriteFloat(base + 1, words, -3);
            WriteFloat(base + 2, words, 1);
            WriteFloat(base + 3, words, batch % 2 == 0 ? 0.2 : 0);
            WriteFloat(base + 4, words, 0);
            WriteFloat(base + 5, words, 0);
            WriteFloat(base + 6, words, 1);
            WriteFloat(base + 7, words, batch % 3 == 0 ? 0 : 0.1);
            WriteFloat(base + 8, words, math::Cos(angle));
            WriteFloat(base + 9, words, math::Sin(angle));
            WriteFloat(base + 10, words, 0);
            words[base + 11] = LUDUS_WAVE_PARITY_CONE_TAG;
            WriteFloat(base + 12, words, batch % 2 == 0 ? 0.25 : 1);
            WriteFloat(base + 13, words, 0.1);
            WriteFloat(base + 14, words, 8);
            const float64 offset = static_cast<float64>(index) * 0.1;
            WriteFloat(base + 16, words, 1.5 + offset);
            WriteFloat(base + 17, words, -4);
            WriteFloat(base + 18, words, 2);
            WriteFloat(base + 20, words, 2.5 + offset);
            WriteFloat(base + 21, words, -4);
            WriteFloat(base + 22, words, 4);
            WriteFloat(base + 24, words, 2 + offset);
            WriteFloat(base + 25, words, -2);
            WriteFloat(base + 26, words, 3);
        }
        Array<uint32> results;
        if (RunParityKernel(words, results) == false)
        {
            SKIP("GPU adapter unavailable");
        }
        for (usize index = 0; index < CASE_COUNT; ++index)
        {
            INFO("batch " << batch << " case " << index);
            const usize base = index * LUDUS_WAVE_PARITY_CASE_RECORDS * WORDS_PER_RECORD;
            const auto readVector = [&](usize position) {
                return math::Vector3d{ReadFloat(words, base + position),
                                      ReadFloat(words, base + position + 1),
                                      ReadFloat(words, base + position + 2)};
            };
            math::EllipticalCone cone;
            cone.OriginWorld = readVector(0);
            cone.RadiusWorld = ReadFloat(words, base + 3);
            cone.DirectionWorld = readVector(4);
            cone.Slope = ReadFloat(words, base + 7);
            cone.AxisXWorld = readVector(8);
            cone.MinorToMajor = ReadFloat(words, base + 12);
            cone.NearWorld = ReadFloat(words, base + 13);
            cone.FarWorld = ReadFloat(words, base + 14);
            const math::ConeTriangle triangle{readVector(16), readVector(20), readVector(24)};
            math::ConeTriangleIntersection intersection;
            REQUIRE(math::TryIntersectEllipticalConeTriangle(cone, triangle, intersection) == true);
            const usize destination = index * LUDUS_WAVE_PARITY_RESULT_RECORDS * WORDS_PER_RECORD;
            REQUIRE(ReadFloat(results, destination + 3) == 1);
            REQUIRE((ReadFloat(results, destination + 2) == 1) == intersection.IsHit);
            if (intersection.IsHit == true)
            {
                CHECK(math::Abs(ReadFloat(results, destination) - intersection.NearWorld) < 2e-4);
                CHECK(math::Abs(ReadFloat(results, destination + 1) - intersection.FarWorld) < 2e-4);
                const math::Vector3d barycentric{ReadFloat(results, destination + 4),
                                                 ReadFloat(results, destination + 5),
                                                 ReadFloat(results, destination + 6)};
                const math::Vector3d pointWorld = triangle.FirstWorld * barycentric.X +
                                                  triangle.SecondWorld * barycentric.Y +
                                                  triangle.ThirdWorld * barycentric.Z;
                CHECK(math::Abs(math::Dot(pointWorld - cone.OriginWorld, cone.DirectionWorld) -
                                intersection.NearWorld) < 2e-4);
            }
        }
    }
}

TEST_CASE("Wave region GPU traversal matches the CPU scene BVH", "[renderer][gpu][wave-region][parity]")
{
    namespace internal = ludus::graphics::renderer::internal;
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    internal::ReferenceSceneData scene;
    for (uint32 layer = 0; layer < 10; ++layer)
    {
        for (uint32 column = 0; column < 10; ++column)
        {
            internal::ReferenceTriangle triangle;
            const float32 x = static_cast<float32>(column) * 0.3F - 1;
            const float32 z = static_cast<float32>(layer) * 0.2F + 2;
            triangle.Positions[0] = {x, -1, z};
            triangle.Positions[1] = {x + 1, -1, z};
            triangle.Positions[2] = {x, 1, z};
            triangle.Area = 1;
            triangle.GeometricNormal = {0, 0, 1};
            REQUIRE(scene.Triangles.TryAdd(triangle) == true);
        }
    }
    SECTION("Layered aperture regions") {}
    SECTION("Grazing near-field wall")
    {
        internal::ReferenceTriangle wall;
        wall.Positions[0] = {-0.001F, -1, 0};
        wall.Positions[1] = {-0.001F, 1, 0};
        wall.Positions[2] = {-0.001F, -1, 10};
        wall.Area = 10;
        wall.GeometricNormal = {1, 0, 0};
        REQUIRE(scene.Triangles.TryAdd(wall) == true);
        wall.Positions[0] = {-0.001F, 1, 0};
        wall.Positions[1] = {-0.001F, 1, 10};
        REQUIRE(scene.Triangles.TryAdd(wall) == true);
    }
    REQUIRE(internal::BuildReferenceBvh(scene) == RasterStatus::Ready);
    Array<uint32> words;
    REQUIRE(words.TryResize(CASE_WORDS, uint32{0}) == true);
    const usize nodeFirst = LUDUS_WAVE_PARITY_REGION_CONE_FIRST + CASE_COUNT * LUDUS_WAVE_PARITY_REGION_CONE_RECORDS;
    const usize triangleFirst = nodeFirst + scene.Nodes.GetSize() * 3;
    const usize orderFirst = triangleFirst + scene.Triangles.GetSize() * 3;
    REQUIRE(orderFirst * WORDS_PER_RECORD + scene.Order.GetSize() < CASE_WORDS);
    words[0] = static_cast<uint32>(nodeFirst);
    words[1] = static_cast<uint32>(scene.Nodes.GetSize());
    words[2] = static_cast<uint32>(triangleFirst);
    words[3] = LUDUS_WAVE_PARITY_REGION_TAG;
    words[4] = static_cast<uint32>(orderFirst);
    words[5] = static_cast<uint32>(scene.Triangles.GetSize());
    const auto writeVector = [&](usize position, math::Vector3 value) {
        WriteFloat(position, words, static_cast<float64>(value.X));
        WriteFloat(position + 1, words, static_cast<float64>(value.Y));
        WriteFloat(position + 2, words, static_cast<float64>(value.Z));
    };
    for (usize index = 0; index < scene.Nodes.GetSize(); ++index)
    {
        const internal::ReferenceBvhNode& node = scene.Nodes[index];
        const usize base = (nodeFirst + index * 3) * WORDS_PER_RECORD;
        writeVector(base, node.Bounds.Min);
        writeVector(base + WORDS_PER_RECORD, node.Bounds.Max);
        words[base + 3] = node.Left;
        words[base + 7] = node.Right;
        words[base + 8] = node.First;
        words[base + 9] = node.Count;
    }
    for (usize index = 0; index < scene.Triangles.GetSize(); ++index)
    {
        for (usize corner = 0; corner < 3; ++corner)
        {
            writeVector((triangleFirst + index * 3 + corner) * WORDS_PER_RECORD,
                        scene.Triangles[index].Positions[corner]);
        }
        words[orderFirst * WORDS_PER_RECORD + index] = scene.Order[index];
    }
    StaticArray<math::EllipticalCone, CASE_COUNT> cones;
    for (usize index = 0; index < CASE_COUNT; ++index)
    {
        const usize base =
            (LUDUS_WAVE_PARITY_REGION_CONE_FIRST + index * LUDUS_WAVE_PARITY_REGION_CONE_RECORDS) * WORDS_PER_RECORD;
        WriteFloat(base, words, static_cast<float64>(index) * 0.17);
        WriteFloat(base + 3, words, 0.2);
        WriteFloat(base + 6, words, 1);
        WriteFloat(base + 7, words, 0.02);
        WriteFloat(base + 8, words, 1);
        WriteFloat(base + 12, words, 0.3);
        WriteFloat(base + 14, words, 10);
        math::EllipticalCone& cone = cones[index];
        cone.OriginWorld.X = ReadFloat(words, base);
        cone.RadiusWorld = ReadFloat(words, base + 3);
        cone.Slope = ReadFloat(words, base + 7);
        cone.MinorToMajor = ReadFloat(words, base + 12);
        cone.FarWorld = ReadFloat(words, base + 14);
    }
    Array<uint32> results;
    if (RunParityKernel(words, results) == false)
    {
        SKIP("GPU adapter unavailable");
    }
    for (usize index = 0; index < CASE_COUNT; ++index)
    {
        INFO("region " << index);
        internal::WaveInteractionRegion region;
        REQUIRE(internal::FindWaveInteractionRegion(scene, cones[index], region) == RasterStatus::Ready);
        const usize destination = index * LUDUS_WAVE_PARITY_RESULT_RECORDS * WORDS_PER_RECORD;
        REQUIRE(results[destination + 3] == 0);
        CHECK(results[destination + 2] == region.TriangleCount);
        CHECK(math::Abs(ReadFloat(results, destination) - region.Frustum.NearWorld) < 2e-5);
        CHECK(math::Abs(ReadFloat(results, destination + 1) - region.Frustum.FarWorld) < 2e-5);
        uint32 hash = 0;
        for (uint32 slot = 0; slot < region.TriangleCount; ++slot)
        {
            hash = hash * LUDUS_WAVE_PARITY_REGION_HASH_FACTOR + region.Triangles[slot];
        }
        CHECK(results[destination + 4] == hash);
    }
    // Run the same production connection propagator through these scene regions, including successive masks.
    rhi::Shutdown();
    words[3] = LUDUS_WAVE_PARITY_CONNECTION_TAG;
    float64 wavelengthWorld = 0.01;
    SECTION("Macroscopic diffraction lattice") {}
    SECTION("Visible wavelength diffraction lattice")
    {
        wavelengthWorld = 550e-9;
    }
    for (usize index = 0; index < CASE_COUNT; ++index)
    {
        const usize base =
            (LUDUS_WAVE_PARITY_REGION_CONE_FIRST + index * LUDUS_WAVE_PARITY_REGION_CONE_RECORDS) * WORDS_PER_RECORD;
        WriteFloat(base + 15, words, wavelengthWorld);
    }
    REQUIRE(RunParityKernel(words, results) == true);
    for (usize index = 0; index < CASE_COUNT; ++index)
    {
        INFO("connection " << index);
        WaveField field;
        const usize base =
            (LUDUS_WAVE_PARITY_REGION_CONE_FIRST + index * LUDUS_WAVE_PARITY_REGION_CONE_RECORDS) * WORDS_PER_RECORD;
        REQUIRE(internal::EvaluateWaveConnection(scene,
                                                 cones[index].OriginWorld,
                                                 cones[index].DirectionWorld,
                                                 cones[index].FarWorld,
                                                 ReadFloat(words, base + 15),
                                                 field) == RasterStatus::Ready);
        const usize destination = index * LUDUS_WAVE_PARITY_RESULT_RECORDS * WORDS_PER_RECORD;
        REQUIRE(results[destination + 3] == 0);
        CHECK(math::Abs(ReadFloat(results, destination) - field.Real) < 3e-4);
        CHECK(math::Abs(ReadFloat(results, destination + 1) - field.Imaginary) < 3e-4);
        CHECK(math::Abs(ReadFloat(results, destination + 2) - field.Real * field.Real -
                        field.Imaginary * field.Imaginary) < 3e-4);
    }
}
