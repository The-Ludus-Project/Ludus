#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/graphics/rhi/adapter.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/gpu_scope.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/measurement.h>
#include <ludus/graphics/rhi/native_interop.h>
#include <ludus/graphics/rhi/render.h>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/platform/base/window.h>
#include <ludus/platform/native_window.h>

#include "internal/readback.h"

#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string_view>

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace
{
struct Guard final
{
    Guard() noexcept
    {
        rhi::Shutdown();
    }
    ~Guard() noexcept
    {
        rhi::Shutdown();
    }
};
constexpr std::string_view MSL = R"MSL(
#include <metal_stdlib>
using namespace metal;
vertex float4 vertexMain(uint index [[vertex_id]]) {
    float2 p = index == 0 ? float2(-1,-1) : index == 1 ? float2(3,-1) : float2(-1,3);
    return float4(p,0,1);
}
fragment float4 fragmentMain(constant float4& color [[buffer(0)]]) { return color; }
fragment float4 badBinding(constant float4& color [[buffer(1)]]) { return color; }
fragment float4 greenMain() { return float4(0,1,0,1); }
)MSL";
void RequireDevice()
{
    if (rhi::Start({}, { .Width = 17, .Height = 9 }) != rhi::StartStatus::Ready)
    {
        REQUIRE(rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable);
        SKIP("No Metal adapter on this host");
    }
}
rhi::ShaderDescription Description(rhi::ShaderStage stage, std::string_view entry, usize size = 0)
{
    rhi::ShaderDescription result;
    result.Stage = stage;
    result.UniformSize = size;
    result.Msl = MSL;
    result.MslEntry = entry;
    return result;
}
} // namespace

