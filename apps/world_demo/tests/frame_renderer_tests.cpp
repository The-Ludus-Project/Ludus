#include "internal/camera_view.h"
#include "internal/frame_renderer.h"
#include "internal/world.h"
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdlib>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/lifetime.h>
using namespace ludus::world_demo;
namespace math = ludus::foundation::math;
namespace rhi = ludus::graphics::rhi;
using ludus::foundation::time::NANOSECONDS_PER_SECOND;
using ludus::foundation::time::NowTicks;
namespace
{
constexpr uint32 WIDTH = 192;
constexpr uint32 HEIGHT = 128;
constexpr uint32 BYTES_PER_PIXEL = 4;
constexpr uint32 STAGING_ROW_ALIGNMENT = 256;
// Every asynchronous GPU step must finish well inside this bound.
constexpr uint64 TIMEOUT_NANOSECONDS = 15 * NANOSECONDS_PER_SECOND;
// Rounding of the 8-bit linear scene target and the 8-bit display image.
constexpr int32 CHANNEL_TOLERANCE = 2;
// Pixel centres this close (world units) to a box edge are skipped: coverage there is
// a rasterization rule, not part of the contract.
constexpr float32 EDGE_TOLERANCE = 0.05F;
struct Gpu final
{
    rhi::DeviceHandle Device;
    rhi::SurfaceHandle Surface;
    bool HasDevice = false;
    ~Gpu()
    {
        if (HasDevice == true)
        {
            (void)rhi::DestroyDevice(Device);
        }
    }
};
// Poll an asynchronous step to completion, servicing RHI lifetime between polls.
template <typename Poll>
rhi::RasterStatus WaitFor(const Gpu& gpu, Poll poll)
{
    const uint64 started = NowTicks();
    while (NowTicks() - started < TIMEOUT_NANOSECONDS)
    {
        (void)rhi::PollLifetime(gpu.Device);
        const rhi::RasterStatus status = poll();
        if (status != rhi::RasterStatus::Pending && status != rhi::RasterStatus::NotReady)
        {
            return status;
        }
    }
    return rhi::RasterStatus::Pending;
}
RenderFrame ExtractExample()
{
    Session session;
    REQUIRE(session.RequestLoad(ExampleLevel()).Error == LevelError::None);
    while (session.GetLoadStage() != LoadStage::Ready)
    {
        REQUIRE(session.PollLoad() == Status::Success);
    }
    REQUIRE(session.Activate());
    RenderFrame frame;
    REQUIRE(session.World().Extract(0, frame, false, 1));
    return frame;
}
// Independent restatement of the Renderer's display transform for a Linear8 RasterViewport at
// exposure one: the linear colour is stored in an 8-bit target, then per-channel Reinhard, then
// the sRGB transfer function, then an 8-bit display image (renderer_output.slang).
uint8 GetDisplayCode(float32 linear, bool isAlpha)
{
    if (isAlpha == true)
    {
        return 255;
    }
    const float32 stored = std::round(linear * 255.0F) / 255.0F;
    const float32 mapped = stored / (1 + stored);
    const float32 encoded = mapped <= 0.0031308F ? 12.92F * mapped : 1.055F * std::pow(mapped, 1 / 2.4F) - 0.055F;
    return static_cast<uint8>(std::lround(encoded * 255.0F));
}
// The last box containing the pixel centre wins over the Renderer's black clear.
bool TryGetReferenceColor(const RenderFrame& frame, math::Vector2 point, uint8 (&outColor)[RGBA_CHANNELS])
{
    for (usize channel = 0; channel < RGBA_CHANNELS; ++channel)
    {
        outColor[channel] = GetDisplayCode(0, channel == RGBA_ALPHA);
    }
    for (usize index = 0; index < frame.Count; ++index)
    {
        const Draw& draw = frame.Draws[index];
        const float32 deltaX = point.X - draw.Position.X;
        const float32 deltaY = point.Y - draw.Position.Y;
        const float32 localX = std::cos(draw.Rotation) * deltaX + std::sin(draw.Rotation) * deltaY;
        const float32 localY = -std::sin(draw.Rotation) * deltaX + std::cos(draw.Rotation) * deltaY;
        if (std::abs(std::abs(localX) - draw.HalfExtent.X) < EDGE_TOLERANCE ||
            std::abs(std::abs(localY) - draw.HalfExtent.Y) < EDGE_TOLERANCE)
        {
            return false;
        }
        if (std::abs(localX) < draw.HalfExtent.X && std::abs(localY) < draw.HalfExtent.Y)
        {
            for (usize channel = 0; channel < RGBA_CHANNELS; ++channel)
            {
                outColor[channel] = GetDisplayCode(draw.Color[channel], channel == RGBA_ALPHA);
            }
        }
    }
    return true;
}
} // namespace

