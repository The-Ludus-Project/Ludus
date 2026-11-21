#include "internal/adapter.h"
#include "internal/backend.h"
#include "internal/lifecycle.h"
#include "internal/owner.h"
#include "internal/resources.h"
#include "internal/surface.h"
#include "reference_raster.h"
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/render.h>
#include <ludus/graphics/rhi/rhi.h>

#include <initializer_list>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
namespace ludus::graphics::rhi::backend
{
using ludus::foundation::uint32;
uint32 PendingToken = 0;
uint32 ShutdownCount = 0;
StartupError ImmediateError = StartupError::None;
FrameStatus NextFrame = FrameStatus::Ready;
/// Result of the next frame submission.
FrameStatus NextFrameEnd = FrameStatus::Ready;
Backend ActiveKind = Backend::WebGPU;
bool SelectionSupported = true;
Backend Kind() noexcept
{
    return ActiveKind;
}
/// R14a fake catalog: CatalogStatus is returned and Catalog copied on Ready.
AdapterStatus CatalogStatus = AdapterStatus::Unsupported;
AdapterCatalog Catalog;
/// Adapter reported for the live session; HasSelectedAdapter false reports none.
bool HasSelectedAdapter = false;
AdapterInfo SelectedAdapter;
/// Identity the facade requested when Start was last called; automatic when none.
AdapterIdentity StartedAdapter;
/// Complete startup inside Start, as the synchronous native backends do.
bool ShouldCompleteDuringStart = false;
AdapterStatus EnumerateAdapters(AdapterCatalog& outCatalog) noexcept
{
    if (CatalogStatus == AdapterStatus::Ready)
    {
        outCatalog = Catalog;
    }
    return CatalogStatus;
}
bool GetSelectedAdapter(AdapterInfo& outAdapter) noexcept
{
    if (HasSelectedAdapter == true)
    {
        outAdapter = SelectedAdapter;
    }
    return HasSelectedAdapter;
}
bool Supports(BackendSelection) noexcept
{
    return SelectionSupported;
}
StartupError Start(const ApplicationInfo&, const WindowInfo&, uint32 token, BackendSelection) noexcept
{
    const AdapterIdentity* requestedOrNull = internal::GetRequestedAdapterOrNull();
    StartedAdapter = requestedOrNull != nullptr ? *requestedOrNull : AdapterIdentity{};
    PendingToken = token;
    if (ShouldCompleteDuringStart == true && ImmediateError == StartupError::None)
    {
        // Native backends publish Ready synchronously from Start.
        internal::Complete(token, StartupError::None, { .MaxFrameDimension2D = 4096, .MaxUniformBufferSize = 16384 });
    }
    return ImmediateError;
}
void Shutdown() noexcept
{
    ++ShutdownCount;
}
// R14b fake surfaces: AdditionalSurfaces sets the host limit; the slot SelectSurface
// last chose receives SetTarget, Begin and End.
foundation::usize AdditionalSurfaces = 0;
StartupError NextSurfaceError = StartupError::None;
foundation::usize SelectedSurface = 0;
/// DestroySurface calls and the slot of the most recent one.
foundation::uint32 DestroyedSurfaces = 0;
foundation::usize LastDestroyedSurface = 0;
/// The next Begin on a created surface reports that surface lost instead of acquiring.
bool ShouldLoseNextSurface = false;
foundation::StaticArray<SurfaceCompletion, MAX_DEVICE_SURFACES> SurfaceCompletions{};
foundation::StaticArray<FrameTarget, MAX_DEVICE_SURFACES> SurfaceTargets{};
// R14c: owners this reference backend admits; owner tests raise it and restore 1. A native
// device is hosted only as the sole owner, as on Vulkan, whatever DeviceOwners allows.
foundation::usize DeviceOwners = 1;
foundation::usize GetDeviceOwnerLimit(bool isNativeDevice) noexcept
{
    return isNativeDevice == true ? 1 : DeviceOwners;
}
foundation::usize AdditionalSurfaceLimit() noexcept
{
    return AdditionalSurfaces;
}
StartupError CreateSurface(foundation::usize slot, const WindowInfo&) noexcept
{
    if (NextSurfaceError == StartupError::None)
    {
        SurfaceCompletions[slot] = {};
        SurfaceTargets[slot] = {};
    }
    return NextSurfaceError;
}
void DestroySurface(foundation::usize slot) noexcept
{
    ++DestroyedSurfaces;
    LastDestroyedSurface = slot;
    SurfaceCompletions[slot] = {};
}
void SelectSurface(foundation::usize slot) noexcept
{
    SelectedSurface = slot;
}
SurfaceCompletion GetSurfaceCompletion(foundation::usize slot) noexcept
{
    return SurfaceCompletions[slot];
}
FrameStatus SetTarget(const FrameTarget& target) noexcept
{
    SurfaceTargets[SelectedSurface] = target;
    return FrameStatus::Ready;
}
FrameStatus Begin() noexcept
{
    if (ShouldLoseNextSurface == true && SelectedSurface != 0)
    {
        ShouldLoseNextSurface = false;
        internal::LoseSurface(SelectedSurface);
        return FrameStatus::Skipped;
    }
    return NextFrame;
}
FrameStatus End() noexcept
{
    reference::CloseAnnotationScopes();
    if (NextFrameEnd == FrameStatus::Ready)
    {
        ++SurfaceCompletions[SelectedSurface].PresentedFrames;
    }
    return NextFrameEnd;
}
} // namespace ludus::graphics::rhi::backend
using namespace ludus::graphics::rhi;
TEST_CASE("RHI pending requests are nonblocking and invalidated across restart", "[rhi][lifecycle]")
{
    Shutdown();
    backend::ImmediateError = StartupError::None;
    REQUIRE(Start({}, {}) == StartStatus::Pending);
    const auto old = backend::PendingToken;
    CHECK(BeginFrameStatus() == FrameStatus::NotReady);
    CHECK(EndFrameStatus() == FrameStatus::NotReady);
    CHECK(Start({}, {}) == StartStatus::Busy);
    Shutdown();
    Shutdown();
    CHECK(GetStartup().State == StartupState::Idle);
    REQUIRE(Start({}, {}) == StartStatus::Pending);
    const auto current = backend::PendingToken;
    CHECK(current != old);
    internal::Complete(old, StartupError::None, { .MaxFrameDimension2D = 999, .MaxUniformBufferSize = 16384 });
    internal::Fail(old, StartupError::DeviceLost);
    CHECK(GetStartup().State == StartupState::Pending);
    internal::Complete(current, StartupError::None, { .MaxFrameDimension2D = 4096, .MaxUniformBufferSize = 16384 });
    CHECK(GetStartup().State == StartupState::Ready);
    CHECK(GetStartup().MaxTextureDimension2D == 4096);
    CHECK(Start({}, {}) == StartStatus::Busy);
    CHECK(EndFrameStatus() == FrameStatus::InvalidState);
    CHECK(BeginFrameStatus() == FrameStatus::Ready);
    CHECK(BeginFrameStatus() == FrameStatus::InvalidState);
    CHECK(EndFrameStatus() == FrameStatus::Ready);
    internal::Fail(current, StartupError::DeviceLost);
    CHECK(GetStartup().State == StartupState::DeviceLost);
    CHECK(BeginFrameStatus() == FrameStatus::NotReady);
    internal::Complete(current, StartupError::None, { .MaxFrameDimension2D = 8192, .MaxUniformBufferSize = 16384 });
    CHECK(GetStartup().State == StartupState::DeviceLost);
    Shutdown();
}
TEST_CASE("RHI request failures remain explicit and require shutdown before retry", "[rhi][lifecycle]")
{
    const StartupError errors[] = {StartupError::InstanceUnavailable,
                                   StartupError::AdapterUnavailable,
                                   StartupError::DeviceUnavailable,
                                   StartupError::SurfaceUnavailable,
                                   StartupError::RenderingUnavailable,
                                   StartupError::Validation};
    for (const auto error : errors)
    {
        Shutdown();
        backend::ImmediateError = StartupError::None;
        REQUIRE(Start({}, {}) == StartStatus::Pending);
        const auto token = backend::PendingToken;
        internal::Complete(token, error, { .MaxFrameDimension2D = 0, .MaxUniformBufferSize = 16384 });
        CHECK(GetStartup().State == StartupState::Failed);
        CHECK(GetStartup().Error == error);
        CHECK_FALSE(internal::Current(token));
        CHECK(Start({}, {}) == StartStatus::Busy);
        CHECK(BeginFrameStatus() == FrameStatus::NotReady);
        Shutdown();
        backend::ImmediateError = error;
        CHECK(Start({}, {}) == StartStatus::Failed);
        CHECK(GetStartup().Error == error);
    }
    Shutdown();
    backend::ImmediateError = StartupError::None;
}