TEST_CASE("Metal validates the native startup policy before device acquisition", "[rhi][macos]")
{
    Guard guard;
    CHECK(rhi::Start({}, {}, rhi::BackendSelection::WebGPU) == rhi::StartStatus::Failed);
    CHECK(rhi::GetStartup().Error == rhi::StartupError::BackendUnavailable);
    rhi::Shutdown();
    rhi::WindowInfo invalid;
    invalid.System = ludus::platform::WindowSystem::Cocoa;
    CHECK(rhi::Start({}, invalid) == rhi::StartStatus::Failed);
    CHECK(rhi::GetStartup().Error == rhi::StartupError::InvalidWindow);
    CHECK(rhi::Start({}, {}) == rhi::StartStatus::Busy);
}
TEST_CASE("Metal clears a headless target and restarts after shutdown", "[rhi][macos][gpu]")
{
    Guard guard;
    RequireDevice();
    CHECK(rhi::GetStartup().SelectedBackend == rhi::Backend::Metal);
    REQUIRE(rhi::SetFrameTarget({ .Width = 17, .Height = 9, .Red = 0.25, .Green = 0.5, .Blue = 0.75, .Alpha = 1 }) ==
            rhi::FrameStatus::Ready);
    REQUIRE(rhi::BeginFrameStatus() == rhi::FrameStatus::Ready);
    CHECK(rhi::GetFrameInfo().Width == 17);
    CHECK(rhi::GetFrameInfo().Encoding == rhi::SurfaceEncoding::Unorm);
    REQUIRE(rhi::EndFrameStatus() == rhi::FrameStatus::Ready);
    uint8 pixels[17 * 9 * 4]{};
    REQUIRE(rhi::backend::ReadHeadlessPixels(pixels));
    for (usize i = 0; i < sizeof(pixels); i += 4)
    {
        CHECK(pixels[i] == 64);
        CHECK(pixels[i + 1] == 128);
        CHECK(pixels[i + 2] == 191);
        CHECK(pixels[i + 3] == 255);
    }
    CHECK(rhi::SetFrameTarget({ .Width = 0, .Height = 9 }) == rhi::FrameStatus::Ready);
    CHECK(rhi::BeginFrameStatus() == rhi::FrameStatus::Skipped);
    CHECK(rhi::SetFrameTarget({ .Width = 17, .Height = 9, .Red = -1 }) == rhi::FrameStatus::Failed);
    rhi::Shutdown();
    REQUIRE(rhi::Start({}, { .Width = 17, .Height = 9 }) == rhi::StartStatus::Ready);
    REQUIRE(rhi::GetStartup().State == rhi::StartupState::Ready);
    REQUIRE(rhi::BeginFrameStatus() == rhi::FrameStatus::Ready);
    REQUIRE(rhi::EndFrameStatus() == rhi::FrameStatus::Ready);
    rhi::Shutdown();
    REQUIRE(rhi::Start({}, { .Width = 17, .Height = 9 }) == rhi::StartStatus::Ready);
    REQUIRE(rhi::BeginFrameStatus() == rhi::FrameStatus::Ready);
    // Abort an unsubmitted encoder.
    rhi::Shutdown();
    REQUIRE(rhi::Start({}, {}) == rhi::StartStatus::Ready);
}
TEST_CASE("Metal verifies compiled stages and actual pipeline bindings", "[rhi][macos][gpu]")
{
    Guard guard;
    RequireDevice();
    rhi::ShaderHandle vertex, fragment, invalid;
    rhi::UniformHandle uniform;
    rhi::PipelineHandle pipeline;
    REQUIRE(rhi::CreateShader(Description(rhi::ShaderStage::Vertex, "vertexMain"), vertex) ==
            rhi::ResourceStatus::Ready);
    auto bad = Description(rhi::ShaderStage::Vertex, "fragmentMain");
    REQUIRE(rhi::CreateShader(bad, invalid) == rhi::ResourceStatus::Failed);
    REQUIRE(rhi::Destroy(invalid) == rhi::ResourceStatus::Ready);
    invalid = {};
    bad.Msl = "invalid shader";
    REQUIRE(rhi::CreateShader(bad, invalid) == rhi::ResourceStatus::Failed);
    REQUIRE(rhi::Destroy(invalid) == rhi::ResourceStatus::Ready);
    invalid = {};
    REQUIRE(rhi::CreateUniform(16, uniform) == rhi::ResourceStatus::Ready);
    REQUIRE(rhi::CreateShader(Description(rhi::ShaderStage::Fragment, "badBinding", 16), fragment) ==
            rhi::ResourceStatus::Ready);
    REQUIRE(rhi::CreatePipeline({vertex, fragment, uniform}, pipeline) == rhi::ResourceStatus::Failed);
    REQUIRE(rhi::Destroy(pipeline) == rhi::ResourceStatus::Ready);
    pipeline = {};
    REQUIRE(rhi::Destroy(fragment) == rhi::ResourceStatus::Ready);
    fragment = {};
    // Under-reporting occupied bytes must fail even though the allocated buffer fits.
    REQUIRE(rhi::CreateShader(Description(rhi::ShaderStage::Fragment, "fragmentMain", 0), fragment) ==
            rhi::ResourceStatus::Ready);
    REQUIRE(rhi::CreatePipeline({vertex, fragment, uniform}, pipeline) == rhi::ResourceStatus::Failed);
    REQUIRE(rhi::Destroy(pipeline) == rhi::ResourceStatus::Ready);
    pipeline = {};
    REQUIRE(rhi::Destroy(fragment) == rhi::ResourceStatus::Ready);
    fragment = {};
    REQUIRE(rhi::CreateShader(Description(rhi::ShaderStage::Fragment, "fragmentMain", 16), fragment) ==
            rhi::ResourceStatus::Ready);
    REQUIRE(rhi::CreatePipeline({vertex, fragment, uniform}, pipeline) == rhi::ResourceStatus::Ready);
    CHECK(rhi::Destroy(uniform) == rhi::ResourceStatus::InUse);
    const float32 color[] = {0.25F, 0.5F, 0.75F, 1};
    const ludus::foundation::BorrowedView<const uint8> bytes(reinterpret_cast<const uint8*>(color), sizeof(color));
    REQUIRE(rhi::BeginFrameStatus() == rhi::FrameStatus::Ready);
    CHECK(rhi::DrawFullscreen(pipeline) == rhi::ResourceStatus::InvalidState);
    REQUIRE(rhi::UpdateUniform(uniform, bytes) == rhi::ResourceStatus::Ready);
    REQUIRE(rhi::DrawFullscreen(pipeline) == rhi::ResourceStatus::Ready);
    CHECK(rhi::DrawFullscreen(pipeline) == rhi::ResourceStatus::InvalidState);
    REQUIRE(rhi::EndFrameStatus() == rhi::FrameStatus::Ready);
    // Submitted buffers retain resources until completion, even after handles die.
    REQUIRE(rhi::Destroy(pipeline) == rhi::ResourceStatus::Ready);
    REQUIRE(rhi::Destroy(uniform) == rhi::ResourceStatus::Ready);
    REQUIRE(rhi::Destroy(vertex) == rhi::ResourceStatus::Ready);
    REQUIRE(rhi::Destroy(fragment) == rhi::ResourceStatus::Ready);
    uint8 pixels[17 * 9 * 4]{};
    REQUIRE(rhi::backend::ReadHeadlessPixels(pixels));
    CHECK(pixels[0] == 64);
    CHECK(pixels[1] == 128);
    CHECK(pixels[2] == 191);
    CHECK(pixels[3] == 255);
    rhi::Shutdown();
    CHECK(rhi::GetStatus(pipeline) == rhi::ResourceStatus::InvalidHandle);
}
TEST_CASE("Metal presents through a borrowed Cocoa view and restores its layer", "[rhi][macos][cocoa][gpu]")
{
    if (std::getenv("LUDUS_TEST_COCOA") == nullptr)
    {
        SKIP("Set LUDUS_TEST_COCOA=1 with a WindowServer session");
    }
    @autoreleasepool
    {
        Guard guard;
        RequireDevice();
        rhi::Shutdown();
        using namespace ludus::platform;
        WindowManager manager;
        REQUIRE(manager.Initialize({}));
        UniquePtr<Window> window;
        REQUIRE(manager.CreateWindow({ .Name = "Metal RHI test", .Width = 160, .Height = 120 }, window));
        // Destroy the connection before the borrowed view on assertion failure.
        Guard connectionGuard;
        const auto native = window->GetNativeWindowInfo();
        NSView* view = (__bridge NSView*)native.CocoaView;
        const BOOL wantsLayer = view.wantsLayer;
        CALayer* previous = view.layer;
        REQUIRE(rhi::Start({}, native) == rhi::StartStatus::Ready);
        CHECK([view.layer isKindOfClass:[CAMetalLayer class]]);
        REQUIRE(rhi::SetFrameTarget({ .Width = native.Width, .Height = native.Height, .Green = 0.5, .Alpha = 1 }) ==
                rhi::FrameStatus::Ready);
        REQUIRE(rhi::BeginFrameStatus() == rhi::FrameStatus::Ready);
        CHECK(rhi::GetFrameInfo().Width == native.Width);
        REQUIRE(rhi::EndFrameStatus() == rhi::FrameStatus::Ready);
        rhi::Shutdown();
        CHECK(view.wantsLayer == wantsLayer);
        CHECK(view.layer == previous);
    }
}

