#include "internal/d3d12_device.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/lifetime.h>

#include <d3d12sdklayers.h>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace native = rhi::backend::d3d12;
namespace
{
constexpr usize BUFFER_BYTES = 16;
struct GatedDevice final
{
    rhi::DeviceHandle Device;
    rhi::SurfaceHandle Surface;
    native::ComOwner<ID3D12Fence> Gate;
    native::ComOwner<ID3D12InfoQueue> Messages;
    GatedDevice()
    {
        rhi::Shutdown();
        rhi::DeviceDescription description;
        description.Storage.TransferSlices = 1;
        REQUIRE(rhi::CreateDevice({}, {}, description, Device, Surface) == rhi::DeviceStatus::Ready);
        ID3D12InfoQueue* messagesOrNull = nullptr;
        if (SUCCEEDED(native::GetDeviceOrNull()->QueryInterface(IID_ID3D12InfoQueue,
                                                                native::GetInterfaceOutput(messagesOrNull))) == true)
        {
            Messages = native::ComOwner<ID3D12InfoQueue>(messagesOrNull);
            Messages->ClearStoredMessages();
        }
    }
    void BlockQueue()
    {
        ID3D12Fence* gateOrNull = nullptr;
        REQUIRE(SUCCEEDED(native::GetDeviceOrNull()->CreateFence(0,
                                                                 D3D12_FENCE_FLAG_NONE,
                                                                 native::GetInterfaceId(gateOrNull),
                                                                 native::GetInterfaceOutput(gateOrNull))) == true);
        Gate = native::ComOwner<ID3D12Fence>(gateOrNull);
        REQUIRE(SUCCEEDED(native::GetQueueOrNull()->Wait(Gate.Get(), 1)) == true);
    }
    void CompleteQueue()
    {
        REQUIRE(SUCCEEDED(Gate->Signal(1)) == true);
        REQUIRE(native::WaitForQueue() == true);
        REQUIRE(rhi::PollLifetime(Device) == rhi::RasterStatus::Ready);
    }
    void CheckDebugMessages()
    {
        if (Messages.Get() == nullptr)
        {
            WARN("D3D12 debug layer unavailable; transfer tests still execute");
            return;
        }
        for (uint64 index = 0; index < Messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index)
        {
            SIZE_T size = 0;
            REQUIRE(SUCCEEDED(Messages->GetMessage(index, nullptr, &size)) == true);
            Array<uint8> bytes;
            REQUIRE(bytes.TryResize(size) == true);
            D3D12_MESSAGE* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.GetData());
            REQUIRE(SUCCEEDED(Messages->GetMessage(index, message, &size)) == true);
            INFO(message->pDescription);
            CHECK(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR);
            CHECK(message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION);
        }
    }
    ~GatedDevice()
    {
        // Release the queue even when a REQUIRE aborts the test body.
        if (Gate.Get() != nullptr)
        {
            static_cast<void>(Gate->Signal(1));
        }
        static_cast<void>(rhi::DestroyDevice(Device));
    }
};
} // namespace

TEST_CASE("D4a cancellation retains pending upload and readback slices until their fence", "[rhi][windows][transfer]")
{
    GatedDevice owner;
    StaticArray<uint8, BUFFER_BYTES> bytes{};
    rhi::BufferHandle source;
    REQUIRE(rhi::CreateBuffer(owner.Device, {rhi::BufferRole::Vertex, BUFFER_BYTES}, bytes.AsSpan(), source) ==
            rhi::RasterStatus::Ready);
    owner.BlockQueue();
    rhi::UploadTicket upload;
    REQUIRE(rhi::RequestBufferUpload(owner.Device,
                                     {rhi::BufferRole::Vertex, BUFFER_BYTES},
                                     bytes.GetData(),
                                     bytes.GetSize(),
                                     upload) == rhi::RasterStatus::Pending);
    CHECK(rhi::GetStatus(owner.Device, upload) == rhi::RasterStatus::Pending);
    rhi::BufferHandle unavailable;
    CHECK(rhi::TakeUploadedBuffer(owner.Device, upload, unavailable) == rhi::RasterStatus::Pending);
    CHECK(rhi::GetStatus(owner.Device, unavailable) == rhi::RasterStatus::InvalidHandle);
    REQUIRE(rhi::Release(owner.Device, upload) == rhi::RasterStatus::Ready);
    CHECK(rhi::RequestBufferUpload(owner.Device,
                                   {rhi::BufferRole::Vertex, BUFFER_BYTES},
                                   bytes.GetData(),
                                   bytes.GetSize(),
                                   upload) == rhi::RasterStatus::CapacityExceeded);
    rhi::ReadbackTicket readback;
    REQUIRE(rhi::RequestBufferReadback(owner.Device, source, 0, BUFFER_BYTES, readback) == rhi::RasterStatus::Pending);
    StaticArray<uint8, BUFFER_BYTES> output{};
    output[0] = 99;
    CHECK(rhi::CopyReadback(owner.Device, readback, output.GetData(), output.GetSize()) == rhi::RasterStatus::Pending);
    CHECK(output[0] == 99);
    REQUIRE(rhi::Release(owner.Device, readback) == rhi::RasterStatus::Ready);
    CHECK(rhi::RequestBufferReadback(owner.Device, source, 0, BUFFER_BYTES, readback) ==
          rhi::RasterStatus::CapacityExceeded);
    REQUIRE(rhi::Destroy(owner.Device, source) == rhi::RasterStatus::Ready);
    owner.CompleteQueue();
    REQUIRE(rhi::RequestBufferUpload(owner.Device,
                                     {rhi::BufferRole::Vertex, BUFFER_BYTES},
                                     bytes.GetData(),
                                     bytes.GetSize(),
                                     upload) == rhi::RasterStatus::Pending);
    REQUIRE(native::WaitForQueue() == true);
    REQUIRE(rhi::TakeUploadedBuffer(owner.Device, upload, source) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::RequestBufferReadback(owner.Device, source, 0, BUFFER_BYTES, readback) == rhi::RasterStatus::Pending);
    REQUIRE(native::WaitForQueue() == true);
    REQUIRE(rhi::CopyReadback(owner.Device, readback, output.GetData(), output.GetSize()) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Release(owner.Device, readback) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, source) == rhi::RasterStatus::Ready);
    owner.CheckDebugMessages();
}