TEST_CASE("Skipped RHI frames never open a frame and targets cannot change during encoding", "[rhi][lifecycle]")
{
    Shutdown();
    backend::ImmediateError = StartupError::None;
    CHECK(SetFrameTarget({}) == FrameStatus::NotReady);
    REQUIRE(Start({}, {}) == StartStatus::Pending);
    internal::Complete(backend::PendingToken,
                       StartupError::None,
                       { .MaxFrameDimension2D = 4096, .MaxUniformBufferSize = 16384 });
    backend::NextFrame = FrameStatus::Skipped;
    CHECK(SetFrameTarget({}) == FrameStatus::Ready);
    CHECK(BeginFrameStatus() == FrameStatus::Skipped);
    CHECK(EndFrameStatus() == FrameStatus::InvalidState);
    CHECK(SetFrameTarget({ .Width = 640, .Height = 360 }) == FrameStatus::Ready);
    backend::NextFrame = FrameStatus::Ready;
    REQUIRE(BeginFrameStatus() == FrameStatus::Ready);
    CHECK(SetFrameTarget({}) == FrameStatus::InvalidState);
    internal::Fail(backend::PendingToken, StartupError::DeviceLost);
    CHECK(EndFrameStatus() == FrameStatus::NotReady);
    Shutdown();
}

