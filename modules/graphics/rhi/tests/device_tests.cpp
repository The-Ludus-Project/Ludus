#include "internal/adapter.h"
#include "internal/device.h"
#include "internal/lifecycle.h"
#include "internal/resources.h"
#include "internal/surface.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/device.h>

#include <initializer_list>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace ludus::graphics::rhi::backend
{
extern foundation::uint32 PendingToken;
extern StartupError ImmediateError;
extern FrameStatus NextFrame;
extern Backend ActiveKind;
extern bool SelectionSupported;
extern ResourceStatus NextResource;
extern foundation::uint32 LastResource;
extern foundation::uint32 Draws;
extern bool LoseDuringCreation;
extern foundation::uint32 ShutdownCount;
extern AdapterStatus CatalogStatus;
extern AdapterCatalog Catalog;
extern bool HasSelectedAdapter;
extern AdapterInfo SelectedAdapter;
extern AdapterIdentity StartedAdapter;
extern bool ShouldCompleteDuringStart;
extern foundation::usize AdditionalSurfaces;
extern StartupError NextSurfaceError;
extern foundation::usize SelectedSurface;
extern foundation::uint32 DestroyedSurfaces;
extern foundation::usize LastDestroyedSurface;
extern bool ShouldLoseNextSurface;
extern foundation::StaticArray<SurfaceCompletion, MAX_DEVICE_SURFACES> SurfaceCompletions;
extern foundation::StaticArray<FrameTarget, MAX_DEVICE_SURFACES> SurfaceTargets;
} // namespace ludus::graphics::rhi::backend
using namespace ludus::graphics::rhi;
namespace
{
void Reset() noexcept
{
    Shutdown();
    backend::ImmediateError = StartupError::None;
    backend::NextFrame = FrameStatus::Ready;
    backend::ActiveKind = Backend::WebGPU;
    backend::SelectionSupported = true;
    backend::NextResource = ResourceStatus::Ready;
    backend::LoseDuringCreation = false;
    backend::CatalogStatus = AdapterStatus::Unsupported;
    backend::Catalog = {};
    backend::HasSelectedAdapter = false;
    backend::SelectedAdapter = {};
    backend::StartedAdapter = {};
    backend::ShouldCompleteDuringStart = false;
    backend::AdditionalSurfaces = 0;
    backend::NextSurfaceError = StartupError::None;
    backend::SelectedSurface = 0;
    backend::DestroyedSurfaces = 0;
    backend::LastDestroyedSurface = 0;
    backend::ShouldLoseNextSurface = false;
    backend::SurfaceCompletions.Fill({});
    backend::SurfaceTargets.Fill({});
}
void Ready() noexcept
{
    internal::Complete(backend::PendingToken,
                       StartupError::None,
                       { .MaxFrameDimension2D = 4096, .MaxUniformBufferSize = 16384 });
}
} // namespace

TEST_CASE("Explicit RHI owners cancel pending work and isolate foreign surfaces and resources", "[rhi][device]")
{
    Reset();
    DeviceHandle device;
    SurfaceHandle surface;
    DeviceDescription description;
    description.Limits.MinUniformBufferSize = 48;
    description.Preferred.Compute = true;
    REQUIRE(CreateDevice({}, {}, description, device, surface) == DeviceStatus::Pending);
    const auto cancelledToken = backend::PendingToken;
    const auto oldDevice = device;
    const auto oldSurface = surface;
    description.Limits.MinUniformBufferSize = 99999;
    DeviceInfo info;
    REQUIRE(GetDeviceInfo(device, info) == DeviceStatus::Pending);
    CHECK(info.Description.Limits.MinUniformBufferSize == 48);
    CHECK(info.Description.Preferred.Compute);
    CHECK_FALSE(info.Enabled.FullscreenRaster);
    UniformHandle uniform;
    CHECK(CreateUniform(device, 48, uniform) == ResourceStatus::NotReady);
    CHECK(BeginFrame(device, surface) == DeviceStatus::NotReady);
    CHECK(CreateDevice({}, {}, {}, device, surface) == DeviceStatus::InvalidState);
    REQUIRE(DestroyDevice(device) == DeviceStatus::Ready);
    CHECK(DestroyDevice(device) == DeviceStatus::InvalidHandle);
    DeviceHandle next;
    SurfaceHandle target;
    REQUIRE(CreateDevice({}, {}, {}, next, target) == DeviceStatus::Pending);
    internal::Complete(cancelledToken, StartupError::None, { .MaxFrameDimension2D = 1, .MaxUniformBufferSize = 16 });
    internal::Fail(cancelledToken, StartupError::DeviceLost);
    CHECK(GetDeviceInfo(next, info) == DeviceStatus::Pending);
    Ready();
    REQUIRE(GetDeviceInfo(next, info) == DeviceStatus::Ready);
    CHECK(info.Supported.FullscreenRaster);
    CHECK(info.Enabled.FullscreenRaster);
    CHECK(info.Enabled.Compute);
    const auto before = info.Startup.Capabilities.MaxFrameDimension2D;
    CHECK(GetDeviceInfo(oldDevice, info) == DeviceStatus::InvalidHandle);
    CHECK(info.Startup.Capabilities.MaxFrameDimension2D == before);
    auto staleOwner = oldDevice;
    CHECK(DestroyDevice(staleOwner) == DeviceStatus::InvalidHandle);
    CHECK(GetDeviceInfo(next, info) == DeviceStatus::Ready);
    CHECK(SetFrameTarget(next, oldSurface, {}) == DeviceStatus::InvalidHandle);
    CHECK(BeginFrame(oldDevice, target) == DeviceStatus::InvalidHandle);
    CHECK(EndFrame(oldDevice) == DeviceStatus::InvalidHandle);
    CHECK(CreateUniform(oldDevice, 48, uniform) == ResourceStatus::InvalidHandle);
    REQUIRE(CreateUniform(next, 48, uniform) == ResourceStatus::Ready);
    const auto oldUniform = uniform;
    FrameInfo acquired{ .Width = 777 };
    CHECK(GetFrameInfo(next, target, acquired) == DeviceStatus::InvalidState);
    CHECK(acquired.Width == 777);
    backend::NextFrame = FrameStatus::Skipped;
    CHECK(BeginFrame(next, target) == DeviceStatus::Skipped);
    CHECK(EndFrame(next) == DeviceStatus::InvalidState);
    backend::NextFrame = FrameStatus::Ready;
    REQUIRE(BeginFrame(next, target) == DeviceStatus::Ready);
    CHECK(BeginFrame(next, target) == DeviceStatus::InvalidState);
    CHECK(SetFrameTarget(next, target, {}) == DeviceStatus::InvalidState);
    CHECK(GetFrameInfo(next, oldSurface, acquired) == DeviceStatus::InvalidHandle);
    CHECK(acquired.Width == 777);
    REQUIRE(GetFrameInfo(next, target, acquired) == DeviceStatus::Ready);
    CHECK(acquired.Width == 96);
    CHECK(EndFrame(next) == DeviceStatus::Ready);
    REQUIRE(DestroyDevice(next) == DeviceStatus::Ready);
    target = {};
    REQUIRE(CreateDevice({}, {}, {}, next, target) == DeviceStatus::Pending);
    Ready();
    CHECK(GetStatus(next, oldUniform) == ResourceStatus::InvalidHandle);
    CHECK(Destroy(next, oldUniform) == ResourceStatus::InvalidHandle);
    REQUIRE(DestroyDevice(next) == DeviceStatus::Ready);
}

