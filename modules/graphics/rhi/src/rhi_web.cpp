#include "internal/backend.h"
#include "internal/lifecycle.h"
#include "internal/resources.h"
#include "internal/web_backends.h"
#include "internal/webgpu_probe.h"
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/graphics/rhi/render.h>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/platform/browser/window.h>

#include <span>

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

// The sub-backend currently bound for Kind()/resource/frame dispatch. Null until
// a backend commits (an attempt is in flight) and after shutdown.
Backend gActive = Backend::WebGPU;
bool gBound = false;

// Session context captured for a possible Auto fallback from WebGPU to WebGL 2.
BackendSelection gSelection = BackendSelection::Auto;
ApplicationInfo gApp;
WindowInfo gWindow;
bool gWebGlTried = false;

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
    gWebGlTried = true;
    gActive = Backend::WebGL2;
    internal::SelectBackend(token, Backend::WebGL2);
    // A WebGPU attempt may have committed the canvas to the 'webgpu' context
    // mode, which blocks 'webgl2' on the same node. Ask Platform (which owns the
    // DOM node and its listeners) to swap in a fresh canvas before the WebGL 2
    // attempt. The forced-WebGL2 path (afterWebGpu == false) skips this.
    if (afterWebGpu && gWindow.CanvasSelector != nullptr)
    {
        if (ludus::platform::browser::ReplaceCanvas(gWindow.CanvasSelector) == false)
        {
            internal::RecordAttempt(token, Backend::WebGL2, StartupError::SurfaceUnavailable);
            return StartupError::SurfaceUnavailable;
        }
    }
    gBound = true;
    const auto error = webgl::Start(gApp, gWindow, token, gSelection);
    internal::RecordAttempt(token, Backend::WebGL2, error);
    return error;
}