namespace ludus::graphics::rhi::backend
{
using foundation::uint8;
using foundation::usize;
ResourceStatus NextResource = ResourceStatus::Ready;
uint32 LastResource = 0;
uint32 Released = 0;
uint32 Draws = 0;
bool LoseDuringCreation = false;
ResourceStatus CreateShader(usize, const ShaderDescription&, uint32 id) noexcept
{
    LastResource = id;
    return NextResource;
}
ResourceStatus CreateUniform(usize, const UniformDescription&, uint32 id) noexcept
{
    LastResource = id;
    if (LoseDuringCreation)
    {
        LoseDuringCreation = false;
        internal::Fail(PendingToken, StartupError::DeviceLost);
    }
    return NextResource;
}
ResourceStatus CreatePipeline(usize, const PipelineResources&, uint32 id) noexcept
{
    LastResource = id;
    return NextResource;
}
void DestroyShader(usize) noexcept
{
    ++Released;
}
void DestroyUniform(usize) noexcept
{
    ++Released;
}
void DestroyPipeline(usize) noexcept
{
    ++Released;
}
void UpdateUniform(usize, ludus::foundation::BorrowedView<const uint8>) noexcept {}
FrameInfo GetFrameInfo() noexcept
{
    return { .Width = 96, .Height = 64 };
}
ResourceStatus Draw(usize) noexcept
{
    ++Draws;
    return ResourceStatus::Ready;
}
} // namespace ludus::graphics::rhi::backend
TEST_CASE("Public resources reject pending, stale and incomplete draws across sessions", "[rhi][resources]")
{
    Shutdown();
    backend::ImmediateError = StartupError::None;
    backend::NextFrame = FrameStatus::Ready;
    REQUIRE(Start({}, {}) == StartStatus::Pending);
    internal::Complete(backend::PendingToken,
                       StartupError::None,
                       { .MaxFrameDimension2D = 4096, .MaxUniformBufferSize = 16384 });
    ShaderHandle vertex, fragment;
    UniformHandle uniform;
    PipelineHandle pipeline;
    const ShaderDescription vertexDesc
    {
        .Stage = ShaderStage::Vertex,
        .UniformSize = 48,
        .Spirv = {},
        .Wgsl = "generated vertex",
        .SpirvEntry = {},
        .WgslEntry = "vertexMain",
    };
    backend::NextResource = ResourceStatus::Pending;
    REQUIRE(CreateShader(vertexDesc, vertex) == ResourceStatus::Pending);
    const auto abandoned = backend::LastResource;
    REQUIRE(Destroy(vertex) == ResourceStatus::Ready);
    vertex = {};
    REQUIRE(CreateShader(vertexDesc, vertex) == ResourceStatus::Pending);
    internal::ResourceComplete(abandoned, ResourceStatus::Ready);
    REQUIRE(GetStatus(vertex) == ResourceStatus::Pending);
    internal::ResourceComplete(backend::LastResource, ResourceStatus::Ready);
    backend::NextResource = ResourceStatus::Ready;
    REQUIRE(CreateShader(
                {
                    .Stage = ShaderStage::Fragment,
                    .Spirv = {},
                    .Wgsl = "generated fragment",
                    .SpirvEntry = {},
                    .WgslEntry = "fragmentMain",
                },
                fragment) == ResourceStatus::Ready);
    UniformHandle undersized;
    REQUIRE(CreateUniform(16, undersized) == ResourceStatus::Ready);
    CHECK(CreatePipeline({vertex, fragment, undersized}, pipeline) == ResourceStatus::InvalidDescription);
    REQUIRE(Destroy(undersized) == ResourceStatus::Ready);
    REQUIRE(CreateUniform(48, uniform) == ResourceStatus::Ready);
    CHECK(CreateUniform(12, uniform) == ResourceStatus::InvalidState);
    backend::NextResource = ResourceStatus::Pending;
    ShaderHandle waiting;
    REQUIRE(CreateShader(vertexDesc, waiting) == ResourceStatus::Pending);
    CHECK(CreatePipeline({waiting, fragment, uniform}, pipeline) == ResourceStatus::NotReady);
    CHECK(GetStatus(pipeline) == ResourceStatus::InvalidHandle);
    REQUIRE(Destroy(waiting) == ResourceStatus::Ready);
    REQUIRE(CreatePipeline({vertex, fragment, uniform}, pipeline) == ResourceStatus::Pending);
    const auto abandonedPipeline = backend::LastResource;
    REQUIRE(Destroy(pipeline) == ResourceStatus::Ready);
    pipeline = {};
    REQUIRE(CreatePipeline({vertex, fragment, uniform}, pipeline) == ResourceStatus::Pending);
    internal::ResourceComplete(abandonedPipeline, ResourceStatus::Ready);
    const auto pendingPipeline = backend::LastResource;
    const auto submitted = backend::Draws;
    CHECK(GetStatus(pipeline) == ResourceStatus::Pending);
    CHECK(Destroy(vertex) == ResourceStatus::InUse);
    CHECK(Destroy(uniform) == ResourceStatus::InUse);
    REQUIRE(BeginFrameStatus() == FrameStatus::Ready);
    CHECK(DrawFullscreen(pipeline) == ResourceStatus::Pending);
    CHECK(backend::Draws == submitted);
    REQUIRE(EndFrameStatus() == FrameStatus::Ready);
    internal::ResourceComplete(pendingPipeline, ResourceStatus::Ready);
    backend::NextResource = ResourceStatus::Ready;
    REQUIRE(BeginFrameStatus() == FrameStatus::Ready);
    CHECK(DrawFullscreen(pipeline) == ResourceStatus::InvalidState);
    CHECK(Destroy(pipeline) == ResourceStatus::InvalidState);
    REQUIRE(EndFrameStatus() == FrameStatus::Ready);
    ludus::foundation::uint8 bytes[48]{};
    REQUIRE(UpdateUniform(uniform, bytes) == ResourceStatus::Ready);
    REQUIRE(BeginFrameStatus() == FrameStatus::Ready);
    CHECK(UpdateUniform(uniform, bytes) == ResourceStatus::Ready);
    CHECK(GetFrameInfo().Width == 96);
    REQUIRE(DrawFullscreen(pipeline) == ResourceStatus::Ready);
    CHECK(DrawFullscreen(pipeline) == ResourceStatus::InvalidState);
    CHECK(UpdateUniform(uniform, bytes) == ResourceStatus::InvalidState);
    REQUIRE(EndFrameStatus() == FrameStatus::Ready);
    const auto oldPipeline = pipeline;
    REQUIRE(Destroy(pipeline) == ResourceStatus::Ready);
    CHECK(GetStatus(oldPipeline) == ResourceStatus::InvalidHandle);
    pipeline = {};
    backend::NextResource = ResourceStatus::Failed;
    const auto partialReleases = backend::Released;
    REQUIRE(CreatePipeline({vertex, fragment, uniform}, pipeline) == ResourceStatus::Failed);
    CHECK(backend::Released == partialReleases + 1);
    CHECK(DrawFullscreen(pipeline) == ResourceStatus::Failed);
    // Failed creation releases partial state and no longer owns dependencies.
    REQUIRE(Destroy(uniform) == ResourceStatus::Ready);
    REQUIRE(Destroy(vertex) == ResourceStatus::Ready);
    REQUIRE(Destroy(fragment) == ResourceStatus::Ready);
    REQUIRE(Destroy(pipeline) == ResourceStatus::Ready);
    backend::NextResource = ResourceStatus::Ready;
    Shutdown();
    CHECK(DrawFullscreen(oldPipeline) == ResourceStatus::InvalidHandle);
}
TEST_CASE("Public resource failure releases partial backend allocations and capacity is bounded", "[rhi][resources]")
{
    Shutdown();
    backend::ImmediateError = StartupError::None;
    REQUIRE(Start({}, {}) == StartStatus::Pending);
    internal::Complete(backend::PendingToken,
                       StartupError::None,
                       { .MaxFrameDimension2D = 4096, .MaxUniformBufferSize = 16384 });
    UniformHandle handles[internal::RESOURCE_CAPACITY];
    backend::NextResource = ResourceStatus::Failed;
    const auto released = backend::Released;
    REQUIRE(CreateUniform(16, handles[0]) == ResourceStatus::Failed);
    CHECK(backend::Released == released + 1);
    CHECK(GetStatus(handles[0]) == ResourceStatus::Failed);
    REQUIRE(Destroy(handles[0]) == ResourceStatus::Ready);
    handles[0] = {};
    backend::NextResource = ResourceStatus::Ready;
    for (auto& handle : handles)
    {
        REQUIRE(CreateUniform(16, handle) == ResourceStatus::Ready);
    }
    UniformHandle overflow;
    CHECK(CreateUniform(16, overflow) == ResourceStatus::CapacityExceeded);
    CHECK(CreateUniform(17, overflow) == ResourceStatus::InvalidDescription);
    CHECK(UpdateUniform(handles[0], {}) == ResourceStatus::InvalidDescription);
    internal::Fail(backend::PendingToken, StartupError::DeviceLost);
    for (auto handle : handles)
    {
        CHECK(GetStatus(handle) == ResourceStatus::InvalidHandle);
    }
    Shutdown();
    REQUIRE(Start({}, {}) == StartStatus::Pending);
    internal::Complete(backend::PendingToken,
                       StartupError::None,
                       { .MaxFrameDimension2D = 4096, .MaxUniformBufferSize = 16384 });
    REQUIRE(CreateUniform(16, overflow) == ResourceStatus::Ready);
    CHECK(GetStatus(handles[0]) == ResourceStatus::InvalidHandle);
    backend::NextResource = ResourceStatus::Pending;
    backend::LoseDuringCreation = true;
    UniformHandle interrupted;
    CHECK(CreateUniform(16, interrupted) == ResourceStatus::Failed);
    CHECK(GetStartup().State == StartupState::DeviceLost);
    CHECK(GetStatus(interrupted) == ResourceStatus::InvalidHandle);
    CHECK(GetStatus(overflow) == ResourceStatus::InvalidHandle);
    internal::ResourceComplete(backend::LastResource, ResourceStatus::Ready);
    CHECK(GetStatus(interrupted) == ResourceStatus::InvalidHandle);
    backend::NextResource = ResourceStatus::Ready;
    Shutdown();
}