TEST_CASE("Required workloads reject before startup and synchronous failures preserve outputs", "[rhi][device]")
{
    Reset();
    DeviceHandle device;
    SurfaceHandle surface;
    DeviceDescription description;
    const auto token = backend::PendingToken;
    StartupInfo diagnostic;
    diagnostic.MaxTextureDimension2D = 777;
    description.Selection = BackendSelection::WebGL2;
    description.Required.Compute = true;
    CHECK(ValidateDeviceDescription(description) == DeviceStatus::Unsupported);
    CHECK(CreateDevice({}, {}, description, device, surface, &diagnostic) == DeviceStatus::Unsupported);
    CHECK(diagnostic.MaxTextureDimension2D == 777);
    CHECK(backend::PendingToken == token);
    CHECK(GetStartup().State == StartupState::Idle);
    description.Required.Compute = false;
    description.Required.Compute = true;
    CHECK(CreateDevice({}, {}, description, device, surface) == DeviceStatus::Unsupported);
    description.Required.Compute = false;
    description.Required.IndirectRendering = true;
    CHECK(CreateDevice({}, {}, description, device, surface) == DeviceStatus::Unsupported);
    description.Required.IndirectRendering = false;
    description.Selection = static_cast<BackendSelection>(255);
    CHECK(CreateDevice({}, {}, description, device, surface) == DeviceStatus::InvalidDescription);
    description.Selection = BackendSelection::Auto;
    backend::ImmediateError = StartupError::DeviceUnavailable;
    CHECK(CreateDevice({}, {}, description, device, surface, &diagnostic) == DeviceStatus::Failed);
    CHECK(diagnostic.State == StartupState::Failed);
    CHECK(diagnostic.Error == StartupError::DeviceUnavailable);
    CHECK(GetStartup().State == StartupState::Idle);
    backend::ImmediateError = StartupError::None;
    REQUIRE(CreateDevice({}, {}, description, device, surface) == DeviceStatus::Pending);
    Ready();
    REQUIRE(DestroyDevice(device) == DeviceStatus::Ready);
    // A borrowed stale target must be cleared explicitly before it becomes an output.
    CHECK(CreateDevice({}, {}, description, device, surface) == DeviceStatus::InvalidState);
    surface = {};
    REQUIRE(Start({}, {}) == StartStatus::Pending);
    const auto facadeToken = backend::PendingToken;
    CHECK(CreateDevice({}, {}, description, device, surface) == DeviceStatus::InvalidState);
    CHECK(backend::PendingToken == facadeToken);
    description.Required.Compute = true;
    CHECK(CreateDevice({}, {}, description, device, surface) == DeviceStatus::InvalidState);
    CHECK(backend::PendingToken == facadeToken);
    CHECK(GetStartup().State == StartupState::Pending);
    Shutdown();
}

