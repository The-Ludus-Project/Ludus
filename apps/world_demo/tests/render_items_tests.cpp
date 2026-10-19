#include "internal/camera_view.h"
#include "internal/render_items.h"
#include "internal/world.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
using namespace ludus::world_demo;
namespace math = ludus::foundation::math;
namespace rhi = ludus::graphics::rhi;
namespace rr = ludus::graphics::renderer;
namespace
{
// The drawable the reference player opened before this change.
constexpr uint32 WIDTH = 960;
constexpr uint32 HEIGHT = 640;
// Pixels this close to a box edge are not compared: rasterization and the reference
// shader's strict inequality may legitimately disagree there.
constexpr float32 EDGE_TOLERANCE = 1.0e-3F;
constexpr float32 CLIP_TOLERANCE = 1.0e-4F;
// Sampling stride across the drawable, in pixels.
constexpr uint32 PIXEL_STRIDE = 7;
void Load(Session& inoutSession)
{
    REQUIRE(inoutSession.RequestLoad(ExampleLevel()).Error == LevelError::None);
    while (inoutSession.GetLoadStage() != LoadStage::Ready)
    {
        REQUIRE(inoutSession.PollLoad() == Status::Success);
    }
    REQUIRE(inoutSession.Activate());
}
RenderFrame ExtractExample()
{
    Session session;
    Load(session);
    RenderFrame frame;
    REQUIRE(session.World().Extract(0, frame, false, 1));
    REQUIRE(frame.Count > 0);
    return frame;
}
// Independent restatement of the retired world_scene.slang coverage test: rotate the
// point by -angle into the box frame, then compare against the half extent.
bool IsInsideReference(const Draw& draw, math::Vector2 point, bool& outIsNearEdge)
{
    const float32 deltaX = point.X - draw.Position.X;
    const float32 deltaY = point.Y - draw.Position.Y;
    const float32 cosine = std::cos(draw.Rotation);
    const float32 sine = std::sin(draw.Rotation);
    const float32 localX = cosine * deltaX + sine * deltaY;
    const float32 localY = -sine * deltaX + cosine * deltaY;
    outIsNearEdge = std::abs(std::abs(localX) - draw.HalfExtent.X) < EDGE_TOLERANCE ||
                    std::abs(std::abs(localY) - draw.HalfExtent.Y) < EDGE_TOLERANCE;
    return std::abs(localX) < draw.HalfExtent.X && std::abs(localY) < draw.HalfExtent.Y;
}
// Coverage of the unit quad through an item's transform: invert its 2x2 linear block.
bool IsInsideItem(const math::Affine3& transform, math::Vector2 point)
{
    const float32 a = transform.Columns[0].X;
    const float32 b = transform.Columns[1].X;
    const float32 c = transform.Columns[0].Y;
    const float32 d = transform.Columns[1].Y;
    const float32 determinant = a * d - b * c;
    const float32 deltaX = point.X - transform.Translation.X;
    const float32 deltaY = point.Y - transform.Translation.Y;
    const float32 u = (d * deltaX - b * deltaY) / determinant;
    const float32 v = (-c * deltaX + a * deltaY) / determinant;
    return std::abs(u) < 1 && std::abs(v) < 1;
}
math::Vector4 TransformToClip(const math::Matrix4& matrix, math::Vector3 point)
{
    return matrix * math::Vector4{point.X, point.Y, point.Z, 1};
}
} // namespace

TEST_CASE("The unit quad covers [-1, 1] counter-clockwise from +Z", "[world-demo][renderer]")
{
    const rr::MeshDescription mesh = GetUnitQuadMesh();
    REQUIRE(mesh.VertexCount == UNIT_QUAD_VERTEX_COUNT);
    REQUIRE(mesh.IndexCount == UNIT_QUAD_INDEX_COUNT);
    for (usize triangle = 0; triangle < UNIT_QUAD_INDEX_COUNT; triangle += UNIT_QUAD_INDEX_COUNT / 2)
    {
        const math::Vector3 first = mesh.Positions[mesh.Indices[triangle]];
        const math::Vector3 second = mesh.Positions[mesh.Indices[triangle + 1]];
        const math::Vector3 third = mesh.Positions[mesh.Indices[triangle + 2]];
        const float32 area = (second.X - first.X) * (third.Y - first.Y) - (second.Y - first.Y) * (third.X - first.X);
        CHECK(area > 0);
    }
}

