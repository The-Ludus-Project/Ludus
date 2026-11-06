// External consumer of the installed Windows runtime SDK (D6b). It prepares a
// built-in renderer recipe compiled to DXIL by the SDK's shader helper, uploads
// a mesh on a headless Direct3D 12 device and loads the bundled FreeType and
// HarfBuzz closure through Ludus::Text.
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/text/font_system.h>

#include "renderer_flat.h"

using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::usize;
using ludus::foundation::time::NANOSECONDS_PER_SECOND;
using ludus::foundation::time::NowTicks;
namespace math = ludus::foundation::math;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace text = ludus::text;

namespace
{
constexpr uint32 WIDTH = 64;
constexpr uint32 HEIGHT = 64;
/// Every asynchronous GPU step must finish well inside this bound on WARP.
constexpr uint64 TIMEOUT_NANOSECONDS = 30 * NANOSECONDS_PER_SECOND;
constexpr usize QUAD_VERTEX_COUNT = 4;
constexpr usize QUAD_INDEX_COUNT = 6;
constexpr math::Vector3 QUAD_POSITIONS[QUAD_VERTEX_COUNT]{
    {-1, -1, 0},
    {1, -1, 0},
    {1, 1, 0},
    {-1, 1, 0},
};
constexpr uint32 QUAD_INDICES[QUAD_INDEX_COUNT] = {0, 1, 2, 0, 2, 3};

/// Poll an asynchronous step to completion, servicing RHI lifetime between polls.
template <typename Poll>
rhi::RasterStatus WaitFor(rhi::DeviceHandle device, Poll poll) noexcept
{
    const uint64 started = NowTicks();
    while (NowTicks() - started < TIMEOUT_NANOSECONDS)
    {
        (void)rhi::PollLifetime(device);
        const rhi::RasterStatus status = poll();
        if (status != rhi::RasterStatus::Pending && status != rhi::RasterStatus::NotReady)
        {
            return status;
        }
    }
    return rhi::RasterStatus::Pending;
}
} // namespace

int main()
{
    text::FontSystem* fontsOrNull = nullptr;
    LUDUS_REQUIRE(text::CreateFontSystem({}, &fontsOrNull) == text::Status::Ok);
    LUDUS_REQUIRE(fontsOrNull != nullptr);
    LUDUS_REQUIRE(text::DestroyFontSystem(fontsOrNull) == text::Status::Ok);

    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    const rhi::DeviceStatus created =
        rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, device, surface);
    LUDUS_REQUIRE(created == rhi::DeviceStatus::Ready || created == rhi::DeviceStatus::Pending);
    rhi::DeviceInfo info;
    const uint64 started = NowTicks();
    while (rhi::GetDeviceInfo(device, info) == rhi::DeviceStatus::Pending && NowTicks() - started < TIMEOUT_NANOSECONDS)
    {
    }
    LUDUS_REQUIRE(rhi::GetDeviceInfo(device, info) == rhi::DeviceStatus::Ready);

    {
        // The renderer owns pipelines created from the installed recipe's DXIL.
        renderer::Renderer frame;
        const rhi::RasterStatus initialized = frame.Initialize(device,
                                                               ludus::shaders::renderer_flat::Vertex(),
                                                               ludus::shaders::renderer_flat::Fragment());
        LUDUS_REQUIRE(initialized == rhi::RasterStatus::Ready || initialized == rhi::RasterStatus::Pending);
        LUDUS_REQUIRE(WaitFor(device, [&] { return frame.Poll(); }) == rhi::RasterStatus::Ready);

        renderer::MeshDescription quad;
        quad.Positions = QUAD_POSITIONS;
        quad.VertexCount = QUAD_VERTEX_COUNT;
        quad.Indices = QUAD_INDICES;
        quad.IndexCount = QUAD_INDEX_COUNT;
        renderer::Mesh mesh;
        const rhi::RasterStatus uploaded = frame.CreateMesh(quad, mesh);
        LUDUS_REQUIRE(uploaded == rhi::RasterStatus::Ready || uploaded == rhi::RasterStatus::Pending);
        LUDUS_REQUIRE(WaitFor(device, [&] { return frame.GetStatus(mesh); }) == rhi::RasterStatus::Ready);
        frame.Reset();
    }
    LUDUS_REQUIRE(rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready);
    return 0;
}