TEST_CASE("Explicit owner survives fallback attempts but loss stops all work", "[rhi][device]")
{
    Reset();
    DeviceHandle device;
    SurfaceHandle surface;
    DeviceDescription description;
    description.Limits.MinUniformBufferSize = 48;
    REQUIRE(CreateDevice({}, {}, description, device, surface) == DeviceStatus::Pending);
    const auto first = backend::PendingToken;
    const auto owner = internal::DeviceOwner();
    internal::SetFallback([](ludus::foundation::uint32 token, StartupError error) noexcept {
        internal::RecordAttempt(token, Backend::WebGPU, error);
        const auto retry = internal::Reissue(token);
        internal::SelectBackend(retry, Backend::WebGL2);
        internal::Complete(retry, StartupError::None, { .MaxFrameDimension2D = 2048, .MaxUniformBufferSize = 16384 });
        return true;
    });
    internal::Complete(first, StartupError::None, { .MaxFrameDimension2D = 4096, .MaxUniformBufferSize = 16 });
    DeviceInfo info;
    REQUIRE(GetDeviceInfo(device, info) == DeviceStatus::Ready);
    CHECK(internal::DeviceOwner() == owner);
    CHECK(info.Startup.SelectedBackend == Backend::WebGL2);
    CHECK(info.Startup.WebGpu.Error == StartupError::RequirementsUnsatisfied);
    CHECK(info.Startup.WebGL2.Error == StartupError::None);
    CHECK(info.Enabled.FullscreenRaster);
    CHECK(info.Enabled.Compute);
    const auto current = internal::Reissue(first);
    CHECK(current == 0);
    // The current fallback token is not the cancelled first request.
    internal::Fail(first, StartupError::DeviceLost);
    CHECK(GetDeviceInfo(device, info) == DeviceStatus::Ready);
    // Resource creation can synchronously notify loss before returning.
    Shutdown();
    CHECK(GetDeviceInfo(device, info) == DeviceStatus::InvalidHandle);
    device = {};
    surface = {};
    REQUIRE(CreateDevice({}, {}, {}, device, surface) == DeviceStatus::Pending);
    Ready();
    UniformHandle uniform;
    REQUIRE(CreateUniform(device, 16, uniform) == ResourceStatus::Ready);
    const auto lostToken = backend::PendingToken;
    internal::Fail(lostToken, StartupError::DeviceLost);
    CHECK(GetDeviceInfo(device, info) == DeviceStatus::DeviceLost);
    CHECK_FALSE(info.Supported.FullscreenRaster);
    CHECK_FALSE(info.Enabled.FullscreenRaster);
    CHECK(GetStatus(device, uniform) == ResourceStatus::DeviceLost);
    CHECK(CreateUniform(device, 16, uniform) == ResourceStatus::DeviceLost);
    CHECK(BeginFrame(device, surface) == DeviceStatus::DeviceLost);
    CHECK(EndFrame(device) == DeviceStatus::DeviceLost);
    internal::Complete(lostToken, StartupError::None, { .MaxFrameDimension2D = 4096, .MaxUniformBufferSize = 16384 });
    CHECK(GetDeviceInfo(device, info) == DeviceStatus::DeviceLost);
    REQUIRE(DestroyDevice(device) == DeviceStatus::Ready);
}

TEST_CASE("Published startup failure remains owned until explicit destruction", "[rhi][device]")
{
    Reset();
    DeviceHandle device;
    SurfaceHandle surface;
    DeviceDescription description;
    description.Limits.MinUniformBufferSize = 16400;
    REQUIRE(CreateDevice({}, {}, description, device, surface) == DeviceStatus::Pending);
    Ready();
    DeviceInfo info;
    CHECK(GetDeviceInfo(device, info) == DeviceStatus::Failed);
    CHECK(info.Startup.Error == StartupError::RequirementsUnsatisfied);
    CHECK(info.Startup.UnmetRequirement == RequirementFailure::UniformBufferSize);
    CHECK_FALSE(info.Enabled.FullscreenRaster);
    CHECK(SetFrameTarget(device, surface, {}) == DeviceStatus::Failed);
    CHECK(BeginFrame(device, surface) == DeviceStatus::Failed);
    CHECK(EndFrame(device) == DeviceStatus::Failed);
    FrameInfo frame{ .Width = 777 };
    CHECK(GetFrameInfo(device, surface, frame) == DeviceStatus::Failed);
    CHECK(frame.Width == 777);
    DeviceHandle other;
    SurfaceHandle otherTarget;
    CHECK(CreateDevice({}, {}, {}, other, otherTarget) == DeviceStatus::InvalidState);
    REQUIRE(DestroyDevice(device) == DeviceStatus::Ready);
}

TEST_CASE("Synchronous resource loss reports the explicit device loss result", "[rhi][device]")
{
    Reset();
    DeviceHandle device;
    SurfaceHandle surface;
    REQUIRE(CreateDevice({}, {}, {}, device, surface) == DeviceStatus::Pending);
    Ready();
    backend::LoseDuringCreation = true;
    UniformHandle interrupted;
    CHECK(CreateUniform(device, 16, interrupted) == ResourceStatus::DeviceLost);
    DeviceInfo info;
    CHECK(GetDeviceInfo(device, info) == DeviceStatus::DeviceLost);
    CHECK(GetStatus(device, interrupted) == ResourceStatus::DeviceLost);
    REQUIRE(DestroyDevice(device) == DeviceStatus::Ready);
}

