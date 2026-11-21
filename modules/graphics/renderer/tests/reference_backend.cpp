#include "internal/adapter.h"
#include "internal/backend.h"
#include "internal/lifecycle.h"
#include "internal/owner.h"
#include "internal/resources.h"
#include "internal/surface.h"
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/graphics/rhi/render.h>

namespace ludus::graphics::rhi::backend
{
using ludus::foundation::uint32;
uint32 PendingToken = 0;
uint32 ShutdownCount = 0;
StartupError ImmediateError = StartupError::None;
FrameStatus NextFrame = FrameStatus::Ready;
Backend ActiveKind = Backend::WebGPU;
bool SelectionSupported = true;
Backend Kind() noexcept
{
    return ActiveKind;
}
bool Supports(BackendSelection) noexcept
{
    return SelectionSupported;
}
// R14a: the renderer's reference backend publishes no adapter catalog.
AdapterStatus EnumerateAdapters(AdapterCatalog&) noexcept
{
    return AdapterStatus::Unsupported;
}
bool GetSelectedAdapter(AdapterInfo&) noexcept
{
    return false;
}
// R14c: the renderer's reference backend keeps one session.
foundation::usize GetDeviceOwnerLimit(bool) noexcept
{
    return 1;
}
// R14b: the renderer's reference backend hosts only its primary surface.
foundation::usize AdditionalSurfaceLimit() noexcept
{
    return 0;
}
StartupError CreateSurface(foundation::usize, const WindowInfo&) noexcept
{
    return StartupError::BackendUnavailable;
}
void DestroySurface(foundation::usize) noexcept {}
void SelectSurface(foundation::usize) noexcept {}
SurfaceCompletion GetSurfaceCompletion(foundation::usize) noexcept
{
    return {};
}
StartupError Start(const ApplicationInfo&, const WindowInfo&, uint32 token, BackendSelection) noexcept
{
    PendingToken = token;
    return ImmediateError;
}
void Shutdown() noexcept
{
    ++ShutdownCount;
}
FrameStatus SetTarget(const FrameTarget&) noexcept
{
    return FrameStatus::Ready;
}
FrameStatus Begin() noexcept
{
    return NextFrame;
}
FrameStatus End() noexcept
{
    return FrameStatus::Ready;
}
} // namespace ludus::graphics::rhi::backend
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