TEST_CASE("Metal detaches a borrowed Cocoa view before retained device retirement", "[rhi][macos][cocoa][gpu]")
{
    if (std::getenv("LUDUS_TEST_COCOA") == nullptr)
    {
        SKIP("Set LUDUS_TEST_COCOA=1 with a WindowServer session");
    }
    @autoreleasepool
    {
        Guard guard;
        RequireDevice();
        rhi::Shutdown();
        ludus::platform::WindowManager manager;
        REQUIRE(manager.Initialize({}));
        UniquePtr<ludus::platform::Window> window;
        REQUIRE(manager.CreateWindow({ .Name = "Native bridge retirement", .Width = 160, .Height = 120 }, window));
        struct LeaseGuard final
        {
            rhi::NativeDeviceLease Lease;
            ~LeaseGuard() noexcept
            {
                rhi::Shutdown();
                static_cast<void>(rhi::ReleaseNativeDevice(Lease));
            }
        } retained;
        const auto native = window->GetNativeWindowInfo();
        __weak NSView* borrowed = (__bridge NSView*)native.CocoaView;
        const BOOL wantsLayer = borrowed.wantsLayer;
        CALayer* previous = borrowed.layer;
        rhi::DeviceHandle device;
        rhi::SurfaceHandle surface;
        REQUIRE(rhi::CreateDevice({}, native, {}, device, surface) == rhi::DeviceStatus::Ready);
        REQUIRE(rhi::AcquireNativeDevice(device, retained.Lease) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready);
        CHECK(borrowed.wantsLayer == wantsLayer);
        CHECK(borrowed.layer == previous);
        window.Reset();
        rhi::NativeDeviceInfo info;
        REQUIRE(rhi::GetNativeDeviceInfo(retained.Lease, info) == rhi::RasterStatus::Ready);
        CHECK(info.Retiring);
        CHECK(info.Identity.Device.IsNull() == false);
        REQUIRE(rhi::ReleaseNativeDevice(retained.Lease) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::Start({}, {}) == rhi::StartStatus::Ready);
    }
}