TEST_CASE("Device identity exhaustion never wraps to an old owner", "[rhi][device]")
{
    const auto maximum = ~ludus::foundation::uint64{0};
    internal::DeviceOwnerSequence sequence{maximum - 1};
    CHECK(sequence.Take() == maximum - 1);
    CHECK(sequence.Take() == maximum);
    CHECK(sequence.Take() == 0);
    CHECK(sequence.Take() == 0);
}
TEST_CASE("Resources from a destroyed RHI session are rejected by the next session", "[rhi][device][resources]")
{
    // Device owners and legacy resource IDs come from separate process-wide RHI
    // counters that never reuse a value (docs/architecture/object-handles-uuid.md,
    // stage 2). The replacement session reuses the same backend slots, so only the
    // identity checks separate the old handles from the new resources.
    Reset();
    const ShaderDescription vertexDescription
    {
        .Stage = ShaderStage::Vertex,
        .UniformSize = 48,
        .Spirv = {},
        .Wgsl = "generated vertex",
        .SpirvEntry = {},
        .WgslEntry = "vertexMain",
    };
    const ShaderDescription fragmentDescription
    {
        .Stage = ShaderStage::Fragment,
        .Spirv = {},
        .Wgsl = "generated fragment",
        .SpirvEntry = {},
        .WgslEntry = "fragmentMain",
    };
    DeviceHandle oldDevice;
    SurfaceHandle oldSurface;
    REQUIRE(CreateDevice({}, {}, {}, oldDevice, oldSurface) == DeviceStatus::Pending);
    Ready();
    ShaderHandle oldVertex;
    ShaderHandle oldFragment;
    UniformHandle oldUniform;
    PipelineHandle oldPipeline;
    REQUIRE(CreateShader(oldDevice, vertexDescription, oldVertex) == ResourceStatus::Ready);
    REQUIRE(CreateShader(oldDevice, fragmentDescription, oldFragment) == ResourceStatus::Ready);
    REQUIRE(CreateUniform(oldDevice, 48, oldUniform) == ResourceStatus::Ready);
    REQUIRE(CreatePipeline(oldDevice, {oldVertex, oldFragment, oldUniform}, oldPipeline) == ResourceStatus::Ready);
    // Ending the session releases every resource without explicit destruction.
    REQUIRE(DestroyDevice(oldDevice) == DeviceStatus::Ready);

    DeviceHandle device;
    SurfaceHandle surface;
    REQUIRE(CreateDevice({}, {}, {}, device, surface) == DeviceStatus::Pending);
    Ready();
    ShaderHandle vertex;
    ShaderHandle fragment;
    UniformHandle uniform;
    PipelineHandle pipeline;
    REQUIRE(CreateShader(device, vertexDescription, vertex) == ResourceStatus::Ready);
    REQUIRE(CreateShader(device, fragmentDescription, fragment) == ResourceStatus::Ready);
    REQUIRE(CreateUniform(device, 48, uniform) == ResourceStatus::Ready);
    REQUIRE(CreatePipeline(device, {vertex, fragment, uniform}, pipeline) == ResourceStatus::Ready);

    UniformHandle unissued;
    CHECK(CreateUniform(oldDevice, 48, unissued) == ResourceStatus::InvalidHandle);
    CHECK(GetStatus(device, oldVertex) == ResourceStatus::InvalidHandle);
    CHECK(GetStatus(device, oldFragment) == ResourceStatus::InvalidHandle);
    CHECK(GetStatus(device, oldUniform) == ResourceStatus::InvalidHandle);
    CHECK(GetStatus(device, oldPipeline) == ResourceStatus::InvalidHandle);
    ludus::foundation::uint8 bytes[48]{};
    CHECK(UpdateUniform(device, oldUniform, bytes) == ResourceStatus::InvalidHandle);
    PipelineHandle mixed;
    CHECK(CreatePipeline(device, {oldVertex, fragment, uniform}, mixed) == ResourceStatus::InvalidHandle);
    CHECK(CreatePipeline(device, {vertex, fragment, oldUniform}, mixed) == ResourceStatus::InvalidHandle);
    CHECK(GetStatus(device, mixed) == ResourceStatus::InvalidHandle);

    // A draw requires written uniform contents. The stale update above did not mark
    // the replacement uniform in the reused slot as written.
    REQUIRE(BeginFrame(device, surface) == DeviceStatus::Ready);
    CHECK(DrawFullscreen(device, pipeline) == ResourceStatus::InvalidState);
    REQUIRE(EndFrame(device) == DeviceStatus::Ready);
    REQUIRE(UpdateUniform(device, uniform, bytes) == ResourceStatus::Ready);
    REQUIRE(BeginFrame(device, surface) == DeviceStatus::Ready);
    const ludus::foundation::uint32 submitted = backend::Draws;
    CHECK(DrawFullscreen(device, oldPipeline) == ResourceStatus::InvalidHandle);
    CHECK(backend::Draws == submitted);
    REQUIRE(DrawFullscreen(device, pipeline) == ResourceStatus::Ready);
    CHECK(backend::Draws == submitted + 1);
    REQUIRE(EndFrame(device) == DeviceStatus::Ready);

    CHECK(Destroy(device, oldPipeline) == ResourceStatus::InvalidHandle);
    CHECK(Destroy(device, oldUniform) == ResourceStatus::InvalidHandle);
    CHECK(Destroy(device, oldVertex) == ResourceStatus::InvalidHandle);
    CHECK(Destroy(device, oldFragment) == ResourceStatus::InvalidHandle);

    // The rejected calls left the replacement resources in the reused slots intact.
    CHECK(GetStatus(device, pipeline) == ResourceStatus::Ready);
    CHECK(GetStatus(device, uniform) == ResourceStatus::Ready);
    CHECK(UpdateUniform(device, uniform, bytes) == ResourceStatus::Ready);
    REQUIRE(Destroy(device, pipeline) == ResourceStatus::Ready);
    REQUIRE(Destroy(device, uniform) == ResourceStatus::Ready);
    REQUIRE(Destroy(device, vertex) == ResourceStatus::Ready);
    REQUIRE(Destroy(device, fragment) == ResourceStatus::Ready);
    REQUIRE(DestroyDevice(device) == DeviceStatus::Ready);
}