TEST_CASE("Fallback generations isolate stale callbacks and retain both attempt outcomes", "[rhi][lifecycle]")
{
    Shutdown();
    backend::ImmediateError = StartupError::None;
    REQUIRE(Start({}, {}) == StartStatus::Pending);
    const auto old = backend::PendingToken;
    internal::SelectBackend(old, Backend::WebGPU);
    internal::SetFallback([](ludus::foundation::uint32 token, StartupError error) noexcept {
        internal::RecordAttempt(token, Backend::WebGPU, error);
        const auto next = internal::Reissue(token);
        if (next == 0)
        {
            return false;
        }
        internal::SelectBackend(next, Backend::WebGL2);
        internal::Complete(next, StartupError::None, { .MaxFrameDimension2D = 2048, .MaxUniformBufferSize = 16384 });
        return true;
    });
    internal::Fail(old, StartupError::AdapterUnavailable);
    CHECK(GetStartup().State == StartupState::Ready);
    CHECK(GetStartup().SelectedBackend == Backend::WebGL2);
    CHECK(GetStartup().WebGpu.Attempted);
    CHECK(GetStartup().WebGpu.Error == StartupError::AdapterUnavailable);
    CHECK(GetStartup().WebGL2.Attempted);
    CHECK(GetStartup().WebGL2.Error == StartupError::None);
    CHECK_FALSE(internal::Current(old));
    internal::Fail(old, StartupError::DeviceLost);
    internal::Complete(old, StartupError::None, { .MaxFrameDimension2D = 9999, .MaxUniformBufferSize = 16384 });
    CHECK(GetStartup().State == StartupState::Ready);
    CHECK(GetStartup().MaxTextureDimension2D == 2048);
    backend::ActiveKind = Backend::WebGL2;
    ShaderHandle shader;
    ShaderDescription description;
    description.GlslEs = "#version 300 es\nvoid main() {}";
    description.GlslEsEntry = "main";
    CHECK(CreateShader(description, shader) == ResourceStatus::Ready);
    description.GlslEsEntry = "arbitrary";
    ShaderHandle bad;
    CHECK(CreateShader(description, bad) == ResourceStatus::InvalidDescription);
    Shutdown();
    CHECK(GetStatus(shader) == ResourceStatus::InvalidHandle);
    backend::ActiveKind = Backend::WebGPU;
}