TEST_CASE("R7 optional Metal probe survives skipped acquisition, resize and pending teardown",
          "[rhi][macos][cocoa][measurement][gpu]")
{
    if (std::getenv("LUDUS_TEST_COCOA") == nullptr)
    {
        SKIP("Set LUDUS_TEST_COCOA=1 with a WindowServer session");
    }
    @autoreleasepool
    {
        Guard guard;
        RequireDevice();
        rhi::Shutdown();
        ludus::platform::WindowManager manager;
        REQUIRE(manager.Initialize({}));
        UniquePtr<ludus::platform::Window> window;
        REQUIRE(manager.CreateWindow({ .Name = "R7 Metal qualification", .Width = 160, .Height = 120 }, window));
        Guard connectionGuard;
        rhi::DeviceHandle device;
        rhi::SurfaceHandle surface;
        rhi::DeviceDescription description;
        description.EnableGpuTiming = true;
        REQUIRE(rhi::CreateDevice({}, window->GetNativeWindowInfo(), description, device, surface) ==
                rhi::DeviceStatus::Ready);
        rhi::MeasurementAdapter adapter;
        REQUIRE(rhi::GetMeasurementAdapter(device, adapter) == rhi::RasterStatus::Ready);
        CHECK(adapter.Name[0] != '\0');
        CHECK(adapter.SoftwareKnown);
        CHECK_FALSE(adapter.Software);
        REQUIRE(rhi::ArmGpuTiming(device) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::SetFrameTarget(device, surface, {}) == rhi::DeviceStatus::Ready);
        REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Skipped);
        rhi::GpuTiming timing;
        REQUIRE(rhi::GetGpuTiming(device, timing) == rhi::RasterStatus::Ready);
        CHECK(timing.State == rhi::GpuTimingState::Armed);
        for (const uint32 width : {160U, 80U, 96U})
        {
            REQUIRE(rhi::SetFrameTarget(device, surface, { .Width = width, .Height = 64 }) == rhi::DeviceStatus::Ready);
            REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
            rhi::FrameInfo frame;
            REQUIRE(rhi::GetFrameInfo(device, surface, frame) == rhi::DeviceStatus::Ready);
            CHECK(frame.Width == width);
            CHECK(frame.Height == 64);
            REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
            for (uint32 poll = 0; poll < 5000; ++poll)
            {
                REQUIRE(rhi::GetGpuTiming(device, timing) == rhi::RasterStatus::Ready);
                if (timing.State != rhi::GpuTimingState::Pending)
                {
                    break;
                }
                [NSThread sleepForTimeInterval:.001];
            }
            CHECK(timing.State == rhi::GpuTimingState::Available);
            CHECK(timing.DurationNanoseconds >= 0);
            REQUIRE(rhi::ArmGpuTiming(device) == rhi::RasterStatus::Ready);
        }
        REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
        REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
        const auto stale = device;
        REQUIRE(rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready);
        timing.DurationNanoseconds = 123;
        CHECK(rhi::GetGpuTiming(stale, timing) == rhi::RasterStatus::InvalidHandle);
        CHECK(timing.DurationNanoseconds == 123);
        surface = {};
        REQUIRE(rhi::CreateDevice({}, window->GetNativeWindowInfo(), description, device, surface) ==
                rhi::DeviceStatus::Ready);
        REQUIRE(rhi::GetGpuTiming(device, timing) == rhi::RasterStatus::Ready);
        CHECK(timing.State == rhi::GpuTimingState::Empty);
        REQUIRE(rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready);
    }
}

namespace
{
struct BorrowedMetal final
{
    id<MTLDevice> Device = nil;
    id<MTLCommandQueue> Queue = nil;
    usize Retained = 0;
    usize Released = 0;
    static bool Retain(void* context) noexcept
    {
        ++static_cast<BorrowedMetal*>(context)->Retained;
        return true;
    }
    static void Release(void* context) noexcept
    {
        ++static_cast<BorrowedMetal*>(context)->Released;
    }
};
} // namespace

TEST_CASE("Metal labels named textures and balances nested scopes across submission", "[rhi][macos][gpu][annotation]")
{
    Guard guard;
    BorrowedMetal source;
    source.Device = MTLCreateSystemDefaultDevice();
    if (source.Device == nil)
    {
        SKIP("No Metal adapter on this host");
    }
    source.Queue = [source.Device newCommandQueue];
    REQUIRE(source.Queue != nil);
    const rhi::NativeDeviceDescription native
    {
        .Identity =
        {
            .Api = rhi::Backend::Metal,
            .Device = rhi::NativeLogicalDevice{(__bridge void*)source.Device},
            .CommandQueue = rhi::NativeCommandQueue{(__bridge void*)source.Queue},
        },
        .Lifetime = { .Context = &source, .Retain = BorrowedMetal::Retain, .Release = BorrowedMetal::Release },
        .SerializedOwnerQueue = true,
    };
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    constexpr uint32 EXTENT = 4;
    REQUIRE(rhi::CreateNativeDevice({}, native, description, EXTENT, EXTENT, device, surface) ==
            rhi::DeviceStatus::Ready);
    rhi::TextureHandle texture;
    REQUIRE(rhi::CreateTexture(device, { .Width = EXTENT, .Height = EXTENT, .Attachment = true }, {}, texture) ==
            rhi::RasterStatus::Ready);
    constexpr char NAME[] = "World/MainView/Opaque";
    REQUIRE(rhi::SetDebugName(device, texture, {NAME, sizeof(NAME) - 1}) == rhi::RasterStatus::Ready);
    rhi::GpuScopeDescription view;
    rhi::GpuScopeDescription pass;
    REQUIRE(rhi::MakeGpuScope(1, {"Viewport/Scene", sizeof("Viewport/Scene") - 1}, view) == true);
    REQUIRE(rhi::MakeGpuScope(2, {"Graph/Opaque", sizeof("Graph/Opaque") - 1}, pass) == true);
    REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::BeginGpuScope(device, view) == rhi::RasterStatus::Ready);
    {
        const rhi::GpuScope scope(device, pass);
        REQUIRE(rhi::BeginRasterPass(device, { .Color = texture }) == rhi::RasterStatus::Ready);
    }
    // "Viewport/Scene" stays open; committing the frame must close its debug group.
    rhi::SubmissionToken producer;
    REQUIRE(rhi::EndFrame(device, producer) == rhi::RasterStatus::Ready);
    CHECK(rhi::EndGpuScope(device) == rhi::RasterStatus::InvalidState);
    rhi::NativeDeviceLease lease;
    REQUIRE(rhi::AcquireNativeDevice(device, lease) == rhi::RasterStatus::Ready);
    rhi::NativeTextureRead read;
    rhi::NativeTextureInfo exported;
    REQUIRE(rhi::AcquireNativeTextureRead(lease, native.Identity, texture, producer, read, exported) ==
            rhi::RasterStatus::Ready);
    id<MTLTexture> nativeTexture = (__bridge id<MTLTexture>)exported.MetalTexture.GetObject();
    REQUIRE(nativeTexture != nil);
    CHECK([nativeTexture.label isEqualToString:@"World/MainView/Opaque"] == YES);
    REQUIRE(rhi::DrainNativeTextureRead(read) == rhi::RasterStatus::Ready);
    // Clearing the name clears the native label.
    REQUIRE(rhi::SetDebugName(device, texture, {}) == rhi::RasterStatus::Ready);
    CHECK(nativeTexture.label == nil);
    REQUIRE(rhi::ReleaseNativeDevice(lease) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, texture) == rhi::RasterStatus::Ready);
    rhi::Shutdown();
    CHECK(source.Retained == source.Released);
}