namespace
{
constexpr ludus::foundation::uint8 FIRST_ADAPTER_BYTE = 0x11;
constexpr ludus::foundation::uint8 SECOND_ADAPTER_BYTE = 0x22;
constexpr ludus::foundation::uint8 ABSENT_ADAPTER_BYTE = 0x33;
constexpr ludus::foundation::uint32 RAW_TEXTURE_DIMENSION = 32768;

AdapterIdentity MakeAdapter(Backend api, ludus::foundation::uint8 firstByte) noexcept
{
    AdapterIdentity identity{ .Api = api };
    identity.Bytes[0] = firstByte;
    return identity;
}
// Restores the shared fake backend even when a REQUIRE ends a case early.
struct AdapterFixture final
{
    AdapterFixture() noexcept
    {
        Reset();
    }
    ~AdapterFixture() noexcept
    {
        Reset();
    }
    AdapterFixture(const AdapterFixture&) = delete;
    AdapterFixture& operator=(const AdapterFixture&) = delete;
};
// Two Metal adapters: the first meets the backend baseline, the second does not.
void PublishCatalog() noexcept
{
    backend::ActiveKind = Backend::Metal;
    backend::CatalogStatus = AdapterStatus::Ready;
    backend::Catalog = { .Api = Backend::Metal };
    AdapterInfo* first = internal::ReserveCatalogEntryOrNull(backend::Catalog);
    AdapterInfo* second = internal::ReserveCatalogEntryOrNull(backend::Catalog);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    first->Identity = MakeAdapter(Backend::Metal, FIRST_ADAPTER_BYTE);
    internal::AssignAdapterName("First GPU", first->Name);
    first->Kind = AdapterKind::Discrete;
    first->MaxTextureDimension2D = RAW_TEXTURE_DIMENSION;
    first->HasBackendBaseline = true;
    second->Identity = MakeAdapter(Backend::Metal, SECOND_ADAPTER_BYTE);
    second->Kind = AdapterKind::Integrated;
}
} // namespace

TEST_CASE("R14a adapter catalogs are bounded and preserve outputs on failure", "[rhi][device][adapter]")
{
    const AdapterFixture fixture;
    AdapterCatalog catalog;
    catalog.Count = 1;
    CHECK(GetAdapterCatalog(catalog) == AdapterStatus::Unsupported);
    CHECK(catalog.Count == 1);
    backend::CatalogStatus = AdapterStatus::Failed;
    CHECK(GetAdapterCatalog(catalog) == AdapterStatus::Failed);
    CHECK(catalog.Count == 1);

    PublishCatalog();
    REQUIRE(GetAdapterCatalog(catalog) == AdapterStatus::Ready);
    CHECK(catalog.Api == Backend::Metal);
    CHECK(catalog.Count == 2);
    CHECK(catalog.ReportedCount == 2);
    CHECK(catalog.Adapters[0].Name.GetView() == "First GPU");
    CHECK(catalog.Adapters[1].HasBackendBaseline == false);

    AdapterCatalog full{ .Api = Backend::Vulkan };
    for (ludus::foundation::usize i = 0; i < MAX_CATALOG_ADAPTERS; ++i)
    {
        AdapterInfo* entry = internal::ReserveCatalogEntryOrNull(full);
        REQUIRE(entry != nullptr);
        CHECK(entry->Identity.Api == Backend::Vulkan);
    }
    CHECK(internal::ReserveCatalogEntryOrNull(full) == nullptr);
    CHECK(full.Count == MAX_CATALOG_ADAPTERS);
    CHECK(full.ReportedCount == MAX_CATALOG_ADAPTERS + 1);
}

TEST_CASE("R14a adapter names are shortened at a UTF-8 code point boundary", "[rhi][device][adapter]")
{
    // 254 ASCII bytes, then a two-byte scalar straddling the 255-byte capacity.
    std::string reported(MAX_ADAPTER_NAME_BYTES - 1, 'a');
    reported += "\xC3\xA9"
                "b";
    AdapterName name;
    internal::AssignAdapterName(reported, name);
    CHECK(name.GetSize() == MAX_ADAPTER_NAME_BYTES - 1);
    internal::AssignAdapterName("GPU \xC3\xA9", name);
    CHECK(name.GetView() == "GPU \xC3\xA9");
}