TEST_CASE("Two failed attempts finalize without recursive fallback", "[rhi][lifecycle]")
{
    Shutdown();
    backend::ImmediateError = StartupError::None;
    REQUIRE(Start({}, {}) == StartStatus::Pending);
    const auto old = backend::PendingToken;
    internal::SelectBackend(old, Backend::WebGPU);
    internal::SetFallback([](ludus::foundation::uint32 token, StartupError error) noexcept {
        if (GetStartup().SelectedBackend != Backend::WebGPU)
        {
            return false;
        }
        internal::RecordAttempt(token, Backend::WebGPU, error);
        const auto next = internal::Reissue(token);
        if (next == 0)
        {
            return false;
        }
        internal::SelectBackend(next, Backend::WebGL2);
        internal::Fail(next, StartupError::DeviceUnavailable);
        return true;
    });
    internal::Fail(old, StartupError::AdapterUnavailable);
    CHECK(GetStartup().State == StartupState::Failed);
    CHECK(GetStartup().Error == StartupError::DeviceUnavailable);
    CHECK(GetStartup().WebGpu.Error == StartupError::AdapterUnavailable);
    CHECK(GetStartup().WebGL2.Error == StartupError::DeviceUnavailable);
    CHECK(GetStartup().WebGL2.Attempted);
    CHECK(BeginFrameStatus() == FrameStatus::NotReady);
    Shutdown();
    REQUIRE(Start({}, {}, BackendSelection::WebGPU) == StartStatus::Pending);
    CHECK(GetStartup().Requested == BackendSelection::WebGPU);
    internal::Complete(backend::PendingToken,
                       StartupError::None,
                       { .MaxFrameDimension2D = 4096, .MaxUniformBufferSize = 16384 });
    internal::SetFallback([](ludus::foundation::uint32, StartupError) noexcept { return true; });
    // A ready session never hides loss behind a startup fallback, even when a
    // handler is installed. Shutdown clears it before the next session.
    internal::Fail(backend::PendingToken, StartupError::DeviceLost);
    CHECK(GetStartup().State == StartupState::DeviceLost);
    Shutdown();
}

