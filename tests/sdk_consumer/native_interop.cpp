#include "native_interop_fixture.h"
#include "native_source.h"

#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/graphics/rhi/graph.h>

namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
constexpr logging::LogCategory LOG_NATIVE{"Native-Interop-Fixture"};
uint32 gStep = 0;
#define NATIVE_REQUIRE(expression)                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expression))                                                                                             \
        {                                                                                                              \
            LUDUS_LOG_ERROR(LOG_NATIVE, "Native bridge fixture failed at step {}", gStep);                             \
            return false;                                                                                              \
        }                                                                                                              \
    } while (false)
struct Session final
{
    native_fixture::Foreign Foreign;
    rhi::DeviceHandle Device;
    rhi::SurfaceHandle Surface;
    rhi::NativeDeviceLease Lease;
    rhi::NativeTextureRead Read;
    ~Session()
    {
        if (native_fixture::RetireConsumers(Foreign) == false)
        {
            LUDUS_LOG_ERROR(LOG_NATIVE,
                            "Foreign GPU borrower could not retire; retain its source for safe failure exit");
            return;
        }
        const auto read = rhi::DrainNativeTextureRead(Read);
        if (read != rhi::RasterStatus::Ready && read != rhi::RasterStatus::InvalidHandle)
        {
            LUDUS_LOG_ERROR(LOG_NATIVE, "Native read could not retire; retain its source for safe failure exit");
            return;
        }
        const auto device = rhi::ReleaseNativeDevice(Lease);
        if (device != rhi::RasterStatus::Ready && device != rhi::RasterStatus::InvalidHandle)
        {
            LUDUS_LOG_ERROR(LOG_NATIVE, "Native device still pinned; retain its source for safe failure exit");
            return;
        }
        rhi::Shutdown();
        native_fixture::DestroyForeign(Foreign);
    }
};
bool Run() noexcept
{
    Session session;
    gStep = 1;
    NATIVE_REQUIRE(native_fixture::CreateForeign(session.Foreign));
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    NATIVE_REQUIRE(
        rhi::CreateNativeDevice({}, session.Foreign.Description, description, 4, 4, session.Device, session.Surface) ==
        rhi::DeviceStatus::Ready);
    NATIVE_REQUIRE(native_fixture::AllowProducer(session.Foreign));
    NATIVE_REQUIRE(native_fixture::Retains(session.Foreign) == 1);
    NATIVE_REQUIRE(native_fixture::Releases(session.Foreign) == 0);
    NATIVE_REQUIRE(rhi::AcquireNativeDevice(session.Device, session.Lease) == rhi::RasterStatus::Ready);
    rhi::NativeDeviceInfo native;
    NATIVE_REQUIRE(rhi::GetNativeDeviceInfo(session.Lease, native) == rhi::RasterStatus::Ready);
    NATIVE_REQUIRE(native.Identity.Device == session.Foreign.Description.Identity.Device);
    NATIVE_REQUIRE(native.Identity.CommandQueue == session.Foreign.Description.Identity.CommandQueue);
    gStep = 2;
    rhi::RasterShaderHandle vertex;
    rhi::RasterShaderHandle fragment;
    rhi::BindingLayoutHandle layout;
    rhi::BindingSetHandle set;
    rhi::RasterPipelineHandle pipeline;
    rhi::BufferHandle indices;
    rhi::BufferHandle vertices;
    rhi::TextureHandle texture;
    NATIVE_REQUIRE(rhi::CreateRasterShader(session.Device, ludus::shaders::native_source::Vertex(), vertex) ==
                   rhi::RasterStatus::Ready);
    NATIVE_REQUIRE(rhi::CreateRasterShader(session.Device, ludus::shaders::native_source::Fragment(), fragment) ==
                   rhi::RasterStatus::Ready);
    NATIVE_REQUIRE(rhi::CreateBindingLayout(session.Device, {}, layout) == rhi::RasterStatus::Ready);
    NATIVE_REQUIRE(rhi::CreateBindingSet(session.Device, layout, {}, set) == rhi::RasterStatus::Ready);
    const rhi::RasterVertexStream streams[]{{ .Stride = native_fixture::VERTEX_STRIDE }};
    const rhi::RasterVertexAttribute attributes[]{{ .Location = 0, .Format = rhi::RasterVertexFormat::Float3 }};
    const rhi::RasterPipelineDescription state
    {
        .Vertex = vertex,
        .Fragment = fragment,
        .Layout = layout,
        .Streams = streams,
        .Attributes = attributes,
        .Target = rhi::RasterTarget::Rgba8Unorm,
    };
    NATIVE_REQUIRE(rhi::CreateRasterPipeline(session.Device, state, pipeline) == rhi::RasterStatus::Ready);
    const uint16 data[]{0, 1, 2};
    NATIVE_REQUIRE(rhi::CreateBuffer(session.Device,
                                     {rhi::BufferRole::Index16, sizeof(data)},
                                     {reinterpret_cast<const uint8*>(data), sizeof(data)},
                                     indices) == rhi::RasterStatus::Ready);
    NATIVE_REQUIRE(
        rhi::CreateBuffer(session.Device,
                          { .Role = rhi::BufferRole::Vertex, .Size = sizeof(native_fixture::TRIANGLE_POSITIONS) },
                          {reinterpret_cast<const uint8*>(native_fixture::TRIANGLE_POSITIONS),
                           sizeof(native_fixture::TRIANGLE_POSITIONS)},
                          vertices) == rhi::RasterStatus::Ready);
    NATIVE_REQUIRE(rhi::CreateTexture(session.Device, { .Width = 4, .Height = 4, .Attachment = true }, {}, texture) ==
                   rhi::RasterStatus::Ready);
    NATIVE_REQUIRE(rhi::BeginFrame(session.Device, session.Surface) == rhi::DeviceStatus::Ready);
    const rhi::RasterPassDescription pass{ .Color = texture };
    NATIVE_REQUIRE(rhi::BeginRasterPass(session.Device, pass) == rhi::RasterStatus::Ready);
    const rhi::RasterVertexSlice slices[]{{ .Buffer = vertices, .Size = sizeof(native_fixture::TRIANGLE_POSITIONS) }};
    const rhi::RasterDraw draw
    {
        .Pipeline = pipeline,
        .Bindings = set,
        .Vertices = slices,
        .Indices = indices,
        .IndexCount = 3,
        .VertexCount = 3,
    };
    NATIVE_REQUIRE(rhi::DrawIndexed(session.Device, draw) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken producer;
    NATIVE_REQUIRE(rhi::EndFrame(session.Device, producer) == rhi::RasterStatus::Ready);
    gStep = 3;
    rhi::NativeTextureInfo exported;
    auto mismatch = native.Identity;
    mismatch.CommandQueue = {};
    exported.Width = 99;
    NATIVE_REQUIRE(rhi::AcquireNativeTextureRead(session.Lease, mismatch, texture, producer, session.Read, exported) ==
                   rhi::RasterStatus::Unsupported);
    NATIVE_REQUIRE(exported.Width == 99);
    NATIVE_REQUIRE(
        rhi::AcquireNativeTextureRead(session.Lease, native.Identity, texture, producer, session.Read, exported) ==
        rhi::RasterStatus::Ready);
    NATIVE_REQUIRE(native_fixture::SubmitSample(session.Foreign, exported));
    gStep = 4;
    // Ordinary owners disappear before the foreign GPU sample completes.
    NATIVE_REQUIRE(rhi::Destroy(session.Device, texture) == rhi::RasterStatus::Ready);
    NATIVE_REQUIRE(rhi::DestroyDevice(session.Device) == rhi::DeviceStatus::Ready);
    NATIVE_REQUIRE(native_fixture::Releases(session.Foreign) == 0);
    NATIVE_REQUIRE(rhi::GetNativeTextureInfo(session.Read, exported) == rhi::RasterStatus::Ready);
    NATIVE_REQUIRE(exported.Device.Retiring);
    uint8 pixels[64]{};
    NATIVE_REQUIRE(native_fixture::ReadSample(session.Foreign, pixels));
    for (usize y = 0; y < 4; ++y)
    {
        for (usize x = 0; x < 4; ++x)
        {
            const uint8 expected[4]{static_cast<uint8>((x < 2 && y < 2) || (x >= 2 && y >= 2) ? 255 : 0),
                                    static_cast<uint8>(x >= 2 ? 255 : 0),
                                    static_cast<uint8>(y >= 2 ? 255 : 0),
                                    255};
            for (usize channel = 0; channel < 4; ++channel)
            {
                NATIVE_REQUIRE(pixels[(y * 4 + x) * 4 + channel] == expected[channel]);
            }
        }
    }
    gStep = 5;
    NATIVE_REQUIRE(rhi::DrainNativeTextureRead(session.Read) == rhi::RasterStatus::Ready);
    NATIVE_REQUIRE(rhi::ReleaseNativeDevice(session.Lease) == rhi::RasterStatus::Ready);
    NATIVE_REQUIRE(native_fixture::Releases(session.Foreign) == 1);
    LUDUS_LOG_INFO(LOG_NATIVE, "Native bridge: exact device, queued GPU sample, retirement and 16 pixel checks passed");
    return true;
}
} // namespace
int main()
{
    return Run() ? 0 : 1;
}