TEST_CASE("R14a explicit adapters are admitted only from the native catalog", "[rhi][device][adapter]")
{
    const AdapterFixture fixture;
    DeviceDescription description;
    description.Adapter = MakeAdapter(Backend::WebGPU, FIRST_ADAPTER_BYTE);
    // Browser builds publish no catalog, so an explicit identity is unsupported there.
    CHECK(ValidateDeviceDescription(description) == DeviceStatus::Unsupported);

    PublishCatalog();
    description.Adapter = MakeAdapter(Backend::Vulkan, FIRST_ADAPTER_BYTE);
    CHECK(ValidateDeviceDescription(description) == DeviceStatus::InvalidDescription);
    description.Adapter = MakeAdapter(Backend::Metal, FIRST_ADAPTER_BYTE);
    CHECK(ValidateDeviceDescription(description) == DeviceStatus::Ready);

    const ludus::foundation::uint32 shutdowns = backend::ShutdownCount;
    for (const ludus::foundation::uint8 rejected : {ABSENT_ADAPTER_BYTE, SECOND_ADAPTER_BYTE})
    {
        backend::PendingToken = 0;
        description.Adapter = MakeAdapter(Backend::Metal, rejected);
        DeviceHandle device;
        SurfaceHandle surface;
        StartupInfo failure;
        CHECK(CreateDevice({}, {}, description, device, surface, &failure) == DeviceStatus::Failed);
        CHECK(failure.Error == StartupError::AdapterUnavailable);
        CHECK(failure.State == StartupState::Failed);
        CHECK(failure.SelectedBackend == Backend::Metal);
        CHECK(device == DeviceHandle{});
        // The backend was never asked to start and no session was opened.
        CHECK(backend::PendingToken == 0);
        CHECK(backend::ShutdownCount == shutdowns);
        CHECK(internal::GetRequestedAdapterOrNull() == nullptr);
    }
    backend::CatalogStatus = AdapterStatus::Failed;
    description.Adapter = MakeAdapter(Backend::Metal, FIRST_ADAPTER_BYTE);
    DeviceHandle device;
    SurfaceHandle surface;
    CHECK(CreateDevice({}, {}, description, device, surface) == DeviceStatus::Failed);
    CHECK(device == DeviceHandle{});
}

TEST_CASE("R14a selected adapters are reported apart from the enabled profile", "[rhi][device][adapter]")
{
    const AdapterFixture fixture;
    PublishCatalog();
    backend::ShouldCompleteDuringStart = true;
    backend::HasSelectedAdapter = true;
    backend::SelectedAdapter = backend::Catalog.Adapters[0];
    DeviceDescription description;
    description.Adapter = backend::Catalog.Adapters[0].Identity;
    DeviceHandle device;
    SurfaceHandle surface;
    REQUIRE(CreateDevice({}, {}, description, device, surface) == DeviceStatus::Ready);
    CHECK(backend::StartedAdapter == description.Adapter);
    DeviceInfo info;
    REQUIRE(GetDeviceInfo(device, info) == DeviceStatus::Ready);
    CHECK(info.Description.Adapter == description.Adapter);
    CHECK(info.Adapter.Identity == description.Adapter);
    CHECK(info.Adapter.Name.GetView() == "First GPU");
    // A raw adapter claim never raises the enabled limits.
    CHECK(info.Adapter.MaxTextureDimension2D == RAW_TEXTURE_DIMENSION);
    CHECK(info.Startup.Capabilities.MaxFrameDimension2D < RAW_TEXTURE_DIMENSION);
    REQUIRE(DestroyDevice(device) == DeviceStatus::Ready);
    CHECK(internal::GetRequestedAdapterOrNull() == nullptr);

    // An automatic request keeps the backend's own choice and still reports it.
    surface = {};
    REQUIRE(CreateDevice({}, {}, {}, device, surface) == DeviceStatus::Ready);
    CHECK(backend::StartedAdapter.IsAutomatic());
    REQUIRE(GetDeviceInfo(device, info) == DeviceStatus::Ready);
    CHECK(info.Adapter.Identity == backend::SelectedAdapter.Identity);
    REQUIRE(DestroyDevice(device) == DeviceStatus::Ready);
}

TEST_CASE("R14a a session on another adapter than requested is torn down", "[rhi][device][adapter]")
{
    const AdapterFixture fixture;
    PublishCatalog();
    backend::ShouldCompleteDuringStart = true;
    // An imported native device, or a backend that ignored the request, reports another adapter.
    backend::HasSelectedAdapter = true;
    backend::SelectedAdapter.Identity = MakeAdapter(Backend::Metal, SECOND_ADAPTER_BYTE);
    DeviceDescription description;
    description.Adapter = MakeAdapter(Backend::Metal, FIRST_ADAPTER_BYTE);
    DeviceHandle device;
    SurfaceHandle surface;
    StartupInfo failure;
    const ludus::foundation::uint32 shutdowns = backend::ShutdownCount;
    CHECK(CreateDevice({}, {}, description, device, surface, &failure) == DeviceStatus::Failed);
    CHECK(failure.Error == StartupError::AdapterUnavailable);
    CHECK(device == DeviceHandle{});
    CHECK(backend::ShutdownCount > shutdowns);
    CHECK(GetStartup().State == StartupState::Idle);
    // The failed attempt leaves no session behind, so the next one may start.
    backend::SelectedAdapter.Identity = description.Adapter;
    REQUIRE(CreateDevice({}, {}, description, device, surface) == DeviceStatus::Ready);
    REQUIRE(DestroyDevice(device) == DeviceStatus::Ready);
}