TEST_CASE("R14a Metal lists system adapters and starts on the selected one", "[rhi][macos][gpu][adapter]")
{
    Guard guard;
    rhi::AdapterCatalog catalog;
    REQUIRE(rhi::GetAdapterCatalog(catalog) == rhi::AdapterStatus::Ready);
    CHECK(catalog.Api == rhi::Backend::Metal);
    CHECK(catalog.ReportedCount >= catalog.Count);
    if (catalog.Count == 0)
    {
        SKIP("No Metal adapter on this host");
    }
    @autoreleasepool
    {
        // The system default device must be in the catalog under its registry ID.
        id<MTLDevice> systemDevice = MTLCreateSystemDefaultDevice();
        REQUIRE(systemDevice != nil);
        bool hasSystemDevice = false;
        for (usize i = 0; i < catalog.Count; ++i)
        {
            uint64 registryId = 0;
            std::memcpy(&registryId, catalog.Adapters[i].Identity.Bytes.GetData(), sizeof(registryId));
            hasSystemDevice = hasSystemDevice || registryId == systemDevice.registryID;
            CHECK(catalog.Adapters[i].Identity.Api == rhi::Backend::Metal);
            CHECK(catalog.Adapters[i].Name.IsEmpty() == false);
        }
        CHECK(hasSystemDevice);
    }
    usize selected = catalog.Count;
    for (usize i = 0; i < catalog.Count && selected == catalog.Count; ++i)
    {
        selected = catalog.Adapters[i].HasBackendBaseline == true ? i : selected;
    }
    if (selected == catalog.Count)
    {
        SKIP("No Metal adapter meets the backend baseline");
    }
    const rhi::AdapterInfo& adapter = catalog.Adapters[selected];
    rhi::DeviceDescription description;
    description.Adapter = adapter.Identity;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    REQUIRE(rhi::CreateDevice({}, { .Width = 17, .Height = 9 }, description, device, surface) ==
            rhi::DeviceStatus::Ready);
    rhi::DeviceInfo info;
    REQUIRE(rhi::GetDeviceInfo(device, info) == rhi::DeviceStatus::Ready);
    CHECK(info.Adapter.Identity == adapter.Identity);
    CHECK(info.Adapter.Name.GetView() == adapter.Name.GetView());
    // Enabled limits come from the negotiated profile, never above the raw claim.
    CHECK(info.Startup.Capabilities.MaxFrameDimension2D <= adapter.MaxTextureDimension2D);
    // The catalog stays available, unchanged, while a session is live.
    rhi::AdapterCatalog during;
    REQUIRE(rhi::GetAdapterCatalog(during) == rhi::AdapterStatus::Ready);
    CHECK(during.Count == catalog.Count);
    CHECK(during.Adapters[selected].Identity == adapter.Identity);
    REQUIRE(rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready);

    // An identity absent from the catalog publishes no device.
    description.Adapter.Bytes.Fill(0xFF);
    rhi::StartupInfo failure;
    surface = {};
    CHECK(rhi::CreateDevice({}, { .Width = 17, .Height = 9 }, description, device, surface, &failure) ==
          rhi::DeviceStatus::Failed);
    CHECK(failure.Error == rhi::StartupError::AdapterUnavailable);
    CHECK(device == rhi::DeviceHandle{});
    description.Adapter.Api = rhi::Backend::Vulkan;
    CHECK(rhi::ValidateDeviceDescription(description) == rhi::DeviceStatus::InvalidDescription);
}