TEST_CASE("The player's FrameRenderer presents the Editor viewport's encoded image of the room",
          "[world-demo][renderer][gpu]")
{
    Gpu gpu;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    const rhi::DeviceStatus created =
        rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, gpu.Device, gpu.Surface);
    REQUIRE((created == rhi::DeviceStatus::Ready || created == rhi::DeviceStatus::Pending));
    gpu.HasDevice = true;
    rhi::DeviceInfo info;
    const uint64 started = NowTicks();
    while (rhi::GetDeviceInfo(gpu.Device, info) == rhi::DeviceStatus::Pending &&
           NowTicks() - started < TIMEOUT_NANOSECONDS)
    {
    }
    REQUIRE(rhi::GetDeviceInfo(gpu.Device, info) == rhi::DeviceStatus::Ready);

    FrameRenderer renderer;
    REQUIRE(WaitFor(gpu, [&] { return renderer.Prepare(gpu.Device); }) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::SetFrameTarget(gpu.Device, gpu.Surface, { .Width = WIDTH, .Height = HEIGHT }) ==
            rhi::DeviceStatus::Ready);
    rhi::SubmissionToken unused;
    CHECK(renderer.Present(gpu.Surface, unused) == rhi::RasterStatus::NotReady);

    const RenderFrame frame = ExtractExample();
    REQUIRE(renderer.Update(frame, WIDTH, HEIGHT) == rhi::RasterStatus::Ready);
    // The viewport presents once its targets and first view are Ready, as the player's next frames do.
    rhi::SubmissionToken completion;
    REQUIRE(WaitFor(gpu, [&] { return renderer.Present(gpu.Surface, completion); }) == rhi::RasterStatus::Ready);
    REQUIRE(WaitFor(gpu, [&] { return rhi::GetStatus(gpu.Device, completion); }) == rhi::RasterStatus::Ready);

    // Rejected and skipped frames keep the last drawable view, which still presents.
    RenderFrame translucent = frame;
    translucent.Draws[0].Color[RGBA_ALPHA] = 0.5F;
    CHECK(renderer.Update(translucent, WIDTH, HEIGHT) == rhi::RasterStatus::InvalidDescription);
    CHECK(renderer.Update(frame, 0, HEIGHT) == rhi::RasterStatus::NotReady);
    rhi::SubmissionToken again;
    REQUIRE(WaitFor(gpu, [&] { return renderer.Present(gpu.Surface, again); }) == rhi::RasterStatus::Ready);
    REQUIRE(WaitFor(gpu, [&] { return rhi::GetStatus(gpu.Device, again); }) == rhi::RasterStatus::Ready);

    // Read the presented display image: what the surface received, tone-mapped and encoded.
    ludus::graphics::renderer::ViewportFrame lease;
    ludus::graphics::renderer::ViewportImage image;
    REQUIRE(WaitFor(gpu, [&] { return renderer.Acquire(lease, image); }) == rhi::RasterStatus::Ready);
    REQUIRE(image.Width == WIDTH);
    REQUIRE(image.Height == HEIGHT);
    const rhi::TextureHandle target = image.Texture;

    // Read back in bands that each fit one transfer slice.
    rhi::LifetimeCapabilities capabilities;
    REQUIRE(rhi::GetLifetimeCapabilities(gpu.Device, capabilities) == rhi::RasterStatus::Ready);
    const usize rowPitch = WIDTH * BYTES_PER_PIXEL;
    const usize stagingRow = (rowPitch + STAGING_ROW_ALIGNMENT - 1) / STAGING_ROW_ALIGNMENT * STAGING_ROW_ALIGNMENT;
    const uint32 rowsPerBand = static_cast<uint32>(capabilities.TransferBytes / stagingRow);
    REQUIRE(rowsPerBand > 0);
    // Static: the image is too large to keep on the test's stack comfortably.
    static ludus::foundation::StaticArray<uint8, WIDTH * HEIGHT * BYTES_PER_PIXEL> pixels{};
    for (uint32 row = 0; row < HEIGHT; row += rowsPerBand)
    {
        const uint32 rows = row + rowsPerBand > HEIGHT ? HEIGHT - row : rowsPerBand;
        rhi::ReadbackTicket ticket;
        REQUIRE(rhi::RequestTextureReadback(gpu.Device,
                                            target,
                                            { .Mip = 0, .X = 0, .Y = row, .Width = WIDTH, .Height = rows },
                                            rowPitch,
                                            ticket) == rhi::RasterStatus::Pending);
        REQUIRE(WaitFor(gpu, [&] { return rhi::GetStatus(gpu.Device, ticket); }) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::CopyReadback(gpu.Device, ticket, pixels.GetData() + row * rowPitch, rows * rowPitch) ==
                rhi::RasterStatus::Ready);
        REQUIRE(rhi::Release(gpu.Device, ticket) == rhi::RasterStatus::Ready);
    }

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
    usize covered = 0;
    for (uint32 y = 0; y < HEIGHT; ++y)
    {
        for (uint32 x = 0; x < WIDTH; ++x)
        {
            const float32 u = (static_cast<float32>(x) + 0.5F) / static_cast<float32>(WIDTH);
            const float32 v = (static_cast<float32>(y) + 0.5F) / static_cast<float32>(HEIGHT);
            const math::Vector2 point{planar.Center.X + (u - 0.5F) * planar.VerticalSpan * aspect,
                                      planar.Center.Y - (v - 0.5F) * planar.VerticalSpan};
            uint8 expected[RGBA_CHANNELS] = {};
            if (TryGetReferenceColor(frame, point, expected) == false)
            {
                continue;
            }
            const uint8* actual = pixels.GetData() + y * rowPitch + x * BYTES_PER_PIXEL;
            bool isMatch = true;
            for (usize channel = 0; channel < RGBA_CHANNELS; ++channel)
            {
                isMatch = isMatch && std::abs(static_cast<int32>(actual[channel]) -
                                              static_cast<int32>(expected[channel])) <= CHANNEL_TOLERANCE;
            }
            INFO("pixel " << x << "," << y);
            CHECK(isMatch);
            ++compared;
            covered += expected[0] != 0 ? 1 : 0;
        }
    }
    // Scatter leaves many pixel centres within half a pixel of an edge; a quarter still covers every room feature.
    CHECK(compared > WIDTH * HEIGHT / 4);
    CHECK(covered > 0);

    REQUIRE(renderer.Release(lease) == rhi::RasterStatus::Ready);
    renderer.Reset();
}