TEST_CASE("R14b surfaces of one device keep independent targets, frames and counters", "[rhi][device][surface]")
{
    Reset();
    backend::AdditionalSurfaces = MAX_DEVICE_SURFACES - 1;
    DeviceHandle device;
    SurfaceHandle primary;
    REQUIRE(CreateDevice({}, { .Width = 64, .Height = 32 }, {}, device, primary) == DeviceStatus::Pending);
    SurfaceHandle second;
    CHECK(CreateSurface(device, { .Width = 320, .Height = 200 }, second) == DeviceStatus::NotReady);
    CHECK(second == SurfaceHandle{});
    Ready();
    REQUIRE(CreateSurface(device, { .Width = 320, .Height = 200 }, second) == DeviceStatus::Ready);
    CHECK(second != SurfaceHandle{});
    CHECK(second != primary);
    const SurfaceHandle published = second;
    CHECK(CreateSurface(device, { .Width = 320, .Height = 200 }, second) == DeviceStatus::InvalidState);
    CHECK(second == published);

    SurfaceInfo info;
    REQUIRE(GetSurfaceInfo(device, primary, info) == DeviceStatus::Ready);
    CHECK(info.IsPrimary);
    CHECK(info.Width == 64);
    REQUIRE(GetSurfaceInfo(device, second, info) == DeviceStatus::Ready);
    CHECK_FALSE(info.IsPrimary);
    CHECK(info.State == SurfaceState::Ready);
    CHECK(info.Width == 320);
    CHECK(info.Height == 200);
    CHECK(info.SubmittedFrames == 0);

    // Each surface keeps its own target; resizing one leaves the other unchanged.
    REQUIRE(SetFrameTarget(device, second, { .Width = 640, .Height = 360 }) == DeviceStatus::Ready);
    REQUIRE(SetFrameTarget(device, primary, { .Width = 100, .Height = 50 }) == DeviceStatus::Ready);
    CHECK(backend::SurfaceTargets[1].Width == 640);
    CHECK(backend::SurfaceTargets[0].Width == 100);
    REQUIRE(GetSurfaceInfo(device, second, info) == DeviceStatus::Ready);
    CHECK(info.Width == 640);
    CHECK(info.Height == 360);

    // One frame per device: it belongs to the surface BeginFrame chose.
    REQUIRE(BeginFrame(device, second) == DeviceStatus::Ready);
    CHECK(backend::SelectedSurface == 1);
    FrameInfo acquired{ .Width = 777 };
    CHECK(GetFrameInfo(device, primary, acquired) == DeviceStatus::InvalidState);
    CHECK(acquired.Width == 777);
    REQUIRE(GetFrameInfo(device, second, acquired) == DeviceStatus::Ready);
    CHECK(BeginFrame(device, primary) == DeviceStatus::InvalidState);
    CHECK(SetFrameTarget(device, primary, {}) == DeviceStatus::InvalidState);
    SurfaceHandle during;
    CHECK(CreateSurface(device, {}, during) == DeviceStatus::InvalidState);
    SurfaceHandle open = second;
    CHECK(DestroySurface(device, open) == DeviceStatus::InvalidState);
    CHECK(open == second);
    REQUIRE(EndFrame(device) == DeviceStatus::Ready);
    REQUIRE(BeginFrame(device, primary) == DeviceStatus::Ready);
    CHECK(backend::SelectedSurface == 0);
    REQUIRE(EndFrame(device) == DeviceStatus::Ready);
    REQUIRE(BeginFrame(device, second) == DeviceStatus::Ready);
    REQUIRE(EndFrame(device) == DeviceStatus::Ready);

    // Submission and presentation are counted per surface and reported apart from rendering.
    backend::SurfaceCompletions[1].RenderedFrames = 1;
    REQUIRE(GetSurfaceInfo(device, second, info) == DeviceStatus::Ready);
    CHECK(info.SubmittedFrames == 2);
    CHECK(info.PresentedFrames == 2);
    CHECK(info.RenderedFrames == 1);
    REQUIRE(GetSurfaceInfo(device, primary, info) == DeviceStatus::Ready);
    CHECK(info.SubmittedFrames == 1);

    // The legacy facade keeps driving the primary surface.
    REQUIRE(BeginFrameStatus() == FrameStatus::Ready);
    CHECK(backend::SelectedSurface == 0);
    REQUIRE(EndFrameStatus() == FrameStatus::Ready);
    REQUIRE(GetSurfaceInfo(device, primary, info) == DeviceStatus::Ready);
    CHECK(info.SubmittedFrames == 2);

    // The primary surface retires only with its device.
    SurfaceHandle primaryCopy = primary;
    CHECK(DestroySurface(device, primaryCopy) == DeviceStatus::InvalidState);
    CHECK(primaryCopy == primary);
    REQUIRE(DestroyDevice(device) == DeviceStatus::Ready);
}

TEST_CASE("R14b surface admission rejects borrowed targets, capacity and backend refusals", "[rhi][device][surface]")
{
    Reset();
    DeviceHandle device;
    SurfaceHandle primary;
    ludus::foundation::int32 view = 0;
    WindowInfo cocoa{ .System = ludus::platform::WindowSystem::Cocoa, .Width = 32, .Height = 32 };
    cocoa.CocoaView = &view;
    REQUIRE(CreateDevice({}, cocoa, {}, device, primary) == DeviceStatus::Pending);
    Ready();
    SurfaceHandle surface;
    CHECK(CreateSurface(device, {}, surface) == DeviceStatus::Unsupported);
    backend::AdditionalSurfaces = 2;
    // A target another live surface borrows, including the primary, is refused.
    CHECK(CreateSurface(device, cocoa, surface) == DeviceStatus::InvalidDescription);
    CHECK(CreateSurface(device, { .Width = 8192, .Height = 1 }, surface) == DeviceStatus::InvalidDescription);
    backend::NextSurfaceError = StartupError::InvalidWindow;
    CHECK(CreateSurface(device, {}, surface) == DeviceStatus::InvalidDescription);
    backend::NextSurfaceError = StartupError::BackendUnavailable;
    CHECK(CreateSurface(device, {}, surface) == DeviceStatus::Unsupported);
    backend::NextSurfaceError = StartupError::SurfaceUnavailable;
    CHECK(CreateSurface(device, {}, surface) == DeviceStatus::Failed);
    CHECK(surface == SurfaceHandle{});
    DeviceInfo deviceInfo;
    CHECK(GetDeviceInfo(device, deviceInfo) == DeviceStatus::Ready);
    backend::NextSurfaceError = StartupError::None;
    SurfaceHandle first;
    SurfaceHandle second;
    SurfaceHandle third;
    REQUIRE(CreateSurface(device, {}, first) == DeviceStatus::Ready);
    REQUIRE(CreateSurface(device, {}, second) == DeviceStatus::Ready);
    CHECK(CreateSurface(device, {}, third) == DeviceStatus::CapacityExceeded);
    CHECK(third == SurfaceHandle{});
    REQUIRE(DestroyDevice(device) == DeviceStatus::Ready);
    DeviceHandle stale = device;
    CHECK(CreateSurface(stale, {}, third) == DeviceStatus::InvalidHandle);
}