TEST_CASE("Public uniforms admit the portable 16 KiB limit and reject larger payloads", "[rhi][resources]")
{
    Shutdown();
    backend::ImmediateError = StartupError::None;
    backend::NextResource = ResourceStatus::Ready;
    REQUIRE(Start({}, {}) == StartStatus::Pending);
    internal::Complete(backend::PendingToken,
                       StartupError::None,
                       { .MaxFrameDimension2D = 4096, .MaxUniformBufferSize = 16384 });
    UniformHandle uniform;
    CHECK(CreateUniform(16385, uniform) == ResourceStatus::InvalidDescription);
    CHECK(CreateUniform(16400, uniform) == ResourceStatus::InvalidDescription);
    REQUIRE(CreateUniform(16384, uniform) == ResourceStatus::Ready);
    const ludus::foundation::uint8 bytes[16384]{};
    CHECK(UpdateUniform(uniform, {nullptr, sizeof(bytes)}) == ResourceStatus::InvalidDescription);
    CHECK(UpdateUniform(uniform, {bytes, 16368}) == ResourceStatus::InvalidDescription);
    CHECK(UpdateUniform(uniform, bytes) == ResourceStatus::Ready);
    CHECK(Destroy(uniform) == ResourceStatus::Ready);
    Shutdown();
}

