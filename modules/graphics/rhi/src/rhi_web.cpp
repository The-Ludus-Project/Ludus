#include "internal/adapter.h"
#include "internal/backend.h"
#include "internal/lifecycle.h"
#include "internal/owner.h"
#include "internal/resources.h"
#include "internal/surface.h"
#include "internal/web_backends.h"
#include "internal/webgpu_probe.h"
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/graphics/rhi/render.h>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/platform/browser/window.h>

#include <webgpu/webgpu.h>

// Browser Auto dispatcher. It routes the backend:: facade contract to one of two
// sub-backends (webgpu, webgl) compiled into the same artifact, and implements
// the WebGPU -> WebGL 2 startup fallback policy. Forced selections pin a single
// sub-backend. Native builds never compile this TU.
namespace ludus::graphics::rhi::backend
{
namespace
{
using namespace ludus::foundation;
constexpr logging::LogCategory LOG_RHI{"RHI-Web"};

/// @brief Sub-backend binding and Auto fallback context of one device owner (R14c).
///
/// Every owner binds its own sub-backend, so one owner may run WebGPU beside another's
/// WebGL 2 context.
struct WebDispatchState final
{
    /// The sub-backend bound for Kind()/resource/frame dispatch; meaningful while IsBound,
    /// which is false until a backend commits and after shutdown.
    Backend Active = Backend::WebGPU;
    bool IsBound = false;
    /// Session context captured for a possible Auto fallback from WebGPU to WebGL 2.
    BackendSelection Selection = BackendSelection::Auto;
    ApplicationInfo App;
    WindowInfo Window;
    bool IsWebGlTried = false;
};
/// One record per device owner slot (R14c); see internal/owner.h.
foundation::StaticArray<WebDispatchState, MAX_DEVICE_OWNERS> gWebDispatchStates{};
/// WebDispatchState of the bound owner slot.
WebDispatchState& GetWebDispatch() noexcept
{
    return gWebDispatchStates[internal::GetBoundOwnerSlot()];
}

/// @brief Make the bound owner's WebGL 2 context current when that owner runs WebGL 2.
///
/// Every GL call addresses the calling thread's current context, and each WebGL 2 owner has
/// its own context (R14c), so every route into the WebGL 2 sub-backend selects it first.
/// @return True when the bound owner runs WebGL 2; false for WebGPU.
bool MakeWebGlCurrent() noexcept
{
    if (GetWebDispatch().Active != Backend::WebGL2)
    {
        return false;
    }
    webgl::MakeContextCurrent();
    return true;
}
/// @brief True when `selector` names a canvas that another owner presents to (R14c).
///
/// The facade compares borrowed selector pointers. Distinct selector strings may name one
/// canvas, and a canvas holds one context, so the browser also compares the elements.
bool IsCanvasUsedByOtherOwner(const char* selector) noexcept
{
    const usize bound = internal::GetBoundOwnerSlot();
    for (usize slot = 0; slot < MAX_DEVICE_OWNERS; ++slot)
    {
        const WebDispatchState& state = gWebDispatchStates[slot];
        if (slot != bound && state.IsBound == true && state.Active == Backend::WebGL2 &&
            state.Window.CanvasSelector != nullptr &&
            webgpu::AreSameCanvases(state.Window.CanvasSelector, selector) == true)
        {
            return true;
        }
    }
    return webgpu::IsCanvasPresentedByOtherOwner(selector);
}
bool IsCapabilityFailure(StartupError error) noexcept
{
    // Only genuine "this backend cannot start here" conditions trigger fallback.
    // Shader/validation and invalid-call defects are never concealed by it.
    return error == StartupError::InstanceUnavailable || error == StartupError::AdapterUnavailable ||
           error == StartupError::DeviceUnavailable || error == StartupError::SurfaceUnavailable ||
           error == StartupError::RenderingUnavailable || error == StartupError::DeviceLost ||
           error == StartupError::RequirementsUnsatisfied;
}

StartupError StartWebGl(uint32 token, bool afterWebGpu) noexcept
{
    // Mark the attempt before preparation: a failed canvas swap must finalize
    // this attempt rather than recursively initiating another fallback.
    GetWebDispatch().IsWebGlTried = true;
    GetWebDispatch().Active = Backend::WebGL2;
    internal::SelectBackend(token, Backend::WebGL2);
    // A WebGPU attempt may have committed the canvas to the 'webgpu' context
    // mode, which blocks 'webgl2' on the same node. Ask Platform (which owns the
    // DOM node and its listeners) to swap in a fresh canvas before the WebGL 2
    // attempt. The forced-WebGL2 path (afterWebGpu == false) skips this.
    if (afterWebGpu && GetWebDispatch().Window.CanvasSelector != nullptr)
    {
        if (ludus::platform::browser::ReplaceCanvas(GetWebDispatch().Window.CanvasSelector) == false)
        {
            internal::RecordAttempt(token, Backend::WebGL2, StartupError::SurfaceUnavailable);
            return StartupError::SurfaceUnavailable;
        }
    }
    GetWebDispatch().IsBound = true;
    const StartupError error =
        webgl::Start(GetWebDispatch().App, GetWebDispatch().Window, token, GetWebDispatch().Selection);
    internal::RecordAttempt(token, Backend::WebGL2, error);
    return error;
}

// Fail() consults this before finalizing a Pending session. For Auto, a WebGPU
// capability failure triggers exactly one WebGL 2 attempt on a fresh generation.
bool Fallback(uint32 token, StartupError error) noexcept
{
    if (GetWebDispatch().Selection != BackendSelection::Auto || GetWebDispatch().Active != Backend::WebGPU ||
        GetWebDispatch().IsWebGlTried || IsCapabilityFailure(error) == false)
    {
        return false;
    }
    LUDUS_LOG_WARN(LOG_RHI, "WebGPU unavailable; attempting WebGL 2 fallback");
    internal::RecordAttempt(token, Backend::WebGPU, error);
    // Invalidate the old attempt BEFORE teardown, which may synchronously deliver
    // a device-lost callback. Advance to a fresh generation so any late callback
    // (carrying the old token) is dropped and cannot kill the WebGL 2 attempt.
    const auto next = internal::Reissue(token);
    if (next == 0)
    {
        return false;
    }
    webgpu::Shutdown();
    GetWebDispatch().IsBound = false;
    const auto error2 = StartWebGl(next, true);
    if (error2 != StartupError::None)
    {
        // The one WebGL 2 attempt also failed; finalize against the current token.
        internal::Fail(next, error2);
    }
    return true;
}
} // namespace

bool AllocatorBlockBytes(uint64& bytes) noexcept
{
    return MakeWebGlCurrent() == false ? webgpu::AllocatorBlockBytes(bytes) : webgl::AllocatorBlockBytes(bytes);
}
MeasurementAdapter TimingAdapter() noexcept
{
    return MakeWebGlCurrent() == false ? webgpu::TimingAdapter() : webgl::TimingAdapter();
}
RasterStatus TimingArm() noexcept
{
    return MakeWebGlCurrent() == false ? webgpu::TimingArm() : webgl::TimingArm();
}
GpuTiming TimingPoll() noexcept
{
    return MakeWebGlCurrent() == false ? webgpu::TimingPoll() : webgl::TimingPoll();
}
void TimingReset() noexcept
{
    if (MakeWebGlCurrent() == false)
    {
        webgpu::TimingReset();
    }
    else
    {
        webgl::TimingReset();
    }
}
bool RasterAllocationBytes(internal::RasterKind kind, usize slot, uint64& bytes) noexcept
{
    return MakeWebGlCurrent() == false ? webgpu::RasterAllocationBytes(kind, slot, bytes)
                                       : webgl::RasterAllocationBytes(kind, slot, bytes);
}
Backend Kind() noexcept
{
    return GetWebDispatch().Active;
}
// R14a: WebGPU requests one adapter through the browser and WebGL 2 has none to list,
// so the browser build publishes no catalog and rejects explicit adapter identities.
AdapterStatus EnumerateAdapters(AdapterCatalog&) noexcept
{
    return AdapterStatus::Unsupported;
}
bool GetSelectedAdapter(AdapterInfo&) noexcept
{
    return false;
}
bool Supports(BackendSelection selection) noexcept
{
    switch (selection)
    {
        case BackendSelection::Auto:
        case BackendSelection::WebGPU:
        case BackendSelection::WebGL2:
            return true;
    }
    return false;
}
StartupError
Start(const ApplicationInfo& app, const WindowInfo& window, uint32 token, BackendSelection selection) noexcept
{
    if (window.System != platform::WindowSystem::WebCanvas || window.CanvasSelector == nullptr)
    {
        return StartupError::InvalidWindow;
    }
    // Replacing the canvas below would detach another owner's context (R14c).
    if (IsCanvasUsedByOtherOwner(window.CanvasSelector) == true)
    {
        return StartupError::InvalidWindow;
    }
    // A restart may select a different API, or follow a lost context. Platform
    // rebinds the canvas before either API acquires its one allowed context mode.
    (void)platform::browser::ReplaceCanvas(window.CanvasSelector);
    internal::SetFallback(Fallback);
    GetWebDispatch().Selection = selection;
    GetWebDispatch().App = app;
    GetWebDispatch().Window = window;
    GetWebDispatch().IsWebGlTried = false;
    GetWebDispatch().IsBound = false;

    if (selection == BackendSelection::WebGL2)
    {
        return StartWebGl(token, false);
    }
    // Auto and forced WebGPU both begin with WebGPU (including its compatibility
    // retry). Auto then falls back to WebGL 2 via Fallback(); forced WebGPU does
    // not (GetWebDispatch().Selection != Auto makes Fallback() decline).
    GetWebDispatch().Active = Backend::WebGPU;
    GetWebDispatch().IsBound = true;
    internal::SelectBackend(token, Backend::WebGPU);
    const StartupError error = webgpu::Start(GetWebDispatch().App, GetWebDispatch().Window, token, selection);
    if (error != StartupError::None)
    {
        // Synchronous WebGPU failure. For Auto, try WebGL 2 now; otherwise report.
        internal::RecordAttempt(token, Backend::WebGPU, error);
        if (selection == BackendSelection::Auto && IsCapabilityFailure(error) && GetWebDispatch().IsWebGlTried == false)
        {
            const auto next = internal::Reissue(token);
            webgpu::Shutdown();
            GetWebDispatch().IsBound = false;
            return next != 0 ? StartWebGl(next, true) : error;
        }
        webgpu::Shutdown();
        GetWebDispatch().IsBound = false;
        return error;
    }
    return StartupError::None;
}
void Shutdown() noexcept
{
    // Clear the fallback first so a teardown-induced loss cannot start an attempt.
    internal::SetFallback(nullptr);
    if (GetWebDispatch().IsBound)
    {
        if (GetWebDispatch().Active == Backend::WebGL2)
        {
            webgl::Shutdown();
        }
        else
        {
            webgpu::Shutdown();
        }
    }
    GetWebDispatch().IsBound = false;
    GetWebDispatch().Active = Backend::WebGPU;
    GetWebDispatch().Selection = BackendSelection::Auto;
    GetWebDispatch().IsWebGlTried = false;
    GetWebDispatch().App = {};
    GetWebDispatch().Window = {};
}
FrameStatus SetTarget(const FrameTarget& target) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::SetTarget(target) : webgpu::SetTarget(target);
}
FrameStatus Begin() noexcept
{
    return MakeWebGlCurrent() == true ? webgl::Begin() : webgpu::Begin();
}
FrameStatus End() noexcept
{
    return MakeWebGlCurrent() == true ? webgl::End() : webgpu::End();
}
FrameInfo GetFrameInfo() noexcept
{
    return MakeWebGlCurrent() == true ? webgl::GetFrameInfo() : webgpu::GetFrameInfo();
}
ResourceStatus CreateShader(usize slot, const ShaderDescription& description, uint32 id) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::CreateShader(slot, description, id)
                                      : webgpu::CreateShader(slot, description, id);
}
ResourceStatus CreateUniform(usize slot, const UniformDescription& description, uint32 id) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::CreateUniform(slot, description, id)
                                      : webgpu::CreateUniform(slot, description, id);
}
ResourceStatus CreatePipeline(usize slot, const PipelineResources& resources, uint32 id) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::CreatePipeline(slot, resources, id)
                                      : webgpu::CreatePipeline(slot, resources, id);
}
void DestroyShader(usize slot) noexcept
{
    if (MakeWebGlCurrent() == true)
    {
        webgl::DestroyShader(slot);
    }
    else
    {
        webgpu::DestroyShader(slot);
    }
}
void DestroyUniform(usize slot) noexcept
{
    if (MakeWebGlCurrent() == true)
    {
        webgl::DestroyUniform(slot);
    }
    else
    {
        webgpu::DestroyUniform(slot);
    }
}
void DestroyPipeline(usize slot) noexcept
{
    if (MakeWebGlCurrent() == true)
    {
        webgl::DestroyPipeline(slot);
    }
    else
    {
        webgpu::DestroyPipeline(slot);
    }
}
void UpdateUniform(usize slot, ludus::foundation::BorrowedView<const uint8> bytes) noexcept
{
    if (MakeWebGlCurrent() == true)
    {
        webgl::UpdateUniform(slot, bytes);
    }
    else
    {
        webgpu::UpdateUniform(slot, bytes);
    }
}
ResourceStatus Draw(usize slot) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::Draw(slot) : webgpu::Draw(slot);
}
// R14c: every owner binds its own sub-backend and canvas. The browser adopts no native device.
usize GetDeviceOwnerLimit(bool isNativeDevice) noexcept
{
    return isNativeDevice == true ? 1 : MAX_DEVICE_OWNERS;
}
// R14b: WebGPU hosts further canvases on its device. A WebGL 2 context belongs to one
// canvas, so a WebGL 2 session hosts only its primary surface and reports no completion.
usize AdditionalSurfaceLimit() noexcept
{
    return GetWebDispatch().IsBound == true && GetWebDispatch().Active == Backend::WebGPU
               ? webgpu::AdditionalSurfaceLimit()
               : 0;
}
StartupError CreateSurface(usize slot, const WindowInfo& window) noexcept
{
    if (window.CanvasSelector != nullptr && IsCanvasUsedByOtherOwner(window.CanvasSelector) == true)
    {
        return StartupError::InvalidWindow;
    }
    return GetWebDispatch().IsBound == true && GetWebDispatch().Active == Backend::WebGPU
               ? webgpu::CreateSurface(slot, window)
               : StartupError::BackendUnavailable;
}
void DestroySurface(usize slot) noexcept
{
    if (GetWebDispatch().IsBound == true && GetWebDispatch().Active == Backend::WebGPU)
    {
        webgpu::DestroySurface(slot);
    }
}
void SelectSurface(usize slot) noexcept
{
    if (GetWebDispatch().IsBound == true && GetWebDispatch().Active == Backend::WebGPU)
    {
        webgpu::SelectSurface(slot);
    }
}
SurfaceCompletion GetSurfaceCompletion(usize slot) noexcept
{
    return GetWebDispatch().IsBound == true && GetWebDispatch().Active == Backend::WebGPU
               ? webgpu::GetSurfaceCompletion(slot)
               : SurfaceCompletion{};
}
// WebGPU-only private interop re-exported for the W5/W6 sample renderers. These
// are only meaningful while the WebGPU sub-backend is bound; the WebGL 2 sample
// path (Prompt 3) uses the public rendering API instead of these.
WGPUDevice ProbeDevice() noexcept
{
    return GetWebDispatch().Active == Backend::WebGPU ? webgpu::ProbeDevice() : nullptr;
}
WGPUTextureFormat ProbeFormat() noexcept
{
    return webgpu::ProbeFormat();
}
WGPURenderPassEncoder ProbePass() noexcept
{
    return GetWebDispatch().Active == Backend::WebGPU ? webgpu::ProbePass() : nullptr;
}
} // namespace ludus::graphics::rhi::backend

