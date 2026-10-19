#include "internal/lifecycle.h"
#include "reference_raster.h"

#include <ludus/foundation/base/compiler.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/rhi/device.h>

#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace format = ludus::content::render_graph_format;
namespace ludus::graphics::rhi::backend
{
extern uint32 PendingToken;
extern StartupError ImmediateError;
extern FrameStatus NextFrame;
extern Backend ActiveKind;
extern bool SelectionSupported;
} // namespace ludus::graphics::rhi::backend
namespace
{
constexpr usize INPUT_COUNT = 6;
constexpr usize COMPOSITE_INPUT_COUNT = 1;
constexpr usize COMPOSITE_BINDING_COUNT = 2;
constexpr usize RESOURCE_COUNT = 2;
constexpr usize PASS_COUNT = 1;
constexpr usize VERTEX_COUNT = 3;
constexpr usize INDEX_COUNT = 3;
constexpr usize MESH_COUNT = 1;
constexpr usize ITEM_COUNT = 1;
constexpr usize CAMERA_COUNT = 1;
constexpr usize UNIFORM_VALUE_COUNT = 4;
constexpr usize UNIFORM_BLOCK_COUNT = 2;
constexpr usize UNIFORM_FIELD_COUNT = 4;
constexpr usize WGSL_MATERIAL_BYTES = 16;
constexpr usize METAL_MATERIAL_BYTES = 32;
struct GraphFixture final
{
    rhi::DeviceHandle Device{};
    rhi::SurfaceHandle Surface{};
    renderer::RenderGraph Graph;
    StaticArray<format::Resource, RESOURCE_COUNT> Resources{
        {{96, 64, format::Aspect::Color, 0, 0, 1}, {96, 64, format::Aspect::Depth, 0, 0, 0}}};
    StaticArray<format::Pass, PASS_COUNT> Passes{{{format::Technique::ColorAndDepth,
                                                   0,
                                                   0,
                                                   1,
                                                   format::Load::Clear,
                                                   format::Store::Store,
                                                   format::Load::Clear,
                                                   format::Store::Store,
                                                   0}}};
    StaticArray<format::Camera, CAMERA_COUNT> Targets{{{0}}};
    /// Material {Float3 tint; Float32 gain} read by the opaque color fragment at slot 0.
    StaticArray<format::UniformBinding, 1> UniformBindings{
        {{format::Technique::ColorAndDepth, 0, format::Visibility::Fragment, 0, UNIFORM_VALUE_COUNT}}};
    StaticArray<format::UniformBlock, UNIFORM_BLOCK_COUNT> UniformBlocks{
        {{0, format::ShaderTarget::Wgsl, WGSL_MATERIAL_BYTES}, {0, format::ShaderTarget::Metal, METAL_MATERIAL_BYTES}}};
    StaticArray<format::UniformField, UNIFORM_FIELD_COUNT> UniformFields{{{0, format::ShaderTarget::Wgsl, 0, 3, 0},
                                                                          {0, format::ShaderTarget::Wgsl, 3, 1, 12},
                                                                          {0, format::ShaderTarget::Metal, 0, 3, 0},
                                                                          {0, format::ShaderTarget::Metal, 3, 1, 16}}};
    StaticArray<rhi::RasterBinding, 1> MaterialBindings{
        {{0, rhi::RasterBindingKind::UniformBuffer, rhi::RasterVisibility::Fragment, METAL_MATERIAL_BYTES}}};
    bool IsBound = false;
    StaticArray<rhi::RasterShaderInput, INPUT_COUNT> Inputs{};
    StaticArray<rhi::RasterShaderInput, COMPOSITE_INPUT_COUNT> CompositeInputs{{{0, rhi::RasterVertexFormat::Float4}}};
    StaticArray<rhi::RasterBinding, COMPOSITE_BINDING_COUNT> CompositeBindings{
        {{0, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
         {1, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0}}};
    StaticArray<math::Vector3, VERTEX_COUNT> Positions{{{-.1F, -.1F, .5F}, {.1F, -.1F, .5F}, {0, .1F, .5F}}};
    StaticArray<uint32, INDEX_COUNT> Indices{{0, 1, 2}};
    StaticArray<renderer::RenderGraphMesh, MESH_COUNT> Meshes{};
    StaticArray<renderer::RenderGraphItem, ITEM_COUNT> Items{};
    StaticArray<renderer::ViewDescription, CAMERA_COUNT> Cameras{};
    explicit GraphFixture(bool hasRasterProfile = true)
    {
        rhi::Shutdown();
        rhi::reference::Reset();
        rhi::reference::ProfileAvailable = hasRasterProfile;
        rhi::backend::ImmediateError = rhi::StartupError::None;
        rhi::backend::NextFrame = rhi::FrameStatus::Ready;
        rhi::backend::ActiveKind = rhi::Backend::WebGPU;
        rhi::backend::SelectionSupported = true;
        REQUIRE(rhi::CreateDevice({}, {}, {}, Device, Surface) == rhi::DeviceStatus::Pending);
        rhi::internal::Complete(rhi::backend::PendingToken, rhi::StartupError::None, {4096, 16384});
        for (uint32 i = 0; i < INPUT_COUNT; ++i)
        {
            Inputs[i] = {i, i == 0 ? rhi::RasterVertexFormat::Float3 : rhi::RasterVertexFormat::Float4};
        }
        Meshes[0].Geometry.Positions = Positions.GetData();
        Meshes[0].Geometry.VertexCount = Positions.GetSize();
        Meshes[0].Geometry.Indices = Indices.GetData();
        Meshes[0].Geometry.IndexCount = Indices.GetSize();
        Items[0].SourceId = 42;
        Items[0].Material.Color = {1, 1, 1, 1};
    }
    ~GraphFixture()
    {
        Graph.Reset();
        rhi::Shutdown();
    }
    [[nodiscard]] LUDUS_INLINE renderer::RenderGraphPlan MakePlan() const noexcept
    {
        if (IsBound == false)
        {
            return {{format::VERSION, RESOURCE_COUNT, PASS_COUNT, CAMERA_COUNT, 0, 0, 0, 0},
                    {Resources.GetData(), Resources.GetSize()},
                    {Targets.GetData(), Targets.GetSize()},
                    {Passes.GetData(), Passes.GetSize()},
                    {},
                    {},
                    {}};
        }
        return {{format::VERSION,
                 RESOURCE_COUNT,
                 PASS_COUNT,
                 CAMERA_COUNT,
                 UNIFORM_VALUE_COUNT,
                 1,
                 UNIFORM_BLOCK_COUNT,
                 UNIFORM_FIELD_COUNT},
                {Resources.GetData(), Resources.GetSize()},
                {Targets.GetData(), Targets.GetSize()},
                {Passes.GetData(), Passes.GetSize()},
                {UniformBindings.GetData(), UniformBindings.GetSize()},
                {UniformBlocks.GetData(), UniformBlocks.GetSize()},
                {UniformFields.GetData(), UniformFields.GetSize()}};
    }
    [[nodiscard]] renderer::RenderGraphPrograms MakePrograms() const
    {
        renderer::RenderGraphPrograms programs;
        programs.ColorVertex.Artifact.Stage = rhi::ShaderStage::Vertex;
        programs.ColorVertex.Artifact.Wgsl = "flat vertex fixture";
        programs.ColorVertex.Artifact.WgslEntry = "VertexMain";
        programs.ColorVertex.Inputs = Inputs.AsSpan();
        programs.ColorFragment.Artifact.Stage = rhi::ShaderStage::Fragment;
        programs.ColorFragment.Artifact.Wgsl = "flat fragment fixture";
        programs.ColorFragment.Artifact.WgslEntry = "FragmentMain";
        programs.DepthVertex = programs.ColorVertex;
        programs.DepthFragment = programs.ColorFragment;
        // The masked family has its own authored stages; its depth entry is a distinct coverage function.
        programs.MaskedColorVertex = programs.ColorVertex;
        programs.MaskedColorFragment = programs.ColorFragment;
        programs.MaskedColorFragment.Artifact.Wgsl = "masked color fixture";
        programs.MaskedDepthVertex = programs.ColorVertex;
        programs.MaskedDepthFragment = programs.ColorFragment;
        programs.MaskedDepthFragment.Artifact.Wgsl = "masked coverage fixture";
        if (IsBound == true)
        {
            programs.ColorFragment.Bindings = MaterialBindings.AsSpan();
        }
        programs.CompositeVertex.Artifact.Stage = rhi::ShaderStage::Vertex;
        programs.CompositeVertex.Artifact.Wgsl = "composite vertex fixture";
        programs.CompositeVertex.Artifact.WgslEntry = "VertexMain";
        programs.CompositeVertex.Inputs = CompositeInputs.AsSpan();
        programs.CompositeFragment.Artifact.Stage = rhi::ShaderStage::Fragment;
        programs.CompositeFragment.Artifact.Wgsl = "composite fragment fixture";
        programs.CompositeFragment.Artifact.WgslEntry = "FragmentMain";
        programs.CompositeFragment.Bindings = CompositeBindings.AsSpan();
        return programs;
    }
    rhi::RasterStatus Initialize()
    {
        return Graph.Initialize(Device, MakePlan(), MakePrograms());
    }
    rhi::RasterStatus
    Update(renderer::RenderGraphReport& outReport,
           renderer::RenderGraphCoveragePolicy policy = renderer::RenderGraphCoveragePolicy::RejectIncomplete,
           uint64 revision = 1)
    {
        return Graph.Update({Meshes.GetData(), Meshes.GetSize()},
                            {Items.GetData(), Items.GetSize()},
                            {Cameras.GetData(), Cameras.GetSize()},
                            revision,
                            policy,
                            outReport);
    }
};
} // namespace
TEST_CASE("RG2 renders a retained extraction without creating shaders or pipelines during draw", "[render-graph]")
{
    GraphFixture fixture;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    CHECK(report.Covered == 2);
    CHECK(report.IsReady == true);
    const usize shaderCreates = rhi::reference::Creates[static_cast<usize>(rhi::internal::RasterKind::Shader)];
    const usize pipelineCreates = rhi::reference::Creates[static_cast<usize>(rhi::internal::RasterKind::Pipeline)];
    rhi::SubmissionToken completion;
    REQUIRE(fixture.Graph.Render(fixture.Surface, 96, 64, completion) == rhi::RasterStatus::Ready);
    CHECK(completion.IsNull() == false);
    CHECK(rhi::reference::Draws == 1);
    CHECK(rhi::reference::Creates[static_cast<usize>(rhi::internal::RasterKind::Shader)] == shaderCreates);
    CHECK(rhi::reference::Creates[static_cast<usize>(rhi::internal::RasterKind::Pipeline)] == pipelineCreates);
    rhi::TextureHandle output;
    rhi::TextureAspect aspect;
    CHECK(fixture.Graph.GetExport(0, output, aspect) == rhi::RasterStatus::NotReady);
    rhi::reference::Completed = rhi::reference::Submitted;
    REQUIRE(fixture.Graph.GetExport(0, output, aspect) == rhi::RasterStatus::Ready);
    CHECK(aspect == rhi::TextureAspect::Color);
}
TEST_CASE("RG2 diagnostic allocation access admits transient depth after producer completion", "[render-graph]")
{
    GraphFixture fixture;
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect = rhi::TextureAspect::Color;
    CHECK(fixture.Graph.GetResourceAllocation(0, texture, aspect) == rhi::RasterStatus::InvalidDescription);
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetResourceAllocation(RESOURCE_COUNT, texture, aspect) ==
          rhi::RasterStatus::InvalidDescription);
    // No completed frame yet: nothing to inspect.
    CHECK(fixture.Graph.GetResourceAllocation(1, texture, aspect) == rhi::RasterStatus::NotReady);
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(fixture.Graph.Render(fixture.Surface, 96, 64, completion) == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetResourceAllocation(1, texture, aspect) == rhi::RasterStatus::NotReady);
    rhi::reference::Completed = rhi::reference::Submitted;
    // The unexported depth resource is GetExport-invisible but inspectable, on its backing's allocation.
    CHECK(fixture.Graph.GetExport(1, texture, aspect) == rhi::RasterStatus::InvalidDescription);
    rhi::TextureHandle exported;
    REQUIRE(fixture.Graph.GetExport(0, exported, aspect) == rhi::RasterStatus::Ready);
    REQUIRE(fixture.Graph.GetResourceAllocation(1, texture, aspect) == rhi::RasterStatus::Ready);
    CHECK(aspect == rhi::TextureAspect::Depth);
    REQUIRE(fixture.Graph.GetResourceAllocation(0, texture, aspect) == rhi::RasterStatus::Ready);
    CHECK(aspect == rhi::TextureAspect::Color);
}
TEST_CASE("RG2 unsupported material coverage retains the previous drawable scene", "[render-graph]")
{
    GraphFixture fixture;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    fixture.Items[0].Coverage = renderer::RenderGraphMaterial::Masked;
    CHECK(fixture.Update(report, renderer::RenderGraphCoveragePolicy::RejectIncomplete, 2) ==
          rhi::RasterStatus::Unsupported);
    CHECK(report.Unsupported == 2);
    CHECK(report.IsAccepted == false);
    CHECK(fixture.Graph.GetCoverage()[0].Reason == renderer::RenderGraphReason::Material);
    rhi::SubmissionToken completion;
    REQUIRE(fixture.Graph.Render(fixture.Surface, 96, 64, completion) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Draws == 1);
}
TEST_CASE("RG2 diagnostic color renders without claiming required depth coverage", "[render-graph]")
{
    GraphFixture fixture;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    fixture.Items[0].Coverage = renderer::RenderGraphMaterial::Unsupported;
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report, renderer::RenderGraphCoveragePolicy::DiagnosticColor) == rhi::RasterStatus::Ready);
    CHECK(report.Covered == 0);
    CHECK(report.Unsupported == 2);
    const BorrowedView<const renderer::RenderGraphCoverageRecord> records = fixture.Graph.GetCoverage();
    CHECK(records[0].HasDiagnosticRoute == true);
    CHECK(records[1].HasDiagnosticRoute == false);
    rhi::SubmissionToken completion;
    REQUIRE(fixture.Graph.Render(fixture.Surface, 96, 64, completion) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Draws == 1);
}
TEST_CASE("RG2 authored exclusion is different from unsupported or culled geometry", "[render-graph]")
{
    GraphFixture fixture;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    fixture.Items[0].IsIntentionallyExcluded = true;
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    CHECK(report.Excluded == 2);
    CHECK(report.Covered == 0);
    CHECK(report.Unsupported == 0);
    rhi::SubmissionToken completion;
    REQUIRE(fixture.Graph.Render(fixture.Surface, 96, 64, completion) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Draws == 0);
}
TEST_CASE("RG2 rejects undefined exported depth and malformed numeric plans before GPU setup", "[render-graph]")
{
    GraphFixture fixture;
    fixture.Resources[1].IsExported = 1;
    fixture.Targets[0].Target = 1;
    fixture.Passes[0].DepthStore = format::Store::Discard;
    CHECK(fixture.Initialize() == rhi::RasterStatus::InvalidDescription);
    fixture.Passes[0].DepthStore = format::Store::Store;
    fixture.Resources[1].Backing = 31;
    CHECK(fixture.Initialize() == rhi::RasterStatus::InvalidDescription);
}
TEST_CASE("RG2 pending and failed replacements retain the last drawable extraction", "[render-graph]")
{
    GraphFixture fixture;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    rhi::reference::Next = rhi::RasterStatus::Pending;
    fixture.Items[0].MeshToWorld.Translation.X = 100;
    REQUIRE(fixture.Update(report, renderer::RenderGraphCoveragePolicy::RejectIncomplete, 2) ==
            rhi::RasterStatus::Pending);
    CHECK(report.IsAccepted == true);
    CHECK(report.IsReady == false);
    rhi::SubmissionToken completion;
    REQUIRE(fixture.Graph.Render(fixture.Surface, 96, 64, completion) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Draws == 1);
    for (usize i = 0; i < rhi::reference::PendingCount; ++i)
    {
        rhi::internal::RasterComplete(rhi::reference::PendingRequests[i], rhi::RasterStatus::Failed);
    }
    rhi::reference::Next = rhi::RasterStatus::Ready;
    CHECK(fixture.Graph.Poll() == rhi::RasterStatus::Failed);
    rhi::reference::Completed = rhi::reference::Submitted;
    completion = {};
    REQUIRE(fixture.Graph.Render(fixture.Surface, 96, 64, completion) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Draws == 2);
}
TEST_CASE("RG2 camera edits and streaming source changes publish only after readiness", "[render-graph]")
{
    GraphFixture fixture;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    rhi::reference::Next = rhi::RasterStatus::Pending;
    fixture.Items[0].MeshToWorld.Translation.X = 100;
    REQUIRE(fixture.Update(report, renderer::RenderGraphCoveragePolicy::RejectIncomplete, 2) ==
            rhi::RasterStatus::Pending);
    for (usize i = 0; i < rhi::reference::PendingCount; ++i)
    {
        rhi::internal::RasterComplete(rhi::reference::PendingRequests[i], rhi::RasterStatus::Ready);
    }
    rhi::reference::Next = rhi::RasterStatus::Ready;
    REQUIRE(fixture.Graph.Poll() == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(fixture.Graph.Render(fixture.Surface, 96, 64, completion) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Draws == 0);
    fixture.Items[0].MeshToWorld.Translation.X = 0;
    REQUIRE(fixture.Update(report, renderer::RenderGraphCoveragePolicy::RejectIncomplete, 3) ==
            rhi::RasterStatus::Ready);
    rhi::reference::Completed = rhi::reference::Submitted;
    completion = {};
    REQUIRE(fixture.Graph.Render(fixture.Surface, 96, 64, completion) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Draws == 1);
}
TEST_CASE("RG2 unmatched layouts can use explicit bounds proxies without reading unknown vertex data", "[render-graph]")
{
    GraphFixture fixture;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    fixture.Meshes[0].Layout = renderer::RenderGraphLayout::Unsupported;
    fixture.Meshes[0].Geometry = {};
    fixture.Meshes[0].HasDiagnosticBounds = true;
    fixture.Meshes[0].BoundsMesh = {{-.1F, -.1F, .4F}, {.1F, .1F, .6F}};
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report, renderer::RenderGraphCoveragePolicy::DiagnosticColor) == rhi::RasterStatus::Ready);
    CHECK(report.Unsupported == 2);
    CHECK(fixture.Graph.GetCoverage()[0].Reason == renderer::RenderGraphReason::Layout);
    rhi::SubmissionToken completion;
    REQUIRE(fixture.Graph.Render(fixture.Surface, 96, 64, completion) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Draws == 1);
}
TEST_CASE("RG2 duplicate draw-item identities are conflicting even under diagnostic policy", "[render-graph]")
{
    GraphFixture fixture;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    constexpr usize DUPLICATE_COUNT = 2;
    StaticArray<renderer::RenderGraphItem, DUPLICATE_COUNT> items{{fixture.Items[0], fixture.Items[0]}};
    renderer::RenderGraphReport report;
    CHECK(fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                               {items.GetData(), items.GetSize()},
                               {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                               1,
                               renderer::RenderGraphCoveragePolicy::DiagnosticColor,
                               report) == rhi::RasterStatus::Unsupported);
    CHECK(report.Conflicting == 4);
    CHECK(report.IsAccepted == false);
}
TEST_CASE("RG2 standalone depth output is authored, initialized and completion gated", "[render-graph]")
{
    GraphFixture fixture;
    fixture.Resources[0].IsExported = 0;
    fixture.Resources[1].IsExported = 1;
    fixture.Targets[0].Target = 1;
    fixture.Passes[0].Program = format::Technique::DepthOnly;
    fixture.Passes[0].Color = format::ABSENT;
    fixture.Passes[0].ColorLoad = format::Load::Discard;
    fixture.Passes[0].ColorStore = format::Store::Discard;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    CHECK(report.Covered == 1);
    rhi::SubmissionToken completion;
    REQUIRE(fixture.Graph.Render(fixture.Surface, 96, 64, completion) == rhi::RasterStatus::Ready);
    rhi::reference::Completed = rhi::reference::Submitted;
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect;
    REQUIRE(fixture.Graph.GetExport(1, texture, aspect) == rhi::RasterStatus::Ready);
    CHECK(aspect == rhi::TextureAspect::Depth);
    rhi::RasterTextureState textureState;
    REQUIRE(rhi::GetTextureState(fixture.Device, texture, textureState) == rhi::RasterStatus::Ready);
    CHECK(textureState.DepthDefined == true);
}
TEST_CASE("RG2 rejects active cameras with no route, independent depth backing and unsupported interfaces",
          "[render-graph]")
{
    GraphFixture fixture;
    renderer::RenderGraphPlan plan = fixture.MakePlan();
    plan.Description.CameraCount = 2;
    CHECK(fixture.Graph.Initialize(fixture.Device, plan, fixture.MakePrograms()) ==
          rhi::RasterStatus::InvalidDescription);
    fixture.Resources[1].Backing = 1;
    CHECK(fixture.Initialize() == rhi::RasterStatus::Unsupported);
    fixture.Resources[1].Backing = 0;
    renderer::RenderGraphPrograms programs = fixture.MakePrograms();
    programs.ColorVertex.Inputs = {};
    CHECK(fixture.Graph.Initialize(fixture.Device, fixture.MakePlan(), programs) == rhi::RasterStatus::Unsupported);
}
TEST_CASE("RG2 a rejected newer report stays distinct from an older pending publication", "[render-graph]")
{
    GraphFixture fixture;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == 1);
    rhi::reference::Next = rhi::RasterStatus::Pending;
    REQUIRE(fixture.Update(report, renderer::RenderGraphCoveragePolicy::RejectIncomplete, 2) ==
            rhi::RasterStatus::Pending);
    fixture.Items[0].Coverage = renderer::RenderGraphMaterial::Masked;
    REQUIRE(fixture.Update(report, renderer::RenderGraphCoveragePolicy::RejectIncomplete, 3) ==
            rhi::RasterStatus::Unsupported);
    for (usize i = 0; i < rhi::reference::PendingCount; ++i)
    {
        rhi::internal::RasterComplete(rhi::reference::PendingRequests[i], rhi::RasterStatus::Ready);
    }
    rhi::reference::Next = rhi::RasterStatus::Ready;
    REQUIRE(fixture.Graph.Poll() == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == 2);
    CHECK(fixture.Graph.GetReport().SceneRevision == 3);
    CHECK(fixture.Graph.GetReport().IsAccepted == false);
    CHECK(fixture.Graph.GetReport().IsReady == false);
}
TEST_CASE("RG2 resize preflight rejects before frame acquisition and keeps the drawable revision", "[render-graph]")
{
    GraphFixture fixture;
    fixture.Resources[0].IsSurface = 1;
    fixture.Resources[0].IsExported = 0;
    fixture.Resources[1].IsSurface = 1;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    CHECK(fixture.Graph.Render(fixture.Surface, 192, 128, completion) == rhi::RasterStatus::NotReady);
    CHECK(completion.IsNull() == true);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == 1);
    rhi::DeviceInfo information;
    REQUIRE(rhi::GetDeviceInfo(fixture.Device, information) == rhi::DeviceStatus::Ready);
    CHECK(information.FrameOpen == false);
    REQUIRE(fixture.Graph.Render(fixture.Surface, 96, 64, completion) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Draws == 1);
}
TEST_CASE("RG2 culling never hides incompatible streamed material slots", "[render-graph]")
{
    GraphFixture fixture;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    constexpr usize MIXED_ITEM_COUNT = 2;
    StaticArray<renderer::RenderGraphItem, MIXED_ITEM_COUNT> items{{fixture.Items[0], fixture.Items[0]}};
    items[1].SourceId = 43;
    items[1].Coverage = renderer::RenderGraphMaterial::Transparent;
    items[1].MeshToWorld.Translation.X = 100;
    renderer::RenderGraphReport report;
    CHECK(fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                               {items.GetData(), items.GetSize()},
                               {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                               1,
                               renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                               report) == rhi::RasterStatus::Unsupported);
    CHECK(report.Covered == 2);
    CHECK(report.Unsupported == 2);
    CHECK(fixture.Graph.GetCoverage().GetSize() == 4);
}
TEST_CASE("RG2 required camera target cannot be replaced by an unrelated export", "[render-graph]")
{
    GraphFixture fixture;
    fixture.Resources[1].IsExported = 1;
    fixture.Targets[0].Target = 0;
    fixture.Passes[0].Program = format::Technique::DepthOnly;
    fixture.Passes[0].Color = format::ABSENT;
    fixture.Passes[0].ColorLoad = format::Load::Discard;
    fixture.Passes[0].ColorStore = format::Store::Discard;
    fixture.Resources[0].IsExported = 0;
    CHECK(fixture.Initialize() == rhi::RasterStatus::InvalidDescription);
    fixture.Targets[0].Target = 1;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
}