TEST_CASE("Negotiated capabilities bound resources and expire with the session", "[rhi][capabilities]")
{
    Shutdown();
    backend::ImmediateError = StartupError::None;
    backend::NextResource = ResourceStatus::Ready;
    CHECK(GetStartup().Capabilities.MaxShaders == 0);
    REQUIRE(Start({}, {}) == StartStatus::Pending);
    CHECK(GetStartup().Capabilities.MaxUniformBufferSize == 0);
    internal::Complete(backend::PendingToken,
                       StartupError::None,
                       { .MaxFrameDimension2D = 2048, .MaxUniformBufferSize = 79 });
    const auto caps = GetStartup().Capabilities;
    REQUIRE(GetStartup().State == StartupState::Ready);
    CHECK(caps.MaxFrameDimension2D == 2048);
    CHECK(caps.MaxUniformBufferSize == 64);
    CHECK(caps.UniformBufferSizeAlignment == 16);
    CHECK(caps.MaxShaders == 8);
    CHECK(caps.MaxUniformBuffers == 8);
    CHECK(caps.MaxPipelines == 8);
    CHECK(caps.MaxDrawsPerFrame == 1);
    CHECK(SetFrameTarget({ .Width = 2049, .Height = 1 }) == FrameStatus::Failed);
    CHECK(SetFrameTarget({ .Width = 1, .Height = 2049 }) == FrameStatus::Failed);
    CHECK(SetFrameTarget({ .Width = 2048, .Height = 2048 }) == FrameStatus::Ready);
    UniformHandle uniform;
    CHECK(CreateUniform(80, uniform) == ResourceStatus::InvalidDescription);
    CHECK(GetStatus(uniform) == ResourceStatus::InvalidHandle);
    ShaderDescription description;
    description.Wgsl = "shader";
    description.WgslEntry = "main";
    description.UniformSize = 65;
    ShaderHandle shader;
    CHECK(CreateShader(description, shader) == ResourceStatus::InvalidDescription);
    description.UniformSize = 64;
    REQUIRE(CreateShader(description, shader) == ResourceStatus::Ready);
    REQUIRE(CreateUniform(64, uniform) == ResourceStatus::Ready);
    internal::Fail(backend::PendingToken, StartupError::DeviceLost);
    CHECK(GetStartup().Capabilities.MaxUniformBufferSize == 0);
    CHECK(GetStartup().Capabilities.MaxFrameDimension2D == 0);
    CHECK(GetStatus(uniform) == ResourceStatus::InvalidHandle);
    Shutdown();
    CHECK(GetStartup().Capabilities.MaxDrawsPerFrame == 0);
    REQUIRE(Start({}, {}) == StartStatus::Pending);
    internal::Complete(backend::PendingToken,
                       StartupError::None,
                       { .MaxFrameDimension2D = 4096, .MaxUniformBufferSize = 65536 });
    CHECK(GetStartup().Capabilities.MaxUniformBufferSize == 16384);
    Shutdown();
}

TEST_CASE("Startup copies requirements and reports the first unmet limit", "[rhi][capabilities]")
{
    Shutdown();
    backend::ImmediateError = StartupError::None;
    DeviceRequirements requirements{ .MinFrameDimension2D = 2048, .MinUniformBufferSize = 64 };
    REQUIRE(Start({}, {}, BackendSelection::WebGPU, requirements) == StartStatus::Pending);
    requirements = {};
    CHECK(GetStartup().Requirements.MinFrameDimension2D == 2048);
    // A Busy request must not mutate the in-flight negotiation.
    CHECK(Start({}, {}, BackendSelection::Auto, {}) == StartStatus::Busy);
    CHECK(GetStartup().Requested == BackendSelection::WebGPU);
    internal::Complete(backend::PendingToken,
                       StartupError::None,
                       { .MaxFrameDimension2D = 1024, .MaxUniformBufferSize = 32 });
    CHECK(GetStartup().State == StartupState::Failed);
    CHECK(GetStartup().Error == StartupError::RequirementsUnsatisfied);
    CHECK(GetStartup().UnmetRequirement == RequirementFailure::FrameDimension2D);
    CHECK(GetStartup().WebGpu.Error == StartupError::RequirementsUnsatisfied);
    CHECK(GetStartup().Capabilities.MaxShaders == 0);
    UniformHandle uniform;
    CHECK(CreateUniform(16, uniform) == ResourceStatus::NotReady);
    CHECK(Start({}, {}) == StartStatus::Busy);
    Shutdown();
    REQUIRE(Start({}, {}, BackendSelection::Auto, { .MinUniformBufferSize = 65 }) == StartStatus::Pending);
    internal::Complete(backend::PendingToken,
                       StartupError::None,
                       { .MaxFrameDimension2D = 4096, .MaxUniformBufferSize = 79 });
    CHECK(GetStartup().UnmetRequirement == RequirementFailure::UniformBufferSize);
    Shutdown();
    REQUIRE(Start({}, {}, BackendSelection::Auto, { .MinFrameDimension2D = 2048, .MinUniformBufferSize = 64 }) ==
            StartStatus::Pending);
    internal::Complete(backend::PendingToken,
                       StartupError::None,
                       { .MaxFrameDimension2D = 2048, .MaxUniformBufferSize = 64 });
    CHECK(GetStartup().State == StartupState::Ready);
    CHECK(GetStartup().UnmetRequirement == RequirementFailure::None);
    Shutdown();
}