namespace ludus::graphics::rhi::backend
{
ComputeCapabilities ComputeLimits() noexcept
{
    return MakeWebGlCurrent() == true ? ComputeCapabilities{} : webgpu::ComputeLimits();
}
// WebGL 2 has no shader storage; its authored alternative remains the CPU/direct path.
RasterStorageCapabilities RasterStorageLimits() noexcept
{
    return MakeWebGlCurrent() == true ? RasterStorageCapabilities{} : webgpu::RasterStorageLimits();
}
// WebGL 2 has no indirect commands; applications keep their authored CPU/direct draws.
IndirectCapabilities IndirectLimits() noexcept
{
    return MakeWebGlCurrent() == true ? IndirectCapabilities{} : webgpu::IndirectLimits();
}
RasterStatus ComputeCreatePipeline(usize slot, const internal::ComputePipelineInfo& info, uint32 request) noexcept
{
    return MakeWebGlCurrent() == true ? RasterStatus::Unsupported : webgpu::ComputeCreatePipeline(slot, info, request);
}
RasterStatus ComputeEncode(const internal::ComputePacket& packet) noexcept
{
    return MakeWebGlCurrent() == true ? RasterStatus::Unsupported : webgpu::ComputeEncode(packet);
}
void ComputeBufferBarrier(usize slot, GraphAccessMode access) noexcept
{
    if (MakeWebGlCurrent() == false)
    {
        webgpu::ComputeBufferBarrier(slot, access);
    }
}
FormatCapabilities FormatLimits(RasterFormat format) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::FormatLimits(format) : webgpu::FormatLimits(format);
}
ludus::foundation::uint32 RasterColorAttachmentLimit() noexcept
{
    return MakeWebGlCurrent() == true ? webgl::RasterColorAttachmentLimit() : webgpu::RasterColorAttachmentLimit();
}
ludus::foundation::uint32 RasterVertexStrideLimit() noexcept
{
    return MakeWebGlCurrent() == true ? webgl::RasterVertexStrideLimit() : webgpu::RasterVertexStrideLimit();
}
bool GetDescriptorArenaUsage(internal::DescriptorArenaUsage&) noexcept
{
    return false;
}
BindingCapabilities BindingLimits() noexcept
{
    return MakeWebGlCurrent() == true ? webgl::BindingLimits() : webgpu::BindingLimits();
}
RasterCapabilities RasterLimits() noexcept
{
    return MakeWebGlCurrent() == true ? webgl::RasterLimits() : webgpu::RasterLimits();
}
RasterStatus RasterCreateBuffer(usize slot,
                                const BufferDescription& info,
                                ludus::foundation::BorrowedView<const uint8> bytes,
                                uint32 id) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::RasterCreateBuffer(slot, info, bytes, id)
                                      : webgpu::RasterCreateBuffer(slot, info, bytes, id);
}
RasterStatus
RasterCreateTexture(usize slot, const TextureDescription& info, const TextureUpload& upload, uint32 id) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::RasterCreateTexture(slot, info, upload, id)
                                      : webgpu::RasterCreateTexture(slot, info, upload, id);
}
RasterStatus RasterCreateView(usize slot, const internal::RasterViewSource& source, uint32 id) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::RasterCreateView(slot, source, id)
                                      : webgpu::RasterCreateView(slot, source, id);
}
RasterStatus RasterCreateSampler(usize slot, const SamplerDescription& info, uint32 id) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::RasterCreateSampler(slot, info, id)
                                      : webgpu::RasterCreateSampler(slot, info, id);
}
RasterStatus RasterCreateShader(usize slot,
                                const ShaderDescription& info,
                                const internal::RasterShaderInfo& metadata,
                                uint32 id) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::RasterCreateShader(slot, info, metadata, id)
                                      : webgpu::RasterCreateShader(slot, info, metadata, id);
}
RasterStatus RasterCreateLayout(usize slot, const internal::RasterLayout& info, uint32 id) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::RasterCreateLayout(slot, info, id)
                                      : webgpu::RasterCreateLayout(slot, info, id);
}
RasterStatus
RasterCreateSet(usize slot, const internal::RasterLayout& layout, const internal::RasterSet& info, uint32 id) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::RasterCreateSet(slot, layout, info, id)
                                      : webgpu::RasterCreateSet(slot, layout, info, id);
}
RasterStatus RasterCreatePipeline(usize slot, const internal::RasterPipelineInfo& info, uint32 id) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::RasterCreatePipeline(slot, info, id)
                                      : webgpu::RasterCreatePipeline(slot, info, id);
}
void RasterSetDebugName(internal::RasterKind kind, usize slot, const char* name) noexcept
{
    if (MakeWebGlCurrent() == true)
    {
        webgl::RasterSetDebugName(kind, slot, name);
    }
    else
    {
        webgpu::RasterSetDebugName(kind, slot, name);
    }
}
void RasterDestroy(internal::RasterKind kind, usize slot) noexcept
{
    if (MakeWebGlCurrent() == true)
    {
        webgl::RasterDestroy(kind, slot);
    }
    else
    {
        webgpu::RasterDestroy(kind, slot);
    }
}
RasterStatus RasterDraw(const internal::RasterPacket& packet) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::RasterDraw(packet) : webgpu::RasterDraw(packet);
}
RasterStatus RasterPreparePass(const internal::RasterPassInfo& info) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::RasterPreparePass(info) : webgpu::RasterPreparePass(info);
}
RasterStatus RasterBeginPass(const internal::RasterPassInfo& info) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::RasterBeginPass(info) : webgpu::RasterBeginPass(info);
}
void RasterEndPass() noexcept
{
    if (MakeWebGlCurrent() == true)
    {
        webgl::RasterEndPass();
    }
    else
    {
        webgpu::RasterEndPass();
    }
}
void RasterTextureBarrier(usize slot, RasterTextureUse use) noexcept
{
    if (MakeWebGlCurrent() == true)
    {
        webgl::RasterTextureBarrier(slot, use);
    }
    else
    {
        webgpu::RasterTextureBarrier(slot, use);
    }
}
RasterStatus RasterReserveSubmission() noexcept
{
    return MakeWebGlCurrent() == true ? webgl::RasterReserveSubmission() : webgpu::RasterReserveSubmission();
}
void RasterSubmit(uint64 ordinal) noexcept
{
    if (MakeWebGlCurrent() == true)
    {
        webgl::RasterSubmit(ordinal);
    }
    else
    {
        webgpu::RasterSubmit(ordinal);
    }
}
uint64 RasterCompleted() noexcept
{
    return MakeWebGlCurrent() == true ? webgl::RasterCompleted() : webgpu::RasterCompleted();
}
void RasterReset() noexcept
{
    if (MakeWebGlCurrent() == true)
    {
        webgl::RasterReset();
    }
    else
    {
        webgpu::RasterReset();
    }
}
RasterStatus RasterGrow(internal::RasterKind kind, usize count) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::RasterGrow(kind, count) : webgpu::RasterGrow(kind, count);
}
RasterStatus RasterPrepareStorage() noexcept
{
    return MakeWebGlCurrent() == true ? webgl::RasterPrepareStorage() : webgpu::RasterPrepareStorage();
}
void RasterShutdown() noexcept
{
    if (MakeWebGlCurrent() == true)
    {
        webgl::RasterShutdown();
    }
    else
    {
        webgpu::RasterShutdown();
    }
}
void RasterDiscardSubmission() noexcept
{
    if (MakeWebGlCurrent() == true)
    {
        webgl::RasterDiscardSubmission();
    }
    else
    {
        webgpu::RasterDiscardSubmission();
    }
}
bool LifetimeTransferAvailable(bool readback, foundation::usize slot) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::LifetimeTransferAvailable(readback, slot)
                                      : webgpu::LifetimeTransferAvailable(readback, slot);
}
RasterStatus LifetimeUpload(usize transfer,
                            const BufferDescription& info,
                            const internal::LifetimeUploadTarget& uploadTarget,
                            const uint8* bytes,
                            usize size) noexcept
{

    return MakeWebGlCurrent() == true ? webgl::LifetimeUpload(transfer, info, uploadTarget, bytes, size)
                                      : webgpu::LifetimeUpload(transfer, info, uploadTarget, bytes, size);
}
RasterStatus
LifetimeReadback(foundation::usize transfer, BufferRole role, const internal::LifetimeCopyRange& range) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::LifetimeReadback(transfer, role, range)
                                      : webgpu::LifetimeReadback(transfer, role, range);
}
RasterStatus LifetimeBufferUpdate(usize transfer,
                                  BufferRole role,
                                  const internal::LifetimeCopyRange& range,
                                  const uint8* bytes) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::LifetimeBufferUpdate(transfer, role, range, bytes)
                                      : webgpu::LifetimeBufferUpdate(transfer, role, range, bytes);
}
RasterStatus LifetimeTextureTransfer(usize transfer, const internal::TextureTransfer& info, const uint8* bytes) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::LifetimeTextureTransfer(transfer, info, bytes)
                                      : webgpu::LifetimeTextureTransfer(transfer, info, bytes);
}
RasterStatus LifetimePollTransfer(bool readback, foundation::usize slot) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::LifetimePollTransfer(readback, slot)
                                      : webgpu::LifetimePollTransfer(readback, slot);
}
RasterStatus LifetimeCopyReadback(foundation::usize slot, foundation::uint8* output, foundation::usize size) noexcept
{
    return MakeWebGlCurrent() == true ? webgl::LifetimeCopyReadback(slot, output, size)
                                      : webgpu::LifetimeCopyReadback(slot, output, size);
}
void LifetimeReleaseTransfer(bool readback, foundation::usize slot) noexcept
{
    if (MakeWebGlCurrent() == true)
    {
        webgl::LifetimeReleaseTransfer(readback, slot);
    }
    else
    {
        webgpu::LifetimeReleaseTransfer(readback, slot);
    }
}
void LifetimeReset() noexcept
{
    if (MakeWebGlCurrent() == true)
    {
        webgl::LifetimeReset();
    }
    else
    {
        webgpu::LifetimeReset();
    }
}
} // namespace ludus::graphics::rhi::backend
