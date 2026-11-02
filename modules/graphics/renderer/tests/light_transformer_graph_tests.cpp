#include "light_transformer_graph.h"
#include "renderer_composite.h"
#include "renderer_light_transformer_encoded_queries.h"
#include "renderer_light_transformer_encoder.h"

#include "../src/internal/render_graph_ray.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/light_transformer_encoder.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>

namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace renderer = ludus::graphics::renderer;
namespace package = ludus::render_graph_packages::light_transformer_graph;
namespace shaders = ludus::shaders;
namespace format = ludus::content::render_graph_format;
using Status = rhi::RasterStatus;
constexpr usize CORNERS = 4;
constexpr usize INDICES = 6;
constexpr usize BUDGET = 512 * 1024 * 1024;
constexpr uint64 TIMEOUT = 120 * time::NANOSECONDS_PER_SECOND;
constexpr uint32 SURFACE_EXTENT = 8;
constexpr usize HISTORY_WORDS = 2;
constexpr float32 ABSOLUTE_TOLERANCE = 0.0001F;
constexpr float32 RELATIVE_TOLERANCE = 0.0001F;
bool IsAccepted(Status status) noexcept
{
    return status == Status::Ready || status == Status::Pending;
}
Array<uint8> ReadFile(const char* path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    REQUIRE(stream.is_open() == true);
    const std::streampos bytes = stream.tellg();
    REQUIRE(bytes > 0);
    REQUIRE(static_cast<usize>(bytes) < BUDGET);
    Array<uint8> result;
    REQUIRE(result.TryResize(static_cast<usize>(bytes)) == true);
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(result.GetData()), static_cast<std::streamsize>(result.GetSize()));
    REQUIRE(stream.good() == true);
    return result;
}
void Wait(rhi::DeviceHandle device, rhi::SubmissionToken completion)
{
    const uint64 started = time::NowTicks();
    Status status = rhi::GetStatus(device, completion);
    while (status == Status::Pending && time::NowTicks() - started < TIMEOUT)
    {
        REQUIRE(rhi::PollLifetime(device) == Status::Ready);
        status = rhi::GetStatus(device, completion);
    }
    REQUIRE(status == Status::Ready);
}
template <typename Element>
void ReadBuffer(rhi::DeviceHandle device, rhi::BufferHandle buffer, BorrowedView<Element> outValues)
{
    rhi::ReadbackTicket ticket;
    Status status = rhi::RequestBufferReadback(device, buffer, 0, outValues.GetSize() * sizeof(Element), ticket);
    const uint64 started = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - started < TIMEOUT)
    {
        REQUIRE(rhi::PollLifetime(device) == Status::Ready);
        status = rhi::GetStatus(device, ticket);
    }
    REQUIRE(status == Status::Ready);
    REQUIRE(rhi::CopyReadback(device,
                              ticket,
                              reinterpret_cast<uint8*>(outValues.GetDataOrNull()),
                              outValues.GetSize() * sizeof(Element)) == Status::Ready);
    REQUIRE(rhi::Release(device, ticket) == Status::Ready);
}
void Render(renderer::RenderGraph& inoutGraph, rhi::DeviceHandle device, rhi::SurfaceHandle surface)
{
    Status status = inoutGraph.Poll();
    const uint64 started = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - started < TIMEOUT)
    {
        REQUIRE(rhi::PollLifetime(device) == Status::Ready);
        status = inoutGraph.Poll();
    }
    REQUIRE(status == Status::Ready);
    REQUIRE(rhi::SetFrameTarget(device, surface, {SURFACE_EXTENT, SURFACE_EXTENT, 0, 0, 0, 1}) ==
            rhi::DeviceStatus::Ready);
    rhi::SubmissionToken completion;
    status = inoutGraph.Render(surface, SURFACE_EXTENT, SURFACE_EXTENT, completion);
    while (status == Status::NotReady && time::NowTicks() - started < TIMEOUT)
    {
        REQUIRE(rhi::PollLifetime(device) == Status::Ready);
        status = inoutGraph.Render(surface, SURFACE_EXTENT, SURFACE_EXTENT, completion);
    }
    REQUIRE(status == Status::Ready);
    Wait(device, completion);
}
struct Scene final
{
    StaticArray<math::Vector3, CORNERS> Positions{
        {{-.75F, -.75F, .5F}, {-.75F, .75F, .5F}, {.75F, -.75F, .5F}, {.75F, .75F, .5F}}};
    StaticArray<uint32, INDICES> Indices{{0, 1, 2, 2, 1, 3}};
    uint32 Binding = 0;
    renderer::RenderGraphMesh Mesh;
    renderer::RenderGraphItem Item;
    renderer::SceneShaderMaterial Material;
    renderer::SceneShaderInputs Inputs;
    renderer::ViewDescription Camera;
    Scene()
    {
        Mesh.Geometry = {Positions.GetData(), Positions.GetSize(), Indices.GetData(), Indices.GetSize()};
        Mesh.MaterialBindings = {&Binding, 1};
        Item.SourceId = 1;
        Material.Scattering.Model = ludus::content::scene_format::MaterialModel::Diffuse;
        Material.BaseColor = {.25F, .5F, .75F};
        Material.Emission = {.01F, .02F, .03F};
        Inputs.Materials = {&Material, 1};
        Camera.OrthographicCamera = true;
        Camera.CameraPosition = {0, 0, -1};
        Camera.CameraForward = {0, 0, 1};
    }
};
void CompareQuery(renderer::RenderGraph& inoutGraph,
                  rhi::DeviceHandle device,
                  rhi::SurfaceHandle surface,
                  const renderer::RenderGraphPlan& plan,
                  const Scene& scene,
                  BorrowedView<const float32> weights)
{
    uint32 resource = format::ABSENT;
    for (usize pass = 0; pass < plan.Passes.GetSize(); ++pass)
    {
        if (plan.Passes[pass].Program == format::Technique::RayTracing)
        {
            resource = plan.Passes[pass].Buffer0;
        }
    }
    REQUIRE(resource != format::ABSENT);
    rhi::BufferHandle packet;
    usize packetBytes = 0;
    REQUIRE(inoutGraph.GetPersistentBuffer(resource, packet, packetBytes) == Status::Ready);
    REQUIRE(packetBytes > sizeof(math::Vector4));
    const renderer::LightTransformerQuery query{
        {.375F, .375F, 0, 0},
        {0, 0, -1, 0},
        {scene.Material.BaseColor.X, scene.Material.BaseColor.Y, scene.Material.BaseColor.Z, 0}};
    rhi::ComputeWorkload decoder;
    Status status =
        renderer::PrepareLightTransformerPacketQueries(weights,
                                                       packet,
                                                       packetBytes,
                                                       {&query, 1},
                                                       device,
                                                       shaders::renderer_light_transformer_encoded_queries::Compute(),
                                                       BUDGET,
                                                       decoder);
    REQUIRE(IsAccepted(status) == true);
    const uint64 started = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - started < TIMEOUT)
    {
        REQUIRE(rhi::PollLifetime(device) == Status::Ready);
        status = decoder.Poll();
    }
    REQUIRE(status == Status::Ready);
    rhi::OrderedGraph graph;
    REQUIRE(decoder.BuildGraph(graph) == Status::Ready);
    rhi::GraphReport report;
    REQUIRE(rhi::CompileOrderedGraph(device, graph, report) == Status::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::ExecuteOrderedGraph(device, surface, graph, completion) == Status::Ready);
    Wait(device, completion);
    rhi::BufferHandle result;
    REQUIRE(decoder.GetBuffer(2, result) == Status::Ready);
    math::Vector4 prediction;
    ReadBuffer(device, result, BorrowedView<math::Vector4>{&prediction, 1});
    rhi::BufferHandle accumulation;
    REQUIRE(renderer::internal::RenderGraphRayPass::GetGraphAccumulationForTesting(inoutGraph, 0, accumulation) ==
            Status::Ready);
    StaticArray<math::Vector4, HISTORY_WORDS> actual{};
    ReadBuffer(device, accumulation, BorrowedView<math::Vector4>{actual.GetData(), actual.GetSize()});
    REQUIRE(math::IsFinite(prediction) == true);
    REQUIRE(math::IsFinite(actual[0].X) == true);
    const math::Vector3 expected{prediction.X * scene.Material.BaseColor.X / math::kPiF + scene.Material.Emission.X,
                                 prediction.Y * scene.Material.BaseColor.Y / math::kPiF + scene.Material.Emission.Y,
                                 prediction.Z * scene.Material.BaseColor.Z / math::kPiF + scene.Material.Emission.Z};
    CHECK(math::Abs(actual[0].X - expected.X) <= ABSOLUTE_TOLERANCE + RELATIVE_TOLERANCE * math::Abs(expected.X));
    CHECK(math::Abs(actual[0].Y - expected.Y) <= ABSOLUTE_TOLERANCE + RELATIVE_TOLERANCE * math::Abs(expected.Y));
    CHECK(math::Abs(actual[0].Z - expected.Z) <= ABSOLUTE_TOLERANCE + RELATIVE_TOLERANCE * math::Abs(expected.Z));
}
} // namespace