namespace
{
/// Clear one surface of `device` to a colour and read back its headless target.
void ClearAndRead(rhi::DeviceHandle device,
                  rhi::SurfaceHandle surface,
                  const rhi::FrameTarget& target,
                  BorrowedView<uint8> pixels)
{
    REQUIRE(rhi::SetFrameTarget(device, surface, target) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
    rhi::FrameInfo frame;
    REQUIRE(rhi::GetFrameInfo(device, surface, frame) == rhi::DeviceStatus::Ready);
    CHECK(frame.Width == target.Width);
    CHECK(frame.Height == target.Height);
    REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::backend::ReadHeadlessPixels({pixels.GetDataOrNull(), pixels.GetSize()}));
}
bool IsFilled(BorrowedView<const uint8> pixels, uint8 red, uint8 green, uint8 blue) noexcept
{
    for (usize i = 0; i < pixels.GetSize(); i += 4)
    {
        const uint8* pixel = pixels.GetDataOrNull() + i;
        if (pixel[0] != red || pixel[1] != green || pixel[2] != blue || pixel[3] != 255)
        {
            return false;
        }
    }
    return true;
}
} // namespace

TEST_CASE("R14b Metal surfaces of one device acquire, resize and retire independently", "[rhi][macos][gpu][surface]")
{
    Guard guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle primary;
    if (rhi::CreateDevice({}, { .Width = 17, .Height = 9 }, {}, device, primary) != rhi::DeviceStatus::Ready)
    {
        SKIP("No Metal adapter on this host");
    }
    rhi::SurfaceHandle second;
    REQUIRE(rhi::CreateSurface(device, { .Width = 11, .Height = 5 }, second) == rhi::DeviceStatus::Ready);

    uint8 primaryPixels[17 * 9 * 4]{};
    ClearAndRead(device, primary, { .Width = 17, .Height = 9, .Red = 1, .Alpha = 1 }, primaryPixels);
    CHECK(IsFilled(primaryPixels, 255, 0, 0));
    uint8 smallPixels[11 * 5 * 4]{};
    ClearAndRead(device, second, { .Width = 11, .Height = 5, .Green = 1, .Alpha = 1 }, smallPixels);
    CHECK(IsFilled(smallPixels, 0, 255, 0));

    // Resizing the second surface leaves the primary target and its contents unchanged.
    uint8 resizedPixels[23 * 7 * 4]{};
    ClearAndRead(device, second, { .Width = 23, .Height = 7, .Blue = 1, .Alpha = 1 }, resizedPixels);
    CHECK(IsFilled(resizedPixels, 0, 0, 255));
    REQUIRE(rhi::BeginFrame(device, primary) == rhi::DeviceStatus::Ready);
    rhi::FrameInfo frame;
    REQUIRE(rhi::GetFrameInfo(device, primary, frame) == rhi::DeviceStatus::Ready);
    CHECK(frame.Width == 17);
    CHECK(frame.Height == 9);
    REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::backend::ReadHeadlessPixels(primaryPixels));
    CHECK(IsFilled(primaryPixels, 255, 0, 0));

    // Readback waited for every frame, so render completion is observed; headless presents nothing.
    rhi::SurfaceInfo info;
    REQUIRE(rhi::GetSurfaceInfo(device, second, info) == rhi::DeviceStatus::Ready);
    CHECK(info.State == rhi::SurfaceState::Ready);
    CHECK(info.Width == 23);
    CHECK(info.SubmittedFrames == 2);
    CHECK(info.RenderedFrames == 2);
    CHECK(info.PresentedFrames == 0);
    REQUIRE(rhi::GetSurfaceInfo(device, primary, info) == rhi::DeviceStatus::Ready);
    CHECK(info.IsPrimary);
    CHECK(info.SubmittedFrames == 2);
    CHECK(info.RenderedFrames == 2);

    // Retiring the second surface leaves the device and its primary surface working.
    const rhi::SurfaceHandle retired = second;
    REQUIRE(rhi::DestroySurface(device, second) == rhi::DeviceStatus::Ready);
    CHECK(rhi::BeginFrame(device, retired) == rhi::DeviceStatus::InvalidHandle);
    ClearAndRead(device, primary, { .Width = 17, .Height = 9, .Red = 1, .Green = 1, .Alpha = 1 }, primaryPixels);
    CHECK(IsFilled(primaryPixels, 255, 255, 0));
    REQUIRE(rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready);
}

