#include "internal/d3d12_device.h"

#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/native_interop.h>
#include <ludus/graphics/rhi/rhi.h>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace native = ludus::graphics::rhi::backend::d3d12;

namespace
{
constexpr uint32 EXTENT = 8;

struct DeviceOwner final
{
    native::ComOwner<ID3D12Device> Device;
    native::ComOwner<ID3D12CommandQueue> Queue;
    usize Pins = 0;

    static bool Retain(void* context) noexcept
    {
        ++static_cast<DeviceOwner*>(context)->Pins;
        return true;
    }

    static void Release(void* context) noexcept
    {
        --static_cast<DeviceOwner*>(context)->Pins;
    }
};

struct ViewportSession final
{
    rhi::DeviceHandle Device;
    rhi::SurfaceHandle Surface;
    rhi::NativeDeviceLease Lease;
    rhi::NativeTextureRead Read;

    ~ViewportSession()
    {
        static_cast<void>(rhi::DrainNativeTextureRead(Read));
        static_cast<void>(rhi::ReleaseNativeDevice(Lease));
        rhi::Shutdown();
    }
};
} // namespace

TEST_CASE("Direct3D 12 authoring leases fence Qt-style reads on the borrowed queue", "[rhi][windows][d6d]")
{
    rhi::Shutdown();
    DeviceOwner source;
    ViewportSession session;
    // Bootstrap through the production adapter selection, including the CI WARP override.
    REQUIRE(rhi::CreateDevice({}, { .Width = EXTENT, .Height = EXTENT }, {}, session.Device, session.Surface) ==
            rhi::DeviceStatus::Ready);
    ID3D12Device* device = native::GetDeviceOrNull();
    ID3D12CommandQueue* queue = native::GetQueueOrNull();
    REQUIRE(device != nullptr);
    REQUIRE(queue != nullptr);
    device->AddRef();
    queue->AddRef();
    source.Device.Reset(device);
    source.Queue.Reset(queue);
    REQUIRE(rhi::DestroyDevice(session.Device) == rhi::DeviceStatus::Ready);
    session.Device = {};
    session.Surface = {};

    const rhi::NativeDeviceDescription description =
    {
        .Identity =
        {
            .Api = rhi::Backend::Direct3D12,
            .Device = rhi::NativeLogicalDevice{device},
            .CommandQueue = rhi::NativeCommandQueue{queue},
        },
        .Lifetime = { .Context = &source, .Retain = DeviceOwner::Retain, .Release = DeviceOwner::Release },
        .SerializedOwnerQueue = true,
    };
    REQUIRE(rhi::CreateNativeDevice({}, description, {}, EXTENT, EXTENT, session.Device, session.Surface) ==
            rhi::DeviceStatus::Ready);
    REQUIRE(source.Pins == 1);
    REQUIRE(rhi::AcquireNativeDevice(session.Device, session.Lease) == rhi::RasterStatus::Ready);
    rhi::NativeDeviceInfo context;
    REQUIRE(rhi::GetNativeDeviceInfo(session.Lease, context) == rhi::RasterStatus::Ready);
    CHECK(context.Identity.Device == description.Identity.Device);
    CHECK(context.Identity.CommandQueue == description.Identity.CommandQueue);

    rhi::TextureHandle texture;
    REQUIRE(rhi::CreateTexture(session.Device,
                               {
                                   .Width = EXTENT,
                                   .Height = EXTENT,
                                   .Attachment = true,
                               },
                               {},
                               texture) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::BeginFrame(session.Device, session.Surface) == rhi::DeviceStatus::Ready);
    rhi::RasterPassDescription pass;
    pass.Color = texture;
    REQUIRE(rhi::BeginRasterPass(session.Device, pass) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken producer;
    REQUIRE(rhi::EndFrame(session.Device, producer) == rhi::RasterStatus::Ready);

    rhi::NativeTextureInfo image;
    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* otherQueueOrNull = nullptr;
    const HRESULT queueCreated = device->CreateCommandQueue(&queueDescription,
                                                            native::GetInterfaceId(otherQueueOrNull),
                                                            native::GetInterfaceOutput(otherQueueOrNull));
    const native::ComOwner<ID3D12CommandQueue> otherQueue(otherQueueOrNull);
    REQUIRE(SUCCEEDED(queueCreated) == true);
    rhi::NativeDeviceIdentity differentQueue = context.Identity;
    differentQueue.CommandQueue = rhi::NativeCommandQueue{otherQueue.Get()};
    CHECK(rhi::AcquireNativeTextureRead(session.Lease, differentQueue, texture, producer, session.Read, image) ==
          rhi::RasterStatus::Unsupported);
    CHECK(image.Direct3D12Resource.IsNull() == true);
    REQUIRE(rhi::AcquireNativeTextureRead(session.Lease, context.Identity, texture, producer, session.Read, image) ==
            rhi::RasterStatus::Ready);
    REQUIRE(image.Direct3D12Resource.IsNull() == false);
    CHECK(image.Width == EXTENT);
    CHECK(image.Height == EXTENT);
    CHECK(rhi::ReleaseNativeDevice(session.Lease) == rhi::RasterStatus::InvalidState);

    // Insert a deterministic GPU stall ahead of the consumer completion marker.
    // Always unblock it before assertions can unwind into session teardown.
    ID3D12Fence* gateOrNull = nullptr;
    const HRESULT created = device->CreateFence(0,
                                                D3D12_FENCE_FLAG_NONE,
                                                native::GetInterfaceId(gateOrNull),
                                                native::GetInterfaceOutput(gateOrNull));
    native::ComOwner<ID3D12Fence> gate(gateOrNull);
    REQUIRE(SUCCEEDED(created) == true);
    REQUIRE(SUCCEEDED(queue->Wait(gate.Get(), 1)) == true);
    const rhi::RasterStatus releasing = rhi::ReleaseNativeTextureRead(session.Read);
    const rhi::RasterStatus pinned = rhi::ReleaseNativeDevice(session.Lease);
    const HRESULT signaled = gate->Signal(1);
    REQUIRE(SUCCEEDED(signaled) == true);
    CHECK(releasing == rhi::RasterStatus::Pending);
    CHECK(pinned == rhi::RasterStatus::InvalidState);
    REQUIRE(rhi::DestroyDevice(session.Device) == rhi::DeviceStatus::Ready);
    CHECK(source.Pins == 1);
    REQUIRE(rhi::DrainNativeTextureRead(session.Read) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::ReleaseNativeDevice(session.Lease) == rhi::RasterStatus::Ready);
    CHECK(source.Pins == 0);
    CHECK(device->GetDeviceRemovedReason() == S_OK);
}