TEST_CASE("D4a rejected staging admission preserves texture state and permits later transfers",
          "[rhi][windows][transfer]")
{
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Storage.ResourceBytes = 512;
    REQUIRE(rhi::CreateDevice({}, {}, description, device, surface) == rhi::DeviceStatus::Ready);
    struct Cleanup final
    {
        rhi::DeviceHandle Device;
        ~Cleanup()
        {
            static_cast<void>(rhi::DestroyDevice(Device));
        }
    } cleanup{device};
    constexpr usize TEXEL_BYTES = 4;
    constexpr uint32 WIDTH = 8;
    constexpr uint32 HEIGHT = 4;
    constexpr usize TEXTURE_BYTES = WIDTH * HEIGHT * TEXEL_BYTES;
    rhi::TextureHandle texture;
    REQUIRE(
        rhi::CreateTexture(device, { .Width = WIDTH, .Height = HEIGHT, .DeferredInitialization = true }, {}, texture) ==
        rhi::RasterStatus::Ready);
    StaticArray<uint8, TEXTURE_BYTES> texels{};
    rhi::TransferTicket update;
    CHECK(rhi::RequestTextureUpdate(device,
                                    texture,
                                    {0, 0, 0, WIDTH, HEIGHT},
                                    texels.GetData(),
                                    texels.GetSize(),
                                    WIDTH * TEXEL_BYTES,
                                    update) == rhi::RasterStatus::CapacityExceeded);
    rhi::TextureResidency residency;
    REQUIRE(rhi::GetTextureResidency(device, texture, residency) == rhi::RasterStatus::Ready);
    CHECK(residency.InitializedMips == 0);
    REQUIRE(rhi::Destroy(device, texture) == rhi::RasterStatus::Ready);
    // The same null ticket and freed budget can admit a smaller transfer.
    StaticArray<uint8, BUFFER_BYTES> bytes{};
    rhi::BufferHandle buffer;
    REQUIRE(rhi::CreateBuffer(device, {rhi::BufferRole::Vertex, BUFFER_BYTES}, bytes.AsSpan(), buffer) ==
            rhi::RasterStatus::Ready);
    REQUIRE(native::WaitForQueue() == true);
    REQUIRE(rhi::PollLifetime(device) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::RequestBufferUpdate(device, buffer, 0, bytes.GetData(), bytes.GetSize(), update) ==
            rhi::RasterStatus::Pending);
    REQUIRE(native::WaitForQueue() == true);
    REQUIRE(rhi::GetStatus(device, update) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Release(device, update) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, buffer) == rhi::RasterStatus::Ready);
}

TEST_CASE("D4a offscreen submission tracks two outstanding frame slots", "[rhi][windows][transfer]")
{
    GatedDevice owner;
    REQUIRE(rhi::SetFrameTarget(owner.Device,
                                owner.Surface,
                                {
                                    .Width = 8,
                                    .Height = 8,
                                    .ShouldPresentToSurface = false,
                                }) == rhi::DeviceStatus::Ready);
    owner.BlockQueue();
    rhi::SubmissionToken first;
    rhi::SubmissionToken second;
    REQUIRE(rhi::BeginFrame(owner.Device, owner.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::EndFrame(owner.Device, first) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::BeginFrame(owner.Device, owner.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::EndFrame(owner.Device, second) == rhi::RasterStatus::Ready);
    CHECK(rhi::GetStatus(owner.Device, first) == rhi::RasterStatus::Pending);
    CHECK(rhi::GetStatus(owner.Device, second) == rhi::RasterStatus::Pending);
    owner.CompleteQueue();
    CHECK(rhi::GetStatus(owner.Device, first) == rhi::RasterStatus::Ready);
    CHECK(rhi::GetStatus(owner.Device, second) == rhi::RasterStatus::Ready);
    owner.CheckDebugMessages();
}

TEST_CASE("D4a initial buffer staging retains detached logical storage until completion", "[rhi][windows][transfer]")
{
    GatedDevice owner;
    owner.BlockQueue();
    StaticArray<uint8, BUFFER_BYTES> bytes{};
    rhi::BufferHandle buffer;
    REQUIRE(rhi::CreateBuffer(owner.Device, {rhi::BufferRole::Vertex, BUFFER_BYTES}, bytes.AsSpan(), buffer) ==
            rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, buffer) == rhi::RasterStatus::Ready);
    rhi::StorageUsage usage;
    REQUIRE(rhi::GetStorageUsage(owner.Device, usage) == rhi::RasterStatus::Ready);
    CHECK(usage.DetachedRecords == 1);
    CHECK(usage.ResourceBytes >= BUFFER_BYTES + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    owner.CompleteQueue();
    REQUIRE(rhi::GetStorageUsage(owner.Device, usage) == rhi::RasterStatus::Ready);
    CHECK(usage.DetachedRecords == 0);
    CHECK(usage.ResourceBytes == 0);
    owner.CheckDebugMessages();
}