TEST_CASE("Fallback rechecks requirements and never publishes a rejected attempt", "[rhi][capabilities]")
{
    for (const bool meetsRequirement : {false, true})
    {
        Shutdown();
        backend::ImmediateError = StartupError::None;
        REQUIRE(Start({}, {}, BackendSelection::Auto, { .MinUniformBufferSize = 64 }) == StartStatus::Pending);
        const auto old = backend::PendingToken;
        internal::SetFallback([](ludus::foundation::uint32 token, StartupError error) noexcept {
            if (GetStartup().SelectedBackend != Backend::WebGPU || error != StartupError::RequirementsUnsatisfied)
            {
                return false;
            }
            internal::RecordAttempt(token, Backend::WebGPU, error);
            const auto next = internal::Reissue(token);
            internal::SelectBackend(next, Backend::WebGL2);
            backend::PendingToken = next;
            return true;
        });
        internal::Complete(old, StartupError::None, { .MaxFrameDimension2D = 4096, .MaxUniformBufferSize = 32 });
        REQUIRE(GetStartup().State == StartupState::Pending);
        CHECK(GetStartup().Capabilities.MaxUniformBufferSize == 0);
        CHECK(GetStartup().UnmetRequirement == RequirementFailure::None);
        internal::Complete(old, StartupError::None, { .MaxFrameDimension2D = 8192, .MaxUniformBufferSize = 16384 });
        CHECK(GetStartup().State == StartupState::Pending);
        internal::Complete(backend::PendingToken,
                           StartupError::None,
                           { .MaxFrameDimension2D = 2048, .MaxUniformBufferSize = meetsRequirement ? 64U : 32U });
        CHECK(GetStartup().WebGpu.Error == StartupError::RequirementsUnsatisfied);
        CHECK(GetStartup().SelectedBackend == Backend::WebGL2);
        CHECK(GetStartup().Requirements.MinUniformBufferSize == 64);
        if (meetsRequirement)
        {
            CHECK(GetStartup().State == StartupState::Ready);
            CHECK(GetStartup().Capabilities.MaxUniformBufferSize == 64);
            CHECK(GetStartup().WebGL2.Error == StartupError::None);
        }
        else
        {
            CHECK(GetStartup().State == StartupState::Failed);
            CHECK(GetStartup().WebGL2.Error == StartupError::RequirementsUnsatisfied);
            CHECK(GetStartup().UnmetRequirement == RequirementFailure::UniformBufferSize);
        }
    }
    Shutdown();
}

TEST_CASE("Invalid device limits and unavailable backends fail explicitly", "[rhi][capabilities]")
{
    const ludus::foundation::uint32 dimensions[] = {0, 1024};
    const ludus::foundation::uint32 uniformSizes[] = {16384, 15};
    for (ludus::foundation::usize i = 0; i < 2; ++i)
    {
        Shutdown();
        backend::ImmediateError = StartupError::None;
        REQUIRE(Start({}, {}) == StartStatus::Pending);
        internal::Complete(backend::PendingToken,
                           StartupError::None,
                           { .MaxFrameDimension2D = dimensions[i], .MaxUniformBufferSize = uniformSizes[i] });
        CHECK(GetStartup().State == StartupState::Failed);
        CHECK(GetStartup().Error == StartupError::DeviceUnavailable);
        CHECK(GetStartup().UnmetRequirement == RequirementFailure::None);
    }
    Shutdown();
    backend::SelectionSupported = false;
    CHECK(Start({}, {}, BackendSelection::WebGL2) == StartStatus::Failed);
    CHECK(GetStartup().Error == StartupError::BackendUnavailable);
    backend::SelectionSupported = true;
    Shutdown();
}

TEST_CASE("Metal facade consumes only the MSL artifact and a bounded explicit entry", "[rhi][resources][metal]")
{
    struct Guard final
    {
        ~Guard() noexcept
        {
            Shutdown();
            backend::ActiveKind = Backend::WebGPU;
        }
    } guard;
    Shutdown();
    backend::ActiveKind = Backend::Metal;
    backend::ImmediateError = StartupError::None;
    backend::NextResource = ResourceStatus::Ready;
    REQUIRE(Start({}, {}) == StartStatus::Pending);
    internal::Complete(backend::PendingToken,
                       StartupError::None,
                       { .MaxFrameDimension2D = 16384, .MaxUniformBufferSize = 16384 });
    ShaderDescription description;
    description.Wgsl = "browser artifact";
    description.WgslEntry = "vertexMain";
    ShaderHandle invalid;
    CHECK(CreateShader(description, invalid) == ResourceStatus::InvalidDescription);
    description.Msl = "Metal artifact";
    CHECK(CreateShader(description, invalid) == ResourceStatus::InvalidDescription);
    description.MslEntry = "vertexMain";
    ShaderHandle valid;
    REQUIRE(CreateShader(description, valid) == ResourceStatus::Ready);
    description.MslEntry = std::string_view("entry\0suffix", 12);
    CHECK(CreateShader(description, invalid) == ResourceStatus::InvalidDescription);
    description.MslEntry = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijkl";
    CHECK(CreateShader(description, invalid) == ResourceStatus::InvalidDescription);
}
