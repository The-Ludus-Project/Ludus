#include "internal/readback.h"
#include "renderer_selection_mask.h"
#include "renderer_selection_outline.h"
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/selection_outline.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>

namespace rhi = ludus::graphics::rhi;
namespace rr = ludus::graphics::renderer;
using namespace ludus::foundation;
using ludus::foundation::math::Matrix4;
using ludus::foundation::math::Vector3;
using ludus::foundation::math::Vector4;

namespace
{
constexpr uint32 TARGET_WIDTH = 96;
constexpr uint32 TARGET_HEIGHT = 64;
constexpr usize BYTES_PER_PIXEL = 4;
constexpr usize POLL_LIMIT = 100000;
constexpr int BLEND_TOLERANCE = 1;
constexpr uint8 FULL = 255;
constexpr uint8 HALF_CASING = 128;
constexpr float32 HALF_CLIP_EXTENT = 0.5f;
constexpr float32 FAR_OFFSET_CLIP = 5.0f;

struct Scene final
{
    rhi::DeviceHandle Device;
    rhi::SurfaceHandle Surface;
    rr::SelectionOutlineRenderer Outline;
    bool IsSkipped = false;
};

struct ShutdownGuard final
{
    ShutdownGuard()
    {
        rhi::Shutdown();
    }
    ~ShutdownGuard()
    {
        rhi::Shutdown();
    }
    ShutdownGuard(const ShutdownGuard&) = delete;
    ShutdownGuard& operator=(const ShutdownGuard&) = delete;
};

rr::SelectionOutlineView MakeView(float32 translationClipX)
{
    rr::SelectionOutlineView view;
    view.MeshToClip = Matrix4::Identity();
    view.MeshToClip.Columns[3].X = translationClipX;
    view.Width = TARGET_WIDTH;
    view.Height = TARGET_HEIGHT;
    view.Style.OutlineColor = Vector4{1, 0, 0, 1};
    view.Style.CasingColor = Vector4{0, 0, 1, 0.5f};
    return view;
}

bool Start(Scene& scene)
{
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    rhi::StartupInfo failure;
    const auto started = rhi::CreateDevice({},
                                           { .Width = TARGET_WIDTH, .Height = TARGET_HEIGHT },
                                           description,
                                           scene.Device,
                                           scene.Surface,
                                           &failure);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
    {
        scene.IsSkipped = true;
        return false;
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    const auto initialized = scene.Outline.Initialize(scene.Device,
                                                      ludus::shaders::renderer_selection_mask::Vertex(),
                                                      ludus::shaders::renderer_selection_mask::Fragment(),
                                                      ludus::shaders::renderer_selection_outline::Vertex(),
                                                      ludus::shaders::renderer_selection_outline::Fragment());
    REQUIRE((initialized == rhi::RasterStatus::Ready || initialized == rhi::RasterStatus::Pending));
    auto setup = initialized;
    for (usize i = 0; i < POLL_LIMIT && setup == rhi::RasterStatus::Pending; ++i)
    {
        setup = scene.Outline.Poll();
    }
    REQUIRE(setup == rhi::RasterStatus::Ready);
    REQUIRE(scene.Outline.IsReady());
    return true;
}

/// An axis-aligned rectangle with the given clip-space bounds.
void SetRectangleMesh(rr::SelectionOutlineRenderer& outline, float32 minX, float32 minY, float32 maxX, float32 maxY)
{
    const Vector3 positions[] = {{minX, minY, 0}, {maxX, minY, 0}, {maxX, maxY, 0}, {minX, maxY, 0}};
    const uint32 indices[] = {0, 1, 2, 0, 2, 3};
    const auto status = outline.SetMesh(BorrowedView<const Vector3>(positions), BorrowedView<const uint32>(indices));
    REQUIRE((status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending));
    bool isReady = false;
    for (usize i = 0; i < POLL_LIMIT && isReady == false; ++i)
    {
        isReady = outline.GetMeshStatus() == rhi::RasterStatus::Ready;
    }
    REQUIRE(isReady);
}

/// A centred square, half of the target wide and high in clip space.
void SetSquareMesh(rr::SelectionOutlineRenderer& outline)
{
    SetRectangleMesh(outline, -HALF_CLIP_EXTENT, -HALF_CLIP_EXTENT, HALF_CLIP_EXTENT, HALF_CLIP_EXTENT);
}

rhi::RasterStatus PrepareReady(rr::SelectionOutlineRenderer& outline,
                               const rr::SelectionOutlineView& view,
                               rr::PreparedSelectionOutline& outPrepared)
{
    const auto prepared = outline.Prepare(view, outPrepared);
    if (prepared != rhi::RasterStatus::Ready && prepared != rhi::RasterStatus::Pending)
    {
        return prepared;
    }
    for (usize i = 0; i < POLL_LIMIT; ++i)
    {
        if (outline.GetStatus(outPrepared) == rhi::RasterStatus::Ready)
        {
            return rhi::RasterStatus::Ready;
        }
    }
    return rhi::RasterStatus::Pending;
}

void DrawFrame(Scene& scene,
               const rr::PreparedSelectionOutline& prepared,
               uint8 (&outImage)[TARGET_WIDTH * TARGET_HEIGHT * BYTES_PER_PIXEL])
{
    REQUIRE(rhi::BeginFrame(scene.Device, scene.Surface) == rhi::DeviceStatus::Ready);
    const rhi::RasterPassDescription clear;
    REQUIRE(rhi::BeginRasterPass(scene.Device, clear) == rhi::RasterStatus::Ready);
    REQUIRE(scene.Outline.Draw(prepared, clear) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(scene.Device, completion) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::backend::ReadHeadlessPixels(outImage));
}

bool IsNear(const uint8 (&image)[TARGET_WIDTH * TARGET_HEIGHT * BYTES_PER_PIXEL],
            uint32 x,
            uint32 y,
            uint8 red,
            uint8 green,
            uint8 blue)
{
    const usize offset = (static_cast<usize>(y) * TARGET_WIDTH + x) * BYTES_PER_PIXEL;
    return std::abs(static_cast<int>(image[offset]) - static_cast<int>(red)) <= BLEND_TOLERANCE &&
           std::abs(static_cast<int>(image[offset + 1]) - static_cast<int>(green)) <= BLEND_TOLERANCE &&
           std::abs(static_cast<int>(image[offset + 2]) - static_cast<int>(blue)) <= BLEND_TOLERANCE;
}
} // namespace

TEST_CASE("Selection outline strokes the projected silhouette with a casing and leaves the rest of the target",
          "[renderer][selection-outline][gpu]")
{
    ShutdownGuard guard;
    Scene scene;
    if (Start(scene) == false)
    {
        SKIP("No Metal adapter on this host");
    }
    SetSquareMesh(scene.Outline);
    rr::PreparedSelectionOutline prepared;
    REQUIRE(PrepareReady(scene.Outline, MakeView(0), prepared) == rhi::RasterStatus::Ready);
    uint8 image[TARGET_WIDTH * TARGET_HEIGHT * BYTES_PER_PIXEL];
    DrawFrame(scene, prepared, image);
    // The square covers x in [24, 72) and y in [16, 48). A two pixel stroke centred on each edge fills the pixel
    // either side; the four pixel casing adds the next pixel out and in.
    constexpr uint32 MIDDLE_ROW = 32;
    constexpr uint32 LEFT_EDGE = 24;
    constexpr uint32 RIGHT_EDGE = 72;
    CHECK(IsNear(image, LEFT_EDGE - 1, MIDDLE_ROW, FULL, 0, 0));
    CHECK(IsNear(image, LEFT_EDGE, MIDDLE_ROW, FULL, 0, 0));
    CHECK(IsNear(image, LEFT_EDGE - 2, MIDDLE_ROW, 0, 0, HALF_CASING));
    CHECK(IsNear(image, LEFT_EDGE + 1, MIDDLE_ROW, 0, 0, HALF_CASING));
    CHECK(IsNear(image, LEFT_EDGE - 3, MIDDLE_ROW, 0, 0, 0));
    CHECK(IsNear(image, LEFT_EDGE + 2, MIDDLE_ROW, 0, 0, 0));
    CHECK(IsNear(image, RIGHT_EDGE - 1, MIDDLE_ROW, FULL, 0, 0));
    CHECK(IsNear(image, RIGHT_EDGE, MIDDLE_ROW, FULL, 0, 0));
    CHECK(IsNear(image, RIGHT_EDGE + 1, MIDDLE_ROW, 0, 0, HALF_CASING));
    // Top edge, and the interior and far corners, keep the cleared colour.
    CHECK(IsNear(image, 48, 15, FULL, 0, 0));
    CHECK(IsNear(image, 48, 16, FULL, 0, 0));
    CHECK(IsNear(image, 48, MIDDLE_ROW, 0, 0, 0));
    CHECK(IsNear(image, 2, 2, 0, 0, 0));
    CHECK(IsNear(image, TARGET_WIDTH - 3, TARGET_HEIGHT - 3, 0, 0, 0));
    REQUIRE(scene.Outline.Release(prepared) == rhi::RasterStatus::Ready);
}

TEST_CASE("Selection outline keeps the image orientation: clip +y is the top row and +x the left-to-right axis",
          "[renderer][selection-outline][gpu]")
{
    ShutdownGuard guard;
    Scene scene;
    if (Start(scene) == false)
    {
        SKIP("No Metal adapter on this host");
    }
    // Upper-left quadrant: pixels x in [12, 36) and y in [8, 24) of the 96 by 64 target, top row first.
    constexpr float32 NEAR_EDGE_CLIP = 0.25f;
    constexpr float32 FAR_EDGE_CLIP = 0.75f;
    SetRectangleMesh(scene.Outline, -FAR_EDGE_CLIP, NEAR_EDGE_CLIP, -NEAR_EDGE_CLIP, FAR_EDGE_CLIP);
    rr::PreparedSelectionOutline prepared;
    REQUIRE(PrepareReady(scene.Outline, MakeView(0), prepared) == rhi::RasterStatus::Ready);
    uint8 image[TARGET_WIDTH * TARGET_HEIGHT * BYTES_PER_PIXEL];
    DrawFrame(scene, prepared, image);
    constexpr uint32 LEFT_EDGE = 12;
    constexpr uint32 RIGHT_EDGE = 36;
    constexpr uint32 TOP_EDGE = 8;
    constexpr uint32 BOTTOM_EDGE = 24;
    constexpr uint32 MIDDLE_COLUMN = 24;
    constexpr uint32 MIDDLE_ROW = 16;
    CHECK(IsNear(image, LEFT_EDGE, MIDDLE_ROW, FULL, 0, 0));
    CHECK(IsNear(image, RIGHT_EDGE - 1, MIDDLE_ROW, FULL, 0, 0));
    CHECK(IsNear(image, MIDDLE_COLUMN, TOP_EDGE, FULL, 0, 0));
    CHECK(IsNear(image, MIDDLE_COLUMN, BOTTOM_EDGE - 1, FULL, 0, 0));
    // The mirrored positions, lower right, stay clear.
    CHECK(IsNear(image, TARGET_WIDTH - LEFT_EDGE, TARGET_HEIGHT - MIDDLE_ROW, 0, 0, 0));
    CHECK(IsNear(image, TARGET_WIDTH - MIDDLE_COLUMN, TARGET_HEIGHT - TOP_EDGE, 0, 0, 0));
    CHECK(IsNear(image, MIDDLE_COLUMN, TARGET_HEIGHT - TOP_EDGE, 0, 0, 0));
    CHECK(IsNear(image, TARGET_WIDTH - MIDDLE_COLUMN, TOP_EDGE, 0, 0, 0));
    REQUIRE(scene.Outline.Release(prepared) == rhi::RasterStatus::Ready);
}

TEST_CASE("Selection outline draws nothing for a mesh entirely outside the target",
          "[renderer][selection-outline][gpu]")
{
    ShutdownGuard guard;
    Scene scene;
    if (Start(scene) == false)
    {
        SKIP("No Metal adapter on this host");
    }
    SetSquareMesh(scene.Outline);
    rr::PreparedSelectionOutline prepared;
    REQUIRE(PrepareReady(scene.Outline, MakeView(FAR_OFFSET_CLIP), prepared) == rhi::RasterStatus::Ready);
    uint8 image[TARGET_WIDTH * TARGET_HEIGHT * BYTES_PER_PIXEL];
    DrawFrame(scene, prepared, image);
    bool isAllClear = true;
    for (usize i = 0; i < TARGET_WIDTH * TARGET_HEIGHT; ++i)
    {
        isAllClear = isAllClear && image[i * BYTES_PER_PIXEL] == 0 && image[i * BYTES_PER_PIXEL + 1] == 0 &&
                     image[i * BYTES_PER_PIXEL + 2] == 0;
    }
    CHECK(isAllClear);
    REQUIRE(scene.Outline.Release(prepared) == rhi::RasterStatus::Ready);
}

TEST_CASE("Selection outline rejects invalid meshes, views and a fifth version", "[renderer][selection-outline][gpu]")
{
    ShutdownGuard guard;
    Scene scene;
    if (Start(scene) == false)
    {
        SKIP("No Metal adapter on this host");
    }
    rr::PreparedSelectionOutline prepared;
    CHECK(scene.Outline.Prepare(MakeView(0), prepared) == rhi::RasterStatus::NotReady);
    const Vector3 positions[] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    const uint32 outOfRange[] = {0, 1, 3};
    const uint32 notTriangles[] = {0, 1};
    CHECK(scene.Outline.SetMesh(BorrowedView<const Vector3>(positions), BorrowedView<const uint32>(outOfRange)) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(scene.Outline.SetMesh(BorrowedView<const Vector3>(positions), BorrowedView<const uint32>(notTriangles)) ==
          rhi::RasterStatus::InvalidDescription);
    const Vector3 nonFinite[] = {{0, 0, 0}, {1, 0, 0}, {0, std::nanf(""), 0}};
    const uint32 triangle[] = {0, 1, 2};
    CHECK(scene.Outline.SetMesh(BorrowedView<const Vector3>(nonFinite), BorrowedView<const uint32>(triangle)) ==
          rhi::RasterStatus::InvalidDescription);
    SetSquareMesh(scene.Outline);
    auto emptyExtent = MakeView(0);
    emptyExtent.Width = 0;
    CHECK(scene.Outline.Prepare(emptyExtent, prepared) == rhi::RasterStatus::InvalidDescription);
    auto thinnerCasing = MakeView(0);
    thinnerCasing.Style.CasingWidthPixels = thinnerCasing.Style.OutlineWidthPixels - 1;
    CHECK(scene.Outline.Prepare(thinnerCasing, prepared) == rhi::RasterStatus::InvalidDescription);
    auto tooWide = MakeView(0);
    tooWide.Style.CasingWidthPixels = rr::SelectionOutlineRenderer::MAX_STROKE_WIDTH_PIXELS + 1;
    CHECK(scene.Outline.Prepare(tooWide, prepared) == rhi::RasterStatus::InvalidDescription);
    constexpr usize VERSION_LIMIT = 4;
    rr::PreparedSelectionOutline versions[VERSION_LIMIT];
    for (auto& version : versions)
    {
        const auto status = scene.Outline.Prepare(MakeView(0), version);
        REQUIRE((status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending));
    }
    rr::PreparedSelectionOutline fifth;
    CHECK(scene.Outline.Prepare(MakeView(0), fifth) == rhi::RasterStatus::CapacityExceeded);
    for (auto& version : versions)
    {
        CHECK(scene.Outline.Release(version) == rhi::RasterStatus::Ready);
    }
}

TEST_CASE("Selection outline strokes an RGBA8 attachment texture the same way as the surface",
          "[renderer][selection-outline][gpu]")
{
    ShutdownGuard guard;
    Scene scene;
    if (Start(scene) == false)
    {
        SKIP("No Metal adapter on this host");
    }
    SetSquareMesh(scene.Outline);
    rr::PreparedSelectionOutline prepared;
    REQUIRE(PrepareReady(scene.Outline, MakeView(0), prepared) == rhi::RasterStatus::Ready);
    rhi::TextureDescription description;
    description.Width = TARGET_WIDTH;
    description.Height = TARGET_HEIGHT;
    description.Attachment = true;
    rhi::TextureHandle texture;
    const auto created = rhi::CreateTexture(scene.Device, description, {}, texture);
    REQUIRE((created == rhi::RasterStatus::Ready || created == rhi::RasterStatus::Pending));
    REQUIRE(rhi::BeginFrame(scene.Device, scene.Surface) == rhi::DeviceStatus::Ready);
    rhi::RasterPassDescription pass;
    pass.Color = texture;
    pass.DepthLoad = rhi::RasterLoad::Discard;
    REQUIRE(rhi::BeginRasterPass(scene.Device, pass) == rhi::RasterStatus::Ready);
    REQUIRE(scene.Outline.Draw(prepared, pass) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::EndFrame(scene.Device) == rhi::DeviceStatus::Ready);
    constexpr usize ROW_PITCH = TARGET_WIDTH * BYTES_PER_PIXEL;
    rhi::ReadbackTicket ticket;
    REQUIRE(
        rhi::RequestTextureReadback(scene.Device, texture, {0, 0, 0, TARGET_WIDTH, TARGET_HEIGHT}, ROW_PITCH, ticket) ==
        rhi::RasterStatus::Pending);
    bool isReady = false;
    for (usize i = 0; i < POLL_LIMIT && isReady == false; ++i)
    {
        isReady = rhi::GetStatus(scene.Device, ticket) == rhi::RasterStatus::Ready;
    }
    REQUIRE(isReady);
    uint8 image[TARGET_WIDTH * TARGET_HEIGHT * BYTES_PER_PIXEL];
    REQUIRE(rhi::CopyReadback(scene.Device, ticket, image, sizeof(image)) == rhi::RasterStatus::Ready);
    constexpr uint32 MIDDLE_ROW = 32;
    constexpr uint32 LEFT_EDGE = 24;
    CHECK(IsNear(image, LEFT_EDGE - 1, MIDDLE_ROW, FULL, 0, 0));
    CHECK(IsNear(image, LEFT_EDGE, MIDDLE_ROW, FULL, 0, 0));
    CHECK(IsNear(image, LEFT_EDGE - 2, MIDDLE_ROW, 0, 0, HALF_CASING));
    CHECK(IsNear(image, 48, MIDDLE_ROW, 0, 0, 0));
    CHECK(IsNear(image, 2, 2, 0, 0, 0));
    REQUIRE(rhi::Release(scene.Device, ticket) == rhi::RasterStatus::Ready);
    REQUIRE(scene.Outline.Release(prepared) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(scene.Device, texture) == rhi::RasterStatus::Ready);
}