TEST_CASE("Light Transformer template encodes the World, decodes GPU queries and preserves failed replacements",
          "[renderer][gpu][light-transformer]")
{
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard() noexcept
        {
            rhi::Shutdown();
        }
    } guard;
    const char* encoderPathOrNull = std::getenv("LUDUS_LIGHT_TRANSFORMER_GRAPH_ENCODER_ASSET");
    const char* decoderPathOrNull = std::getenv("LUDUS_LIGHT_TRANSFORMER_GRAPH_DECODER_ASSET");
    const Array<uint8> encoder =
        ReadFile(encoderPathOrNull == nullptr ? LUDUS_LIGHT_TRANSFORMER_ENCODER_ASSET : encoderPathOrNull);
    const Array<uint8> decoder =
        ReadFile(decoderPathOrNull == nullptr ? LUDUS_LIGHT_TRANSFORMER_DECODER_ASSET : decoderPathOrNull);
    const Array<uint8> packageBytes = ReadFile(LUDUS_LIGHT_TRANSFORMER_PACKAGE_FILE);
    renderer::RenderGraphPackage runtime;
    REQUIRE(runtime.Decode({packageBytes.GetData(), packageBytes.GetSize()}) ==
            renderer::RenderGraphPackageStatus::Ready);
    REQUIRE(runtime.HasLightTransformer() == true);
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    const rhi::DeviceStatus started =
        rhi::CreateDevice({}, { .Width = SURFACE_EXTENT, .Height = SURFACE_EXTENT }, description, device, surface);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    const renderer::RenderGraphPlan plan = runtime.GetPlan();
    const renderer::RenderGraphPrograms programs =
        runtime.GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    REQUIRE(programs.HasLightTransformer == true);
    const renderer::RenderGraphPrograms staticPrograms =
        package::MakePrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    REQUIRE(staticPrograms.HasLightTransformer == true);
    renderer::RenderGraph missing;
    CHECK(missing.Initialize(device, plan, programs, nullptr, SURFACE_EXTENT, SURFACE_EXTENT) ==
          Status::InvalidDescription);
    renderer::LightTransformerModelDescription model;
    model.EncoderAsset = {encoder.GetData(), encoder.GetSize()};
    model.DecoderAsset = {decoder.GetData(), decoder.GetSize()};
    model.EncoderShader = shaders::renderer_light_transformer_encoder::Compute();
    model.WorldUnitsPerModelUnit = 2;
    model.ScenePoints = 128;
    model.MaximumBufferBytes = BUDGET;
    model.MaximumSurfacePreparationBytes = BUDGET;
    renderer::RenderGraph graph;
    Status status = graph.Initialize(device, plan, programs, nullptr, SURFACE_EXTENT, SURFACE_EXTENT, &model);
    REQUIRE(IsAccepted(status) == true);
    const uint64 start = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
    {
        REQUIRE(rhi::PollLifetime(device) == Status::Ready);
        status = graph.Poll();
    }
    REQUIRE(status == Status::Ready);
    Scene scene;
    renderer::RenderGraphReport report;
    status = graph.Update({&scene.Mesh, 1},
                          {&scene.Item, 1},
                          {&scene.Camera, 1},
                          1,
                          renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                          report,
                          &scene.Inputs,
                          1);
    REQUIRE(IsAccepted(status) == true);
    REQUIRE(report.Model.Issue == renderer::LightTransformerPreparationIssue::None);
    Render(graph, device, surface);
    Array<float32> weights;
    REQUIRE(renderer::ReadLightTransformerDecoderWeights({decoder.GetData(), decoder.GetSize()}, weights) ==
            Status::Ready);
    CompareQuery(graph, device, surface, plan, scene, {weights.GetData(), weights.GetSize()});
    uint32 resource = plan.Passes[0].Buffer0;
    rhi::BufferHandle firstPacket;
    usize packetBytes = 0;
    REQUIRE(graph.GetPersistentBuffer(resource, firstPacket, packetBytes) == Status::Ready);
    REQUIRE(IsAccepted(graph.Update({},
                                    {},
                                    {&scene.Camera, 1},
                                    2,
                                    renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                    report,
                                    nullptr,
                                    1)) == true);
    Render(graph, device, surface);
    rhi::BufferHandle cameraPacket;
    REQUIRE(graph.GetPersistentBuffer(resource, cameraPacket, packetBytes) == Status::Ready);
    CHECK(cameraPacket == firstPacket);
    renderer::DirectLight unsupported;
    scene.Inputs.Lights = {&unsupported, 1};
    CHECK(graph.Update({&scene.Mesh, 1},
                       {&scene.Item, 1},
                       {&scene.Camera, 1},
                       3,
                       renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                       report,
                       &scene.Inputs,
                       2) == Status::Unsupported);
    CHECK(report.Model.Issue == renderer::LightTransformerPreparationIssue::AnalyticLights);
    CHECK(report.Model.AnalyticLights == 1);
    CHECK(graph.GetDrawableSceneRevision() == 2);
    REQUIRE(graph.GetPersistentBuffer(resource, cameraPacket, packetBytes) == Status::Ready);
    CHECK(cameraPacket == firstPacket);
    Render(graph, device, surface);
    CHECK(graph.GetOutputSceneRevision() == 2);
    scene.Inputs.Lights = {};
    scene.Material.BaseColor = {.5F, .4F, .3F};
    REQUIRE(IsAccepted(graph.Update({&scene.Mesh, 1},
                                    {&scene.Item, 1},
                                    {&scene.Camera, 1},
                                    4,
                                    renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                    report,
                                    &scene.Inputs,
                                    3)) == true);
    Render(graph, device, surface);
    CHECK(graph.GetDrawableSceneRevision() == 4);
    REQUIRE(graph.GetPersistentBuffer(resource, cameraPacket, packetBytes) == Status::Ready);
    CHECK(cameraPacket != firstPacket);
    CompareQuery(graph, device, surface, plan, scene, {weights.GetData(), weights.GetSize()});
    scene.Positions[3].X = 10;
    CHECK(graph.Update({&scene.Mesh, 1},
                       {&scene.Item, 1},
                       {&scene.Camera, 1},
                       5,
                       renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                       report,
                       &scene.Inputs,
                       4) == Status::CapacityExceeded);
    CHECK(report.Model.Issue == renderer::LightTransformerPreparationIssue::FixedScale);
    CHECK(graph.GetDrawableSceneRevision() == 4);
}