TEST_CASE("RG2 device capability rejection happens before shader or pipeline creation", "[render-graph]")
{
    GraphFixture fixture{false};
    CHECK(fixture.Initialize() == rhi::RasterStatus::Unsupported);
    CHECK(rhi::reference::Creates[static_cast<usize>(rhi::internal::RasterKind::Shader)] == 0);
    CHECK(rhi::reference::Creates[static_cast<usize>(rhi::internal::RasterKind::Pipeline)] == 0);
}

TEST_CASE("RG2 ready candidates never relabel prior exported pixels", "[render-graph]")
{
    GraphFixture fixture;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(fixture.Graph.Render(fixture.Surface, 96, 64, completion) == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetOutputSceneRevision() == 0);
    rhi::reference::Completed = rhi::reference::Submitted;
    CHECK(fixture.Graph.GetOutputSceneRevision() == 1);
    fixture.Items[0].MeshToWorld.Translation.X = 100;
    REQUIRE(fixture.Update(report, renderer::RenderGraphCoveragePolicy::RejectIncomplete, 2) ==
            rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetReport().SceneRevision == 2);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == 2);
    CHECK(fixture.Graph.GetOutputSceneRevision() == 1);
    completion = {};
    REQUIRE(fixture.Graph.Render(fixture.Surface, 96, 64, completion) == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetOutputSceneRevision() == 0);
    rhi::reference::Completed = rhi::reference::Submitted;
    CHECK(fixture.Graph.GetOutputSceneRevision() == 2);
}