// Fail() consults this before finalizing a Pending session. For Auto, a WebGPU
// capability failure triggers exactly one WebGL 2 attempt on a fresh generation.
bool Fallback(uint32 token, StartupError error) noexcept
{
    if (gSelection != BackendSelection::Auto || gActive != Backend::WebGPU || gWebGlTried ||
        IsCapabilityFailure(error) == false)
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
    gBound = false;
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
    return gActive == Backend::WebGPU ? webgpu::AllocatorBlockBytes(bytes) : webgl::AllocatorBlockBytes(bytes);
}
MeasurementAdapter TimingAdapter() noexcept
{
    return gActive == Backend::WebGPU ? webgpu::TimingAdapter() : webgl::TimingAdapter();
}
RasterStatus TimingArm() noexcept
{
    return gActive == Backend::WebGPU ? webgpu::TimingArm() : webgl::TimingArm();
}
GpuTiming TimingPoll() noexcept
{
    return gActive == Backend::WebGPU ? webgpu::TimingPoll() : webgl::TimingPoll();
}
void TimingReset() noexcept
{
    if (gActive == Backend::WebGPU)
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
    return gActive == Backend::WebGPU ? webgpu::RasterAllocationBytes(kind, slot, bytes)
                                      : webgl::RasterAllocationBytes(kind, slot, bytes);
}
Backend Kind() noexcept
{
    return gActive;
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
    // A restart may select a different API, or follow a lost context. Platform
    // rebinds the canvas before either API acquires its one allowed context mode.
    (void)platform::browser::ReplaceCanvas(window.CanvasSelector);
    internal::SetFallback(Fallback);
    gSelection = selection;
    gApp = app;
    gWindow = window;
    gWebGlTried = false;
    gBound = false;

    if (selection == BackendSelection::WebGL2)
    {
        return StartWebGl(token, false);
    }
    // Auto and forced WebGPU both begin with WebGPU (including its compatibility
    // retry). Auto then falls back to WebGL 2 via Fallback(); forced WebGPU does
    // not (gSelection != Auto makes Fallback() decline).
    gActive = Backend::WebGPU;
    gBound = true;
    internal::SelectBackend(token, Backend::WebGPU);
    const auto error = webgpu::Start(gApp, gWindow, token, selection);
    if (error != StartupError::None)
    {
        // Synchronous WebGPU failure. For Auto, try WebGL 2 now; otherwise report.
        internal::RecordAttempt(token, Backend::WebGPU, error);
        if (selection == BackendSelection::Auto && IsCapabilityFailure(error) && gWebGlTried == false)
        {
            const auto next = internal::Reissue(token);
            webgpu::Shutdown();
            gBound = false;
            return next != 0 ? StartWebGl(next, true) : error;
        }
        webgpu::Shutdown();
        gBound = false;
        return error;
    }
    return StartupError::None;
}
void Shutdown() noexcept
{
    // Clear the fallback first so a teardown-induced loss cannot start an attempt.
    internal::SetFallback(nullptr);
    if (gBound)
    {
        if (gActive == Backend::WebGL2)
        {
            webgl::Shutdown();
        }
        else
        {
            webgpu::Shutdown();
        }
    }
    gBound = false;
    gActive = Backend::WebGPU;
    gSelection = BackendSelection::Auto;
    gWebGlTried = false;
    gApp = {};
    gWindow = {};
}
bool Initialize(const ApplicationInfo&) noexcept
{
    return false;
}
bool ConnectWindow(const WindowInfo&) noexcept
{
    return false;
}
bool InitializeRendering() noexcept
{
    return false;
}
void ShutdownRendering() noexcept {}
bool BeginFrame() noexcept
{
    return false;
}
bool EndFrame() noexcept
{
    return false;
}
FrameStatus SetTarget(const FrameTarget& target) noexcept
{
    return gActive == Backend::WebGL2 ? webgl::SetTarget(target) : webgpu::SetTarget(target);
}
FrameStatus Begin() noexcept
{
    return gActive == Backend::WebGL2 ? webgl::Begin() : webgpu::Begin();
}
FrameStatus End() noexcept
{
    return gActive == Backend::WebGL2 ? webgl::End() : webgpu::End();
}
FrameInfo GetFrameInfo() noexcept
{
    return gActive == Backend::WebGL2 ? webgl::GetFrameInfo() : webgpu::GetFrameInfo();
}
ResourceStatus CreateShader(usize slot, const ShaderDescription& description, uint32 id) noexcept
{
    return gActive == Backend::WebGL2 ? webgl::CreateShader(slot, description, id)
                                      : webgpu::CreateShader(slot, description, id);
}
ResourceStatus CreateUniform(usize slot, const UniformDescription& description, uint32 id) noexcept
{
    return gActive == Backend::WebGL2 ? webgl::CreateUniform(slot, description, id)
                                      : webgpu::CreateUniform(slot, description, id);
}
ResourceStatus CreatePipeline(usize slot, const PipelineResources& resources, uint32 id) noexcept
{
    return gActive == Backend::WebGL2 ? webgl::CreatePipeline(slot, resources, id)
                                      : webgpu::CreatePipeline(slot, resources, id);
}
void DestroyShader(usize slot) noexcept
{
    if (gActive == Backend::WebGL2)
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
    if (gActive == Backend::WebGL2)
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
    if (gActive == Backend::WebGL2)
    {
        webgl::DestroyPipeline(slot);
    }
    else
    {
        webgpu::DestroyPipeline(slot);
    }
}
void UpdateUniform(usize slot, std::span<const uint8> bytes) noexcept
{
    if (gActive == Backend::WebGL2)
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
    return gActive == Backend::WebGL2 ? webgl::Draw(slot) : webgpu::Draw(slot);
}
// WebGPU-only private interop re-exported for the W5/W6 sample renderers. These
// are only meaningful while the WebGPU sub-backend is bound; the WebGL 2 sample
// path (Prompt 3) uses the public rendering API instead of these.
WGPUDevice ProbeDevice() noexcept
{
    return gActive == Backend::WebGPU ? webgpu::ProbeDevice() : nullptr;
}
WGPUTextureFormat ProbeFormat() noexcept
{
    return webgpu::ProbeFormat();
}
WGPURenderPassEncoder ProbePass() noexcept
{
    return gActive == Backend::WebGPU ? webgpu::ProbePass() : nullptr;
}
} // namespace ludus::graphics::rhi::backend