TEST_CASE("R14b Metal loses a created Cocoa surface whose view leaves its window", "[rhi][macos][cocoa][gpu][surface]")
{
    if (std::getenv("LUDUS_TEST_COCOA") == nullptr)
    {
        SKIP("Set LUDUS_TEST_COCOA=1 with a WindowServer session");
    }
    @autoreleasepool
    {
        Guard guard;
        using namespace ludus::platform;
        WindowManager manager;
        REQUIRE(manager.Initialize({}));
        UniquePtr<Window> first;
        UniquePtr<Window> second;
        REQUIRE(manager.CreateWindow({ .Name = "R14b primary", .Width = 160, .Height = 120 }, first));
        REQUIRE(manager.CreateWindow({ .Name = "R14b second", .Width = 96, .Height = 64 }, second));
        // Destroy the connections before the borrowed views on assertion failure.
        Guard connectionGuard;
        const NativeWindowInfo primaryWindow = first->GetNativeWindowInfo();
        const NativeWindowInfo secondWindow = second->GetNativeWindowInfo();
        NSView* secondView = (__bridge NSView*)secondWindow.CocoaView;
        NSWindow* secondNative = (__bridge NSWindow*)secondWindow.CocoaWindow;
        CALayer* previous = secondView.layer;
        const BOOL wantsLayer = secondView.wantsLayer;
        rhi::DeviceHandle device;
        rhi::SurfaceHandle primary;
        REQUIRE(rhi::CreateDevice({}, primaryWindow, {}, device, primary) == rhi::DeviceStatus::Ready);
        rhi::SurfaceHandle surface;
        CHECK(rhi::CreateSurface(device, primaryWindow, surface) == rhi::DeviceStatus::InvalidDescription);
        REQUIRE(rhi::CreateSurface(device, secondWindow, surface) == rhi::DeviceStatus::Ready);
        CHECK([secondView.layer isKindOfClass:[CAMetalLayer class]]);
        REQUIRE(rhi::SetFrameTarget(device, surface, { .Width = 96, .Height = 64, .Blue = 1, .Alpha = 1 }) ==
                rhi::DeviceStatus::Ready);
        REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
        REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
        rhi::SurfaceInfo info;
        REQUIRE(rhi::GetSurfaceInfo(device, surface, info) == rhi::DeviceStatus::Ready);
        CHECK(info.System == WindowSystem::Cocoa);
        CHECK(info.SubmittedFrames == 1);
        CHECK(info.PresentedFrames == 1);

        // Replace the borrowed content view: the surface is lost and its view's layer restored.
        NSView* replacement = [[NSView alloc] initWithFrame:secondView.frame];
        secondNative.contentView = replacement;
        CHECK(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::SurfaceLost);
        CHECK(secondView.layer == previous);
        CHECK(secondView.wantsLayer == wantsLayer);
        REQUIRE(rhi::GetSurfaceInfo(device, surface, info) == rhi::DeviceStatus::Ready);
        CHECK(info.State == rhi::SurfaceState::Lost);
        CHECK(info.PresentedFrames == 1);
        rhi::DeviceInfo deviceInfo;
        CHECK(rhi::GetDeviceInfo(device, deviceInfo) == rhi::DeviceStatus::Ready);
        REQUIRE(rhi::SetFrameTarget(device, primary, { .Width = 160, .Height = 120, .Green = 1, .Alpha = 1 }) ==
                rhi::DeviceStatus::Ready);
        REQUIRE(rhi::BeginFrame(device, primary) == rhi::DeviceStatus::Ready);
        REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
        REQUIRE(rhi::DestroySurface(device, surface) == rhi::DeviceStatus::Ready);
        secondNative.contentView = secondView;
        REQUIRE(rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready);
    }
}

namespace
{
/// Destroy every device a test left live, so a failed REQUIRE does not leak an owner.
struct OwnerCleanup final
{
    StaticArray<rhi::DeviceHandle, rhi::MAX_DEVICE_OWNERS> Devices{};
    ~OwnerCleanup()
    {
        for (rhi::DeviceHandle& device : Devices)
        {
            static_cast<void>(rhi::DestroyDevice(device));
        }
    }
};
} // namespace