TEST_CASE("Draw transforms scale, rotate, translate and order boxes", "[world-demo][renderer]")
{
    const Draw draw{ .Position = {1, 2}, .HalfExtent = {0.5F, 0.25F}, .Rotation = 0.3F };
    math::Affine3 transform;
    REQUIRE(TryBuildDrawTransform(draw, 3, transform) == rhi::RasterStatus::Ready);
    // The local +X corner lands at the rotated half extent.
    const math::Vector3 corner = math::TransformPoint(transform, {1, 0, 0});
    CHECK(corner.X == Catch::Approx(1 + 0.5F * std::cos(0.3F)));
    CHECK(corner.Y == Catch::Approx(2 + 0.5F * std::sin(0.3F)));
    CHECK(transform.Translation.Z == Catch::Approx(3 * DRAW_ORDER_DEPTH_STEP));

    math::Affine3 later;
    REQUIRE(TryBuildDrawTransform(draw, MAX_RENDER_DRAWS - 1, later) == rhi::RasterStatus::Ready);
    CHECK(later.Translation.Z > transform.Translation.Z);
    CHECK(later.Translation.Z < DRAW_ORDER_DEPTH_SPAN);

    SECTION("Invalid draws preserve the output")
    {
        const math::Affine3 before = transform;
        Draw invalid = draw;
        invalid.HalfExtent.X = 0;
        CHECK(TryBuildDrawTransform(invalid, 0, transform) == rhi::RasterStatus::InvalidDescription);
        invalid = draw;
        invalid.Rotation = std::numeric_limits<float32>::quiet_NaN();
        CHECK(TryBuildDrawTransform(invalid, 0, transform) == rhi::RasterStatus::InvalidDescription);
        CHECK(TryBuildDrawTransform(draw, MAX_RENDER_DRAWS, transform) == rhi::RasterStatus::InvalidDescription);
        CHECK(transform == before);
    }
}

TEST_CASE("Frame items keep draw order, colour and identity", "[world-demo][renderer]")
{
    const RenderFrame frame = ExtractExample();
    FrameItems items;
    usize count = 0;
    REQUIRE(TryBuildFrameItems(frame, {}, items, count) == rhi::RasterStatus::Ready);
    REQUIRE(count == frame.Count);
    for (usize index = 0; index < count; ++index)
    {
        const rr::SceneItem& item = items[index];
        CHECK(item.SourceId == index + 1);
        CHECK(item.Pass == rr::Layer::Opaque);
        CHECK(item.Material.Color.X == frame.Draws[index].Color[0]);
        CHECK(item.Material.Color.Y == frame.Draws[index].Color[1]);
        CHECK(item.Material.Color.Z == frame.Draws[index].Color[2]);
        CHECK(item.Material.Color.W == 1);
    }

    SECTION("Translucent or oversized frames are rejected without writing")
    {
        RenderFrame translucent = frame;
        translucent.Draws[0].Color[RGBA_ALPHA] = 0.5F;
        usize rejectedCount = count;
        const uint64 firstSource = items[0].SourceId;
        CHECK(TryBuildFrameItems(translucent, {}, items, rejectedCount) == rhi::RasterStatus::InvalidDescription);
        RenderFrame oversized = frame;
        oversized.Count = MAX_RENDER_DRAWS + 1;
        CHECK(TryBuildFrameItems(oversized, {}, items, rejectedCount) == rhi::RasterStatus::InvalidDescription);
        CHECK(rejectedCount == count);
        CHECK(items[0].SourceId == firstSource);
    }
}