namespace ludus::graphics::rhi::backend
{
ComputeCapabilities ComputeLimits() noexcept
{
    return gActive == Backend::WebGL2 ? ComputeCapabilities{} : webgpu::ComputeLimits();
}
RasterStatus ComputeCreatePipeline(usize slot, const internal::ComputePipelineInfo& info, uint32 request) noexcept
{
    return gActive == Backend::WebGL2 ? RasterStatus::Unsupported : webgpu::ComputeCreatePipeline(slot, info, request);
}
RasterStatus ComputeEncode(const internal::ComputePacket& packet) noexcept
{
    return gActive == Backend::WebGL2 ? RasterStatus::Unsupported : webgpu::ComputeEncode(packet);
}
void ComputeBufferBarrier(usize slot, GraphAccessMode access) noexcept
{
    if (gActive != Backend::WebGL2)
    {
        webgpu::ComputeBufferBarrier(slot, access);
    }
}
FormatCapabilities FormatLimits(RasterFormat format) noexcept
{
    return gActive == Backend::WebGL2 ? webgl::FormatLimits(format) : webgpu::FormatLimits(format);
}
ludus::foundation::uint32 RasterColorAttachmentLimit() noexcept
{
    return gActive == Backend::WebGL2 ? webgl::RasterColorAttachmentLimit() : webgpu::RasterColorAttachmentLimit();
}
ludus::foundation::uint32 RasterVertexStrideLimit() noexcept
{
    return gActive == Backend::WebGL2 ? webgl::RasterVertexStrideLimit() : webgpu::RasterVertexStrideLimit();
}
bool GetDescriptorArenaUsage(internal::DescriptorArenaUsage&) noexcept
{
    return false;
}
BindingCapabilities BindingLimits() noexcept
{
    return gActive == Backend::WebGL2 ? webgl::BindingLimits() : webgpu::BindingLimits();
}
RasterCapabilities RasterLimits() noexcept
{
    return gActive == Backend::WebGL2 ? webgl::RasterLimits() : webgpu::RasterLimits();
}
RasterStatus
RasterCreateBuffer(usize slot, const BufferDescription& info, std::span<const uint8> bytes, uint32 id) noexcept
{
    return gActive == Backend::WebGL2 ? webgl::RasterCreateBuffer(slot, info, bytes, id)
                                      : webgpu::RasterCreateBuffer(slot, info, bytes, id);
}
RasterStatus
RasterCreateTexture(usize slot, const TextureDescription& info, const TextureUpload& upload, uint32 id) noexcept
{
    return gActive == Backend::WebGL2 ? webgl::RasterCreateTexture(slot, info, upload, id)
                                      : webgpu::RasterCreateTexture(slot, info, upload, id);
}
RasterStatus RasterCreateView(usize slot, const internal::RasterViewSource& source, uint32 id) noexcept
{
    return gActive == Backend::WebGL2 ? webgl::RasterCreateView(slot, source, id)
                                      : webgpu::RasterCreateView(slot, source, id);
}
RasterStatus RasterCreateSampler(usize slot, const SamplerDescription& info, uint32 id) noexcept
{
    return gActive == Backend::WebGL2 ? webgl::RasterCreateSampler(slot, info, id)
                                      : webgpu::RasterCreateSampler(slot, info, id);
}
RasterStatus RasterCreateShader(usize slot,
                                const ShaderDescription& info,
                                const internal::RasterShaderInfo& metadata,
                                uint32 id) noexcept
{
    return gActive == Backend::WebGL2 ? webgl::RasterCreateShader(slot, info, metadata, id)
                                      : webgpu::RasterCreateShader(slot, info, metadata, id);
}
RasterStatus RasterCreateLayout(usize slot, const internal::RasterLayout& info, uint32 id) noexcept
{
    return gActive == Backend::WebGL2 ? webgl::RasterCreateLayout(slot, info, id)
                                      : webgpu::RasterCreateLayout(slot, info, id);
}
RasterStatus
RasterCreateSet(usize slot, const internal::RasterLayout& layout, const internal::RasterSet& info, uint32 id) noexcept
{
    return gActive == Backend::WebGL2 ? webgl::RasterCreateSet(slot, layout, info, id)
                                      : webgpu::RasterCreateSet(slot, layout, info, id);
}
RasterStatus RasterCreatePipeline(usize slot, const internal::RasterPipelineInfo& info, uint32 id) noexcept
{
    return gActive == Backend::WebGL2 ? webgl::RasterCreatePipeline(slot, info, id)
                                      : webgpu::RasterCreatePipeline(slot, info, id);
}
void RasterDestroy(internal::RasterKind kind, usize slot) noexcept
{
    if (gActive == Backend::WebGL2)
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
    return gActive == Backend::WebGL2 ? webgl::RasterDraw(packet) : webgpu::RasterDraw(packet);
}
RasterStatus RasterPreparePass(const internal::RasterPassInfo& info) noexcept
{
    return gActive == Backend::WebGL2 ? webgl::RasterPreparePass(info) : webgpu::RasterPreparePass(info);
}
RasterStatus RasterBeginPass(const internal::RasterPassInfo& info) noexcept
{
    return gActive == Backend::WebGL2 ? webgl::RasterBeginPass(info) : webgpu::RasterBeginPass(info);
}
void RasterEndPass() noexcept
{
    if (gActive == Backend::WebGL2)
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
    if (gActive == Backend::WebGL2)
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
    return gActive == Backend::WebGL2 ? webgl::RasterReserveSubmission() : webgpu::RasterReserveSubmission();
}
void RasterSubmit(uint64 ordinal) noexcept
{
    if (gActive == Backend::WebGL2)
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
    return gActive == Backend::WebGL2 ? webgl::RasterCompleted() : webgpu::RasterCompleted();
}
void RasterReset() noexcept
{
    if (gActive == Backend::WebGL2)
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
    return gActive == Backend::WebGL2 ? webgl::RasterGrow(kind, count) : webgpu::RasterGrow(kind, count);
}
RasterStatus RasterPrepareStorage() noexcept
{
    return gActive == Backend::WebGL2 ? webgl::RasterPrepareStorage() : webgpu::RasterPrepareStorage();
}
void RasterShutdown() noexcept
{
    if (gActive == Backend::WebGL2)
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
    if (gActive == Backend::WebGL2)
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
    return gActive == Backend::WebGL2 ? webgl::LifetimeTransferAvailable(readback, slot)
                                      : webgpu::LifetimeTransferAvailable(readback, slot);
}
RasterStatus LifetimeUpload(usize transfer,
                            const BufferDescription& info,
                            const internal::LifetimeUploadTarget& uploadTarget,
                            const uint8* bytes,
                            usize size) noexcept
{

    return gActive == Backend::WebGL2 ? webgl::LifetimeUpload(transfer, info, uploadTarget, bytes, size)
                                      : webgpu::LifetimeUpload(transfer, info, uploadTarget, bytes, size);
}
RasterStatus
LifetimeReadback(foundation::usize transfer, BufferRole role, const internal::LifetimeCopyRange& range) noexcept
{
    return gActive == Backend::WebGL2 ? webgl::LifetimeReadback(transfer, role, range)
                                      : webgpu::LifetimeReadback(transfer, role, range);
}
RasterStatus LifetimeBufferUpdate(usize transfer,
                                  BufferRole role,
                                  const internal::LifetimeCopyRange& range,
                                  const uint8* bytes) noexcept
{
    return gActive == Backend::WebGL2 ? webgl::LifetimeBufferUpdate(transfer, role, range, bytes)
                                      : webgpu::LifetimeBufferUpdate(transfer, role, range, bytes);
}
RasterStatus LifetimeTextureTransfer(usize transfer, const internal::TextureTransfer& info, const uint8* bytes) noexcept
{
    return gActive == Backend::WebGL2 ? webgl::LifetimeTextureTransfer(transfer, info, bytes)
                                      : webgpu::LifetimeTextureTransfer(transfer, info, bytes);
}
RasterStatus LifetimePollTransfer(bool readback, foundation::usize slot) noexcept
{
    return gActive == Backend::WebGL2 ? webgl::LifetimePollTransfer(readback, slot)
                                      : webgpu::LifetimePollTransfer(readback, slot);
}
RasterStatus LifetimeCopyReadback(foundation::usize slot, foundation::uint8* output, foundation::usize size) noexcept
{
    return gActive == Backend::WebGL2 ? webgl::LifetimeCopyReadback(slot, output, size)
                                      : webgpu::LifetimeCopyReadback(slot, output, size);
}
void LifetimeReleaseTransfer(bool readback, foundation::usize slot) noexcept
{
    if (gActive == Backend::WebGL2)
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
    if (gActive == Backend::WebGL2)
    {
        webgl::LifetimeReset();
    }
    else
    {
        webgpu::LifetimeReset();
    }
}
} // namespace ludus::graphics::rhi::backend