TEST_CASE("R14b losing or destroying one surface leaves the device and other surfaces working",
          "[rhi][device][surface]")
{
    Reset();
    backend::AdditionalSurfaces = 2;
    DeviceHandle device;
    SurfaceHandle primary;
    REQUIRE(CreateDevice({}, {}, {}, device, primary) == DeviceStatus::Pending);
    Ready();
    SurfaceHandle kept;
    SurfaceHandle lost;
    REQUIRE(CreateSurface(device, {}, kept) == DeviceStatus::Ready);
    REQUIRE(CreateSurface(device, {}, lost) == DeviceStatus::Ready);
    REQUIRE(SetFrameTarget(device, lost, { .Width = 16, .Height = 16 }) == DeviceStatus::Ready);
    backend::ShouldLoseNextSurface = true;
    CHECK(BeginFrame(device, lost) == DeviceStatus::SurfaceLost);
    DeviceInfo deviceInfo;
    REQUIRE(GetDeviceInfo(device, deviceInfo) == DeviceStatus::Ready);
    CHECK_FALSE(deviceInfo.FrameOpen);
    SurfaceInfo info;
    REQUIRE(GetSurfaceInfo(device, lost, info) == DeviceStatus::Ready);
    CHECK(info.State == SurfaceState::Lost);
    CHECK(BeginFrame(device, lost) == DeviceStatus::SurfaceLost);
    CHECK(SetFrameTarget(device, lost, {}) == DeviceStatus::SurfaceLost);
    REQUIRE(GetSurfaceInfo(device, kept, info) == DeviceStatus::Ready);
    CHECK(info.State == SurfaceState::Ready);
    REQUIRE(BeginFrame(device, kept) == DeviceStatus::Ready);
    REQUIRE(EndFrame(device) == DeviceStatus::Ready);
    REQUIRE(BeginFrame(device, primary) == DeviceStatus::Ready);
    REQUIRE(EndFrame(device) == DeviceStatus::Ready);

    // Destroying the lost surface retires only its slot; stale copies are rejected.
    const SurfaceHandle staleLost = lost;
    REQUIRE(DestroySurface(device, lost) == DeviceStatus::Ready);
    CHECK(lost == SurfaceHandle{});
    CHECK(backend::DestroyedSurfaces == 1);
    CHECK(backend::LastDestroyedSurface == 2);
    CHECK(BeginFrame(device, staleLost) == DeviceStatus::InvalidHandle);
    CHECK(GetSurfaceInfo(device, staleLost, info) == DeviceStatus::InvalidHandle);
    SurfaceHandle staleCopy = staleLost;
    CHECK(DestroySurface(device, staleCopy) == DeviceStatus::InvalidHandle);
    CHECK(staleCopy == staleLost);

    // A replacement reuses the slot under a new identity.
    SurfaceHandle replacement;
    REQUIRE(CreateSurface(device, {}, replacement) == DeviceStatus::Ready);
    CHECK(replacement != staleLost);
    CHECK(BeginFrame(device, staleLost) == DeviceStatus::InvalidHandle);
    REQUIRE(GetSurfaceInfo(device, replacement, info) == DeviceStatus::Ready);
    CHECK(info.State == SurfaceState::Ready);
    CHECK(info.SubmittedFrames == 0);
    REQUIRE(BeginFrame(device, replacement) == DeviceStatus::Ready);
    REQUIRE(EndFrame(device) == DeviceStatus::Ready);

    // Device loss stops every surface, yet created surfaces can still be destroyed.
    internal::Fail(backend::PendingToken, StartupError::DeviceLost);
    CHECK(BeginFrame(device, kept) == DeviceStatus::DeviceLost);
    REQUIRE(DestroySurface(device, kept) == DeviceStatus::Ready);

    // Surfaces never outlive their device or move to the next session.
    REQUIRE(DestroyDevice(device) == DeviceStatus::Ready);
    DeviceHandle next;
    SurfaceHandle nextPrimary;
    REQUIRE(CreateDevice({}, {}, {}, next, nextPrimary) == DeviceStatus::Pending);
    Ready();
    CHECK(BeginFrame(next, replacement) == DeviceStatus::InvalidHandle);
    CHECK(BeginFrame(next, primary) == DeviceStatus::InvalidHandle);
    CHECK(GetSurfaceInfo(next, replacement, info) == DeviceStatus::InvalidHandle);
    REQUIRE(DestroyDevice(next) == DeviceStatus::Ready);
}
