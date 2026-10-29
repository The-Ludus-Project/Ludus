#include "../src/internal/reference_data.h"
#include "internal/lifecycle.h"
#include "reference_raster.h"
#include "resource_mesh.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <limits>
#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/overlays.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/viewport.hpp>
#include <ludus/graphics/renderer/views.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>
using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
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
struct Session
{
    rhi::DeviceHandle Device;
    rhi::SurfaceHandle Surface;
    rr::Renderer Renderer;
    rr::Mesh Mesh;
    explicit Session(bool hdrAvailable = true,
                     const rr::Limits& limits = {},
                     const rhi::StorageLimits& storage = {},
                     bool computeAvailable = true)
    {
        rhi::Shutdown();
        rhi::reference::Reset();
        rhi::reference::HdrAvailable = hdrAvailable;
        rhi::reference::ComputeAvailable = computeAvailable;
        rhi::backend::ImmediateError = rhi::StartupError::None;
        rhi::backend::NextFrame = rhi::FrameStatus::Ready;
        rhi::backend::ActiveKind = rhi::Backend::WebGPU;
        rhi::backend::SelectionSupported = true;
        rhi::DeviceDescription deviceDescription;
        deviceDescription.Storage = storage;
        REQUIRE(rhi::CreateDevice({}, {}, deviceDescription, Device, Surface) == rhi::DeviceStatus::Pending);
        rhi::internal::Complete(rhi::backend::PendingToken, rhi::StartupError::None, {4096, 16384});
        Initialize(Renderer, limits);
        const math::Vector3 positions[]{{-.1F, -.1F, .5F}, {.1F, -.1F, .5F}, {0, .1F, .5F}};
        const uint32 indices[]{0, 1, 2};
        REQUIRE(Renderer.CreateMesh({positions, 3, indices, 3}, Mesh) == rhi::RasterStatus::Ready);
    }
    void Initialize(rr::Renderer& renderer, const rr::Limits& limits = {})
    {
        REQUIRE(InitializeStatus(renderer, limits) == rhi::RasterStatus::Ready);
    }
    rhi::RasterStatus InitializeStatus(rr::Renderer& renderer, const rr::Limits& limits)
    {
        rhi::RasterShaderInput inputs[6];
        for (uint32 i = 0; i < 6; ++i)
        {
            inputs[i] = {i, i == 0 ? rhi::RasterVertexFormat::Float3 : rhi::RasterVertexFormat::Float4};
        }
        rhi::RasterShaderDescription vertex, fragment;
        vertex.Artifact.Stage = rhi::ShaderStage::Vertex;
        vertex.Artifact.Wgsl = "flat vertex fixture";
        vertex.Artifact.WgslEntry = "vertexMain";
        vertex.Inputs = inputs;
        fragment.Artifact.Stage = rhi::ShaderStage::Fragment;
        fragment.Artifact.Wgsl = "flat fragment fixture";
        fragment.Artifact.WgslEntry = "fragmentMain";
        return renderer.Initialize(Device, vertex, fragment, limits);
    }
    void EnableViews()
    {
        const rhi::RasterShaderInput inputs[]{{0, rhi::RasterVertexFormat::Float4}};
        const rhi::RasterBinding bindings[]{{0, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
                                            {1, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0}};
        rhi::RasterShaderDescription vertex, fragment;
        vertex.Artifact.Stage = rhi::ShaderStage::Vertex;
        vertex.Artifact.Wgsl = "composite vertex fixture";
        vertex.Artifact.WgslEntry = "vertexMain";
        vertex.Inputs = inputs;
        fragment.Artifact.Stage = rhi::ShaderStage::Fragment;
        fragment.Artifact.Wgsl = "composite fragment fixture";
        fragment.Artifact.WgslEntry = "fragmentMain";
        fragment.Bindings = bindings;
        REQUIRE(Renderer.InitializeViews(vertex, fragment) == rhi::RasterStatus::Ready);
    }
    rhi::RasterStatus EnableGrid()
    {
        rhi::RasterShaderInput inputs[12];
        for (uint32 i = 0; i < 12; ++i)
        {
            inputs[i] = {i, i == 0 ? rhi::RasterVertexFormat::Float3 : rhi::RasterVertexFormat::Float4};
        }
        rhi::RasterShaderDescription vertex, fragment;
        vertex.Artifact.Stage = rhi::ShaderStage::Vertex;
        vertex.Artifact.Wgsl = "grid vertex fixture";
        vertex.Artifact.WgslEntry = "vertexMain";
        vertex.Inputs = inputs;
        fragment.Artifact.Stage = rhi::ShaderStage::Fragment;
        fragment.Artifact.Wgsl = "grid fragment fixture";
        fragment.Artifact.WgslEntry = "fragmentMain";
        return Renderer.InitializeGrid(vertex, fragment);
    }
    ~Session()
    {
        Renderer.Reset();
        rhi::Shutdown();
    }
    [[nodiscard]] rr::SceneItem Item(uint64 id, math::Vector3 translation = {}) const
    {
        rr::SceneItem item;
        item.Geometry = Mesh;
        item.SourceId = id;
        item.Transform.Translation = translation;
        return item;
    }
};
} // namespace
TEST_CASE("Snapshots copy values and retain detached mesh incarnations through view and GPU leases", "[renderer]")
{
    Session session;
    auto item = session.Item(41);
    rr::Snapshot snapshot;
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    item.Transform.Translation.X = 100;
    item.Material.Color = {};
    const auto stale = session.Mesh;
    REQUIRE(session.Renderer.Release(session.Mesh) == rhi::RasterStatus::Ready);
    CHECK(session.Renderer.GetStatus(stale) == rhi::RasterStatus::InvalidHandle);
    rr::PreparedView view;
    rr::ViewReport report;
    REQUIRE(session.Renderer.PrepareView(snapshot, {}, view, report) == rhi::RasterStatus::Ready);
    CHECK(report.Visible == 1);
    CHECK(report.SourceIds[0] == 41);
    REQUIRE(session.Renderer.Release(snapshot) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(session.Renderer.Submit(view, session.Surface, completion) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(view) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Destroys[0] == 0);
    rhi::reference::Completed = rhi::reference::Submitted;
    REQUIRE(rhi::PollLifetime(session.Device) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Destroys[0] == 3);
}
TEST_CASE("Visibility retains tangent/sheared bounds and overlays keep painter order", "[renderer]")
{
    Session session;
    rr::SceneItem items[]{session.Item(1, {3, 0, 0}), session.Item(2, {1.1F, 0, 0}), session.Item(3), session.Item(4)};
    items[2].Pass = items[3].Pass = rr::Layer::Overlay;
    items[2].Material.Color = {1, 0, 0, .5F};
    items[3].Material.Color = {0, 1, 0, .5F};
    rr::Snapshot snapshot;
    REQUIRE(session.Renderer.CreateSnapshot(items, 4, snapshot) == rhi::RasterStatus::Ready);
    rr::PreparedView view;
    rr::ViewReport report;
    REQUIRE(session.Renderer.PrepareView(snapshot, {}, view, report) == rhi::RasterStatus::Ready);
    CHECK(report.Visible == 3);
    CHECK(report.Culled == 1);
    CHECK(report.Draws == 2);
    CHECK(report.SourceIds[0] == 2);
    CHECK(report.SourceIds[1] == 3);
    CHECK(report.SourceIds[2] == 4);
    rhi::SubmissionToken completion;
    REQUIRE(session.Renderer.Submit(view, session.Surface, completion) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Draws == 2);
}
TEST_CASE("Invalid scene admission preserves outputs and stale identities never resolve", "[renderer]")
{
    Session session;
    rr::Snapshot snapshot;
    rr::SceneItem items[]{session.Item(1), session.Item(1)};
    CHECK(session.Renderer.CreateSnapshot(items, 2, snapshot) == rhi::RasterStatus::InvalidDescription);
    items[1].SourceId = 2;
    items[1].Transform.Translation.X = std::numeric_limits<float32>::infinity();
    CHECK(session.Renderer.CreateSnapshot(items, 2, snapshot) == rhi::RasterStatus::InvalidDescription);
    CHECK(session.Renderer.CreateSnapshot(items, 257, snapshot) == rhi::RasterStatus::CapacityExceeded);
    REQUIRE(session.Renderer.CreateSnapshot(items, 1, snapshot) == rhi::RasterStatus::Ready);
    const auto stale = snapshot;
    REQUIRE(session.Renderer.Release(snapshot) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.CreateSnapshot(items, 1, snapshot) == rhi::RasterStatus::Ready);
    rr::PreparedView view;
    rr::ViewReport report;
    report.Visible = 77;
    CHECK(session.Renderer.PrepareView(stale, {}, view, report) == rhi::RasterStatus::InvalidHandle);
    CHECK(report.Visible == 77);
    rr::ViewDescription invalid;
    invalid.WorldToClip = {};
    CHECK(session.Renderer.PrepareView(snapshot, invalid, view, report) == rhi::RasterStatus::InvalidDescription);
    CHECK(report.Visible == 77);
}
TEST_CASE("Debug depth ranges are validated before a view is prepared", "[renderer]")
{
    Session session;
    const rr::SceneItem item = session.Item(1);
    rr::Snapshot snapshot;
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    rr::PreparedView view;
    rr::ViewReport report;
    constexpr float32 INFINITE = std::numeric_limits<float32>::infinity();
    const math::Vector2 invalidRanges[]{{1, 1}, {2, 1}, {-1, 1}, {0, INFINITE}, {0, 2e7F}, {0, std::nanf("")}};
    for (const math::Vector2 range : invalidRanges)
    {
        rr::ViewDescription description;
        description.DebugDepthRange = range;
        CHECK(session.Renderer.PrepareView(snapshot, description, view, report) ==
              rhi::RasterStatus::InvalidDescription);
    }
    for (const math::Vector2 range : {math::Vector2{0, 0}, math::Vector2{0.1F, 100}})
    {
        rr::ViewDescription description;
        description.DebugDepthRange = range;
        rr::PreparedView prepared;
        CHECK(session.Renderer.PrepareView(snapshot, description, prepared, report) == rhi::RasterStatus::Ready);
    }
}
TEST_CASE("Prepared submission retries skipped targets and creates no new resources", "[renderer]")
{
    Session session;
    const auto item = session.Item(1);
    rr::Snapshot snapshot;
    rr::PreparedView view;
    rr::ViewReport report;
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.PrepareView(snapshot, {}, view, report) == rhi::RasterStatus::Ready);
    const auto creates = rhi::reference::Creates[0];
    rhi::backend::NextFrame = rhi::FrameStatus::Skipped;
    rhi::SubmissionToken completion;
    CHECK(session.Renderer.Submit(view, session.Surface, completion) == rhi::RasterStatus::NotReady);
    rhi::backend::NextFrame = rhi::FrameStatus::Ready;
    REQUIRE(session.Renderer.Submit(view, session.Surface, completion) == rhi::RasterStatus::Ready);
    for (usize frame = 0; frame < 12; ++frame)
    {
        rhi::reference::Completed = rhi::reference::Submitted;
        REQUIRE(rhi::PollLifetime(session.Device) == rhi::RasterStatus::Ready);
        completion = {};
        REQUIRE(session.Renderer.Submit(view, session.Surface, completion) == rhi::RasterStatus::Ready);
    }
    CHECK(rhi::reference::Creates[0] == creates);
    CHECK(rhi::reference::Draws == 13);
}
TEST_CASE("Snapshot and view capacities recover after release, including clear-only scenes", "[renderer]")
{
    Session session;
    rr::Snapshot snapshots[4], extra;
    for (auto& snapshot : snapshots)
    {
        REQUIRE(session.Renderer.CreateSnapshot(nullptr, 0, snapshot) == rhi::RasterStatus::Ready);
    }
    CHECK(session.Renderer.CreateSnapshot(nullptr, 0, extra) == rhi::RasterStatus::CapacityExceeded);
    rr::PreparedView views[8], extraView;
    rr::ViewReport report;
    for (auto& view : views)
    {
        REQUIRE(session.Renderer.PrepareView(snapshots[0], {}, view, report) == rhi::RasterStatus::Ready);
    }
    CHECK(session.Renderer.PrepareView(snapshots[0], {}, extraView, report) == rhi::RasterStatus::CapacityExceeded);
    REQUIRE(session.Renderer.Release(views[0]) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.PrepareView(snapshots[0], {}, extraView, report) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(session.Renderer.Submit(extraView, session.Surface, completion) == rhi::RasterStatus::Ready);
    CHECK(report.Draws == 0);
    CHECK(rhi::reference::Draws == 0);
}
TEST_CASE("Many objects instance within the portable limit and overlap sorting preserves depth ties", "[renderer]")
{
    Session session;
    rr::SceneItem items[256];
    for (usize i = 0; i < 256; ++i)
    {
        items[i] = session.Item(i + 1);
        items[i].Transform.Columns[0].X = .1F;
        items[i].Transform.Columns[1].Y = .1F;
        items[i].Transform.Columns[0].Y = .05F; // shear, with outward-rounded bounds
        items[i].Transform.Translation.X = -.8F + static_cast<float32>(i % 16) * .1F;
        const usize row = i / 16;
        items[i].Transform.Translation.Y = -.8F + static_cast<float32>(row) * .1F;
    }
    rr::Snapshot snapshot;
    rr::PreparedView view;
    rr::ViewReport report;
    REQUIRE(session.Renderer.CreateSnapshot(items, 256, snapshot) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.PrepareView(snapshot, {}, view, report) == rhi::RasterStatus::Ready);
    CHECK(report.Visible == 256);
    CHECK(report.Draws == 1);
    rhi::SubmissionToken completion;
    REQUIRE(session.Renderer.Submit(view, session.Surface, completion) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Packet.InstanceCount == 256);
    REQUIRE(session.Renderer.Release(view) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(snapshot) == rhi::RasterStatus::Ready);
    rhi::reference::Completed = rhi::reference::Submitted;
    REQUIRE(rhi::PollLifetime(session.Device) == rhi::RasterStatus::Ready);
    rr::Mesh second;
    const math::Vector3 positions[]{{-.2F, -.2F, .5F}, {.2F, -.2F, .5F}, {0, .2F, .5F}};
    const uint32 indices[]{0, 1, 2};
    REQUIRE(session.Renderer.CreateMesh({positions, 3, indices, 3}, second) == rhi::RasterStatus::Ready);
    items[0] = session.Item(1);
    items[1] = session.Item(2);
    items[2] = session.Item(3);
    items[0].Geometry = items[2].Geometry = second;
    REQUIRE(session.Renderer.CreateSnapshot(items, 3, snapshot) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.PrepareView(snapshot, {}, view, report) == rhi::RasterStatus::Ready);
    CHECK(report.SourceIds[0] == 1);
    CHECK(report.SourceIds[1] == 2);
    CHECK(report.SourceIds[2] == 3);
    CHECK(report.Draws == 3);
}
TEST_CASE("Pending geometry is not published as a snapshot and foreign meshes reject", "[renderer]")
{
    Session session;
    rhi::reference::Next = rhi::RasterStatus::Pending;
    rr::Mesh pending;
    const math::Vector3 positions[]{{0, 0, .5F}, {.2F, 0, .5F}, {0, .2F, .5F}};
    const uint32 indices[]{0, 1, 2};
    REQUIRE(session.Renderer.CreateMesh({positions, 3, indices, 3}, pending) == rhi::RasterStatus::Pending);
    auto item = session.Item(1);
    item.Geometry = pending;
    rr::Snapshot snapshot;
    CHECK(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::NotReady);
    item.Geometry = {};
    CHECK(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::InvalidHandle);
    rhi::reference::Next = rhi::RasterStatus::Ready;
    item.Geometry = session.Mesh;
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    rr::Renderer foreign;
    session.Initialize(foreign);
    CHECK(foreign.GetStatus(session.Mesh) == rhi::RasterStatus::InvalidHandle);
    rr::Snapshot foreignScene;
    CHECK(foreign.CreateSnapshot(&item, 1, foreignScene) == rhi::RasterStatus::InvalidHandle);
    const auto stale = session.Mesh;
    session.Renderer.Reset();
    session.Initialize(session.Renderer);
    CHECK(session.Renderer.GetStatus(stale) == rhi::RasterStatus::InvalidHandle);
}
TEST_CASE("L1 explicit passes pair reverse-Z depth and retain detached offscreen presentations", "[renderer][views]")
{
    Session session;
    session.EnableViews();
    const auto item = session.Item(1);
    rr::Snapshot snapshot;
    rr::PreparedView view;
    rr::ViewReport report;
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    rr::ViewDescription description;
    description.Depth = rr::DepthConvention::ReverseZ;
    REQUIRE(session.Renderer.PrepareView(snapshot, description, view, report) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    CHECK(session.Renderer.Submit(view, session.Surface, completion) == rhi::RasterStatus::InvalidDescription);
    rhi::TextureHandle target;
    REQUIRE(rhi::CreateTexture(session.Device, {32, 32, rhi::RasterFormat::Rgba8Unorm, true}, {}, target) ==
            rhi::RasterStatus::Ready);
    rr::ViewMapping mapping;
    REQUIRE(rr::ResolveViewMapping({32, 32}, {0, 0, 48, 64}, 96, 64, mapping) == rhi::RasterStatus::Ready);
    rr::Presentation presentation;
    REQUIRE(session.Renderer.PreparePresentation(target, mapping, presentation) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::BeginFrame(session.Device, session.Surface) == rhi::DeviceStatus::Ready);
    rhi::RasterPassDescription pass;
    pass.Color = target;
    REQUIRE(session.Renderer.DrawView(view, pass) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Pass.Description.ClearDepth == 0);
    CHECK(rhi::reference::Pipelines[rhi::reference::Packet.Pipeline].DepthCompare == rhi::RasterDepthCompare::Greater);
    REQUIRE(session.Renderer.DrawPresentation(presentation) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Pass.Description.ColorLoad == rhi::RasterLoad::Load);
    CHECK(rhi::reference::Pass.Description.Viewport.Width == 48);
    CHECK(rhi::reference::Pass.Description.Viewport.Y == 8);
    REQUIRE(rhi::EndFrame(session.Device, completion) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(session.Device, target) == rhi::RasterStatus::Ready);
    const auto stale = presentation;
    REQUIRE(session.Renderer.Release(presentation) == rhi::RasterStatus::Ready);
    CHECK(session.Renderer.GetStatus(stale) == rhi::RasterStatus::InvalidHandle);
    CHECK(rhi::reference::Destroys[static_cast<usize>(rhi::internal::RasterKind::Texture)] == 0);
    rhi::reference::Completed = rhi::reference::Submitted;
    REQUIRE(rhi::PollLifetime(session.Device) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Destroys[static_cast<usize>(rhi::internal::RasterKind::Texture)] == 1);
}
TEST_CASE("L1 undefined targets, stale identities and overflowing rectangles reject explicitly", "[renderer][views]")
{
    Session session;
    session.EnableViews();
    rhi::TextureHandle target;
    REQUIRE(rhi::CreateTexture(session.Device, {32, 32, rhi::RasterFormat::Rgba8Unorm, true}, {}, target) ==
            rhi::RasterStatus::Ready);
    rr::ViewMapping mapping;
    REQUIRE(rr::ResolveViewMapping({32, 32}, {0, 0, 96, 64}, 96, 64, mapping) == rhi::RasterStatus::Ready);
    rr::Presentation presentation;
    REQUIRE(session.Renderer.PreparePresentation(target, mapping, presentation) == rhi::RasterStatus::Ready);
    rr::ViewMapping resizedMapping;
    REQUIRE(rr::ResolveViewMapping({32, 32}, {0, 0, 120, 80}, 120, 80, resizedMapping) == rhi::RasterStatus::Ready);
    rr::Presentation resized;
    REQUIRE(session.Renderer.PreparePresentation(target, resizedMapping, resized) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::BeginFrame(session.Device, session.Surface) == rhi::DeviceStatus::Ready);
    const auto passesBeforeResize = rhi::reference::Passes;
    CHECK(session.Renderer.DrawPresentation(resized) == rhi::RasterStatus::NotReady);
    CHECK(rhi::reference::Passes == passesBeforeResize);
    CHECK(session.Renderer.DrawPresentation(presentation) == rhi::RasterStatus::InvalidDescription);
    rhi::RasterPassDescription pass;
    pass.UseViewport = true;
    pass.Viewport = {95, 0, ~uint32{0}, 1};
    const auto previous = rhi::reference::Passes;
    CHECK(rhi::BeginRasterPass(session.Device, pass) == rhi::RasterStatus::InvalidDescription);
    CHECK(rhi::reference::Passes == previous);
    pass.Viewport = {0, 0, 96, 64};
    pass.UseScissor = true;
    pass.Scissor = {~uint32{0}, 0, ~uint32{0}, 1};
    CHECK(rhi::BeginRasterPass(session.Device, pass) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(session.Device, completion) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(presentation) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(session.Device, target) == rhi::RasterStatus::Ready);
}
TEST_CASE("L1 infinite reverse-Z culling keeps distant geometry and reports capacity without publishing",
          "[renderer][views]")
{
    Session session;
    session.EnableViews();
    auto item = session.Item(11, {0, 0, -100000});
    rr::Snapshot snapshot;
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    rr::ViewMapping mapping;
    REQUIRE(rr::ResolveViewMapping({96, 64}, {0, 0, 96, 64}, 96, 64, mapping) == rhi::RasterStatus::Ready);
    rr::ProjectionDescription projection;
    projection.Kind = rr::ProjectionKind::PerspectiveInfinite;
    rr::ViewDescription description;
    REQUIRE(rr::BuildViewDescription(projection, mapping, description) == rhi::RasterStatus::Ready);
    rr::PreparedView view;
    rr::ViewReport report;
    REQUIRE(session.Renderer.PrepareView(snapshot, description, view, report) == rhi::RasterStatus::Ready);
    CHECK(report.Visible == 1);
    rhi::TextureHandle target;
    REQUIRE(rhi::CreateTexture(session.Device, {96, 64, rhi::RasterFormat::Rgba8Unorm, true}, {}, target) ==
            rhi::RasterStatus::Ready);
    rr::Presentation presentations[4];
    for (auto& presentation : presentations)
    {
        REQUIRE(session.Renderer.PreparePresentation(target, mapping, presentation) == rhi::RasterStatus::Ready);
    }
    rr::Presentation overflow;
    CHECK(session.Renderer.PreparePresentation(target, mapping, overflow) == rhi::RasterStatus::CapacityExceeded);
    REQUIRE(session.Renderer.Release(presentations[0]) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.PreparePresentation(target, mapping, overflow) == rhi::RasterStatus::Ready);
}
TEST_CASE("Scheduled dynamic snapshots preserve older instance bytes through GPU completion", "[renderer][l2]")
{
    Session session;
    auto item = session.Item(1);
    rr::Snapshot snapshot;
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    rr::ViewDescription description;
    description.ScheduledUpload = true;
    rr::PreparedView oldView;
    rr::ViewReport report;
    REQUIRE(session.Renderer.PrepareView(snapshot, description, oldView, report) == rhi::RasterStatus::Pending);
    CHECK(session.Renderer.GetStatus(oldView) == rhi::RasterStatus::Pending);
    item.Transform.Translation.X = .5F;
    REQUIRE(session.Renderer.ReplaceSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    rr::PreparedView newView;
    REQUIRE(session.Renderer.PrepareView(snapshot, description, newView, report) == rhi::RasterStatus::Pending);
    // Complete both copies independently of the still outstanding frame ordinal.
    for (auto& transfer : rhi::reference::Transfers[0])
    {
        transfer = rhi::RasterStatus::Ready;
    }
    REQUIRE(rhi::PollLifetime(session.Device) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.GetStatus(oldView) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.GetStatus(newView) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken oldSubmission;
    REQUIRE(session.Renderer.Submit(oldView, session.Surface, oldSubmission) == rhi::RasterStatus::Ready);
    const auto oldSlot = rhi::reference::Packet.Vertices[1];
    uint8 oldBytes[80];
    for (usize i = 0; i < 80; ++i)
    {
        oldBytes[i] = rhi::reference::BufferBytes[oldSlot][i];
    }
    REQUIRE(session.Renderer.Release(oldView) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken newSubmission;
    REQUIRE(session.Renderer.Submit(newView, session.Surface, newSubmission) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Packet.Vertices[1] != oldSlot);
    for (usize i = 0; i < 80; ++i)
    {
        CHECK(rhi::reference::BufferBytes[oldSlot][i] == oldBytes[i]);
    }
    CHECK(rhi::GetStatus(session.Device, oldSubmission) == rhi::RasterStatus::Pending);
    REQUIRE(session.Renderer.Release(newView) == rhi::RasterStatus::Ready);
    rhi::reference::Completed = rhi::reference::Submitted;
    REQUIRE(rhi::PollLifetime(session.Device) == rhi::RasterStatus::Ready);
    CHECK(rhi::GetStatus(session.Device, oldSubmission) == rhi::RasterStatus::Ready);
}
TEST_CASE("Portable line expansion clips before divide and handles degenerate and invalid segments", "[renderer][l2]")
{
    rr::OverlayList list;
    REQUIRE(list.Initialize() == rhi::RasterStatus::Ready);
    REQUIRE(list.Line({0, 0, -.5F, -1}, {.5F, 0, .5F, 1}, {1, 1, 1, 1}, 2, 96, 64) == rhi::RasterStatus::Ready);
    CHECK(list.VertexCount() == 6);
    for (usize i = 0; i < 6; ++i)
    {
        CHECK(math::IsFinite(list.Vertices()[i].Clip));
        CHECK(list.Vertices()[i].Clip.Z >= 0);
        CHECK(list.Vertices()[i].Clip.Z <= 1);
    }
    REQUIRE(list.Line({0, 0, .5F, 1}, {0, 0, .5F, 1}, {1, 0, 0, 1}, .1F, 96, 64) == rhi::RasterStatus::Ready);
    CHECK(list.VertexCount() == 12);
    CHECK(list.Vertices()[6].Clip.X != list.Vertices()[7].Clip.X);
    CHECK(list.Vertices()[6].Clip.Y != list.Vertices()[8].Clip.Y);
    REQUIRE(list.Line({0, 0, -1, 1}, {1, 0, -2, 1}, {1, 1, 1, 1}, 2, 96, 64) == rhi::RasterStatus::Ready);
    CHECK(list.VertexCount() == 12);
    CHECK(list.Line({}, {}, {1, 1, 1, 1}, 0, 96, 64) == rhi::RasterStatus::InvalidDescription);
    CHECK(list.VertexCount() == 12);
    rr::OverlayStyle clip;
    clip.Clip = true;
    clip.Scissor = {5, 7, 10, 10};
    REQUIRE(list.Quad({0, 0, 10, 10}, {}, {0, 1, 0, .5F}, 96, 64, false, clip) == rhi::RasterStatus::Ready);
    CHECK(list.CommandCount() == 2);
    REQUIRE(list.Quad({0, 0, 10, 10}, {}, {0, 0, 1, .5F}, 96, 64) == rhi::RasterStatus::Ready);
    CHECK(list.CommandCount() == 3);
    while (list.VertexCount() < rr::OverlayList::MAX_VERTICES)
    {
        REQUIRE(list.Quad({0, 0, 1, 1}, {}, {1, 1, 1, 1}, 96, 64) == rhi::RasterStatus::Ready);
    }
    CHECK(list.Quad({0, 0, 1, 1}, {}, {1, 1, 1, 1}, 96, 64) == rhi::RasterStatus::CapacityExceeded);
    CHECK(list.DroppedCommands() == 1);
    list.Clear();
    CHECK(list.VertexCount() == 0);
    CHECK(list.DroppedCommands() == 0);
}
TEST_CASE("Prepared overlays copy commands, cancel uploads safely and retain exact atlas versions", "[renderer][l2]")
{
    Session session;
    rr::OverlayRenderer overlays;
    const rhi::RasterShaderInput inputs[]{{0, rhi::RasterVertexFormat::Float4},
                                          {1, rhi::RasterVertexFormat::Float4},
                                          {2, rhi::RasterVertexFormat::Float4}};
    const rhi::RasterBinding bindings[]{{0, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
                                        {1, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0}};
    rhi::RasterShaderDescription vertex, fragment;
    vertex.Artifact.Stage = rhi::ShaderStage::Vertex;
    vertex.Artifact.Wgsl = "overlay vertex fixture";
    vertex.Artifact.WgslEntry = "vertexMain";
    vertex.Inputs = inputs;
    fragment.Artifact.Stage = rhi::ShaderStage::Fragment;
    fragment.Artifact.Wgsl = "overlay fragment fixture";
    fragment.Artifact.WgslEntry = "fragmentMain";
    fragment.Bindings = bindings;
    REQUIRE(overlays.Initialize(session.Device, vertex, fragment) == rhi::RasterStatus::Ready);
    rr::OverlayList list;
    REQUIRE(list.Initialize() == rhi::RasterStatus::Ready);
    rr::OverlayStyle clip;
    clip.Clip = true;
    clip.Scissor = {5, 5, 20, 20};
    REQUIRE(list.Quad({0, 0, 30, 30}, {0, 0, 1, 1}, {1, 1, 1, 1}, 96, 64, true, clip) == rhi::RasterStatus::Ready);
    const uint8 coverage[4]{255, 128, 0, 0};
    rhi::TextureHandle atlas;
    REQUIRE(rhi::CreateTexture(session.Device, {2, 1, rhi::RasterFormat::R8Unorm}, {coverage, 4}, atlas) ==
            rhi::RasterStatus::Ready);
    rr::PreparedOverlay prepared, cancelled;
    REQUIRE(overlays.Prepare(list, atlas, prepared) == rhi::RasterStatus::Pending);
    REQUIRE(overlays.Prepare(list, atlas, cancelled) == rhi::RasterStatus::Pending);
    REQUIRE(overlays.Release(cancelled) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::TransferOccupied[0][1]);
    list.Clear();
    REQUIRE(rhi::Destroy(session.Device, atlas) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Destroys[1] == 0);
    for (auto& transfer : rhi::reference::Transfers[0])
    {
        transfer = rhi::RasterStatus::Ready;
    }
    REQUIRE(overlays.Poll() == rhi::RasterStatus::Ready);
    REQUIRE(overlays.GetStatus(prepared) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::BeginFrame(session.Device, session.Surface) == rhi::DeviceStatus::Ready);
    rhi::RasterPassDescription pass;
    pass.DepthStore = rhi::RasterStore::Store;
    REQUIRE(rhi::BeginRasterPass(session.Device, pass) == rhi::RasterStatus::Ready);
    pass.UseScissor = true;
    pass.Scissor = {10, 0, 20, 30};
    REQUIRE(overlays.Draw(prepared, pass) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Pass.Description.Scissor.X == 10);
    CHECK(rhi::reference::Pass.Description.Scissor.Width == 15);
    CHECK(rhi::reference::Pass.Description.DepthReadOnly);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(session.Device, completion) == rhi::RasterStatus::Ready);
    const auto stale = prepared;
    REQUIRE(overlays.Release(prepared) == rhi::RasterStatus::Ready);
    CHECK(overlays.GetStatus(stale) == rhi::RasterStatus::InvalidHandle);
    CHECK(rhi::reference::Destroys[1] == 0);
    rhi::reference::Completed = rhi::reference::Submitted;
    REQUIRE(overlays.Poll() == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Destroys[1] == 1);
    REQUIRE(list.Quad({0, 0, 10, 10}, {}, {1, 1, 1, 1}, 96, 64) == rhi::RasterStatus::Ready);
    rr::PreparedOverlay versions[4], extra;
    for (auto& version : versions)
    {
        REQUIRE(overlays.Prepare(list, {}, version) == rhi::RasterStatus::Pending);
    }
    CHECK(overlays.Prepare(list, {}, extra) == rhi::RasterStatus::CapacityExceeded);
    const auto previous = versions[0];
    rhi::internal::Fail(rhi::backend::PendingToken, rhi::StartupError::DeviceLost);
    CHECK(overlays.GetStatus(previous) == rhi::RasterStatus::DeviceLost);
    overlays.Reset();
    session.Renderer.Reset();
    rhi::Shutdown();
    session.Device = {};
    session.Surface = {};
    REQUIRE(rhi::CreateDevice({}, {}, {}, session.Device, session.Surface) == rhi::DeviceStatus::Pending);
    rhi::internal::Complete(rhi::backend::PendingToken, rhi::StartupError::None, {4096, 16384});
    REQUIRE(overlays.Initialize(session.Device, vertex, fragment) == rhi::RasterStatus::Ready);
    CHECK(overlays.GetStatus(previous) == rhi::RasterStatus::InvalidHandle);
    REQUIRE(overlays.Prepare(list, {}, extra) == rhi::RasterStatus::Pending);
    REQUIRE(overlays.Release(extra) == rhi::RasterStatus::Ready);
    overlays.Reset();
}
namespace
{
struct MaterialFixture final
{
    rhi::RasterShaderInput Inputs[7];
    rhi::RasterBinding Bindings[3]{{0, rhi::RasterBindingKind::UniformBuffer, rhi::RasterVisibility::Fragment, 48},
                                   {1, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
                                   {2, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0}};
    rhi::RasterUniformMember Members[3]{{0, "Tint", 0, rhi::RasterUniformShape::Float4},
                                        {0, "UvTransform", 16, rhi::RasterUniformShape::Float4},
                                        {0, "Options", 32, rhi::RasterUniformShape::Float4}};
    rhi::RasterShaderDescription Vertex, Fragment;
    MaterialFixture()
    {
        for (uint32 i = 0; i < 7; ++i)
        {
            Inputs[i] = {i,
                         i == 0   ? rhi::RasterVertexFormat::Float3
                         : i == 6 ? rhi::RasterVertexFormat::Float2
                                  : rhi::RasterVertexFormat::Float4};
        }
        Vertex.Artifact.Stage = rhi::ShaderStage::Vertex;
        Vertex.Artifact.Wgsl = "material vertex";
        Vertex.Artifact.WgslEntry = "vertexMain";
        Vertex.Inputs = Inputs;
        Fragment.Artifact.Stage = rhi::ShaderStage::Fragment;
        Fragment.Artifact.Wgsl = "material fragment";
        Fragment.Artifact.WgslEntry = "fragmentMain";
        Fragment.Bindings = Bindings;
        Fragment.UniformMembers = Members;
    }
    rr::MaterialProgram Prewarm(rr::Renderer& renderer)
    {
        rr::MaterialProgram program;
        auto status = renderer.PrewarmMaterialProgram(Vertex, Fragment, program);
        REQUIRE((status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending));
        for (usize i = 0; i < 20 && status == rhi::RasterStatus::Pending; ++i)
        {
            status = renderer.GetStatus(program);
        }
        REQUIRE(status == rhi::RasterStatus::Ready);
        return program;
    }
};
} // namespace
TEST_CASE("L3 prewarms six variants and retains exact material versions through reload and completion",
          "[renderer][l3]")
{
    Session session;
    session.EnableViews();
    MaterialFixture fixture;
    auto program = fixture.Prewarm(session.Renderer);
    const auto pipelines = rhi::reference::Creates[7];
    CHECK(pipelines == 13); // Seven L0/L1 plus six legal material states.
    rr::MaterialVersion old;
    rr::MaterialDescription description;
    description.Tint = {1, 0, 0, 1};
    REQUIRE(session.Renderer.CreateMaterial(program, description, old) == rhi::RasterStatus::Ready);
    auto item = session.Item(1);
    item.Surface = old;
    item.Material.Color.X = std::numeric_limits<float32>::quiet_NaN(); // Ignored by the complete surface schema.
    rr::Snapshot snapshot;
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    rr::PreparedView view;
    rr::ViewReport report;
    REQUIRE(session.Renderer.PrepareView(snapshot, {}, view, report) == rhi::RasterStatus::Ready);
    const auto prior = old;
    rr::MaterialVersion candidate;
    description.Tint = {0, 1, 0, 1};
    REQUIRE(session.Renderer.CreateMaterial(program, description, candidate) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.PublishMaterial(candidate, old) == rhi::RasterStatus::Ready);
    CHECK(session.Renderer.GetStatus(prior) == rhi::RasterStatus::InvalidHandle);
    CHECK(session.Renderer.GetStatus(candidate) == rhi::RasterStatus::InvalidHandle);
    description.Tint.X = 2;
    CHECK(session.Renderer.CreateMaterial(program, description, candidate) == rhi::RasterStatus::InvalidDescription);
    CHECK(session.Renderer.PublishMaterial(candidate, old) == rhi::RasterStatus::InvalidHandle);
    CHECK(session.Renderer.GetStatus(old) == rhi::RasterStatus::Ready);
    fixture.Members[1].Shape = rhi::RasterUniformShape::Float3;
    rr::MaterialProgram wrongShape;
    CHECK(session.Renderer.PrewarmMaterialProgram(fixture.Vertex, fixture.Fragment, wrongShape) ==
          rhi::RasterStatus::InvalidDescription);
    fixture.Members[1].Shape = rhi::RasterUniformShape::Float4;
    fixture.Members[1].Offset = 20;
    rr::MaterialProgram rejected;
    CHECK(session.Renderer.PrewarmMaterialProgram(fixture.Vertex, fixture.Fragment, rejected) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(rhi::reference::Creates[7] == pipelines);
    REQUIRE(session.Renderer.Release(program) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::BeginFrame(session.Device, session.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(session.Renderer.DrawView(view, {}) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(session.Device, completion) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Creates[7] == pipelines);
    REQUIRE(session.Renderer.Release(view) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(snapshot) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(old) == rhi::RasterStatus::Ready);
    const auto retired = rhi::reference::Destroys[7];
    CHECK(retired < 6);
    rhi::reference::Completed = rhi::reference::Submitted;
    REQUIRE(rhi::PollLifetime(session.Device) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Destroys[7] >= 6);
}
TEST_CASE("L3 alpha domains, cache pressure and stale incarnations reject before scene activation", "[renderer][l3]")
{
    Session session;
    MaterialFixture fixture;
    auto program = fixture.Prewarm(session.Renderer);
    rr::MaterialVersion materials[8];
    for (auto& material : materials)
    {
        REQUIRE(session.Renderer.CreateMaterial(program, {}, material) == rhi::RasterStatus::Ready);
    }
    rr::MaterialVersion extra;
    CHECK(session.Renderer.CreateMaterial(program, {}, extra) == rhi::RasterStatus::CapacityExceeded);
    const auto stale = materials[0];
    REQUIRE(session.Renderer.Release(materials[0]) == rhi::RasterStatus::Ready);
    rr::MaterialDescription painter;
    painter.Alpha = rr::MaterialAlpha::Painter;
    painter.Tint = {0, 0, 1, .5F};
    REQUIRE(session.Renderer.CreateMaterial(program, painter, extra) == rhi::RasterStatus::Ready);
    CHECK(session.Renderer.GetStatus(stale) == rhi::RasterStatus::InvalidHandle);
    auto item = session.Item(1);
    item.Surface = extra;
    rr::Snapshot snapshot;
    CHECK(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::InvalidDescription);
    item.Pass = rr::Layer::Overlay;
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(extra) == rhi::RasterStatus::Ready);
    CHECK(session.Renderer.CreateMaterial(program, painter, extra) == rhi::RasterStatus::CapacityExceeded);
    REQUIRE(session.Renderer.Release(snapshot) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.CreateMaterial(program, painter, extra) == rhi::RasterStatus::Ready);
    auto second = fixture.Prewarm(session.Renderer);
    rr::MaterialProgram third;
    CHECK(session.Renderer.PrewarmMaterialProgram(fixture.Vertex, fixture.Fragment, third) ==
          rhi::RasterStatus::CapacityExceeded);
    REQUIRE(session.Renderer.Release(second) == rhi::RasterStatus::Ready);
    CHECK(session.Renderer.GetStatus(second) == rhi::RasterStatus::InvalidHandle);
    session.Renderer.Reset();
    CHECK(session.Renderer.GetStatus(program) == rhi::RasterStatus::InvalidHandle);
}
TEST_CASE("L3 rejects nonfinite parameters and failed replacement admission without changing current", "[renderer][l3]")
{
    Session session;
    MaterialFixture fixture;
    auto program = fixture.Prewarm(session.Renderer);
    rr::MaterialVersion current, candidate;
    REQUIRE(session.Renderer.CreateMaterial(program, {}, current) == rhi::RasterStatus::Ready);
    rr::MaterialDescription invalid;
    invalid.AlphaCutoff = std::numeric_limits<float32>::quiet_NaN();
    CHECK(session.Renderer.CreateMaterial(program, invalid, candidate) == rhi::RasterStatus::InvalidDescription);
    invalid = {};
    invalid.Lod = std::numeric_limits<float32>::quiet_NaN();
    CHECK(session.Renderer.CreateMaterial(program, invalid, candidate) == rhi::RasterStatus::InvalidDescription);
    invalid.Lod = std::numeric_limits<float32>::infinity();
    CHECK(session.Renderer.CreateMaterial(program, invalid, candidate) == rhi::RasterStatus::InvalidDescription);
    rhi::reference::Next = rhi::RasterStatus::Failed;
    CHECK(session.Renderer.CreateMaterial(program, {}, candidate) == rhi::RasterStatus::Failed);
    CHECK(session.Renderer.PublishMaterial(candidate, current) == rhi::RasterStatus::InvalidHandle);
    CHECK(session.Renderer.GetStatus(current) == rhi::RasterStatus::Ready);
    rr::MaterialProgram rejected;
    CHECK(session.Renderer.PrewarmMaterialProgram(fixture.Vertex, fixture.Fragment, rejected) ==
          rhi::RasterStatus::Failed);
    rhi::reference::Next = rhi::RasterStatus::Ready;
    auto replacement = fixture.Prewarm(session.Renderer);
    REQUIRE(session.Renderer.CreateMaterial(replacement, {}, candidate) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.PublishMaterial(candidate, current) == rhi::RasterStatus::Ready);
    CHECK(session.Renderer.GetStatus(candidate) == rhi::RasterStatus::InvalidHandle);
    REQUIRE(session.Renderer.Release(replacement) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(current) == rhi::RasterStatus::Ready);
}

TEST_CASE("L3 pending publication and failed validation keep the ready material active", "[renderer][l3]")
{
    Session session;
    MaterialFixture fixture;
    auto program = fixture.Prewarm(session.Renderer);
    rr::MaterialVersion current, candidate;
    REQUIRE(session.Renderer.CreateMaterial(program, {}, current) == rhi::RasterStatus::Ready);
    rhi::reference::Next = rhi::RasterStatus::Pending;
    rhi::reference::PendingCount = 0;
    REQUIRE(session.Renderer.CreateMaterial(program, {}, candidate) == rhi::RasterStatus::Pending);
    REQUIRE(rhi::reference::PendingCount == 3);
    CHECK(session.Renderer.PublishMaterial(candidate, current) == rhi::RasterStatus::NotReady);
    CHECK(session.Renderer.GetStatus(current) == rhi::RasterStatus::Ready);
    for (usize i = 0; i < rhi::reference::PendingCount; ++i)
    {
        rhi::internal::RasterComplete(rhi::reference::PendingRequests[i],
                                      i == 0 ? rhi::RasterStatus::Failed : rhi::RasterStatus::Ready);
    }
    CHECK(session.Renderer.PublishMaterial(candidate, current) == rhi::RasterStatus::Failed);
    CHECK(session.Renderer.GetStatus(current) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(candidate) == rhi::RasterStatus::Ready);
    rhi::reference::PendingCount = 0;
    REQUIRE(session.Renderer.CreateMaterial(program, {}, candidate) == rhi::RasterStatus::Pending);
    for (usize i = 0; i < rhi::reference::PendingCount; ++i)
    {
        rhi::internal::RasterComplete(rhi::reference::PendingRequests[i], rhi::RasterStatus::Ready);
    }
    rhi::reference::Next = rhi::RasterStatus::Ready;
    REQUIRE(session.Renderer.GetStatus(candidate) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.PublishMaterial(candidate, current) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(current) == rhi::RasterStatus::Ready);
}
namespace
{
struct LitFixture final
{
    rhi::RasterShaderInput Inputs[16];
    // The two unlisted entries use RasterBinding's default member initializers.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
    rhi::RasterBinding Bindings[8]{{0, rhi::RasterBindingKind::UniformBuffer, rhi::RasterVisibility::Fragment, 8336},
                                   {1, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
                                   {2, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0},
                                   {3, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
                                   {4, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
                                   {5, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0}};
    rhi::RasterUniformMember Members[14];
    rhi::RasterShaderDescription Vertex, Fragment, Shadow;
    LitFixture(bool clustered = false)
    {
        for (uint32 i = 0; i < 16; ++i)
        {
            Inputs[i] = {i,
                         (i == 0 || i == 7)    ? rhi::RasterVertexFormat::Float3
                         : (i == 6 || i == 15) ? rhi::RasterVertexFormat::Float2
                                               : rhi::RasterVertexFormat::Float4};
        }
        const char* names[]{"Tint",
                            "UvTransform",
                            "Options",
                            "Pbr",
                            "Emission",
                            "EnvironmentIrradiance",
                            "EnvironmentRadiance",
                            "BakeScale",
                            "BakeUv",
                            "Lights",
                            "ShadowData"};
        for (usize i = 0; i < 11; ++i)
        {
            Members[i] = {0,
                          names[i],
                          i == 10 ? 656 : i * 16,
                          i >= 9 ? rhi::RasterUniformShape::Float4Array : rhi::RasterUniformShape::Float4};
        }
        Vertex.Artifact.Stage = rhi::ShaderStage::Vertex;
        Vertex.Artifact.Wgsl = "lit vertex";
        Vertex.Artifact.WgslEntry = "vertexMain";
        Vertex.Inputs = Inputs;
        Fragment.Artifact.Stage = rhi::ShaderStage::Fragment;
        Fragment.Artifact.Wgsl = "lit fragment";
        Fragment.Artifact.WgslEntry = "fragmentMain";
        if (clustered)
        {
            Bindings[0].MinSize = 16096;
            Bindings[6] = {6, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0};
            Bindings[7] = {7, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0};
            Members[10].Offset = 8336;
            Members[11] = {0, "GridRows", 16016, rhi::RasterUniformShape::Float4Array};
            Members[12] = {0, "GridMinimum", 16064, rhi::RasterUniformShape::Float4};
            Members[13] = {0, "GridScale", 16080, rhi::RasterUniformShape::Float4};
        }
        Fragment.Bindings = {Bindings, clustered ? 8U : 6U};
        Fragment.UniformMembers = {Members, clustered ? 14U : 11U};
        Shadow = Fragment;
        Shadow.Artifact.Wgsl = "masked shadow";
        Shadow.Bindings = {Bindings, 3};
    }
    void Enable(Session& session)
    {
        session.EnableViews();
        const rhi::RasterShaderInput input[]{{0, rhi::RasterVertexFormat::Float4}};
        const rhi::RasterBinding bindings[]{
            {0, rhi::RasterBindingKind::UniformBuffer, rhi::RasterVisibility::Fragment, 16},
            {1, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
            {2, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0}};
        const rhi::RasterUniformMember members[]{{0, "Output", 0, rhi::RasterUniformShape::Float4}};
        auto vertex = Vertex;
        vertex.Inputs = input;
        auto fragment = Fragment;
        fragment.Bindings = bindings;
        fragment.UniformMembers = members;
        auto shadow = Shadow;
        shadow.Bindings = {};
        shadow.UniformMembers = {};
        REQUIRE(session.Renderer.InitializeLighting(vertex, fragment, shadow) == rhi::RasterStatus::Ready);
    }
    rr::MaterialProgram Prewarm(Session& session)
    {
        rr::MaterialProgram program;
        auto status = session.Renderer.PrewarmLitProgram(Vertex, Fragment, Shadow, program);
        for (usize i = 0; i < 20 && status == rhi::RasterStatus::Pending; ++i)
        {
            status = session.Renderer.GetStatus(program);
        }
        REQUIRE(status == rhi::RasterStatus::Ready);
        return program;
    }
};
} // namespace
TEST_CASE("lit depth prepass shares the retained color vertex and omits material shading",
          "[renderer][clustered-prepass]")
{
    Session session;
    const bool isClustered = GENERATE(false, true);
    LitFixture fixture(isClustered);
    fixture.Enable(session);
    rhi::RasterShaderDescription depth;
    depth.Artifact.Stage = rhi::ShaderStage::Fragment;
    depth.Artifact.Wgsl = "void opaque depth fixture";
    depth.Artifact.WgslEntry = "DepthMain";
    rr::MaterialProgram program;
    rhi::RasterStatus prewarm =
        session.Renderer.PrewarmLitProgram(fixture.Vertex, fixture.Fragment, fixture.Shadow, program, &depth);
    constexpr usize PREWARM_ATTEMPTS = 20;
    for (usize attempt = 0; attempt < PREWARM_ATTEMPTS && prewarm == rhi::RasterStatus::Pending; ++attempt)
    {
        prewarm = session.Renderer.GetStatus(program);
    }
    REQUIRE(prewarm == rhi::RasterStatus::Ready);
    rr::LightClusterDescription clusters;
    rr::LitMaterialDescription description;
    description.Clusters = isClustered == true ? &clusters : nullptr;
    rr::MaterialVersion material;
    rr::LightSelection selection;
    REQUIRE(session.Renderer.CreateLitMaterial(program, description, material, selection) == rhi::RasterStatus::Ready);
    constexpr usize CORNER_COUNT = 3;
    const StaticArray<math::Vector3, CORNER_COUNT> positions{{{-.1F, -.1F, .5F}, {.1F, -.1F, .5F}, {0, .1F, .5F}}};
    const StaticArray<math::Vector3, CORNER_COUNT> normals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    const StaticArray<uint32, CORNER_COUNT> indices{{0, 1, 2}};
    rr::MeshDescription geometry;
    geometry.Positions = positions.GetData();
    geometry.VertexCount = positions.GetSize();
    geometry.Indices = indices.GetData();
    geometry.IndexCount = indices.GetSize();
    geometry.Normals = normals.GetData();
    rr::Mesh mesh;
    REQUIRE(session.Renderer.CreateMesh(geometry, mesh) == rhi::RasterStatus::Ready);
    rr::SceneItem item;
    item.Geometry = mesh;
    item.Surface = material;
    item.SourceId = 1;
    rr::Snapshot snapshot;
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(material) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(program) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(mesh) == rhi::RasterStatus::Ready);
    rr::PreparedView view;
    rr::ViewReport report;
    REQUIRE(session.Renderer.PrepareView(snapshot, {}, view, report) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(snapshot) == rhi::RasterStatus::Ready);
    rhi::TextureDescription target;
    target.Width = 64;
    target.Height = 64;
    target.Attachment = true;
    rhi::TextureHandle texture;
    REQUIRE(rhi::CreateTexture(session.Device, target, {}, texture) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::BeginFrame(session.Device, session.Surface) == rhi::DeviceStatus::Ready);
    rhi::RasterPassDescription pass;
    pass.Color = texture;
    pass.DepthStore = rhi::RasterStore::Store;
    REQUIRE(session.Renderer.DrawDepthView(view, pass) == rhi::RasterStatus::Ready);
    const rhi::internal::RasterPipelineInfo prepass = rhi::reference::Pipelines[rhi::reference::Packet.Pipeline];
    pass.DepthLoad = rhi::RasterLoad::Load;
    REQUIRE(session.Renderer.DrawView(view, pass) == rhi::RasterStatus::Ready);
    const rhi::internal::RasterPipelineInfo color = rhi::reference::Pipelines[rhi::reference::Packet.Pipeline];
    CHECK(prepass.Vertex == color.Vertex);
    CHECK(prepass.Fragment != color.Fragment);
    CHECK(prepass.DepthCompare == rhi::RasterDepthCompare::Less);
    CHECK(color.DepthCompare == rhi::RasterDepthCompare::LessEqual);
    CHECK(prepass.DepthWrite == true);
    CHECK(prepass.ShouldWriteColor == false);
    CHECK(color.ShouldWriteColor == true);
    CHECK(prepass.Target == rhi::RasterTarget::Rgba8Unorm);
    CHECK(prepass.AttributeCount == color.AttributeCount);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(session.Device, completion) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(view) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(session.Device, texture) == rhi::RasterStatus::Ready);
}

TEST_CASE("L4 direct lists saturate stably and BRDF guards preserve reference outputs", "[renderer][l4]")
{
    rr::DirectLight lights[10];
    for (usize i = 0; i < 10; ++i)
    {
        lights[i].SourceId = 10 - i;
        lights[i].Direction = {0, 0, -2};
    }
    rr::LightSelection selected;
    REQUIRE(rr::SelectDirectLights(lights, 10, selected) == rhi::RasterStatus::Ready);
    CHECK(selected.Count == 8);
    CHECK(selected.Dropped == 2);
    for (usize i = 0; i < 8; ++i)
    {
        CHECK(selected.Lights[i].SourceId == i + 1);
        CHECK(selected.Lights[i].Direction.Z == -1);
    }
    lights[9].Intensity = std::numeric_limits<float32>::quiet_NaN();
    CHECK(rr::SelectDirectLights(lights, 10, selected) == rhi::RasterStatus::InvalidDescription);
    CHECK(selected.Count == 8);
    CHECK(selected.Dropped == 2);
    CHECK(rr::SelectDirectLights(lights, 129, selected) == rhi::RasterStatus::CapacityExceeded);
    math::Vector3 result{9, 8, 7};
    REQUIRE(rr::EvaluateLitBrdf({1, 1, 1}, 0, 1, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, result) == rhi::RasterStatus::Ready);
    CHECK(math::NearlyEqual(result.X, (0.96F + 0.01F) / math::kPiF, 1e-6F, 1e-6F));
    CHECK(result.X == result.Y);
    CHECK(result.Y == result.Z);
    REQUIRE(rr::EvaluateLitBrdf({1, 1, 1}, 1, 1, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, result) == rhi::RasterStatus::Ready);
    CHECK(math::NearlyEqual(result.X, 0.25F / math::kPiF, 1e-6F, 1e-6F));
    REQUIRE(rr::EvaluateLitBrdf({1, 1, 1}, 0, 0.045F, {0, 0, 1}, {0, 0, 1}, {0, 0, -1}, result) ==
            rhi::RasterStatus::Ready);
    CHECK(result == math::Vector3{});
    CHECK(rr::EvaluateLitBrdf({1, 1, 1}, 0, 0, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, result) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(result == math::Vector3{});
    math::Matrix4 matrix;
    CHECK(rr::BuildDirectionalShadowMatrix({{-1, -1, 0}, {1, 1, 1}}, {0, 0, -1}, 32, matrix) ==
          rhi::RasterStatus::Ready);
    CHECK(math::IsFinite(matrix));
    auto old = matrix;
    CHECK(rr::BuildDirectionalShadowMatrix({{-1, -1, 0}, {1, 1, 1}}, {}, 32, matrix) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(matrix == old);
    CHECK(rr::BuildDirectionalShadowMatrix({{-1, -1, -3e38F}, {1, 1, 3e38F}}, {0, 0, 1}, 32, matrix) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(matrix == old);
}
TEST_CASE("L4 lit versions retain atlases and invalidate changed casters conservatively", "[renderer][l4]")
{
    Session session;
    LitFixture fixture;
    fixture.Enable(session);
    auto program = fixture.Prewarm(session);
    const math::Vector3 positions[]{{-.2F, -.2F, .5F}, {.2F, -.2F, .5F}, {0, .2F, .5F}};
    const math::Vector3 normals[]{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    const uint32 indices[]{0, 1, 2};
    rr::Mesh mesh;
    rr::MeshDescription geometry{positions, 3, indices, 3};
    geometry.Normals = normals;
    REQUIRE(session.Renderer.CreateMesh(geometry, mesh) == rhi::RasterStatus::Ready);
    rr::DirectLight light;
    light.SourceId = 71;
    rr::LitMaterialDescription description;
    description.Lights = &light;
    description.LightCount = 1;
    rr::MaterialVersion initial;
    rr::LightSelection selected;
    REQUIRE(session.Renderer.CreateLitMaterial(program, description, initial, selected) == rhi::RasterStatus::Ready);
    auto item = session.Item(1);
    item.Geometry = mesh;
    item.Surface = initial;
    rr::Snapshot casters;
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, casters) == rhi::RasterStatus::Ready);
    rr::ShadowRegion region;
    region.Light = light;
    region.Interior = {1, 1, 14, 14};
    rr::ShadowAtlasDescription atlasDescription{16, 16, &region, 1};
    rr::ShadowAtlas atlas;
    REQUIRE(session.Renderer.PrepareShadowAtlas(casters, atlasDescription, atlas) == rhi::RasterStatus::Ready);
    description.Shadows = atlas;
    rr::MaterialVersion replacement;
    CHECK(session.Renderer.CreateLitMaterial(program, description, replacement, selected) ==
          rhi::RasterStatus::NotReady);
    REQUIRE(rhi::BeginFrame(session.Device, session.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(session.Renderer.DrawShadowAtlas(atlas) == rhi::RasterStatus::Ready);
    CHECK(session.Renderer.DrawShadowAtlas(atlas) == rhi::RasterStatus::InvalidState);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(session.Device, completion) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.CreateLitMaterial(program, description, replacement, selected) ==
            rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(atlas) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.PublishMaterial(replacement, initial) == rhi::RasterStatus::Ready);
    item.Surface = initial;
    rr::Snapshot same;
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, same) == rhi::RasterStatus::Ready);
    rr::PreparedView view;
    rr::ViewReport report;
    REQUIRE(session.Renderer.PrepareView(same, {}, view, report) == rhi::RasterStatus::Ready);
    CHECK(report.ShadowFallbacks == 0);
    REQUIRE(session.Renderer.Release(view) == rhi::RasterStatus::Ready);
    item.Transform.Translation.X = .01F;
    rr::Snapshot moved;
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, moved) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.PrepareView(moved, {}, view, report) == rhi::RasterStatus::Ready);
    CHECK(report.ShadowFallbacks == 1);
    REQUIRE(session.Renderer.Release(initial) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(program) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(mesh) == rhi::RasterStatus::Ready);
    CHECK(session.Renderer.GetStatus(view) == rhi::RasterStatus::Ready);
}
TEST_CASE("L4 rejects malformed light/layout/bake data before publication", "[renderer][l4]")
{
    Session session;
    LitFixture fixture;
    fixture.Enable(session);
    rr::MaterialProgram invalid;
    fixture.Members[10].Offset = 640;
    CHECK(session.Renderer.PrewarmLitProgram(fixture.Vertex, fixture.Fragment, fixture.Shadow, invalid) ==
          rhi::RasterStatus::InvalidDescription);
    fixture.Members[10].Offset = 656;
    auto program = fixture.Prewarm(session);
    rr::LitMaterialDescription description;
    description.Roughness = std::numeric_limits<float32>::quiet_NaN();
    rr::MaterialVersion material;
    rr::LightSelection report;
    report.Dropped = 99;
    CHECK(session.Renderer.CreateLitMaterial(program, description, material, report) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(report.Dropped == 99);
    description.Roughness = .5F;
    REQUIRE(session.Renderer.CreateLitMaterial(program, description, material, report) == rhi::RasterStatus::Ready);
    auto item = session.Item(1);
    item.Surface = material;
    rr::Snapshot snapshot;
    CHECK(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::InvalidDescription);
    item.Surface = {};
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    rr::ShadowRegion regions[2];
    for (auto& region : regions)
    {
        region.Light.SourceId = 1;
        region.Interior = {1, 1, 6, 6};
    }
    rr::ShadowAtlas atlas;
    CHECK(session.Renderer.PrepareShadowAtlas(snapshot, {16, 16, regions, 2}, atlas) ==
          rhi::RasterStatus::InvalidDescription);
    regions[0].Light.Kind = rr::LightKind::Point;
    CHECK(session.Renderer.PrepareShadowAtlas(snapshot, {16, 16, regions, 1}, atlas) ==
          rhi::RasterStatus::InvalidDescription);
}

TEST_CASE("L4 capability negotiation admits LDR without HDR pipelines", "[renderer][l4]")
{
    Session session(false);
    LitFixture fixture;
    fixture.Enable(session);
    auto program = fixture.Prewarm(session);
    rhi::RasterCapabilities caps;
    REQUIRE(rhi::GetRasterCapabilities(session.Device, caps) == rhi::RasterStatus::Ready);
    CHECK_FALSE(caps.HdrColor);
    rhi::TextureHandle target;
    CHECK(rhi::CreateTexture(session.Device, {16, 16, rhi::RasterFormat::Rgba16Float, true}, {}, target) ==
          rhi::RasterStatus::Unsupported);
    rr::MaterialVersion material;
    rr::LightSelection selected;
    REQUIRE(session.Renderer.CreateLitMaterial(program, {}, material, selected) == rhi::RasterStatus::Ready);
    CHECK(selected.Count == 0);
    for (const auto& pipeline : rhi::reference::Pipelines)
    {
        CHECK(pipeline.Target != rhi::RasterTarget::Rgba16Float);
    }
}
TEST_CASE("L4 reference BRDF remains finite at grazing and reciprocal directions", "[renderer][l4]")
{
    const math::Vector3 view{.6F, 0, .8F}, light{0, .8F, .6F}, normal{0, 0, 1};
    math::Vector3 forward, reverse;
    REQUIRE(rr::EvaluateLitBrdf({.7F, .3F, .1F}, .4F, .6F, normal, view, light, forward) == rhi::RasterStatus::Ready);
    REQUIRE(rr::EvaluateLitBrdf({.7F, .3F, .1F}, .4F, .6F, normal, light, view, reverse) == rhi::RasterStatus::Ready);
    CHECK(math::NearlyEqual(forward.X / .6F, reverse.X / .8F, 1e-6F, 1e-6F));
    CHECK(math::NearlyEqual(forward.Y / .6F, reverse.Y / .8F, 1e-6F, 1e-6F));
    for (float32 roughness : {.045F, .5F, 1.0F})
    {
        REQUIRE(rr::EvaluateLitBrdf({1, 1, 1}, 1, roughness, normal, {1, 0, 1e-6F}, {-1, 0, 1e-6F}, forward) ==
                rhi::RasterStatus::Ready);
        CHECK(math::IsFinite(forward));
        CHECK(forward.X >= 0);
    }
}

TEST_CASE("L4 gutter admission tolerates cooked odd-dimension float rounding", "[renderer][l4]")
{
    Session session;
    LitFixture fixture;
    fixture.Enable(session);
    auto program = fixture.Prewarm(session);
    uint8 pixels[5 * 7 * 4];
    for (auto& byte : pixels)
    {
        byte = 255;
    }
    rhi::TextureHandle texture;
    REQUIRE(rhi::CreateTexture(session.Device, {5, 7, rhi::RasterFormat::Rgba8Unorm}, {pixels, 20}, texture) ==
            rhi::RasterStatus::Ready);
    rr::LitMaterialDescription description;
    description.Bake.Texture = texture;
    description.Bake.SourceId = 1;
    description.Bake.SceneRevision = description.Bake.DependencyRevision = 1;
    description.Bake.UvTransform = {2.0F / 5, 4.0F / 7, 1.5F / 5, 1.5F / 7};
    rr::MaterialVersion material;
    rr::LightSelection selected;
    REQUIRE(session.Renderer.CreateLitMaterial(program, description, material, selected) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(material) == rhi::RasterStatus::Ready);
    description.Bake.UvTransform = {1, 1, 0, 0};
    CHECK(session.Renderer.CreateLitMaterial(program, description, material, selected) ==
          rhi::RasterStatus::InvalidDescription);
}

TEST_CASE("Resource mesh CPU copies retire while independent renderer versions remain submitted",
          "[renderer][resources]")
{
    Session session;
    REQUIRE(session.Renderer.Release(session.Mesh) == rhi::RasterStatus::Ready);
    rr::Mesh quad, triangle;
    REQUIRE(ludus::qa::CreateResourceMeshes(session.Renderer, quad, triangle));
    // The helper's manager/adapter/source metadata have already been destroyed.
    CHECK(session.Renderer.GetStatus(quad) == rhi::RasterStatus::Ready);
    rr::Snapshot snapshot;
    rr::PreparedView view;
    rr::ViewReport report;
    REQUIRE(ludus::qa::BuildL0Scene(session.Renderer, quad, triangle, false, snapshot, view, report) ==
            rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(quad) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(triangle) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(snapshot) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(session.Renderer.Submit(view, session.Surface, completion) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(view) == rhi::RasterStatus::Ready);
    CHECK(rhi::GetStatus(session.Device, completion) == rhi::RasterStatus::Pending);
    rhi::reference::Completed = rhi::reference::Submitted;
    REQUIRE(rhi::PollLifetime(session.Device) == rhi::RasterStatus::Ready);
    CHECK(rhi::GetStatus(session.Device, completion) == rhi::RasterStatus::Ready);
}

TEST_CASE("L5 conservative clusters preserve lights, boundaries and overflow", "[renderer][l5]")
{
    rr::LightClusterDescription description;
    description.Bounds = {{0, 0, 0}, {8, 8, 8}};
    rr::DirectLight lights[128];
    for (usize i = 0; i < 128; ++i)
    {
        lights[i].SourceId = 128 - i;
        lights[i].Kind = rr::LightKind::Point;
        lights[i].Position = {4, 4, 4};
        lights[i].Range = .1F;
    }
    rr::LightClusters clusters;
    REQUIRE(rr::BuildLightClusters(description, lights, 128, clusters) == rhi::RasterStatus::Ready);
    CHECK(clusters.Count == 128);
    CHECK(clusters.Lights[0].SourceId == 1);
    CHECK(clusters.Lights[127].SourceId == 128);
    CHECK(clusters.OverflowCells == 8);
    CHECK(clusters.Assignments == 256);
    CHECK(clusters.Overflow[usize{3} + usize{3} * 8 + usize{3} * 64]);
    CHECK(clusters.Counts[usize{3} + usize{3} * 8 + usize{3} * 64] == 32);
    CHECK(clusters.Texels[(usize{3} + usize{3} * 8 + usize{3} * 64) * 32] == 255);
    CHECK(clusters.Counts[0] == 0);
    auto preserved = clusters;
    lights[127].SourceId = lights[0].SourceId;
    CHECK(rr::BuildLightClusters(description, lights, 128, clusters) == rhi::RasterStatus::InvalidDescription);
    CHECK(clusters.Count == preserved.Count);
    CHECK(clusters.OverflowCells == preserved.OverflowCells);
    CHECK(clusters.Assignments == preserved.Assignments);
    CHECK(std::memcmp(clusters.Texels, preserved.Texels, sizeof(clusters.Texels)) == 0);
    for (usize i = 0; i < 128; ++i)
    {
        CHECK(clusters.Lights[i].SourceId == preserved.Lights[i].SourceId);
        CHECK(clusters.Lights[i].Position == preserved.Lights[i].Position);
    }
    CHECK(rr::BuildLightClusters(description, lights, 129, clusters) == rhi::RasterStatus::CapacityExceeded);
    description.WorldToGrid.Columns[0].X = 2;
    CHECK(rr::BuildLightClusters(description, lights, 1, clusters) == rhi::RasterStatus::InvalidDescription);
    description.WorldToGrid = {};
    lights[0].Kind = rr::LightKind::Directional;
    REQUIRE(rr::BuildLightClusters(description, lights, 1, clusters) == rhi::RasterStatus::Ready);
    CHECK(clusters.Assignments == 512);
    CHECK(clusters.OverflowCells == 0);
    lights[0].Kind = rr::LightKind::Spot;
    lights[0].Position = {4, 4, 4};
    REQUIRE(rr::BuildLightClusters(description, lights, 1, clusters) == rhi::RasterStatus::Ready);
    CHECK(clusters.Assignments == 8); // Cone pruning must not miss transparent recipients.
}

TEST_CASE("L5 groups reject complete ranges and LOD preserves retained incarnations", "[renderer][l5]")
{
    Session session;
    rr::SceneItem items[64];
    for (usize i = 0; i < 64; ++i)
    {
        items[i].Geometry = session.Mesh;
        items[i].SourceId = i + 1;
        items[i].Transform.Translation.X = i < 32 ? 5 : 0;
    }
    rr::Snapshot snapshot;
    REQUIRE(session.Renderer.CreateSnapshot(items, 64, snapshot) == rhi::RasterStatus::Ready);
    rr::PreparedView view;
    rr::ViewReport report;
    REQUIRE(session.Renderer.PrepareView(snapshot, {}, view, report) == rhi::RasterStatus::Ready);
    CHECK(report.Visible == 32);
    CHECK(report.CulledGroups == 2);
    CHECK(report.BoundsTests == 0);
    REQUIRE(session.Renderer.Release(view) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(snapshot) == rhi::RasterStatus::Ready);
    rr::Mesh coarse;
    const math::Vector3 positions[]{{-.1F, -.1F, .5F}, {.1F, -.1F, .5F}, {0, .1F, .5F}};
    const uint32 indices[]{0, 1, 2};
    REQUIRE(session.Renderer.CreateMesh({positions, 3, indices, 3}, coarse) == rhi::RasterStatus::Ready);
    items[0].Transform = {};
    items[0].LodGeometry = coarse;
    items[0].LodPixels = 10;
    rr::ViewDescription description;
    description.EnableLod = true;
    description.LodWidth = description.LodHeight = 80;
    REQUIRE(session.Renderer.CreateSnapshot(items, 1, snapshot) == rhi::RasterStatus::Ready);
    rr::Snapshot previous;
    items[0].PreviousLod = true;
    REQUIRE(session.Renderer.CreateSnapshot(items, 1, previous) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(coarse) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.PrepareView(snapshot, description, view, report) == rhi::RasterStatus::Ready);
    CHECK(report.LodObjects == 1); // 8 pixel diameter, below the 9 pixel admission edge.
    CHECK(report.SelectedLods[0]);
    REQUIRE(session.Renderer.Release(view) == rhi::RasterStatus::Ready);
    description.LodWidth = description.LodHeight = 100;
    REQUIRE(session.Renderer.PrepareView(snapshot, description, view, report) == rhi::RasterStatus::Ready);
    CHECK(report.LodObjects == 0); // Hysteresis preserves full detail inside the band.
    REQUIRE(session.Renderer.Release(view) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.PrepareView(previous, description, view, report) == rhi::RasterStatus::Ready);
    CHECK(report.LodObjects == 1); // Previous coarse detail survives the 10-pixel hysteresis band.
    REQUIRE(session.Renderer.Release(view) == rhi::RasterStatus::Ready);
    description.ShadowCasters = true;
    REQUIRE(session.Renderer.PrepareView(previous, description, view, report) == rhi::RasterStatus::Ready);
    CHECK(report.LodObjects == 0);
    REQUIRE(session.Renderer.Release(view) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(previous) == rhi::RasterStatus::Ready);
    description.ShadowCasters = false;
    description.Reference = true;
    description.LodWidth = description.LodHeight = 1;
    REQUIRE(session.Renderer.PrepareView(snapshot, description, view, report) == rhi::RasterStatus::Ready);
    CHECK(report.LodObjects == 0);
    REQUIRE(session.Renderer.Release(view) == rhi::RasterStatus::Ready);
    description.LodWidth = 0;
    CHECK(session.Renderer.PrepareView(snapshot, description, view, report) == rhi::RasterStatus::InvalidDescription);
    REQUIRE(session.Renderer.Release(snapshot) == rhi::RasterStatus::Ready);
}

TEST_CASE("L5 clustered material admits all lights and owns texture bindings", "[renderer][l5]")
{
    Session session;
    LitFixture fixture(true);
    fixture.Enable(session);
    auto program = fixture.Prewarm(session);
    rr::DirectLight lights[128];
    for (usize i = 0; i < 128; ++i)
    {
        lights[i].SourceId = i + 1;
        lights[i].Kind = rr::LightKind::Point;
        lights[i].Position = {0, 0, 1};
        lights[i].Range = .75F;
    }
    rr::LightClusterDescription grid;
    grid.Bounds = {{-2, -2, 0}, {2, 2, 2}};
    rr::LightClusters clusters;
    REQUIRE(rr::BuildLightClusters(grid, lights, 128, clusters) == rhi::RasterStatus::Ready);
    rr::LitMaterialDescription description;
    description.Lights = lights;
    description.LightCount = 128;
    description.Clusters = &grid;
    rr::MaterialVersion material;
    rr::LightSelection report;
    const auto beforeBuffers = rhi::reference::Creates[0];
    const auto status = session.Renderer.CreateLitMaterial(program, description, material, report);
    INFO("Created buffer delta " << (rhi::reference::Creates[0] - beforeBuffers));
    REQUIRE(status == rhi::RasterStatus::Ready);
    CHECK(report.Count == 128);
    CHECK(report.Dropped == 0);
    CHECK(report.OverflowCells > 0);
    REQUIRE(session.Renderer.Release(material) == rhi::RasterStatus::Ready);
    rhi::reference::Next = rhi::RasterStatus::Pending;
    rhi::reference::PendingCount = 0;
    REQUIRE(session.Renderer.CreateLitMaterial(program, description, material, report) == rhi::RasterStatus::Pending);
    for (usize i = 0; i < rhi::reference::PendingCount; ++i)
    {
        rhi::internal::RasterComplete(rhi::reference::PendingRequests[i], rhi::RasterStatus::Ready);
    }
    rhi::reference::Next = rhi::RasterStatus::Ready;
    REQUIRE(session.Renderer.GetStatus(material) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(material) == rhi::RasterStatus::Ready);
}

TEST_CASE("Lighting scratch reuse resets forward and clustered reports", "[renderer][lighting-scratch]")
{
    Session session;
    LitFixture forwardFixture;
    forwardFixture.Enable(session);
    const rr::MaterialProgram forwardProgram = forwardFixture.Prewarm(session);
    LitFixture clusteredFixture(true);
    const rr::MaterialProgram clusteredProgram = clusteredFixture.Prewarm(session);
    constexpr usize LIGHT_COUNT = 10;
    StaticArray<rr::DirectLight, LIGHT_COUNT> lights;
    for (usize i = 0; i < LIGHT_COUNT; ++i)
    {
        lights[i].SourceId = i + 1;
    }
    rr::LitMaterialDescription description;
    description.Lights = lights.GetData();
    description.LightCount = LIGHT_COUNT;
    rr::MaterialVersion material;
    rr::LightSelection report;
    const usize initialBytes = session.Renderer.GetHostBytes();
    REQUIRE(session.Renderer.CreateLitMaterial(forwardProgram, description, material, report) ==
            rhi::RasterStatus::Ready);
    CHECK(report.Count == 8);
    CHECK(report.Dropped == 2);
    CHECK(session.Renderer.GetHostBytes() > initialBytes);
    REQUIRE(session.Renderer.Release(material) == rhi::RasterStatus::Ready);
    rr::LightClusterDescription grid;
    grid.Bounds = {{-2, -2, 0}, {2, 2, 2}};
    description.Clusters = &grid;
    REQUIRE(session.Renderer.CreateLitMaterial(clusteredProgram, description, material, report) ==
            rhi::RasterStatus::Ready);
    CHECK(report.Count == LIGHT_COUNT);
    CHECK(report.Dropped == 0);
    CHECK(report.Assignments > 0);
    const usize scratchBytes = session.Renderer.GetHostBytes();
    REQUIRE(session.Renderer.Release(material) == rhi::RasterStatus::Ready);
    description.Clusters = nullptr;
    REQUIRE(session.Renderer.CreateLitMaterial(forwardProgram, description, material, report) ==
            rhi::RasterStatus::Ready);
    CHECK(report.Count == 8);
    CHECK(report.Dropped == 2);
    CHECK(report.OverflowCells == 0);
    CHECK(report.Assignments == 0);
    CHECK(session.Renderer.GetHostBytes() == scratchBytes);
    REQUIRE(session.Renderer.Release(material) == rhi::RasterStatus::Ready);
}

TEST_CASE("Lighting scratch admission preserves outputs at the host budget", "[renderer][lighting-scratch]")
{
    usize reservedBytes = 0;
    {
        Session session;
        reservedBytes = session.Renderer.GetHostBytes();
    }
    rr::Limits limits;
    limits.HostBytes = reservedBytes;
    Session session(true, limits);
    LitFixture fixture;
    fixture.Enable(session);
    const rr::MaterialProgram program = fixture.Prewarm(session);
    rr::MaterialVersion material;
    rr::LightSelection report;
    report.Count = 17;
    report.Dropped = 29;
    CHECK(session.Renderer.CreateLitMaterial(program, {}, material, report) == rhi::RasterStatus::CapacityExceeded);
    CHECK(session.Renderer.GetStatus(material) == rhi::RasterStatus::InvalidHandle);
    CHECK(report.Count == 17);
    CHECK(report.Dropped == 29);
    CHECK(session.Renderer.GetHostBytes() == reservedBytes);
}

TEST_CASE("RT1 capture replay, target continuation, resets and retained lifetimes", "[renderer][rt1]")
{
    Session session;
    REQUIRE(session.Renderer.Release(session.Mesh) == rhi::RasterStatus::Ready);
    rr::ReferenceRenderingOptions options{rr::ReferenceTraversal::CpuOracle, usize{4} * 1024 * 1024, 0, 3};
    REQUIRE(session.Renderer.EnableReferenceRendering({}, options) == rhi::RasterStatus::Ready);
    rr::Snapshot snapshot;
    REQUIRE(session.Renderer.CreateSnapshot(nullptr, 0, snapshot) == rhi::RasterStatus::Ready);
    rr::ReferenceCaptureDescription description;
    description.OrthographicCamera = true;
    description.Width = 3;
    description.Height = 2;
    description.TargetSamples = 2;
    description.Environment = {2, 3, 4};
    description.Seed = 1977;
    rr::ReferenceCapture first, replay;
    REQUIRE(session.Renderer.BeginReferenceCapture(snapshot, description, first) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.BeginReferenceCapture(snapshot, description, replay) == rhi::RasterStatus::Ready);
    const auto finish = [&session](rr::ReferenceCapture capture) {
        auto status = rhi::RasterStatus::Pending;
        for (usize i = 0; i < 10000 && status == rhi::RasterStatus::Pending; ++i)
        {
            status = session.Renderer.AdvanceReferenceCapture(capture);
        }
        REQUIRE(status == rhi::RasterStatus::Ready);
    };
    finish(first);
    finish(replay);
    rr::ReferencePixel pixels[6], other[6];
    REQUIRE(session.Renderer.CopyReferencePixels(first, pixels, 6) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.CopyReferencePixels(replay, other, 6) == rhi::RasterStatus::Ready);
    for (usize i = 0; i < 6; ++i)
    {
        CHECK(pixels[i].Samples == 2);
        CHECK(pixels[i].Radiance == description.Environment);
        CHECK(pixels[i].Emission == description.Environment);
        CHECK(pixels[i].Direct == math::Vector3{});
        CHECK(pixels[i].Indirect == math::Vector3{});
        CHECK(pixels[i].Radiance == other[i].Radiance);
    }
    rr::ReferenceCaptureInfo info;
    REQUIRE(session.Renderer.GetReferenceCaptureInfo(first, info) == rhi::RasterStatus::Ready);
    CHECK(info.CommittedSamples == 12);
    CHECK(info.ClosestQueries == 12);
    CHECK(info.Complete);
    uint8 hdr[352]{};
    usize written = 7;
    CHECK(session.Renderer.EncodeReferenceHdr(first, hdr, 1, written) == rhi::RasterStatus::CapacityExceeded);
    CHECK(written == 7);
    CHECK(hdr[0] == 0);
    REQUIRE(session.Renderer.EncodeReferenceHdr(first, hdr, sizeof(hdr), written) == rhi::RasterStatus::Ready);
    CHECK(written == sizeof(hdr));
    CHECK(hdr[0] == 'L');
    CHECK(hdr[24] == 3);
    CHECK(hdr[268] == 2);
    description.TargetSamples = 4;
    REQUIRE(session.Renderer.UpdateReferenceCapture(first, snapshot, description) == rhi::RasterStatus::Pending);
    finish(first);
    REQUIRE(session.Renderer.GetReferenceCaptureInfo(first, info) == rhi::RasterStatus::Ready);
    CHECK(info.Revision == 1);
    CHECK(info.CommittedSamples == 24);
    auto invalid = description;
    invalid.Width = 0;
    CHECK(session.Renderer.UpdateReferenceCapture(first, snapshot, invalid) == rhi::RasterStatus::InvalidDescription);
    REQUIRE(session.Renderer.GetReferenceCaptureInfo(first, info) == rhi::RasterStatus::Ready);
    CHECK(info.CommittedSamples == 24);
    description.Environment = {5, 6, 7};
    REQUIRE(session.Renderer.UpdateReferenceCapture(first, snapshot, description) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.GetReferenceCaptureInfo(first, info) == rhi::RasterStatus::Ready);
    CHECK(info.Revision == 2);
    CHECK(info.CommittedSamples == 0);
    REQUIRE(session.Renderer.Release(snapshot) == rhi::RasterStatus::Ready);
    finish(first);
    REQUIRE(session.Renderer.CopyReferencePixels(first, pixels, 6) == rhi::RasterStatus::Ready);
    CHECK(pixels[0].Radiance == description.Environment);
    const auto stale = first;
    REQUIRE(session.Renderer.Release(first) == rhi::RasterStatus::Ready);
    CHECK(session.Renderer.GetStatus(stale) == rhi::RasterStatus::InvalidHandle);
    REQUIRE(session.Renderer.DisableReferenceRendering() == rhi::RasterStatus::Ready);
    CHECK(session.Renderer.GetStatus(replay) == rhi::RasterStatus::InvalidHandle);
}

TEST_CASE("RT1 visible-GGX mixture keeps null mass and agrees with hemisphere integration", "[renderer][rt1]")
{
    const math::Vector3 normal{0, 0, 1};
    for (const auto metallic : {0.0F, 1.0F})
    {
        const rr::ReferenceBsdf bsdf{{1, 1, 1}, metallic, 1};
        float64 pdfIntegral = 0, energyIntegral = 0, energySample = 0;
        usize accepted = 0;
        constexpr usize side = 256;
        for (usize y = 0; y < side; ++y)
        {
            const auto z = (float32(y) + .5F) / float32(side);
            for (usize x = 0; x < side; ++x)
            {
                const auto angle = math::kTauF * (float32(x) + .5F) / float32(side);
                const auto radius = math::Sqrt(1 - z * z);
                rr::ReferenceBsdfEvaluation evaluation;
                REQUIRE(rr::EvaluateReferenceBsdf(bsdf,
                                                  normal,
                                                  normal,
                                                  {radius * math::Cos(angle), radius * math::Sin(angle), z},
                                                  evaluation) == rhi::RasterStatus::Ready);
                pdfIntegral += float64(evaluation.Pdf) * float64(math::kTauF);
                energyIntegral += float64(evaluation.Value.X * z) * float64(math::kTauF);
                rr::ReferenceBsdfSample sample;
                const auto choose = float32((x + y) % side) / float32(side);
                REQUIRE(rr::SampleReferenceBsdf(bsdf,
                                                normal,
                                                normal,
                                                {choose, z, (float32(x) + .5F) / float32(side)},
                                                sample) == rhi::RasterStatus::Ready);
                if (sample.Accepted)
                {
                    ++accepted;
                    rr::ReferenceBsdfEvaluation matched;
                    REQUIRE(rr::EvaluateReferenceBsdf(bsdf, normal, normal, sample.Direction, matched) ==
                            rhi::RasterStatus::Ready);
                    CHECK(matched.Pdf == sample.Evaluation.Pdf);
                    energySample += float64(sample.Evaluation.Value.X * sample.Direction.Z / sample.Evaluation.Pdf);
                }
            }
        }
        const auto count = float64(side * side);
        CHECK(math::Abs(pdfIntegral / count - float64(accepted) / count) < .008);
        CHECK(math::Abs(energyIntegral / count - energySample / count) < .015);
        CHECK(energySample / count <= 1.001);
        CHECK(accepted < side * side);
    }
    rr::ReferenceBsdfEvaluation keep{{7, 8, 9}, 11};
    CHECK(rr::EvaluateReferenceBsdf({}, normal, {0, 0, 2}, normal, keep) == rhi::RasterStatus::InvalidDescription);
    CHECK(keep.Pdf == 11);
}

TEST_CASE("RT1 fixed-mip opacity retains texture views and refuses unavailable content", "[renderer][rt1]")
{
    Session session;
    REQUIRE(session.Renderer.Release(session.Mesh) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.EnableReferenceRendering(
                {},
                {rr::ReferenceTraversal::CpuOracle, usize{4} * 1024 * 1024, 0, 8}) == rhi::RasterStatus::Ready);
    LitFixture fixture;
    fixture.Enable(session);
    const auto program = fixture.Prewarm(session);
    const math::Vector3 positions[]{{-1, -1, .5F}, {1, -1, .5F}, {1, 1, .5F}, {-1, 1, .5F}};
    const math::Vector3 normals[]{{0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}};
    const math::Vector2 uv[]{{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    const uint32 indices[]{0, 2, 1, 0, 3, 2};
    rr::MeshDescription meshDescription{positions, 4, indices, 6};
    meshDescription.Normals = normals;
    meshDescription.Texcoords = uv;
    REQUIRE(session.Renderer.CreateMesh(meshDescription, session.Mesh) == rhi::RasterStatus::Ready);
    const uint8 bytes[]{255, 255, 255, 0, 255, 255, 255, 255, 255, 255, 255, 0, 255, 255, 255, 255};
    rhi::TextureDescription textureDescription{2, 2};
    textureDescription.RetainCpuContent = true;
    rhi::TextureHandle texture;
    REQUIRE(rhi::CreateTexture(session.Device, textureDescription, {bytes, 8}, texture) == rhi::RasterStatus::Ready);
    rhi::TextureViewHandle view;
    REQUIRE(rhi::CreateTextureView(session.Device, texture, view) == rhi::RasterStatus::Ready);
    rhi::TextureCpuContent content;
    REQUIRE(rhi::GetTextureCpuContent(session.Device, view, 0, content) == rhi::RasterStatus::Ready);
    CHECK(content.Width == 2);
    CHECK(content.Bytes[3] == 0);
    const auto keep = content;
    CHECK(rhi::GetTextureCpuContent(session.Device, view, 1, content) == rhi::RasterStatus::InvalidDescription);
    CHECK(content.Bytes == keep.Bytes);
    rr::LitMaterialDescription material;
    material.Surface.Texture = texture;
    material.Surface.Alpha = rr::MaterialAlpha::Masked;
    material.Surface.Sampler.Filter = rhi::RasterFilter::Nearest;
    material.Emission = {3, 2, 1};
    rr::MaterialVersion version;
    rr::LightSelection selection;
    auto extreme = material;
    extreme.Surface.UvTransform.X = std::numeric_limits<float32>::max();
    CHECK(session.Renderer.CreateLitMaterial(program, extreme, version, selection) == rhi::RasterStatus::Unsupported);
    CHECK(session.Renderer.GetStatus(version) == rhi::RasterStatus::InvalidHandle);
    REQUIRE(session.Renderer.CreateLitMaterial(program, material, version, selection) == rhi::RasterStatus::Ready);
    rr::SceneItem item;
    item.SourceId = 1;
    item.Geometry = session.Mesh;
    item.Surface = version;
    rr::Snapshot snapshot;
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    rr::ReferenceCaptureDescription description;
    description.OrthographicCamera = true;
    description.Width = 2;
    description.Height = 1;
    description.CenterRays = true;
    description.TargetSamples = 2;
    rr::ReferenceCapture capture;
    REQUIRE(session.Renderer.BeginReferenceCapture(snapshot, description, capture) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(session.Device, texture) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::GetTextureCpuContent(session.Device, view, 0, content) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(session.Device, view) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(version) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(session.Mesh) == rhi::RasterStatus::Ready);
    auto status = rhi::RasterStatus::Pending;
    for (usize attempt = 0; attempt < 1000 && status == rhi::RasterStatus::Pending; ++attempt)
    {
        status = session.Renderer.AdvanceReferenceCapture(capture);
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
    rr::ReferencePixel pixels[2];
    REQUIRE(session.Renderer.CopyReferencePixels(capture, pixels, 2) == rhi::RasterStatus::Ready);
    CHECK(pixels[0].Radiance == math::Vector3{});
    CHECK(pixels[1].Emission == material.Emission);
    description.TextureMip = 1;
    CHECK(session.Renderer.UpdateReferenceCapture(capture, snapshot, description) ==
          rhi::RasterStatus::InvalidDescription);
    rr::ReferenceCaptureInfo info;
    REQUIRE(session.Renderer.GetReferenceCaptureInfo(capture, info) == rhi::RasterStatus::Ready);
    CHECK(info.Complete);
    CHECK(info.Revision == 1);
}

TEST_CASE("RT1 white furnace preserves energy across seeds and explicit depth limits", "[renderer][rt1]")
{
    Session session;
    REQUIRE(session.Renderer.Release(session.Mesh) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.EnableReferenceRendering(
                {},
                {rr::ReferenceTraversal::CpuOracle, usize{4} * 1024 * 1024, 0, 1}) == rhi::RasterStatus::Ready);
    LitFixture fixture;
    fixture.Enable(session);
    const auto program = fixture.Prewarm(session);
    const math::Vector3 positions[]{{-100, -100, .5F}, {100, -100, .5F}, {100, 100, .5F}, {-100, 100, .5F}};
    const math::Vector3 normals[]{{0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}};
    const uint32 indices[]{0, 2, 1, 0, 3, 2};
    rr::MeshDescription meshDescription{positions, 4, indices, 6};
    meshDescription.Normals = normals;
    REQUIRE(session.Renderer.CreateMesh(meshDescription, session.Mesh) == rhi::RasterStatus::Ready);
    rr::LitMaterialDescription material;
    material.Roughness = 1;
    rr::MaterialVersion version;
    rr::LightSelection selection;
    REQUIRE(session.Renderer.CreateLitMaterial(program, material, version, selection) == rhi::RasterStatus::Ready);
    rr::SceneItem item;
    item.SourceId = 1;
    item.Geometry = session.Mesh;
    item.Surface = version;
    rr::Snapshot snapshot;
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    float32 means[4]{};
    for (usize experiment = 0; experiment < 4; ++experiment)
    {
        rr::ReferenceCaptureDescription description;
        description.OrthographicCamera = true;
        description.Width = description.Height = 1;
        description.CenterRays = true;
        description.TargetSamples = 8192;
        description.Environment = {1, 1, 1};
        description.MaxDepth = experiment % 2 == 0 ? 1 : 4;
        description.RouletteDepth = 1;
        description.Seed = experiment / 2 + 1;
        rr::ReferenceCapture capture;
        REQUIRE(session.Renderer.BeginReferenceCapture(snapshot, description, capture) == rhi::RasterStatus::Ready);
        auto status = rhi::RasterStatus::Pending;
        for (usize attempt = 0; attempt < 1000000 && status == rhi::RasterStatus::Pending; ++attempt)
        {
            status = session.Renderer.AdvanceReferenceCapture(capture);
        }
        REQUIRE(status == rhi::RasterStatus::Ready);
        rr::ReferencePixel pixel;
        REQUIRE(session.Renderer.CopyReferencePixels(capture, &pixel, 1) == rhi::RasterStatus::Ready);
        CHECK(pixel.Emission == math::Vector3{});
        CHECK(pixel.Indirect == math::Vector3{});
        CHECK(pixel.Samples == 8192);
        CHECK(pixel.Radiance.X > .92F);
        CHECK(pixel.Radiance.X < 1.01F);
        means[experiment] = pixel.Radiance.X;
        REQUIRE(session.Renderer.Release(capture) == rhi::RasterStatus::Ready);
    }
    CHECK(means[0] == means[1]);
    CHECK(means[2] == means[3]);
    CHECK(math::Abs(means[0] - means[2]) < .035F);
}

TEST_CASE("Budgeted renderer pools exceed old limits and retain detached versions", "[renderer][capacity]")
{
    rr::Limits limits;
    limits.Meshes = 20;
    limits.Materials = 16;
    limits.Objects = 513; // Exercises the incomplete final 16-object culling group.
    rhi::StorageLimits storage;
    storage.ResourcesPerKind = 128;
    storage.DrawsPerPlan = 1024;
    Session session(true, limits, storage);
    CHECK(session.Renderer.GetConfiguredLimits().Objects == 513);
    CHECK(session.Renderer.GetHostBytes() <= limits.HostBytes);
    session.EnableViews();
    MaterialFixture fixture;
    auto program = fixture.Prewarm(session.Renderer);
    rr::MaterialVersion materials[16];
    for (auto& material : materials)
    {
        REQUIRE(session.Renderer.CreateMaterial(program, {}, material) == rhi::RasterStatus::Ready);
    }
    rr::MaterialVersion excessMaterial;
    CHECK(session.Renderer.CreateMaterial(program, {}, excessMaterial) == rhi::RasterStatus::CapacityExceeded);
    CHECK(session.Renderer.GetStatus(excessMaterial) == rhi::RasterStatus::InvalidHandle);
    const math::Vector3 positions[]{{-.1F, -.1F, .5F}, {.1F, -.1F, .5F}, {0, .1F, .5F}};
    const uint32 indices[]{0, 1, 2};
    rr::Mesh meshes[20];
    meshes[0] = session.Mesh;
    for (usize i = 1; i < 20; ++i)
    {
        REQUIRE(session.Renderer.CreateMesh({positions, 3, indices, 3}, meshes[i]) == rhi::RasterStatus::Ready);
    }
    rr::Mesh excessMesh;
    CHECK(session.Renderer.CreateMesh({positions, 3, indices, 3}, excessMesh) == rhi::RasterStatus::CapacityExceeded);
    CHECK(session.Renderer.GetStatus(excessMesh) == rhi::RasterStatus::InvalidHandle);
    rr::SceneItem items[514];
    for (usize i = 0; i < 514; ++i)
    {
        items[i] = session.Item(i + 1);
        items[i].Geometry = meshes[i % 20];
        items[i].Surface = materials[i % 16];
    }
    rr::Snapshot snapshot;
    CHECK(session.Renderer.CreateSnapshot(items, 514, snapshot) == rhi::RasterStatus::CapacityExceeded);
    REQUIRE(session.Renderer.CreateSnapshot(items, 513, snapshot) == rhi::RasterStatus::Ready);
    const auto staleMesh = meshes[19];
    const auto staleMaterial = materials[15];
    for (auto& mesh : meshes)
    {
        REQUIRE(session.Renderer.Release(mesh) == rhi::RasterStatus::Ready);
    }
    for (auto& material : materials)
    {
        REQUIRE(session.Renderer.Release(material) == rhi::RasterStatus::Ready);
    }
    CHECK(session.Renderer.GetStatus(staleMesh) == rhi::RasterStatus::InvalidHandle);
    CHECK(session.Renderer.GetStatus(staleMaterial) == rhi::RasterStatus::InvalidHandle);
    // Snapshot retention consumes every mesh/material slot despite owner detachment.
    CHECK(session.Renderer.CreateMesh({positions, 3, indices, 3}, excessMesh) == rhi::RasterStatus::CapacityExceeded);
    CHECK(session.Renderer.CreateMaterial(program, {}, excessMaterial) == rhi::RasterStatus::CapacityExceeded);
    uint64 order[513];
    bool lod[513];
    order[0] = 777;
    lod[0] = true;
    rr::ViewReport report;
    report.Visible = 999;
    report.SourceIdOutput = order;
    report.SelectedLodOutput = lod;
    report.DetailCapacity = 512;
    rr::PreparedView view;
    CHECK(session.Renderer.PrepareView(snapshot, { .Reference = true }, view, report) ==
          rhi::RasterStatus::CapacityExceeded);
    CHECK(report.Visible == 999);
    CHECK(order[0] == 777);
    CHECK(lod[0]);
    report.DetailCapacity = 513;
    REQUIRE(session.Renderer.PrepareView(snapshot, { .Reference = true }, view, report) == rhi::RasterStatus::Ready);
    CHECK(report.Visible == 513);
    CHECK(report.Draws == 513);
    CHECK(report.Reported == 256);
    for (usize i = 0; i < 513; ++i)
    {
        CHECK(order[i] == i + 1);
        CHECK_FALSE(lod[i]);
        if (i < 256)
        {
            CHECK(report.SourceIds[i] == order[i]);
        }
    }
    rr::PreparedView grouped;
    rr::ViewReport prefix;
    REQUIRE(session.Renderer.PrepareView(snapshot, {}, grouped, prefix) == rhi::RasterStatus::Ready);
    CHECK(prefix.Visible == 513);
    CHECK(prefix.Reported == 256);
    REQUIRE(session.Renderer.Release(grouped) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(snapshot) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(session.Renderer.Submit(view, session.Surface, completion) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(view) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.CreateMesh({positions, 3, indices, 3}, excessMesh) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.CreateMaterial(program, {}, excessMaterial) == rhi::RasterStatus::Ready);
    CHECK(session.Renderer.GetStatus(staleMesh) == rhi::RasterStatus::InvalidHandle);
    CHECK(session.Renderer.GetStatus(staleMaterial) == rhi::RasterStatus::InvalidHandle);
    rhi::reference::Completed = rhi::reference::Submitted;
    REQUIRE(rhi::PollLifetime(session.Device) == rhi::RasterStatus::Ready);
}
TEST_CASE("Renderer capacity validation and exact host budget preserve failed initialization", "[renderer][capacity]")
{
    Session session;
    rr::Renderer renderer;
    rr::Limits limits;
    limits.Materials = 0;
    CHECK(session.InitializeStatus(renderer, limits) == rhi::RasterStatus::InvalidDescription);
    CHECK(renderer.GetHostBytes() == 0);
    limits = {};
    limits.Objects = ~uint32{0};
    CHECK(session.InitializeStatus(renderer, limits) == rhi::RasterStatus::InvalidDescription);
    limits = {};
    limits.HostBytes = 1;
    CHECK(session.InitializeStatus(renderer, limits) == rhi::RasterStatus::CapacityExceeded);
    CHECK(renderer.GetHostBytes() == 0);
    limits = {};
    limits.Views = ~uint32{0} - 3;
    CHECK(session.InitializeStatus(renderer, limits) == rhi::RasterStatus::InvalidDescription);
    limits = {};
    limits.Meshes = ~uint32{0} - 1;
    CHECK(session.InitializeStatus(renderer, limits) == rhi::RasterStatus::CapacityExceeded);
    CHECK(renderer.GetHostBytes() == 0);
    limits = {};
    limits.Objects = 1025; // More than the negotiated frame packet count.
    CHECK(session.InitializeStatus(renderer, limits) == rhi::RasterStatus::Unsupported);
    const auto exact = session.Renderer.GetHostBytes();
    limits = {};
    limits.HostBytes = exact - 1;
    CHECK(session.InitializeStatus(renderer, limits) == rhi::RasterStatus::CapacityExceeded);
    limits.HostBytes = exact;
    REQUIRE(session.InitializeStatus(renderer, limits) == rhi::RasterStatus::Ready);
    CHECK(renderer.GetHostBytes() == exact);
    renderer.Reset();
    CHECK(renderer.GetHostBytes() == 0);
    REQUIRE(session.InitializeStatus(renderer, {}) == rhi::RasterStatus::Ready);
}

TEST_CASE("RT1 retains and shades instances beyond the old 256-entry table", "[renderer][rt1][capacity]")
{
    rr::Limits limits;
    limits.Objects = 513;
    Session session(true, limits);
    REQUIRE(session.Renderer.Release(session.Mesh) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.EnableReferenceRendering(
                {},
                {rr::ReferenceTraversal::CpuOracle, usize{4} * 1024 * 1024, 0, 1}) == rhi::RasterStatus::Ready);
    LitFixture fixture;
    fixture.Enable(session);
    auto program = fixture.Prewarm(session);
    const math::Vector3 positions[]{{-1, -1, .5F}, {1, -1, .5F}, {1, 1, .5F}, {-1, 1, .5F}};
    const math::Vector3 normals[]{{0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}};
    const uint32 indices[]{0, 2, 1, 0, 3, 2};
    rr::MeshDescription description{positions, 4, indices, 6};
    description.Normals = normals;
    REQUIRE(session.Renderer.CreateMesh(description, session.Mesh) == rhi::RasterStatus::Ready);
    rr::LitMaterialDescription material;
    material.Emission = {3, 2, 1};
    rr::MaterialVersion version;
    rr::LightSelection selection;
    REQUIRE(session.Renderer.CreateLitMaterial(program, material, version, selection) == rhi::RasterStatus::Ready);
    rr::SceneItem items[513];
    for (usize i = 0; i < 513; ++i)
    {
        items[i] = session.Item(i + 1);
        items[i].Surface = version;
        items[i].Transform.Translation.X = i == 512 ? 0 : static_cast<float32>(10 + i * 3);
    }
    rr::Snapshot snapshot;
    REQUIRE(session.Renderer.CreateSnapshot(items, 513, snapshot) == rhi::RasterStatus::Ready);
    rr::ReferenceCaptureDescription captureDescription;
    captureDescription.OrthographicCamera = true;
    captureDescription.Width = captureDescription.Height = 1;
    captureDescription.CenterRays = true;
    captureDescription.MaxDepth = 1;
    rr::ReferenceCapture capture;
    REQUIRE(session.Renderer.BeginReferenceCapture(snapshot, captureDescription, capture) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(session.Mesh) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(version) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(snapshot) == rhi::RasterStatus::Ready);
    auto status = rhi::RasterStatus::Pending;
    for (usize step = 0; step < 20 && status == rhi::RasterStatus::Pending; ++step)
    {
        status = session.Renderer.AdvanceReferenceCapture(capture);
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
    rr::ReferencePixel pixel;
    REQUIRE(session.Renderer.CopyReferencePixels(capture, &pixel, 1) == rhi::RasterStatus::Ready);
    CHECK(pixel.Samples == 1);
    CHECK(pixel.Emission == math::Vector3{3, 2, 1});
    REQUIRE(session.Renderer.Release(capture) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(program) == rhi::RasterStatus::Ready);
}

TEST_CASE("GPU preview capability rejection preserves disabled input retention", "[renderer][gpu-preview]")
{
    Session session(true, {}, {}, false);
    rr::Renderer renderer;
    session.Initialize(renderer);
    // Raster readiness cannot authorize a CPU fallback when compute is unavailable.
    CHECK(renderer.EnableGpuPreview({}, {}, {}, {usize{1024} * 1024, usize{1024} * 1024, 1}) ==
          rhi::RasterStatus::Unsupported);
    CHECK(renderer.PollGpuPreview() == rhi::RasterStatus::NotReady);
    CHECK(renderer.EnableGpuPreview({}, {}, {}, {usize{1024} * 1024, 0, 1}) == rhi::RasterStatus::InvalidDescription);
    CHECK(renderer.EnableReferenceRendering({}, {rr::ReferenceTraversal::CpuOracle, usize{1024} * 1024, 0, 4}) ==
          rhi::RasterStatus::Ready);
    CHECK(renderer.DisableReferenceRendering() == rhi::RasterStatus::Ready);
    renderer.Reset();
}

TEST_CASE("Reference surface admission rejects retained narrow-channel texels", "[renderer][rt1][gpu-preview]")
{
    Session session;
    REQUIRE(session.Renderer.Release(session.Mesh) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.EnableReferenceRendering(
                {},
                {rr::ReferenceTraversal::CpuOracle, usize{4} * 1024 * 1024, 0, 8}) == rhi::RasterStatus::Ready);
    LitFixture fixture;
    fixture.Enable(session);
    rr::MaterialProgram program = fixture.Prewarm(session);
    for (const rhi::RasterFormat format : {rhi::RasterFormat::R8Unorm, rhi::RasterFormat::Rg8Unorm})
    {
        CAPTURE(format);
        const uint8 bytes[]{255, 255, 0, 0};
        rhi::TextureDescription textureDescription{2, 1, format};
        textureDescription.RetainCpuContent = true;
        rhi::TextureHandle texture;
        REQUIRE(rhi::CreateTexture(session.Device, textureDescription, {bytes, 4}, texture) ==
                rhi::RasterStatus::Ready);
        rr::LitMaterialDescription material;
        material.Surface.Texture = texture;
        rr::MaterialVersion version;
        rr::LightSelection lights;
        CHECK(session.Renderer.CreateLitMaterial(program, material, version, lights) ==
              rhi::RasterStatus::InvalidDescription);
        CHECK(session.Renderer.GetStatus(version) == rhi::RasterStatus::InvalidHandle);
        REQUIRE(rhi::Destroy(session.Device, texture) == rhi::RasterStatus::Ready);
    }
    REQUIRE(session.Renderer.Release(program) == rhi::RasterStatus::Ready);
}

TEST_CASE("Native query artifacts require ray support while software compute artifacts do not",
          "[renderer][gpu-preview]")
{
    Session session;
    rr::Renderer renderer;
    session.Initialize(renderer);
    rhi::RayCapabilities capabilities;
    CHECK(rhi::GetRayCapabilities(session.Device, capabilities) == rhi::RasterStatus::Unsupported);
    CHECK(capabilities.InlineQueries == false);
    rhi::RasterShaderDescription compute;
    rr::SetRenderGraphRayInterface(compute, rhi::ShaderStage::Compute);
    CHECK(renderer.EnableGpuPreview(compute, {}, {}, {usize{1024} * 1024, usize{1024} * 1024, 1}) ==
          rhi::RasterStatus::Unsupported);
    CHECK(renderer.PollGpuPreview() == rhi::RasterStatus::NotReady);
    rhi::RasterShaderDescription vertex;
    rhi::RasterShaderDescription fragment;
    rr::SetRenderGraphRayInterface(compute, rhi::ShaderStage::Compute, false, false, false);
    rr::SetRenderGraphRayInterface(vertex, rhi::ShaderStage::Vertex, false, false, false);
    rr::SetRenderGraphRayInterface(fragment, rhi::ShaderStage::Fragment, false, false, false);
    compute.Artifact.Wgsl = "software compute fixture";
    compute.Artifact.WgslEntry = "main";
    vertex.Artifact.Wgsl = "software image vertex fixture";
    vertex.Artifact.WgslEntry = "main";
    fragment.Artifact.Wgsl = "software image fragment fixture";
    fragment.Artifact.WgslEntry = "main";
    REQUIRE(renderer.EnableGpuPreview(compute, vertex, fragment, {usize{1024} * 1024, usize{1024} * 1024, 1}) ==
            rhi::RasterStatus::Ready);
    for (const rhi::RasterBinding& binding : compute.Bindings)
    {
        CHECK(binding.Kind != rhi::RasterBindingKind::RayScene);
    }
    rr::Snapshot snapshot;
    CHECK(renderer.CreateSnapshot(nullptr, 0, snapshot) == rhi::RasterStatus::Ready);
    CHECK(renderer.Release(snapshot) == rhi::RasterStatus::Ready);
    renderer.Reset();
}

TEST_CASE("Raster viewport works without compute and retries an exhausted frame reservation", "[renderer][viewport]")
{
    rhi::StorageLimits storage;
    storage.Submissions = 1;
    Session session(false, {}, storage, false);
    LitFixture lighting;
    lighting.Enable(session);
    rr::Snapshot snapshot;
    const rr::SceneItem item = session.Item(7);
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    rr::RasterViewport viewport;
    REQUIRE(viewport.Initialize(session.Renderer, session.Device, 8, 8) == rhi::RasterStatus::Ready);
    rr::ViewReport report;
    REQUIRE(viewport.Update(snapshot, {}, report) == rhi::RasterStatus::Ready);
    rr::PreparedView earlierView;
    REQUIRE(session.Renderer.PrepareView(snapshot, {}, earlierView, report) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(snapshot) == rhi::RasterStatus::Ready);
    // Keep separate work in flight, then inject backend reservation exhaustion.
    REQUIRE(rhi::BeginFrame(session.Device, session.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(session.Renderer.DrawView(earlierView, {}) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken earlier;
    REQUIRE(rhi::EndFrame(session.Device, earlier) == rhi::RasterStatus::Ready);
    CHECK_FALSE(earlier.IsNull());
    CHECK(viewport.Render(session.Surface, earlier) == rhi::RasterStatus::InvalidDescription);
    rhi::FrameInfo closed;
    CHECK(rhi::GetFrameInfo(session.Device, session.Surface, closed) == rhi::DeviceStatus::InvalidState);
    rhi::reference::Submission = rhi::RasterStatus::CapacityExceeded;
    rhi::SubmissionToken partial;
    CHECK(partial.IsNull());
    CHECK(viewport.Render(session.Surface, partial) == rhi::RasterStatus::CapacityExceeded);
    rhi::FrameInfo frameInfo;
    CHECK(rhi::GetFrameInfo(session.Device, session.Surface, frameInfo) == rhi::DeviceStatus::Ready);
    rhi::reference::Completed = rhi::reference::Submitted;
    rhi::reference::Submission = rhi::RasterStatus::Ready;
    // Retry closes exactly the owned frame, preserves the recording failure and
    // publishes completion for any accepted partial work; no false image success.
    CHECK(viewport.Render(session.Surface, partial) == rhi::RasterStatus::CapacityExceeded);
    CHECK(rhi::GetFrameInfo(session.Device, session.Surface, frameInfo) == rhi::DeviceStatus::InvalidState);
    rhi::reference::Completed = rhi::reference::Submitted;
    REQUIRE(viewport.Poll() == rhi::RasterStatus::Ready);
    rr::ViewportFrame frame;
    rr::ViewportImage image;
    CHECK(viewport.Acquire(frame, image) == rhi::RasterStatus::NotReady);
    rhi::SubmissionToken complete;
    REQUIRE(viewport.Render(session.Surface, complete) == rhi::RasterStatus::Ready);
    rhi::reference::Completed = rhi::reference::Submitted;
    REQUIRE(viewport.Acquire(frame, image) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Dispatches == 0);
    CHECK(rhi::reference::Copies == 0);
    REQUIRE(viewport.Release(frame) == rhi::RasterStatus::Ready);
    viewport.Reset();
    REQUIRE(session.Renderer.Release(earlierView) == rhi::RasterStatus::Ready);
}

TEST_CASE("Raster viewport presents its encoded GPU image in the producer submission", "[renderer][viewport]")
{
    Session session(false);
    LitFixture lighting;
    lighting.Enable(session);
    REQUIRE(rhi::SetFrameTarget(session.Device, session.Surface, { .Width = 96, .Height = 64 }) ==
            rhi::DeviceStatus::Ready);
    rr::Snapshot snapshot;
    const rr::SceneItem item = session.Item(1);
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    rr::RasterViewport viewport;
    REQUIRE(viewport.Initialize(session.Renderer, session.Device, 96, 64, 1, rr::ViewportColorMode::Linear8, true) ==
            rhi::RasterStatus::Ready);
    rr::ViewReport report;
    REQUIRE(viewport.Update(snapshot, {}, report) == rhi::RasterStatus::Ready);
    const usize draws = rhi::reference::Draws;
    rhi::SubmissionToken completion;
    REQUIRE(viewport.Render(session.Surface, completion) == rhi::RasterStatus::Ready);
    // Scene, tone mapping, then a plain composite to the acquired surface.
    CHECK(rhi::reference::Draws == draws + 3);
    CHECK(rhi::reference::Pass.Texture == rhi::internal::RASTER_INVALID);
    CHECK(rhi::reference::Copies == 0);
    rhi::reference::Completed = rhi::reference::Submitted;
    rr::ViewportFrame frame;
    rr::ViewportImage image;
    REQUIRE(viewport.Acquire(frame, image) == rhi::RasterStatus::Ready);
    CHECK(image.Width == 96);
    CHECK(image.Height == 64);
    REQUIRE(viewport.Release(frame) == rhi::RasterStatus::Ready);
    viewport.Reset();
    REQUIRE(session.Renderer.Release(snapshot) == rhi::RasterStatus::Ready);
}

TEST_CASE("Linear surface-map decoding preserves contracts and handles the normal disk", "[renderer][fidelity]")
{
    rr::SurfaceParameters parameters;
    REQUIRE(rr::DecodeSurfaceParameters({.8F, .5F, .5F, .25F}, .8F, .6F, 1, parameters) == rhi::RasterStatus::Ready);
    CHECK(parameters.Metallic == Catch::Approx(.2F));
    CHECK(parameters.Roughness == Catch::Approx(.3F));
    CHECK(parameters.NormalTangent.X == Catch::Approx(.6F));
    CHECK(parameters.NormalTangent.Y == 0);
    CHECK(parameters.NormalTangent.Z == Catch::Approx(.8F));
    REQUIRE(rr::DecodeSurfaceParameters({1, 1, 0, 0}, 1, 1, 0, parameters) == rhi::RasterStatus::Ready);
    CHECK(parameters.NormalTangent == math::Vector3{0, 0, 1});
    CHECK(parameters.Roughness == .045F);
    REQUIRE(rr::DecodeSurfaceParameters({1, 1, 1, 1}, 1, 1, 8, parameters) == rhi::RasterStatus::Ready);
    CHECK(math::Dot(parameters.NormalTangent, parameters.NormalTangent) == Catch::Approx(1));
    CHECK(parameters.NormalTangent.Z == 0);
    const rr::SurfaceParameters saved = parameters;
    CHECK(rr::DecodeSurfaceParameters({-1, 0, 0, 0}, 1, 1, 1, parameters) == rhi::RasterStatus::InvalidDescription);
    CHECK(rr::DecodeSurfaceParameters({0, 0, 0, 0}, 1, 1, 9, parameters) == rhi::RasterStatus::InvalidDescription);
    CHECK(parameters.NormalTangent == saved.NormalTangent);
    CHECK(parameters.Metallic == saved.Metallic);
    CHECK(parameters.Roughness == saved.Roughness);
}

TEST_CASE("UV frames preserve mirrored direction, scale invariance and degenerate fallback", "[renderer][fidelity]")
{
    namespace reference = ludus::graphics::renderer::internal;
    reference::ReferenceTriangle triangle;
    triangle.Positions[0] = {0, 0, 0};
    triangle.Positions[1] = {2, 0, 0};
    triangle.Positions[2] = {0, 3, 0};
    triangle.Uvs[0] = {0, 0};
    triangle.Uvs[1] = {1, 0};
    triangle.Uvs[2] = {0, 1};
    reference::ReferenceSurfaceData surface;
    const math::Vector3 normalWorld{0, 0, -1};
    const math::Vector3 normalTangent{.6F, 0, .8F};
    const math::Vector3 forward = reference::ApplyReferenceNormal(triangle, surface, normalWorld, normalTangent);
    CHECK(math::Length(forward - math::Vector3{.6F, 0, -.8F}) < 1e-6F);
    surface.UvTransform.X = -1;
    const math::Vector3 mirrored = reference::ApplyReferenceNormal(triangle, surface, normalWorld, normalTangent);
    CHECK(math::Length(mirrored - math::Vector3{-.6F, 0, -.8F}) < 1e-6F);
    surface.UvTransform.X = 1e-12F;
    surface.UvTransform.Y = 1e-12F;
    CHECK(math::Length(reference::ApplyReferenceNormal(triangle, surface, normalWorld, normalTangent) - forward) <
          1e-6F);
    surface.UvTransform.X = 0;
    CHECK(reference::ApplyReferenceNormal(triangle, surface, normalWorld, normalTangent) == normalWorld);
}

TEST_CASE("Surface map admission preserves outputs and rejects incompatible programs and encoding",
          "[renderer][fidelity]")
{
    Session session;
    LitFixture fixture;
    fixture.Enable(session);
    rr::MaterialProgram legacy = fixture.Prewarm(session);
    const uint8 bytes[]{128, 128, 255, 255};
    rhi::TextureHandle map;
    REQUIRE(rhi::CreateTexture(session.Device, {1, 1, rhi::RasterFormat::Rgba8Unorm}, {bytes, 4}, map) ==
            rhi::RasterStatus::Ready);
    rr::LitMaterialDescription material;
    material.SurfaceMap = map;
    rr::MaterialVersion version;
    rr::LightSelection report;
    report.Count = 77;
    CHECK(session.Renderer.CreateLitMaterial(legacy, material, version, report) == rhi::RasterStatus::Unsupported);
    CHECK(report.Count == 77);
    REQUIRE(session.Renderer.Release(legacy) == rhi::RasterStatus::Ready);
    // A reflected optional surface profile extends the old ABI by one vec4 and texture.
    rhi::RasterUniformMember members[12];
    for (usize i = 0; i < 11; ++i)
    {
        members[i] = fixture.Members[i];
    }
    members[11] = {0, "SurfaceMap", 8336, rhi::RasterUniformShape::Float4};
    fixture.Bindings[0].MinSize = 8352;
    fixture.Bindings[6] = {6, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0};
    fixture.Fragment.Bindings = {fixture.Bindings, 7};
    fixture.Fragment.UniformMembers = members;
    fixture.Shadow.Bindings = {fixture.Bindings, 3};
    fixture.Shadow.UniformMembers = members;
    rr::MaterialProgram program = fixture.Prewarm(session);
    material.NormalScale = 9;
    CHECK(session.Renderer.CreateLitMaterial(program, material, version, report) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(report.Count == 77);
    material.NormalScale = 1;
    REQUIRE(session.Renderer.CreateLitMaterial(program, material, version, report) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(version) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(session.Device, map) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateTexture(session.Device, {1, 1, rhi::RasterFormat::Rgba8Srgb}, {bytes, 4}, map) ==
            rhi::RasterStatus::Ready);
    material.SurfaceMap = map;
    report.Count = 77;
    CHECK(session.Renderer.CreateLitMaterial(program, material, version, report) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(report.Count == 77);
    CHECK(session.Renderer.GetStatus(version) == rhi::RasterStatus::InvalidHandle);
    REQUIRE(rhi::Destroy(session.Device, map) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(program) == rhi::RasterStatus::Ready);
}

TEST_CASE("Pending viewport replacements retain the drawable view through failure and success", "[renderer][viewport]")
{
    Session session;
    LitFixture lighting;
    lighting.Enable(session);
    const rr::SceneItem first = session.Item(7);
    rr::Snapshot original;
    REQUIRE(session.Renderer.CreateSnapshot(&first, 1, original) == rhi::RasterStatus::Ready);
    rr::RasterViewport viewport;
    REQUIRE(viewport.Initialize(session.Renderer, session.Device, 8, 8) == rhi::RasterStatus::Ready);
    rr::ViewReport report;
    REQUIRE(viewport.Update(original, {}, report) == rhi::RasterStatus::Ready);
    const math::Vector3 positions[]{{-.1F, -.1F, .5F}, {.1F, -.1F, .5F}, {0, .1F, .5F}};
    const uint32 indices[]{0, 1, 2};
    rr::Mesh other;
    REQUIRE(session.Renderer.CreateMesh({positions, 3, indices, 3}, other) == rhi::RasterStatus::Ready);
    rr::SceneItem items[]{first, session.Item(8, {.5F, 0, 0})};
    items[1].Geometry = other;
    rr::Snapshot replacement;
    REQUIRE(session.Renderer.CreateSnapshot(items, 2, replacement) == rhi::RasterStatus::Ready);
    rr::ViewDescription scheduled;
    scheduled.ScheduledUpload = true;
    const auto draw = [&](usize expectedDraws) {
        const usize before = rhi::reference::Draws;
        rhi::SubmissionToken producer;
        REQUIRE(viewport.Render(session.Surface, producer) == rhi::RasterStatus::Ready);
        CHECK(rhi::reference::Draws - before == expectedDraws);
        rhi::reference::Completed = rhi::reference::Submitted;
    };
    REQUIRE(viewport.Update(replacement, scheduled, report) == rhi::RasterStatus::Pending);
    CHECK(report.Visible == 2); // Accepted candidate report; the prior image is still drawable.
    draw(2);                    // One old scene draw and presentation, while upload is pending.
    for (rhi::RasterStatus& status : rhi::reference::Transfers[0])
    {
        status = rhi::RasterStatus::Failed;
    }
    REQUIRE(rhi::PollLifetime(session.Device) == rhi::RasterStatus::Ready);
    CHECK(viewport.Poll() == rhi::RasterStatus::Failed);
    draw(2); // Failed replacement has not destroyed the last drawable snapshot.
    REQUIRE(viewport.Update(replacement, scheduled, report) == rhi::RasterStatus::Pending);
    for (rhi::RasterStatus& status : rhi::reference::Transfers[0])
    {
        status = rhi::RasterStatus::Ready;
    }
    REQUIRE(rhi::PollLifetime(session.Device) == rhi::RasterStatus::Ready);
    REQUIRE(viewport.Poll() == rhi::RasterStatus::Ready);
    draw(3); // Both distinct mesh packets become visible after successful publication.
    viewport.Reset();
    REQUIRE(session.Renderer.Release(replacement) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(original) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(other) == rhi::RasterStatus::Ready);
}

TEST_CASE("HDR environment preparation rejects invalid input and preserves empty ownership", "[renderer][authored]")
{
    Session session;
    rr::EnvironmentMap environment;
    rr::EnvironmentDescription description;
    const math::Vector4 pixels[]{{125952, 4, 1, 0}, {0, 2, 0, 1}, {1, 0, 0, 1}, {0, 0, 0, 1}};
    description.PixelsOrNull = pixels;
    description.Width = 2;
    description.Height = 2;
    description.FilterWidth = 16;
    description.FilterSamples = 16;
    CHECK(environment.Initialize(session.Device, description) == rhi::RasterStatus::CapacityExceeded);
    CHECK(environment.GetStatus() == rhi::RasterStatus::InvalidHandle);
    description.CpuByteBudget = usize{1024} * 1024;
    description.FilterWidth = 17;
    CHECK(environment.Initialize(session.Device, description) == rhi::RasterStatus::InvalidDescription);
    description.FilterWidth = 16;
    REQUIRE(environment.Initialize(session.Device, description) == rhi::RasterStatus::Ready);
    CHECK(environment.Initialize(session.Device, description) == rhi::RasterStatus::InvalidState);
    rr::EnvironmentMap moved(core::Move(environment));
    CHECK(environment.GetStatus() == rhi::RasterStatus::InvalidHandle);
    CHECK(moved.GetStatus() == rhi::RasterStatus::Ready);
    moved.Reset();
    CHECK(moved.GetStatus() == rhi::RasterStatus::InvalidHandle);
}

TEST_CASE("Authored tangent frame overrides the UV fallback and preserves mirrored handedness", "[renderer][authored]")
{
    namespace reference = rr::internal;
    reference::ReferenceTriangle triangle;
    triangle.Positions[0] = {0, 0, 0};
    triangle.Positions[1] = {1, 0, 0};
    triangle.Positions[2] = {0, 1, 0};
    triangle.Uvs[0] = {0, 0};
    triangle.Uvs[1] = {1, 0};
    triangle.Uvs[2] = {0, 1};
    for (math::Vector4& tangent : triangle.Tangents)
    {
        tangent = {0, 1, 0, -1};
    }
    reference::ReferenceSurfaceData surface;
    surface.AreAuthoredInputsEnabled = true;
    const math::Vector3 normalWorld{0, 0, 1};
    CHECK(reference::ApplyReferenceNormal(triangle, surface, normalWorld, {1, 0, 0}, .2F, .3F) ==
          math::Vector3{0, 1, 0});
    CHECK(reference::ApplyReferenceNormal(triangle, surface, normalWorld, {0, 1, 0}, .2F, .3F) ==
          math::Vector3{1, 0, 0});
    surface.AreAuthoredInputsEnabled = false;
    CHECK(reference::ApplyReferenceNormal(triangle, surface, normalWorld, {1, 0, 0}, .2F, .3F) ==
          math::Vector3{1, 0, 0});
}

TEST_CASE("HDR importance sampling uses exact solid angles and leaves dark cells unselected", "[renderer][authored]")
{
    rr::internal::EnvironmentData environment;
    environment.Width = 4;
    environment.Height = 2;
    REQUIRE(environment.Pixels.TryResize(8));
    REQUIRE(environment.Distribution.TryResize(8));
    environment.Pixels[3] = {8, 8, 8, 1};
    environment.Weight = 8 * math::kPi / 2;
    for (usize pixel = 0; pixel < 8; ++pixel)
    {
        environment.Distribution[pixel] = pixel < 3 ? 0 : environment.Weight;
    }
    for (uint32 sample = 0; sample < 128; ++sample)
    {
        const rr::internal::EnvironmentSample result =
            rr::internal::SampleEnvironment(environment, (static_cast<float32>(sample) + .5F) / 128, .37F);
        CHECK(math::Length(result.DirectionWorld) == Catch::Approx(1).margin(1e-6));
        CHECK(result.Radiance == math::Vector3{8, 8, 8});
        CHECK(result.Pdf == Catch::Approx(2 / math::kPi).margin(1e-6));
        CHECK(rr::internal::EvaluateEnvironmentPdf(environment, result.DirectionWorld) ==
              Catch::Approx(result.Pdf).margin(1e-6));
        CHECK(rr::internal::EvaluateEnvironment(environment, result.DirectionWorld) == result.Radiance);
    }
}

TEST_CASE("All thirteen area proxies require complete cube coverage and atomic draw budget admission",
          "[renderer][lighting]")
{
    constexpr usize SOURCE_COUNT = 13;
    constexpr usize REGION_COUNT = SOURCE_COUNT * rr::POINT_SHADOW_FACES;
    constexpr uint32 CELL_PIXELS = 16;
    rr::Limits limits;
    limits.Views = 160;
    rhi::StorageLimits storage;
    storage.ResourcesPerKind = 256;
    Session session(true, limits, storage);
    LitFixture fixture;
    fixture.Enable(session);
    const rr::SceneItem item = session.Item(1);
    rr::Snapshot snapshot;
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    StaticArray<rr::ShadowRegion, REGION_COUNT> regions;
    for (usize i = 0; i < REGION_COUNT; ++i)
    {
        rr::ShadowRegion& region = regions[i];
        region.Light.SourceId = i / rr::POINT_SHADOW_FACES + 1;
        region.Light.Kind = rr::LightKind::AreaOneSided;
        region.Light.Position = {0, 0, 1};
        region.Light.Direction = {0, 0, -1};
        region.CubeFace = static_cast<uint32>(i % rr::POINT_SHADOW_FACES);
        REQUIRE(rr::BuildPointShadowMatrix(region.Light.Position,
                                           region.CubeFace,
                                           region.NearDistance,
                                           region.Light.Range,
                                           region.WorldToClip) == rhi::RasterStatus::Ready);
        region.Interior = {static_cast<uint32>(i / rr::POINT_SHADOW_FACES) * CELL_PIXELS + 1,
                           region.CubeFace * CELL_PIXELS + 1,
                           CELL_PIXELS - 4,
                           CELL_PIXELS - 4};
    }
    rr::ShadowAtlas atlas;
    const rr::ShadowAtlasDescription description{static_cast<uint32>(SOURCE_COUNT) * CELL_PIXELS,
                                                 rr::POINT_SHADOW_FACES * CELL_PIXELS,
                                                 regions.GetData(),
                                                 REGION_COUNT};
    rr::ShadowAtlasDescription missing = description;
    --missing.Count;
    REQUIRE(session.Renderer.PrepareShadowAtlas(snapshot, missing, atlas) == rhi::RasterStatus::InvalidDescription);
    REQUIRE(session.Renderer.PrepareShadowAtlas(snapshot, description, atlas) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::BeginFrame(session.Device, session.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(session.Renderer.DrawShadowAtlas(atlas, storage.DrawsPerFrame + 1) == rhi::RasterStatus::CapacityExceeded);
    REQUIRE(session.Renderer.GetStatus(atlas) == rhi::RasterStatus::Ready);
    const usize earlierDraws = rhi::reference::Draws;
    REQUIRE(session.Renderer.DrawShadowAtlas(atlas) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::reference::Draws - earlierDraws ==
            SOURCE_COUNT); // One visible -Z face per source; other faces cull the caster.
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(session.Device, completion) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(atlas) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(snapshot) == rhi::RasterStatus::Ready);
}

TEST_CASE("Triangle bake texture admission requires canonical dimensions and preserves failed material output",
          "[renderer][lighting]")
{
    constexpr uint32 WIDTH = 6;
    constexpr uint32 HEIGHT = 4;
    constexpr usize PIXEL_BYTES = usize(WIDTH) * HEIGHT * 4;
    Session session;
    LitFixture fixture;
    fixture.Enable(session);
    const rr::MaterialProgram program = fixture.Prewarm(session);
    StaticArray<uint8, PIXEL_BYTES> pixels{};
    rhi::TextureHandle texture;
    REQUIRE(rhi::CreateTexture(session.Device,
                               {WIDTH, HEIGHT, rhi::RasterFormat::Rgba8Unorm},
                               {{pixels.GetData(), pixels.GetSize()}, usize(WIDTH) * 4},
                               texture) == rhi::RasterStatus::Ready);
    rr::LitMaterialDescription description;
    description.Bake.Texture = texture;
    description.Bake.SourceId = 1;
    description.Bake.SceneRevision = 1;
    description.Bake.DependencyRevision = 1;
    description.Bake.AreTriangleChartsEnabled = true;
    rr::MaterialVersion output;
    rr::LightSelection report;
    report.Dropped = 99;
    REQUIRE(session.Renderer.CreateLitMaterial(program, description, output, report) ==
            rhi::RasterStatus::InvalidDescription);
    REQUIRE(report.Dropped == 99);
    REQUIRE(rhi::Destroy(session.Device, texture) == rhi::RasterStatus::Ready);
}

TEST_CASE("World grids use independent budgets and read-only conventional and reverse depth", "[renderer][grid]")
{
    rr::Limits limits;
    limits.Meshes = 1;
    limits.Objects = 1;
    Session session(false, limits);
    session.EnableViews();
    const rr::SceneItem item = session.Item(1);
    rr::Snapshot snapshot;
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    for (const rr::DepthConvention depth : {rr::DepthConvention::Conventional, rr::DepthConvention::ReverseZ})
    {
        rr::ViewDescription description;
        description.Depth = depth;
        description.OrthographicCamera = true;
        description.WorldGrid = { .IsVisible = true, .FocusDistance = 5, .Width = 64, .Height = 64 };
        rr::PreparedView view;
        rr::ViewReport report;
        REQUIRE(session.Renderer.PrepareView(snapshot, description, view, report) == rhi::RasterStatus::Ready);
        CHECK(report.Submitted == 1);
        CHECK(report.Visible == 1);
        CHECK(report.Draws == 2);
        const usize retainedHostBytes = session.Renderer.GetHostBytes();
        REQUIRE(rhi::BeginFrame(session.Device, session.Surface) == rhi::DeviceStatus::Ready);
        REQUIRE(session.Renderer.DrawView(view, {}) == rhi::RasterStatus::Ready);
        const rhi::internal::RasterPipelineInfo& pipeline = rhi::reference::Pipelines[rhi::reference::Packet.Pipeline];
        CHECK(pipeline.Depth == true);
        CHECK(pipeline.DepthWrite == false);
        CHECK(pipeline.DepthCompare == (depth == rr::DepthConvention::Conventional ? rhi::RasterDepthCompare::Less
                                                                                   : rhi::RasterDepthCompare::Greater));
        rhi::SubmissionToken completion;
        REQUIRE(rhi::EndFrame(session.Device, completion) == rhi::RasterStatus::Ready);
        REQUIRE(session.Renderer.Release(view) == rhi::RasterStatus::Ready);
        CHECK(session.Renderer.GetHostBytes() < retainedHostBytes);
    }
}

TEST_CASE("Analytic world grids draw one blended plane and one axis ribbon, and are opt in", "[renderer][grid]")
{
    Session session;
    session.EnableViews();
    const rr::SceneItem item = session.Item(1);
    rr::Snapshot snapshot;
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    rr::ViewDescription description;
    description.WorldGrid = { .IsVisible = true, .FocusDistance = 5, .Width = 64, .Height = 64 };
    rr::PreparedView ribbons;
    rr::ViewReport ribbonReport;
    REQUIRE(session.Renderer.PrepareView(snapshot, description, ribbons, ribbonReport) == rhi::RasterStatus::Ready);
    CHECK(ribbonReport.Draws == 2);
    REQUIRE(session.EnableGrid() == rhi::RasterStatus::Ready);
    CHECK(session.EnableGrid() == rhi::RasterStatus::InvalidState);
    rr::PreparedView analytic;
    rr::ViewReport analyticReport;
    REQUIRE(session.Renderer.PrepareView(snapshot, description, analytic, analyticReport) == rhi::RasterStatus::Ready);
    CHECK(analyticReport.Draws == 3);
    CHECK(session.Renderer.GetHostBytes() > 0);
    usize blended = 0;
    for (const auto& pipeline : rhi::reference::Pipelines)
    {
        if (pipeline.Blend == true && pipeline.AttributeCount == 12)
        {
            CHECK(pipeline.Depth == true);
            CHECK(pipeline.DepthWrite == false);
            CHECK(pipeline.StreamCount == 2);
            ++blended;
        }
    }
    CHECK(blended > 0);
    REQUIRE(rhi::BeginFrame(session.Device, session.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(session.Renderer.DrawView(ribbons, {}) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Draws == 2);
    REQUIRE(session.Renderer.DrawView(analytic, {}) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Draws == 5);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(session.Device, completion) == rhi::RasterStatus::Ready);
    description.ShadowCasters = true;
    rr::PreparedView casters;
    rr::ViewReport casterReport;
    REQUIRE(session.Renderer.PrepareView(snapshot, description, casters, casterReport) == rhi::RasterStatus::Ready);
    CHECK(casterReport.Draws == 1);
}

TEST_CASE("Analytic grid shaders are validated before use", "[renderer][grid]")
{
    Session session;
    rhi::RasterShaderInput inputs[12];
    for (uint32 i = 0; i < 12; ++i)
    {
        inputs[i] = {i, i == 0 ? rhi::RasterVertexFormat::Float3 : rhi::RasterVertexFormat::Float4};
    }
    rhi::RasterShaderDescription vertex, fragment;
    vertex.Artifact.Stage = rhi::ShaderStage::Vertex;
    vertex.Artifact.Wgsl = "grid vertex fixture";
    vertex.Artifact.WgslEntry = "vertexMain";
    fragment.Artifact.Stage = rhi::ShaderStage::Fragment;
    fragment.Artifact.Wgsl = "grid fragment fixture";
    fragment.Artifact.WgslEntry = "fragmentMain";
    vertex.Inputs = {inputs, 6};
    CHECK(session.Renderer.InitializeGrid(vertex, fragment) == rhi::RasterStatus::InvalidDescription);
    vertex.Inputs = inputs;
    inputs[3].Format = rhi::RasterVertexFormat::Float3;
    CHECK(session.Renderer.InitializeGrid(vertex, fragment) == rhi::RasterStatus::InvalidDescription);
    inputs[3].Format = rhi::RasterVertexFormat::Float4;
    rr::Renderer uninitialized;
    CHECK(uninitialized.InitializeGrid(vertex, fragment) == rhi::RasterStatus::NotReady);
    CHECK(session.Renderer.InitializeGrid(vertex, fragment) == rhi::RasterStatus::Ready);
}

TEST_CASE("Pending and failed grid preparation preserve the previous drawable view", "[renderer][grid]")
{
    Session session;
    const rr::SceneItem item = session.Item(1);
    rr::Snapshot snapshot;
    REQUIRE(session.Renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    rr::PreparedView previous;
    rr::ViewReport previousReport;
    REQUIRE(session.Renderer.PrepareView(snapshot, {}, previous, previousReport) == rhi::RasterStatus::Ready);
    rr::ViewDescription description;
    description.WorldGrid = { .IsVisible = true, .FocusDistance = 5, .Width = 64, .Height = 64 };
    rr::PreparedView candidate;
    rr::ViewReport report;
    report.Visible = 99;
    rhi::reference::Next = rhi::RasterStatus::Failed;
    CHECK(session.Renderer.PrepareView(snapshot, description, candidate, report) == rhi::RasterStatus::Failed);
    CHECK(report.Visible == 99);
    CHECK(session.Renderer.GetStatus(previous) == rhi::RasterStatus::Ready);
    CHECK(session.Renderer.GetStatus(candidate) == rhi::RasterStatus::InvalidHandle);
    rhi::reference::Next = rhi::RasterStatus::Pending;
    REQUIRE(session.Renderer.PrepareView(snapshot, description, candidate, report) == rhi::RasterStatus::Pending);
    CHECK(session.Renderer.GetStatus(previous) == rhi::RasterStatus::Ready);
    CHECK(session.Renderer.GetStatus(candidate) == rhi::RasterStatus::Pending);
    rhi::reference::Next = rhi::RasterStatus::Ready;
    for (usize i = 0; i < rhi::reference::PendingCount; ++i)
    {
        rhi::internal::RasterComplete(rhi::reference::PendingRequests[i], rhi::RasterStatus::Ready);
    }
    CHECK(session.Renderer.GetStatus(candidate) == rhi::RasterStatus::Ready);
    REQUIRE(session.Renderer.Release(candidate) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(session.Renderer.Submit(previous, session.Surface, completion) == rhi::RasterStatus::Ready);
}

TEST_CASE("RT1 authored white vertex colors remain in the BSDF domain at interior barycentrics",
          "[renderer][rt1][authored-color]")
{
    namespace reference = rr::internal;
    reference::ReferenceJob job;
    reference::ReferenceSurfaceData surface;
    surface.Bsdf.BaseColor = {1, 1, 1};
    surface.AreAuthoredInputsEnabled = true;
    reference::ReferenceTriangle triangle;
    triangle.Positions[0] = {0, 0, 0};
    triangle.Positions[1] = {1, 0, 0};
    triangle.Positions[2] = {0, 1, 0};
    triangle.GeometricNormal = {0, 0, 1};
    triangle.Area = 0.5F;
    for (math::Vector3& normalWorld : triangle.Normals)
    {
        normalWorld = triangle.GeometricNormal;
    }
    for (math::Vector4& color : triangle.Colors)
    {
        color = {1, 1, 1, 1};
    }
    REQUIRE(job.Scene.Triangles.TryAdd(triangle) == true);
    reference::ReferenceInstance instance;
    instance.Surface = &surface;
    instance.TriangleCount = 1;
    REQUIRE(job.Scene.Instances.TryAdd(instance) == true);
    job.Scene.InstanceCount = 1;
    REQUIRE(math::TryMakeRandomKey(1, reference::REFERENCE_RANDOM_DOMAIN, job.Key) == math::MathStatus::Success);
    for (const bool hasDirectLight : {false, true})
    {
        job.Description.LightCount = hasDirectLight == true ? 1 : 0;
        job.Lights[0].Kind = rr::LightKind::Directional;
        job.Lights[0].Direction = {0, 0, -1};
        surface.Bsdf.BaseColor = {1, 1, 1};
        reference::ReferencePath path;
        path.Pixel = 23;
        path.Sample = 17;
        path.Ray = reference::ReferenceRay({0, 0, 1}, {0, 0, -1}, 2);
        // The naive three-term interpolation of constant white rounds one ULP above one here.
        // These coordinates were also observed at a failing Staircase2 hit.
        rhi::RayResult hit;
        hit.Hit = 1;
        hit.Distance = 1;
        hit.U = 0.1534976065158844F;
        hit.V = 0.22540529072284698F;
        rr::ReferenceCaptureInfo information;
        CHECK(reference::PrepareReferenceVertex(job, information, path, hit) == rhi::RasterStatus::Ready);
        CHECK(information.InvalidContributions == 0);
        CHECK(information.Failure == rr::ReferenceFailure::None);
        CHECK(math::IsFinite(path.Throughput) == true);
        // A genuinely invalid BSDF still fails, with its exact sample location retained.
        surface.Bsdf.BaseColor = {2, 1, 1};
        path.Depth = 0;
        information = {};
        CHECK(reference::PrepareReferenceVertex(job, information, path, hit) == rhi::RasterStatus::Failed);
        CHECK(information.InvalidContributions == 1);
        CHECK(information.Failure == (hasDirectLight == true ? rr::ReferenceFailure::InvalidBsdfEvaluation
                                                             : rr::ReferenceFailure::InvalidBsdfSample));
        CHECK(information.FailurePixel == path.Pixel);
        CHECK(information.FailureSample == path.Sample);
        CHECK(information.FailureDepth == 0);
    }
}

TEST_CASE("Reference vertex color interpolation preserves invalid arithmetic for validation",
          "[renderer][rt1][authored-color]")
{
    rr::internal::ReferenceTriangle triangle;
    for (math::Vector4& color : triangle.Colors)
    {
        color = {1, 1, 1, 1};
    }
    CHECK(rr::internal::InterpolateReferenceColor(triangle, .1534976065158844F, .22540529072284698F) ==
          math::Vector4{1, 1, 1, 1});
    triangle.Colors[0].X = std::numeric_limits<float32>::quiet_NaN();
    CHECK(math::IsFinite(rr::internal::InterpolateReferenceColor(triangle, .1534976065158844F, .22540529072284698F)) ==
          false);
}

TEST_CASE("Compute reference status retains the selected failure and addressed sample",
          "[renderer][reference-compute-status]")
{
    namespace internal = rr::internal;
    constexpr usize WORD_COUNT = internal::REFERENCE_COMPUTE_STATUS_BYTES / sizeof(uint32);
    // First two vectors: sample extrema, commits, invalid pixels, and query/event counts.
    // Third vector: the same lowest failing pixel's index, depth, capacity count, reason.
    StaticArray<uint32, WORD_COUNT> words{
        {7, 8, 15, 2, 31, 19, 4, 3, 128, 1, 0, LUDUS_REFERENCE_FAILURE_INVALID_BSDF_SAMPLE}};
    StaticArray<uint8, internal::REFERENCE_COMPUTE_STATUS_BYTES> bytes{};
    const auto encodeStatus = [&words, &bytes]() {
        for (usize index = 0; index < words.GetSize(); ++index)
        {
            REQUIRE(TryWriteLittleEndian(words[index], {bytes.GetData() + index * sizeof(uint32), sizeof(uint32)}) ==
                    true);
        }
    };
    encodeStatus();
    rr::ReferenceCaptureInfo information;
    information.MinimumSamples = 7;
    information.CommittedSamples = 100;
    CHECK(internal::ApplyReferenceComputeStatus(bytes, information) == rhi::RasterStatus::Failed);
    CHECK(information.MinimumSamples == 7);
    CHECK(information.MaximumSamples == 8);
    CHECK(information.CommittedSamples == 115);
    CHECK(information.InvalidContributions == 2);
    CHECK(information.FailurePixel == 128);
    CHECK(information.FailureDepth == 1);
    CHECK(information.FailureSample == 7);
    CHECK(information.Failure == rr::ReferenceFailure::InvalidBsdfSample);
    CHECK(information.ClosestQueries == 31);
    CHECK(information.VisibilityQueries == 19);
    CHECK(information.NullBsdfSamples == 4);
    CHECK(information.RouletteTerminations == 3);
    words[10] = 1;
    words[11] = LUDUS_REFERENCE_FAILURE_TRAVERSAL_CAPACITY_EXCEEDED;
    encodeStatus();
    CHECK(internal::ApplyReferenceComputeStatus(bytes, information) == rhi::RasterStatus::CapacityExceeded);
    CHECK(information.Failure == rr::ReferenceFailure::TraversalCapacityExceeded);
    words[3] = 0;
    words[10] = 0;
    words[11] = LUDUS_REFERENCE_FAILURE_NONE;
    encodeStatus();
    CHECK(internal::ApplyReferenceComputeStatus(bytes, information) == rhi::RasterStatus::Ready);
    // A successful dispatch must not replace diagnostic evidence with a different pixel/reason.
    CHECK(information.FailurePixel == 128);
    CHECK(information.FailureSample == 7);
    CHECK(information.Failure == rr::ReferenceFailure::TraversalCapacityExceeded);
}