TEST_CASE("RG2 invalid surfaces report ownership failure without pretending preparation is pending", "[render-graph]")
{
    GraphFixture fixture;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    CHECK(fixture.Graph.Render({}, 96, 64, completion) == rhi::RasterStatus::InvalidHandle);
    CHECK(completion.IsNull() == true);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == 1);
    REQUIRE(fixture.Graph.Render(fixture.Surface, 96, 64, completion) == rhi::RasterStatus::Ready);
}

TEST_CASE("RG2 offscreen encoding shares one caller-owned frame and tracks its actual producer", "[render-graph]")
{
    GraphFixture fixture;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect;
    REQUIRE(fixture.Graph.GetExportAllocation(0, texture, aspect) == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetExport(0, texture, aspect) == rhi::RasterStatus::NotReady);
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.Encode(fixture.Surface) == rhi::RasterStatus::InvalidState);
    REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(fixture.Graph.Encode(fixture.Surface) == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.HasUnconfirmedEncoding() == true);
    CHECK(fixture.Graph.Poll() == rhi::RasterStatus::InvalidState);
    CHECK(fixture.Graph.GetOutputSceneRevision() == 0);
    CHECK(fixture.Graph.Encode(fixture.Surface) == rhi::RasterStatus::InvalidState);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(fixture.Device, completion) == rhi::RasterStatus::Ready);
    CHECK(fixture.Update(report) == rhi::RasterStatus::InvalidState);
    REQUIRE(fixture.Graph.ConfirmSubmission(completion) == rhi::RasterStatus::Pending);
    CHECK(fixture.Graph.HasUnconfirmedEncoding() == false);
    CHECK(fixture.Graph.GetOutputSceneRevision() == 0);
    rhi::reference::Completed = rhi::reference::Submitted;
    CHECK(fixture.Graph.GetOutputSceneRevision() == 1);
    REQUIRE(fixture.Graph.GetExport(0, texture, aspect) == rhi::RasterStatus::Ready);
}
TEST_CASE("RG2 untracked external submission invalidates output without replacing its drawable scene", "[render-graph]")
{
    GraphFixture fixture;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(fixture.Graph.Encode(fixture.Surface) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::EndFrame(fixture.Device) == rhi::DeviceStatus::Ready);
    CHECK(fixture.Graph.ConfirmSubmission({}) == rhi::RasterStatus::Failed);
    CHECK(fixture.Graph.GetOutputSceneRevision() == 0);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == 1);
}
TEST_CASE("RG2 independent logical depths cannot silently share one private allocation", "[render-graph]")
{
    GraphFixture fixture;
    constexpr usize ALIASED_RESOURCE_COUNT = 3;
    StaticArray<format::Resource, ALIASED_RESOURCE_COUNT> resources{
        {fixture.Resources[0], fixture.Resources[1], fixture.Resources[1]}};
    resources[2].IsExported = 1;
    renderer::RenderGraphPlan plan = fixture.MakePlan();
    plan.Description.ResourceCount = ALIASED_RESOURCE_COUNT;
    plan.Resources = {resources.GetData(), resources.GetSize()};
    CHECK(fixture.Graph.Initialize(fixture.Device, plan, fixture.MakePrograms()) == rhi::RasterStatus::Unsupported);
    CHECK(rhi::reference::Creates[static_cast<usize>(rhi::internal::RasterKind::Shader)] == 0);
}
TEST_CASE("RG2 excluded items do not require their unneeded geometry uploads", "[render-graph]")
{
    GraphFixture fixture;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    fixture.Meshes[0].Geometry = {};
    fixture.Items[0].IsIntentionallyExcluded = true;
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    CHECK(report.Excluded == 2);
    rhi::SubmissionToken completion;
    REQUIRE(fixture.Graph.Render(fixture.Surface, 96, 64, completion) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Draws == 0);
}
TEST_CASE("RG2 bound techniques pack per-draw uniforms and share equal value sets", "[render-graph]")
{
    GraphFixture fixture;
    fixture.IsBound = true;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    constexpr usize BOUND_ITEM_COUNT = 3;
    StaticArray<float32, UNIFORM_VALUE_COUNT> red{{1, 0, 0, 1}};
    StaticArray<float32, UNIFORM_VALUE_COUNT> blue{{0, 0, 1, .5F}};
    StaticArray<renderer::RenderGraphItem, BOUND_ITEM_COUNT> items{
        {fixture.Items[0], fixture.Items[0], fixture.Items[0]}};
    for (usize i = 0; i < BOUND_ITEM_COUNT; ++i)
    {
        items[i].SourceId = 42 + i;
        items[i].Uniforms = {(i == 2 ? blue : red).GetData(), UNIFORM_VALUE_COUNT};
    }
    const usize buffers = rhi::reference::Creates[static_cast<usize>(rhi::internal::RasterKind::Buffer)];
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                 {items.GetData(), items.GetSize()},
                                 {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                 1,
                                 renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                 report) == rhi::RasterStatus::Ready);
    CHECK(report.Covered == 2 * BOUND_ITEM_COUNT);
    // Mesh vertices/indices, two distinct uniform sets and one instance buffer per prepared view.
    CHECK(rhi::reference::Creates[static_cast<usize>(rhi::internal::RasterKind::Buffer)] == buffers + 2 + 2 + 1);
    const usize shaderCreates = rhi::reference::Creates[static_cast<usize>(rhi::internal::RasterKind::Shader)];
    rhi::SubmissionToken completion;
    REQUIRE(fixture.Graph.Render(fixture.Surface, 96, 64, completion) == rhi::RasterStatus::Ready);
    // Equal values stay one instanced draw; the distinct set is a second packet, never a new shader.
    CHECK(rhi::reference::Draws == 2);
    CHECK(rhi::reference::Creates[static_cast<usize>(rhi::internal::RasterKind::Shader)] == shaderCreates);
}
TEST_CASE("RG2 bound uniform values pack the selected target's reflected layout", "[render-graph]")
{
    GraphFixture fixture;
    fixture.IsBound = true;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    StaticArray<float32, UNIFORM_VALUE_COUNT> values{{.25F, .5F, .75F, 2}};
    fixture.Items[0].Uniforms = {values.GetData(), values.GetSize()};
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    // The reference device is WebGPU: the gain sits at byte 12 of the WGSL block, not Metal's 16.
    bool hasPackedBlock = false;
    for (usize slot = 0; slot < rhi::reference::BufferBytes.GetSize(); ++slot)
    {
        const Array<uint8>& bytes = rhi::reference::BufferBytes[slot];
        if (bytes.GetSize() < WGSL_MATERIAL_BYTES)
        {
            continue;
        }
        StaticArray<float32, UNIFORM_VALUE_COUNT> decoded{};
        for (usize value = 0; value < UNIFORM_VALUE_COUNT; ++value)
        {
            uint8* destination = reinterpret_cast<uint8*>(&decoded[value]);
            for (usize byte = 0; byte < sizeof(float32); ++byte)
            {
                destination[byte] = bytes[value * sizeof(float32) + byte];
            }
        }
        hasPackedBlock = hasPackedBlock == true ||
                         (decoded[0] == .25F && decoded[1] == .5F && decoded[2] == .75F && decoded[3] == 2);
    }
    CHECK(hasPackedBlock == true);
}
TEST_CASE("RG2 bound uniform admission rejects incomplete values and mismatched interfaces", "[render-graph]")
{
    GraphFixture fixture;
    fixture.IsBound = true;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    renderer::RenderGraphReport report;
    // Missing, short and nonfinite values reject before any preparation.
    CHECK(fixture.Update(report) == rhi::RasterStatus::InvalidDescription);
    StaticArray<float32, UNIFORM_VALUE_COUNT> values{{1, 1, 1, 1}};
    fixture.Items[0].Uniforms = {values.GetData(), UNIFORM_VALUE_COUNT - 1};
    CHECK(fixture.Update(report) == rhi::RasterStatus::InvalidDescription);
    values[1] = std::numeric_limits<float32>::infinity();
    fixture.Items[0].Uniforms = {values.GetData(), values.GetSize()};
    CHECK(fixture.Update(report) == rhi::RasterStatus::InvalidDescription);
    // An excluded item may omit its values.
    fixture.Items[0].Uniforms = {};
    fixture.Items[0].IsIntentionallyExcluded = true;
    CHECK(fixture.Update(report) == rhi::RasterStatus::Ready);
    fixture.Graph.Reset();
    // A stage range that differs from the plan's largest carried block is a different interface.
    fixture.MaterialBindings[0].MinSize = WGSL_MATERIAL_BYTES;
    CHECK(fixture.Initialize() == rhi::RasterStatus::Unsupported);
    fixture.MaterialBindings[0].MinSize = METAL_MATERIAL_BYTES;
    // Values supplied to an unbound plan are not silently ignored.
    GraphFixture unbound;
    REQUIRE(unbound.Initialize() == rhi::RasterStatus::Ready);
    unbound.Items[0].Uniforms = {values.GetData(), values.GetSize()};
    values[1] = 1;
    CHECK(unbound.Update(report) == rhi::RasterStatus::InvalidDescription);
}
TEST_CASE("RG2 bound plans reject malformed or incomplete target layouts before GPU setup", "[render-graph]")
{
    GraphFixture fixture;
    fixture.IsBound = true;
    // A binding for a technique no pass uses is not admitted.
    fixture.UniformBindings[0].Program = format::Technique::DepthOnly;
    CHECK(fixture.Initialize() == rhi::RasterStatus::InvalidDescription);
    fixture.UniformBindings[0].Program = format::Technique::ColorAndDepth;
    // Unused capacity may have a zero stage; an active record must never publish it.
    fixture.UniformBindings[0].Stages = static_cast<format::Visibility>(0);
    CHECK(fixture.Initialize() == rhi::RasterStatus::InvalidDescription);
    fixture.UniformBindings[0].Stages = format::Visibility::Fragment;
    // A field outside its block is malformed.
    fixture.UniformFields[1].ByteOffset = WGSL_MATERIAL_BYTES;
    CHECK(fixture.Initialize() == rhi::RasterStatus::InvalidDescription);
    fixture.UniformFields[1].ByteOffset = 12;
    CHECK(rhi::reference::Creates[static_cast<usize>(rhi::internal::RasterKind::Shader)] == 0);
    // The WebGPU device needs WGSL layout records; another target's offsets are never reinterpreted.
    fixture.UniformBlocks[0].Target = format::ShaderTarget::Spirv;
    fixture.UniformFields[0].Target = format::ShaderTarget::Spirv;
    fixture.UniformFields[1].Target = format::ShaderTarget::Spirv;
    CHECK(fixture.Initialize() == rhi::RasterStatus::Unsupported);
    CHECK(rhi::reference::Creates[static_cast<usize>(rhi::internal::RasterKind::Shader)] == 0);
}
TEST_CASE("RG2 bound techniques bound distinct value sets per extraction", "[render-graph]")
{
    GraphFixture fixture;
    fixture.IsBound = true;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    constexpr usize MANY_ITEM_COUNT = format::MAX_UNIFORM_SETS + 1;
    StaticArray<StaticArray<float32, UNIFORM_VALUE_COUNT>, MANY_ITEM_COUNT> values{};
    StaticArray<renderer::RenderGraphItem, MANY_ITEM_COUNT> items{};
    for (usize i = 0; i < MANY_ITEM_COUNT; ++i)
    {
        values[i] = {{static_cast<float32>(i) / MANY_ITEM_COUNT, 0, 0, 1}};
        items[i] = fixture.Items[0];
        items[i].SourceId = 100 + i;
        items[i].Uniforms = {values[i].GetData(), UNIFORM_VALUE_COUNT};
    }
    renderer::RenderGraphReport report;
    CHECK(fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                               {items.GetData(), items.GetSize()},
                               {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                               1,
                               renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                               report) == rhi::RasterStatus::CapacityExceeded);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == 0);
}
TEST_CASE("RG2 an explicitly authored masked technique covers masked items only", "[render-graph]")
{
    GraphFixture fixture;
    fixture.Passes[0].Program = format::Technique::MaskedColorAndDepth;
    REQUIRE(fixture.Initialize() == rhi::RasterStatus::Ready);
    fixture.Items[0].Coverage = renderer::RenderGraphMaterial::Masked;
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    CHECK(report.Covered == 2);
    rhi::SubmissionToken completion;
    REQUIRE(fixture.Graph.Render(fixture.Surface, 96, 64, completion) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Draws == 1);
    // Opaque and transparent items need their own route; masked coverage is not a substitute.
    for (const renderer::RenderGraphMaterial material :
         {renderer::RenderGraphMaterial::Opaque, renderer::RenderGraphMaterial::Transparent})
    {
        fixture.Items[0].Coverage = material;
        CHECK(fixture.Update(report, renderer::RenderGraphCoveragePolicy::RejectIncomplete, 2) ==
              rhi::RasterStatus::Unsupported);
        CHECK(report.Unsupported == 2);
        CHECK(fixture.Graph.GetCoverage()[0].Reason == renderer::RenderGraphReason::Material);
    }
    // A masked-only graph has no flat-color context for diagnostic markers.
    fixture.Items[0].Coverage = renderer::RenderGraphMaterial::Opaque;
    REQUIRE(fixture.Update(report, renderer::RenderGraphCoveragePolicy::DiagnosticColor, 3) ==
            rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetCoverage()[0].HasDiagnosticRoute == false);
}
TEST_CASE("RG2 opaque and masked passes share a view without conflicting coverage", "[render-graph]")
{
    GraphFixture fixture;
    constexpr usize MIXED_PASS_COUNT = 2;
    StaticArray<format::Pass, MIXED_PASS_COUNT> passes{{fixture.Passes[0], fixture.Passes[0]}};
    passes[1].Program = format::Technique::MaskedColorAndDepth;
    passes[1].ColorLoad = format::Load::Load;
    passes[1].DepthLoad = format::Load::Load;
    renderer::RenderGraphPlan plan = fixture.MakePlan();
    plan.Description.PassCount = MIXED_PASS_COUNT;
    plan.Passes = {passes.GetData(), passes.GetSize()};
    REQUIRE(fixture.Graph.Initialize(fixture.Device, plan, fixture.MakePrograms()) == rhi::RasterStatus::Ready);
    constexpr usize MIXED_ITEM_COUNT = 2;
    StaticArray<renderer::RenderGraphItem, MIXED_ITEM_COUNT> items{{fixture.Items[0], fixture.Items[0]}};
    items[1].SourceId = 43;
    items[1].Coverage = renderer::RenderGraphMaterial::Masked;
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                 {items.GetData(), items.GetSize()},
                                 {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                 1,
                                 renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                 report) == rhi::RasterStatus::Ready);
    CHECK(report.Covered == 4);
    CHECK(report.Conflicting == 0);
    rhi::SubmissionToken completion;
    REQUIRE(fixture.Graph.Render(fixture.Surface, 96, 64, completion) == rhi::RasterStatus::Ready);
    // Each family draws only its own items.
    CHECK(rhi::reference::Draws == 2);
}
TEST_CASE("RG2 masked depth needs its own authored coverage function and matching geometry", "[render-graph]")
{
    GraphFixture fixture;
    constexpr usize DEPTH_PASS_COUNT = 2;
    StaticArray<format::Pass, DEPTH_PASS_COUNT> passes{{fixture.Passes[0], fixture.Passes[0]}};
    for (format::Pass& pass : passes)
    {
        pass.Color = format::ABSENT;
        pass.ColorLoad = format::Load::Discard;
        pass.ColorStore = format::Store::Discard;
    }
    passes[0].Program = format::Technique::DepthOnly;
    passes[1].Program = format::Technique::MaskedDepthOnly;
    passes[1].DepthLoad = format::Load::Load;
    fixture.Resources[0].IsExported = 0;
    fixture.Resources[1].IsExported = 1;
    fixture.Targets[0].Target = 1;
    renderer::RenderGraphPlan plan = fixture.MakePlan();
    plan.Description.PassCount = DEPTH_PASS_COUNT;
    plan.Passes = {passes.GetData(), passes.GetSize()};
    renderer::RenderGraphPrograms programs = fixture.MakePrograms();
    REQUIRE(fixture.Graph.Initialize(fixture.Device, plan, programs) == rhi::RasterStatus::Ready);
    fixture.Graph.Reset();
    // The opaque void depth function never stands in for masked coverage.
    programs.MaskedDepthFragment = programs.DepthFragment;
    CHECK(fixture.Graph.Initialize(fixture.Device, plan, programs) == rhi::RasterStatus::Unsupported);
    programs = fixture.MakePrograms();
    // A masked pre-Z needs the masked color technique's exact geometry stage.
    passes[1].Program = format::Technique::MaskedColorAndDepth;
    passes[1].Color = 0;
    passes[1].ColorLoad = format::Load::Clear;
    passes[1].ColorStore = format::Store::Store;
    passes[0].Program = format::Technique::MaskedDepthOnly;
    programs.MaskedDepthVertex.Artifact.Wgsl = "other geometry fixture";
    CHECK(fixture.Graph.Initialize(fixture.Device, plan, programs) == rhi::RasterStatus::Unsupported);
    CHECK(fixture.Graph.Initialize(fixture.Device, plan, fixture.MakePrograms()) == rhi::RasterStatus::Ready);
}
