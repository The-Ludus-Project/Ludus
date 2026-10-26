#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/native_interop.h>

#include "internal/lifecycle.h"
#include "reference_raster.h"

#include <catch2/catch_test_macros.hpp>

namespace ludus::graphics::rhi::backend
{
extern StartupError ImmediateError;
extern foundation::uint32 PendingToken;
extern bool SelectionSupported;
extern FrameStatus NextFrame;
extern Backend ActiveKind;
} // namespace ludus::graphics::rhi::backend
using namespace ludus::foundation;
using namespace ludus::graphics::rhi;
namespace
{
struct SourceOwner final
{
    usize Retained = 0;
    usize Released = 0;
    bool Accept = true;
    static bool Retain(void* context) noexcept
    {
        auto& self = *static_cast<SourceOwner*>(context);
        ++self.Retained;
        return self.Accept;
    }
    static void Release(void* context) noexcept
    {
        ++static_cast<SourceOwner*>(context)->Released;
    }
};
NativeDeviceDescription SourceDescription(SourceOwner& source)
{
    return
    {
        .Identity =
        {
            .Api = Backend::Vulkan,
            .Instance = NativeInstance{&reference::NativeObjects[0]},
            .PhysicalDevice = NativePhysicalDevice{&reference::NativeObjects[1]},
            .Device = NativeLogicalDevice{&reference::NativeObjects[2]},
            .CommandQueue = NativeCommandQueue{&reference::NativeObjects[3]},
        },
        .Lifetime = { .Context = &source, .Retain = SourceOwner::Retain, .Release = SourceOwner::Release },
        .InstanceApiVersion = (1U << 22) | (1U << 12),
        .SerializedOwnerQueue = true,
    };
}
void PrepareReference()
{
    Shutdown();
    reference::Reset();
    backend::ImmediateError = StartupError::None;
    backend::NextFrame = FrameStatus::Ready;
    backend::ActiveKind = Backend::Vulkan;
    backend::SelectionSupported = true;
}
struct NativeSession final
{
    SourceOwner Owner;
    DeviceHandle Device;
    SurfaceHandle Surface;
    NativeDeviceLease Lease;
    NativeDeviceInfo Info;
    TextureHandle Texture;
    SubmissionToken Producer;
    NativeTextureRead Reads[3];
    NativeSession()
    {
        PrepareReference();
        REQUIRE(CreateNativeDevice({}, SourceDescription(Owner), {}, 8, 8, Device, Surface) == DeviceStatus::Pending);
        internal::Complete(backend::PendingToken, StartupError::None, {4096, 16384});
        REQUIRE(AcquireNativeDevice(Device, Lease) == RasterStatus::Ready);
        REQUIRE(GetNativeDeviceInfo(Lease, Info) == RasterStatus::Ready);
        REQUIRE(CreateTexture(Device, { .Width = 8, .Height = 8, .Attachment = true }, {}, Texture) ==
                RasterStatus::Ready);
        REQUIRE(BeginFrame(Device, Surface) == DeviceStatus::Ready);
        RasterPassDescription pass;
        pass.Color = Texture;
        REQUIRE(BeginRasterPass(Device, pass) == RasterStatus::Ready);
        REQUIRE(EndFrame(Device, Producer) == RasterStatus::Ready);
    }
    ~NativeSession()
    {
        for (auto& read : Reads)
        {
            (void)DrainNativeTextureRead(read);
        }
        (void)ReleaseNativeDevice(Lease);
        Shutdown();
    }
    NativeTextureInfo Acquire(usize i = 0)
    {
        NativeTextureInfo info;
        REQUIRE(AcquireNativeTextureRead(Lease, Info.Identity, Texture, Producer, Reads[i], info) ==
                RasterStatus::Ready);
        return info;
    }
};
} // namespace
TEST_CASE("Native admission balances source ownership and preserves rejected outputs", "[rhi][native-interop]")
{
    PrepareReference();
    SourceOwner owner;
    auto source = SourceDescription(owner);
    DeviceHandle device;
    SurfaceHandle surface;
    SECTION("Different compiled backend")
    {
        source.Identity.Api = Backend::Metal;
        CHECK(CreateNativeDevice({}, source, {}, 8, 8, device, surface) == DeviceStatus::Unsupported);
        CHECK(owner.Retained == 0);
    }
    SECTION("Missing lifetime contract")
    {
        source.Lifetime.Release = nullptr;
        CHECK(CreateNativeDevice({}, source, {}, 8, 8, device, surface) == DeviceStatus::InvalidDescription);
        CHECK(owner.Retained == 0);
    }
    SECTION("Concurrent submission is outside the profile")
    {
        source.SerializedOwnerQueue = false;
        CHECK(CreateNativeDevice({}, source, {}, 8, 8, device, surface) == DeviceStatus::Unsupported);
        CHECK(owner.Retained == 0);
    }
    SECTION("Undeclared calibrated-timestamp extension")
    {
        source.CalibratedTimestamps = static_cast<NativeCalibratedTimestamps>(3);
        CHECK(CreateNativeDevice({}, source, {}, 8, 8, device, surface) == DeviceStatus::InvalidDescription);
        CHECK(owner.Retained == 0);
    }
    SECTION("Declared calibrated timestamps are optional admission metadata")
    {
        source.CalibratedTimestamps = NativeCalibratedTimestamps::Ext;
        CHECK(CreateNativeDevice({}, source, {}, 8, 8, device, surface) == DeviceStatus::Pending);
        CHECK(owner.Retained == 1);
        Shutdown();
        CHECK(owner.Released == 1);
        device = {};
    }
    SECTION("Metal sources cannot declare Vulkan calibrated timestamps")
    {
        backend::ActiveKind = Backend::Metal;
        source.Identity =
        {
            .Api = Backend::Metal,
            .Device = source.Identity.Device,
            .CommandQueue = source.Identity.CommandQueue,
        };
        source.InstanceApiVersion = 0;
        source.CalibratedTimestamps = NativeCalibratedTimestamps::Khr;
        CHECK(CreateNativeDevice({}, source, {}, 8, 8, device, surface) == DeviceStatus::InvalidDescription);
        CHECK(owner.Retained == 0);
    }
    SECTION("Source retention rejection")
    {
        owner.Accept = false;
        CHECK(CreateNativeDevice({}, source, {}, 8, 8, device, surface) == DeviceStatus::Failed);
        CHECK(owner.Retained == 1);
        CHECK(owner.Released == 0);
    }
    SECTION("Backend startup failure releases the retained source")
    {
        backend::ImmediateError = StartupError::DeviceUnavailable;
        CHECK(CreateNativeDevice({}, source, {}, 8, 8, device, surface) == DeviceStatus::Failed);
        CHECK(owner.Retained == 1);
        CHECK(owner.Released == 1);
    }
    DeviceInfo untouched;
    CHECK(GetDeviceInfo(device, untouched) == DeviceStatus::InvalidHandle);
    Shutdown();
}
TEST_CASE("Native reads reject another logical context on the same adapter", "[rhi][native-interop]")
{
    NativeSession session;
    auto consumer = session.Info.Identity;
    uint32 another = 0;
    consumer.Device = NativeLogicalDevice{&another};
    NativeTextureInfo unchanged;
    unchanged.Width = 71;
    CHECK(AcquireNativeTextureRead(session.Lease,
                                   consumer,
                                   session.Texture,
                                   session.Producer,
                                   session.Reads[0],
                                   unchanged) == RasterStatus::Unsupported);
    CHECK(unchanged.Width == 71);
    CHECK(reference::NativePublications == 0);
    CHECK(GetStatus(session.Reads[0]) == RasterStatus::InvalidHandle);
    consumer = session.Info.Identity;
    consumer.CommandQueue = NativeCommandQueue{&another};
    CHECK(AcquireNativeTextureRead(session.Lease,
                                   consumer,
                                   session.Texture,
                                   session.Producer,
                                   session.Reads[0],
                                   unchanged) == RasterStatus::Unsupported);
    CHECK(unchanged.Width == 71);
    CHECK(reference::NativePublications == 0);
}
TEST_CASE("Native publication orders pending producers and rejects stale dependencies", "[rhi][native-interop]")
{
    NativeSession session;
    CHECK(GetStatus(session.Device, session.Producer) == RasterStatus::Pending);
    const auto info = session.Acquire();
    CHECK(info.ProducerOrdinal != 0);
    CHECK(info.VulkanImage.IsNull() == false);
    CHECK(info.VulkanLayout == 5);
    CHECK(GetStatus(session.Reads[0]) == RasterStatus::Ready);
    NativeTextureInfo preserved;
    preserved.Width = 73;
    SubmissionToken invalid;
    CHECK(AcquireNativeTextureRead(session.Lease,
                                   session.Info.Identity,
                                   session.Texture,
                                   invalid,
                                   session.Reads[1],
                                   preserved) == RasterStatus::InvalidHandle);
    CHECK(preserved.Width == 73);
    CHECK(reference::NativePublications == 1);
    CHECK(AcquireNativeTextureRead(session.Lease,
                                   session.Info.Identity,
                                   session.Texture,
                                   session.Producer,
                                   session.Reads[0],
                                   preserved) == RasterStatus::InvalidState);
}
TEST_CASE("Native read retirement excludes writes through every live consumer", "[rhi][native-interop]")
{
    NativeSession session;
    (void)session.Acquire(0);
    (void)session.Acquire(1);
    CHECK(ReleaseNativeDevice(session.Lease) == RasterStatus::InvalidState);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    RasterPassDescription pass;
    pass.Color = session.Texture;
    CHECK(BeginRasterPass(session.Device, pass) == RasterStatus::InvalidState);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
    reference::NativePending = true;
    CHECK(ReleaseNativeTextureRead(session.Reads[0]) == RasterStatus::Pending);
    NativeTextureInfo unchanged;
    unchanged.Width = 79;
    CHECK(GetNativeTextureInfo(session.Reads[0], unchanged) == RasterStatus::InvalidState);
    CHECK(unchanged.Width == 79);
    CHECK(reference::NativeRetirements == 0);
    reference::NativePending = false;
    REQUIRE(ReleaseNativeTextureRead(session.Reads[0]) == RasterStatus::Ready);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    CHECK(BeginRasterPass(session.Device, pass) == RasterStatus::InvalidState);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
    REQUIRE(DrainNativeTextureRead(session.Reads[1]) == RasterStatus::Ready);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    CHECK(BeginRasterPass(session.Device, pass) == RasterStatus::Ready);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
}
TEST_CASE("Native reads reject graph writers and mutable destination transfers", "[rhi][native-interop]")
{
    NativeSession session;
    (void)session.Acquire();
    OrderedGraph graph;
    GraphVersion initial, output;
    RasterTextureState state;
    REQUIRE(GetTextureState(session.Device, session.Texture, state) == RasterStatus::Ready);
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    REQUIRE(
        ImportGraphTexture(session.Device, graph, session.Texture, state, RasterTextureUse::ColorAttachment, initial) ==
        RasterStatus::Ready);
    REQUIRE(NextGraphVersion(session.Device, graph, initial, output) == RasterStatus::Ready);
    const GraphUse use{output, GraphAccessMode::ColorWrite};
    GraphPassDescription pass;
    pass.Name = "blocked-native-write";
    pass.Uses = &use;
    pass.UseCount = 1;
    pass.SideEffect = true;
    pass.Attachment.Color = session.Texture;
    CHECK(AddGraphPass(session.Device, graph, pass) == RasterStatus::InvalidState);
    REQUIRE(DiscardOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    const uint8 bytes[256]{};
    const TextureRegion region{ .Width = 8, .Height = 8 };
    TransferTicket update;
    CHECK(RequestTextureUpdate(session.Device, session.Texture, region, bytes, sizeof(bytes), 32, update) ==
          RasterStatus::InvalidState);
    CHECK(GetStatus(session.Device, update) == RasterStatus::InvalidHandle);
    TextureHandle source;
    REQUIRE(CreateTexture(session.Device, { .Width = 8, .Height = 8 }, {bytes, 32}, source) == RasterStatus::Ready);
    CHECK(RequestTextureCopy(session.Device, source, region, session.Texture, region, update) ==
          RasterStatus::InvalidState);
    CHECK(GetStatus(session.Device, update) == RasterStatus::InvalidHandle);
}
TEST_CASE("Replacement attachments never overwrite a leased old incarnation", "[rhi][native-interop]")
{
    NativeSession session;
    const auto old = session.Acquire();
    REQUIRE(Destroy(session.Device, session.Texture) == RasterStatus::Ready);
    TextureHandle replacement;
    REQUIRE(CreateTexture(session.Device, { .Width = 4, .Height = 4, .Attachment = true }, {}, replacement) ==
            RasterStatus::Ready);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    RasterPassDescription pass;
    pass.Color = replacement;
    REQUIRE(BeginRasterPass(session.Device, pass) == RasterStatus::Ready);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
    NativeTextureInfo retained;
    REQUIRE(GetNativeTextureInfo(session.Reads[0], retained) == RasterStatus::Ready);
    CHECK(retained.Width == 8);
    CHECK(retained.VulkanImage == old.VulkanImage);
    CHECK(retained.Revision == old.Revision);
    CHECK(reference::Destroys[static_cast<usize>(internal::RasterKind::Texture)] == 0);
}
TEST_CASE("Shutdown retains native image device and source until consumer completion", "[rhi][native-interop]")
{
    NativeSession session;
    (void)session.Acquire();
    const auto stale = session.Reads[0];
    REQUIRE(Destroy(session.Device, session.Texture) == RasterStatus::Ready);
    CHECK(reference::Destroys[static_cast<usize>(internal::RasterKind::Texture)] == 0);
    REQUIRE(DestroyDevice(session.Device) == DeviceStatus::Ready);
    CHECK(session.Owner.Released == 0);
    NativeDeviceInfo native;
    REQUIRE(GetNativeDeviceInfo(session.Lease, native) == RasterStatus::Ready);
    CHECK(native.Retiring);
    NativeTextureInfo image;
    REQUIRE(GetNativeTextureInfo(session.Reads[0], image) == RasterStatus::Ready);
    CHECK(image.Device.Retiring);
    CHECK(image.VulkanImage.IsNull() == false);
    DeviceHandle replacement;
    SurfaceHandle target;
    CHECK(CreateDevice({}, {}, {}, replacement, target) == DeviceStatus::InvalidState);
    CHECK_FALSE(Initialize({}));
    reference::NativePending = true;
    CHECK(ReleaseNativeTextureRead(session.Reads[0]) == RasterStatus::Pending);
    CHECK(reference::Destroys[static_cast<usize>(internal::RasterKind::Texture)] == 0);
    CHECK(session.Owner.Released == 0);
    REQUIRE(DrainNativeTextureRead(session.Reads[0]) == RasterStatus::Ready);
    CHECK(GetStatus(stale) == RasterStatus::InvalidHandle);
    CHECK(session.Owner.Released == 0);
    REQUIRE(ReleaseNativeDevice(session.Lease) == RasterStatus::Ready);
    CHECK(session.Owner.Released == 1);
    CHECK(reference::Destroys[static_cast<usize>(internal::RasterKind::Texture)] == 1);
    CHECK(CreateDevice({}, {}, {}, replacement, target) == DeviceStatus::Pending);
    Shutdown();
}
TEST_CASE("Native publication failure preserves descriptors and loss safely retires reads", "[rhi][native-interop]")
{
    NativeSession session;
    NativeTextureInfo image;
    image.Width = 83;
    reference::NativePublishResult = RasterStatus::OutOfMemory;
    CHECK(AcquireNativeTextureRead(session.Lease,
                                   session.Info.Identity,
                                   session.Texture,
                                   session.Producer,
                                   session.Reads[0],
                                   image) == RasterStatus::OutOfMemory);
    CHECK(image.Width == 83);
    CHECK(GetStatus(session.Reads[0]) == RasterStatus::InvalidHandle);
    reference::NativePublishResult = RasterStatus::Ready;
    (void)session.Acquire();
    reference::NativeLost = true;
    CHECK(GetStatus(session.Reads[0]) == RasterStatus::DeviceLost);
    CHECK(GetNativeTextureInfo(session.Reads[0], image) == RasterStatus::DeviceLost);
    CHECK(image.Width == 83);
    REQUIRE(ReleaseNativeTextureRead(session.Reads[0]) == RasterStatus::Ready);
    CHECK(GetStatus(session.Reads[0]) == RasterStatus::InvalidHandle);
}

TEST_CASE("Browser native bridge capability rejection preserves outputs", "[rhi][native-interop]")
{
    PrepareReference();
    backend::ActiveKind = Backend::WebGPU;
    DeviceHandle device;
    SurfaceHandle surface;
    REQUIRE(CreateDevice({}, {}, {}, device, surface) == DeviceStatus::Pending);
    internal::Complete(backend::PendingToken, StartupError::None, {4096, 16384});
    NativeInteropCapabilities capabilities;
    capabilities.MaxLeases = 71;
    CHECK(GetNativeInteropCapabilities(device, capabilities) == RasterStatus::Unsupported);
    CHECK(capabilities.MaxLeases == 71);
    NativeDeviceLease lease;
    CHECK(AcquireNativeDevice(device, lease) == RasterStatus::Unsupported);
    NativeDeviceInfo info;
    CHECK(GetNativeDeviceInfo(lease, info) == RasterStatus::InvalidHandle);
    Shutdown();
}
TEST_CASE("Terminal device loss remains observable after native retirement begins", "[rhi][native-interop]")
{
    NativeSession session;
    (void)session.Acquire();
    internal::Fail(backend::PendingToken, StartupError::DeviceLost);
    REQUIRE(GetStartup().State == StartupState::DeviceLost);
    CHECK(session.Owner.Released == 0);
    CHECK(reference::Destroys[static_cast<usize>(internal::RasterKind::Texture)] == 0);
    Shutdown();
    CHECK(GetStatus(session.Reads[0]) == RasterStatus::DeviceLost);
    NativeTextureInfo unchanged;
    unchanged.Width = 93;
    CHECK(GetNativeTextureInfo(session.Reads[0], unchanged) == RasterStatus::DeviceLost);
    CHECK(unchanged.Width == 93);
    REQUIRE(DrainNativeTextureRead(session.Reads[0]) == RasterStatus::Ready);
    REQUIRE(ReleaseNativeDevice(session.Lease) == RasterStatus::Ready);
    CHECK(session.Owner.Released == 1);
}