TEST_CASE("The frame view and items reproduce the reference renderer's image", "[world-demo][renderer]")
{
    const RenderFrame frame = ExtractExample();
    rr::ViewDescription view;
    REQUIRE(TryBuildFrameView(frame, WIDTH, HEIGHT, view) == rhi::RasterStatus::Ready);
    CHECK(view.Depth == rr::DepthConvention::ReverseZ);
    CHECK(view.OrthographicCamera);
    FrameItems items;
    usize count = 0;
    REQUIRE(TryBuildFrameItems(frame, {}, items, count) == rhi::RasterStatus::Ready);
    PlanarCameraView planar;
    REQUIRE(TryBuildPlanarCameraView(frame.Camera,
                                     {
                                         .Width = WIDTH,
                                         .Height = HEIGHT,
                                     },
                                     {},
                                     1.0e6,
                                     planar) == ludus::gameplay::camera::CameraStatus::Success);
    const float32 aspect = static_cast<float32>(WIDTH) / static_cast<float32>(HEIGHT);
    usize compared = 0;
    for (uint32 y = 0; y < HEIGHT; y += PIXEL_STRIDE)
    {
        for (uint32 x = 0; x < WIDTH; x += PIXEL_STRIDE)
        {
            // Pixel centre in the reference shader's world mapping (top-left origin, Y down).
            const float32 u = (static_cast<float32>(x) + 0.5F) / static_cast<float32>(WIDTH);
            const float32 v = (static_cast<float32>(y) + 0.5F) / static_cast<float32>(HEIGHT);
            const math::Vector2 point{planar.Center.X + (u - 0.5F) * planar.VerticalSpan * aspect,
                                      planar.Center.Y - (v - 0.5F) * planar.VerticalSpan};
            // The view maps that world point back onto the same pixel centre.
            const math::Vector4 clip = TransformToClip(view.WorldToClip, {point.X, point.Y, 0});
            CHECK(clip.X / clip.W == Catch::Approx(u * 2 - 1).margin(CLIP_TOLERANCE));
            CHECK(clip.Y / clip.W == Catch::Approx(1 - v * 2).margin(CLIP_TOLERANCE));
            // The last covering draw is on top in both renderers.
            bool isNearEdge = false;
            usize referenceTop = MAX_RENDER_DRAWS;
            usize itemTop = MAX_RENDER_DRAWS;
            for (usize index = 0; index < count; ++index)
            {
                bool isDrawNearEdge = false;
                if (IsInsideReference(frame.Draws[index], point, isDrawNearEdge) == true)
                {
                    referenceTop = index;
                }
                isNearEdge = isNearEdge || isDrawNearEdge;
                if (IsInsideItem(items[index].Transform, point) == true)
                {
                    itemTop = index;
                }
            }
            if (isNearEdge == true)
            {
                continue;
            }
            CHECK(itemTop == referenceTop);
            ++compared;
        }
    }
    CHECK(compared > 0);
}

TEST_CASE("Later draws are nearer in reverse-Z and stay inside the depth range", "[world-demo][renderer]")
{
    const RenderFrame frame = ExtractExample();
    rr::ViewDescription view;
    REQUIRE(TryBuildFrameView(frame, WIDTH, HEIGHT, view) == rhi::RasterStatus::Ready);
    FrameItems items;
    usize count = 0;
    REQUIRE(TryBuildFrameItems(frame, {}, items, count) == rhi::RasterStatus::Ready);
    float32 previousDepth = -1;
    for (usize index = 0; index < count; ++index)
    {
        const math::Vector4 clip = TransformToClip(view.WorldToClip, items[index].Transform.Translation);
        const float32 depth = clip.Z / clip.W;
        CHECK(depth > 0);
        CHECK(depth < 1);
        CHECK(depth > previousDepth);
        previousDepth = depth;
    }
}

TEST_CASE("Frame views reject what the Renderer cannot draw", "[world-demo][renderer]")
{
    const RenderFrame frame = ExtractExample();
    rr::ViewDescription view;
    view.CullMargin = 1;
    CHECK(TryBuildFrameView(frame, 0, HEIGHT, view) == rhi::RasterStatus::NotReady);
    RenderFrame perspective = frame;
    perspective.Camera.Lens.Projection = ludus::gameplay::camera::CameraProjection::PerspectiveFinite;
    CHECK(TryBuildFrameView(perspective, WIDTH, HEIGHT, view) == rhi::RasterStatus::InvalidDescription);
    CHECK(view.CullMargin == 1);
}