TEST_CASE("Metal keeps discard-only surface depth in tile memory", "[rhi][macos][gpu]")
{
    Guard guard;
    OwnerCleanup owners;
    id<MTLDevice> nativeDeviceOrNull = MTLCreateSystemDefaultDevice();
    if (nativeDeviceOrNull == nil)
    {
        SKIP("No Metal adapter on this host");
    }
    constexpr uint32 WIDTH = 8;
    constexpr uint32 HEIGHT = 4;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    rhi::SurfaceHandle surface;
    REQUIRE(rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, owners.Devices[0], surface) ==
            rhi::DeviceStatus::Ready);
    const rhi::DeviceHandle device = owners.Devices[0];
    const bool hasMemorylessSupport = [nativeDeviceOrNull supportsFamily:MTLGPUFamilyApple2] == YES;

    rhi::RasterShaderHandle vertex;
    rhi::RasterShaderHandle fragment;
    REQUIRE(rhi::CreateRasterShader(device,
                                    {
                                        .Artifact = Description(rhi::ShaderStage::Vertex, "vertexMain"),
                                        .Inputs = {},
                                        .Bindings = {},
                                    },
                                    vertex) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateRasterShader(device,
                                    {
                                        .Artifact = Description(rhi::ShaderStage::Fragment, "greenMain"),
                                        .Inputs = {},
                                        .Bindings = {},
                                    },
                                    fragment) == rhi::RasterStatus::Ready);
    rhi::BindingLayoutHandle layout;
    rhi::BindingSetHandle bindings;
    REQUIRE(rhi::CreateBindingLayout(device, {}, layout) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBindingSet(device, layout, {}, bindings) == rhi::RasterStatus::Ready);
    rhi::RasterPipelineHandle pipeline;
    REQUIRE(rhi::CreateRasterPipeline(device,
                                      {
                                          .Vertex = vertex,
                                          .Fragment = fragment,
                                          .Layout = layout,
                                          .Streams = {},
                                          .Attributes = {},
                                          .Depth = true,
                                      },
                                      pipeline) == rhi::RasterStatus::Ready);
    const uint16 indices[]{0, 1, 2};
    rhi::BufferHandle indexBuffer;
    REQUIRE(rhi::CreateBuffer(device,
                              { .Role = rhi::BufferRole::Index16, .Size = sizeof(indices) },
                              {reinterpret_cast<const uint8*>(indices), sizeof(indices)},
                              indexBuffer) == rhi::RasterStatus::Ready);
    const rhi::RasterDraw draw
    {
        .Pipeline = pipeline,
        .Bindings = bindings,
        .Vertices = {},
        .Indices = indexBuffer,
        .IndexCount = 3,
        .VertexCount = 3,
    };

    REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
    rhi::backend::MetalSurfaceDepthAllocations allocations = rhi::backend::GetMetalSurfaceDepthAllocations();
    CHECK(allocations.HasMemorylessTexture == hasMemorylessSupport);
    CHECK(allocations.MemorylessBytes == 0);
    CHECK((allocations.PrivateBytes == 0) == hasMemorylessSupport);
    rhi::RasterPassDescription pass;
    REQUIRE(rhi::BeginRasterPass(device, pass) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::DrawIndexed(device, draw) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
    uint8 pixels[WIDTH * HEIGHT * 4]{};
    REQUIRE(rhi::backend::ReadHeadlessPixels(pixels));
    constexpr uint8 GREEN[]{0, 255, 0, 255};
    for (usize i = 0; i < sizeof(pixels); i += sizeof(GREEN))
    {
        CHECK(std::memcmp(pixels + i, GREEN, sizeof(GREEN)) == 0);
    }

    REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
    pass.Clear[0] = 1;
    pass.Clear[3] = 1;
    pass.ClearDepth = 0;
    pass.DepthStore = rhi::RasterStore::Store;
    REQUIRE(rhi::BeginRasterPass(device, pass) == rhi::RasterStatus::Ready);
    allocations = rhi::backend::GetMetalSurfaceDepthAllocations();
    CHECK(allocations.HasMemorylessTexture == hasMemorylessSupport);
    CAPTURE(allocations.MemorylessBytes, allocations.PrivateBytes);
    CHECK(allocations.PrivateBytes > 0);
    pass.ColorLoad = rhi::RasterLoad::Load;
    pass.DepthLoad = rhi::RasterLoad::Load;
    pass.DepthStore = rhi::RasterStore::Discard;
    REQUIRE(rhi::BeginRasterPass(device, pass) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::DrawIndexed(device, draw) == rhi::RasterStatus::Ready);
    CHECK(rhi::BeginRasterPass(device, pass) == rhi::RasterStatus::InvalidDescription);
    REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::backend::ReadHeadlessPixels(pixels));
    constexpr uint8 RED[]{255, 0, 0, 255};
    for (usize i = 0; i < sizeof(pixels); i += sizeof(RED))
    {
        CHECK(std::memcmp(pixels + i, RED, sizeof(RED)) == 0);
    }

    REQUIRE(rhi::SetFrameTarget(device, surface, { .Width = WIDTH + 1, .Height = HEIGHT }) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
    CHECK(rhi::GetFrameInfo().Width == WIDTH + 1);
    allocations = rhi::backend::GetMetalSurfaceDepthAllocations();
    CHECK(allocations.HasMemorylessTexture == hasMemorylessSupport);
    CHECK(allocations.MemorylessBytes == 0);
    CHECK((allocations.PrivateBytes == 0) == hasMemorylessSupport);
    REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
}
