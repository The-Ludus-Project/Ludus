#include <ludus/graphics/renderer/viewport.hpp>
#include <ludus/graphics/rhi/frame_producer.h>
#include <ludus/graphics/rhi/gpu_scope.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/views.hpp>

#include <new>

namespace ludus::graphics::renderer
{
using namespace ludus::foundation;
using Status = rhi::RasterStatus;
namespace
{
// All viewport creation shares the RHI's single owner-thread affinity.
uint64 NextViewportOwner = 1;
// The scene target's clear alpha: opaque by default, zero when the output pass composites a backdrop under it.
constexpr float32 OPAQUE_CLEAR_ALPHA = 1;
constexpr float32 BACKDROP_CLEAR_ALPHA = 0;
bool Accepted(Status status) noexcept
{
    return status == Status::Ready || status == Status::Pending;
}
} // namespace
struct RasterViewport::State final
{
    Renderer* SceneRenderer = nullptr;
    rhi::DeviceHandle Device{};
    rhi::TextureHandle SceneColor{};
    static constexpr usize DISPLAY_IMAGE_COUNT = rhi::FrameProducer::SLOT_COUNT;
    /// Display images named by Output's slots; Output owns their leases and producers.
    StaticArray<rhi::TextureHandle, DISPLAY_IMAGE_COUNT> Images{};
    rhi::FrameProducer Output;
    Presentation Display{};
    StaticArray<Presentation, DISPLAY_IMAGE_COUNT> SurfaceDisplays{};
    StaticArray<bool, DISPLAY_IMAGE_COUNT> HasSurfaceDisplay{};
    bool ShouldPresentToSurface = false;
    ViewMapping Mapping;
    /// Scene target format; only linear RGBA8 admits an overlay.
    rhi::RasterFormat SceneFormat = rhi::RasterFormat::Rgba8Unorm;
    /// Borrowed debug overlay composed into the scene; null without one.
    OverlayRenderer* OverlaysOrNull = nullptr;
    PreparedOverlay Overlay{};
    PreparedView View{};
    PreparedView PendingView{};
    bool HasPendingView = false;
    uint64 Owner = 0;
    uint32 Width = 0;
    uint32 Height = 0;
    bool DisplayCreated = false;
    float32 Exposure = 1;
    ToneCurve Curve = ToneCurve::Reinhard;
    bool HasBackdrop = false;
    SpatialAntialiasing Antialiasing = SpatialAntialiasing::Off;
    ~State() noexcept
    {
        static_cast<void>(SceneRenderer->Release(PendingView));
        static_cast<void>(SceneRenderer->Release(View));
        static_cast<void>(SceneRenderer->Release(Display));
        for (Presentation& display : SurfaceDisplays)
        {
            static_cast<void>(SceneRenderer->Release(display));
        }
        static_cast<void>(rhi::Destroy(Device, SceneColor));
        for (rhi::TextureHandle image : Images)
        {
            static_cast<void>(rhi::Destroy(Device, image));
        }
    }
};
RasterViewport::RasterViewport() noexcept = default;
RasterViewport::~RasterViewport() noexcept = default;
void RasterViewport::Reset() noexcept
{
    mState.Reset();
}
Status RasterViewport::Initialize(Renderer& renderer,
                                  rhi::DeviceHandle device,
                                  uint32 width,
                                  uint32 height,
                                  float32 exposure,
                                  ViewportColorMode colorMode,
                                  bool shouldPresentToSurface,
                                  ToneCurve toneCurve,
                                  bool hasBackdrop,
                                  SpatialAntialiasing antialiasing) noexcept
{
    if (mState)
    {
        return Status::InvalidState;
    }
    if (width == 0 || height == 0 || math::IsFinite(exposure) == false || exposure < .000001F || exposure > 1000 ||
        (colorMode != ViewportColorMode::PreferHdr && colorMode != ViewportColorMode::Linear8) ||
        (toneCurve != ToneCurve::Reinhard && toneCurve != ToneCurve::PbrNeutral) ||
        antialiasing > SpatialAntialiasing::Balanced)
    {
        return Status::InvalidDescription;
    }
    if (NextViewportOwner == ~uint64{0})
    {
        return Status::CapacityExceeded;
    }
    State* allocation = new (std::nothrow) State;
    core::UniquePtr<State> candidate{allocation};
    if (candidate.Get() == nullptr)
    {
        return Status::OutOfMemory;
    }
    candidate->SceneRenderer = &renderer;
    candidate->Device = device;
    candidate->Output = rhi::FrameProducer{device};
    candidate->Width = width;
    candidate->Height = height;
    candidate->Owner = NextViewportOwner++;
    rhi::TextureDescription texture;
    texture.Width = width;
    texture.Height = height;
    texture.Attachment = true;
    rhi::RasterCapabilities capabilities;
    const Status admitted = rhi::GetRasterCapabilities(device, capabilities);
    if (admitted != Status::Ready)
    {
        return admitted;
    }
    texture.Format = capabilities.HdrColor == true && colorMode == ViewportColorMode::PreferHdr
                         ? rhi::RasterFormat::Rgba16Float
                         : rhi::RasterFormat::Rgba8Unorm;
    candidate->SceneFormat = texture.Format;
    Status status = rhi::CreateTexture(device, texture, {}, candidate->SceneColor);
    if (Accepted(status) == false)
    {
        return status;
    }
    // Capture names only; naming failures never reject preparation.
    constexpr char SCENE_COLOR_NAME[] = "Viewport/SceneColor";
    constexpr char IMAGE_NAME[] = "Viewport/Image";
    static_cast<void>(
        rhi::SetDebugName(device, candidate->SceneColor, {SCENE_COLOR_NAME, sizeof(SCENE_COLOR_NAME) - 1}));
    texture.Format = rhi::RasterFormat::Rgba8Unorm;
    for (rhi::TextureHandle& image : candidate->Images)
    {
        status = rhi::CreateTexture(device, texture, {}, image);
        if (Accepted(status) == false)
        {
            return status;
        }
        static_cast<void>(rhi::SetDebugName(device, image, {IMAGE_NAME, sizeof(IMAGE_NAME) - 1}));
    }
    // Creation can be asynchronous. Poll prepares display only after the source is Ready.
    ViewMapping mapping;
    status = ResolveViewMapping({float32(width), float32(height), ScreenFit::Stretch},
                                {0, 0, width, height},
                                width,
                                height,
                                mapping);
    if (status != Status::Ready)
    {
        return status;
    }
    // Store exposure before asynchronous presentation preparation using an explicit field below.
    candidate->Mapping = mapping;
    candidate->ShouldPresentToSurface = shouldPresentToSurface;
    candidate->Exposure = exposure;
    candidate->Curve = toneCurve;
    candidate->HasBackdrop = hasBackdrop;
    candidate->Antialiasing = antialiasing;
    mState = Move(candidate);
    const Status setup = Poll();
    if (Accepted(setup) == false)
    {
        Reset();
    }
    return setup;
}
Status RasterViewport::Poll() noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    State& state = *mState;
    const rhi::TextureHandle textures[]{state.SceneColor, state.Images[0], state.Images[1]};
    for (rhi::TextureHandle texture : textures)
    {
        const Status status = rhi::GetStatus(state.Device, texture);
        if (status != Status::Ready)
        {
            return status;
        }
    }
    if (state.DisplayCreated == false)
    {
        const Status created = state.SceneRenderer->PrepareLightingPresentation(state.SceneColor,
                                                                                state.Mapping,
                                                                                {
                                                                                    .Exposure = state.Exposure,
                                                                                    .ToneMap = true,
                                                                                    .Curve = state.Curve,
                                                                                    .Encode = true,
                                                                                    .HasBackdrop = state.HasBackdrop,
                                                                                    .Antialiasing = state.Antialiasing,
                                                                                },
                                                                                state.Display);
        if (Accepted(created) == false)
        {
            return created;
        }
        state.DisplayCreated = true;
    }
    const Status display = state.SceneRenderer->GetStatus(state.Display);
    if (display != Status::Ready)
    {
        return display;
    }
    if (state.ShouldPresentToSurface == true)
    {
        for (usize index = 0; index < State::DISPLAY_IMAGE_COUNT; ++index)
        {
            // Preparation is outside frames; each presentation retains its display texture.
            if (state.HasSurfaceDisplay[index] == false)
            {
                const Status created = state.SceneRenderer->PreparePresentation(state.Images[index],
                                                                                state.Mapping,
                                                                                state.SurfaceDisplays[index]);
                if (Accepted(created) == false)
                {
                    return created;
                }
                state.HasSurfaceDisplay[index] = true;
            }
            const Status ready = state.SceneRenderer->GetStatus(state.SurfaceDisplays[index]);
            if (ready != Status::Ready)
            {
                return ready;
            }
        }
    }
    const Status producers = state.Output.PollProducers();
    if (producers != Status::Ready)
    {
        return producers;
    }
    if (state.HasPendingView == true)
    {
        const Status candidate = state.SceneRenderer->GetStatus(state.PendingView);
        if (candidate == Status::Ready)
        {
            static_cast<void>(state.SceneRenderer->Release(state.View));
            state.View = state.PendingView;
            state.PendingView = {};
            state.HasPendingView = false;
        }
        else if (candidate != Status::Pending)
        {
            static_cast<void>(state.SceneRenderer->Release(state.PendingView));
            state.HasPendingView = false;
            return candidate;
        }
    }
    return Status::Ready;
}
Status RasterViewport::Update(Snapshot snapshot, const ViewDescription& view, ViewReport& report) noexcept
{
    const Status setup = Poll();
    if (setup != Status::Ready)
    {
        return setup == Status::Pending ? Status::NotReady : setup;
    }
    PreparedView candidate;
    ViewReport candidateReport = report;
    const Status status = mState->SceneRenderer->PrepareView(snapshot, view, candidate, candidateReport);
    if (Accepted(status) == false)
    {
        return status;
    }
    static_cast<void>(mState->SceneRenderer->Release(mState->PendingView));
    mState->HasPendingView = status == Status::Pending;
    if (status == Status::Ready)
    {
        static_cast<void>(mState->SceneRenderer->Release(mState->View));
        mState->View = candidate;
    }
    else
    {
        mState->PendingView = candidate;
    }
    report = candidateReport;
    return status;
}
Status RasterViewport::Render(rhi::SurfaceHandle surface, rhi::SubmissionToken& completion) noexcept
{
    if (completion.IsNull() == false)
    {
        return Status::InvalidDescription;
    }
    if (mState.Get() != nullptr && mState->Output.IsFrameOpen() == true)
    {
        return mState->Output.EndFrame(completion);
    }
    const Status setup = Poll();
    if (setup != Status::Ready)
    {
        return setup == Status::Pending ? Status::NotReady : setup;
    }
    State& state = *mState;
    const Status ready = state.SceneRenderer->GetStatus(state.View);
    if (ready != Status::Ready)
    {
        return state.HasPendingView == true && ready == Status::InvalidHandle ? Status::NotReady : ready;
    }
    uint32 index = 0;
    const Status started = state.Output.BeginFrame(surface, index);
    if (started != Status::Ready)
    {
        return started;
    }
    rhi::GpuScopeDescription sceneScope;
    static_cast<void>(rhi::MakeGpuScope(1, {"Viewport/Scene", sizeof("Viewport/Scene") - 1}, sceneScope));
    {
        const rhi::GpuScope scope(state.Device, sceneScope);
        // An overlay depth-tests against this frame's scene depth, so keep it past the scene pass.
        const bool hasOverlay = state.OverlaysOrNull != nullptr;
        state.Output.Record(state.SceneRenderer->DrawView(
            state.View,
            {
                .Color = state.SceneColor,
                .Clear = {0, 0, 0, state.HasBackdrop == true ? BACKDROP_CLEAR_ALPHA : OPAQUE_CLEAR_ALPHA},
                .DepthStore = hasOverlay == true ? rhi::RasterStore::Store : rhi::RasterStore::Discard,
            }));
    }
    if (state.Output.GetRecordingStatus() == Status::Ready && state.OverlaysOrNull != nullptr)
    {
        constexpr uint64 OVERLAY_SCOPE_ID = 4;
        rhi::GpuScopeDescription overlayScope;
        static_cast<void>(
            rhi::MakeGpuScope(OVERLAY_SCOPE_ID, {"Viewport/Overlay", sizeof("Viewport/Overlay") - 1}, overlayScope));
        const rhi::GpuScope scope(state.Device, overlayScope);
        state.Output.Record(state.OverlaysOrNull->Draw(state.Overlay, { .Color = state.SceneColor }));
    }
    if (state.Output.GetRecordingStatus() == Status::Ready)
    {
        rhi::GpuScopeDescription compositeScope;
        static_cast<void>(
            rhi::MakeGpuScope(2, {"Viewport/Composite", sizeof("Viewport/Composite") - 1}, compositeScope));
        const rhi::GpuScope scope(state.Device, compositeScope);
        state.Output.Record(
            state.SceneRenderer->DrawLightingPresentation(state.Display, { .Color = state.Images[index] }));
    }
    if (state.Output.GetRecordingStatus() == Status::Ready && state.ShouldPresentToSurface == true)
    {
        constexpr uint64 PRESENT_SCOPE_ID = 3;
        rhi::GpuScopeDescription presentationScope;
        static_cast<void>(rhi::MakeGpuScope(PRESENT_SCOPE_ID,
                                            {"Viewport/Present", sizeof("Viewport/Present") - 1},
                                            presentationScope));
        const rhi::GpuScope scope(state.Device, presentationScope);
        state.Output.Record(state.SceneRenderer->DrawPresentation(state.SurfaceDisplays[index]));
    }
    return state.Output.EndFrame(completion);
}

Status RasterViewport::SetOverlay(OverlayRenderer* overlaysOrNull, PreparedOverlay overlay) noexcept
{
    if (mState.Get() == nullptr || mState->Output.IsFrameOpen() == true)
    {
        return Status::InvalidState;
    }
    State& state = *mState;
    if (overlaysOrNull == nullptr)
    {
        state.OverlaysOrNull = nullptr;
        state.Overlay = {};
        return Status::Ready;
    }
    if (state.SceneFormat != rhi::RasterFormat::Rgba8Unorm)
    {
        return Status::Unsupported;
    }
    const Status ready = overlaysOrNull->GetStatus(overlay);
    if (ready != Status::Ready)
    {
        return ready;
    }
    state.OverlaysOrNull = overlaysOrNull;
    state.Overlay = overlay;
    return Status::Ready;
}

Status RasterViewport::Acquire(ViewportFrame& inoutFrame, ViewportImage& outImage) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    if (inoutFrame.mOwner != 0)
    {
        return Status::InvalidDescription;
    }
    State& state = *mState;
    rhi::FrameLease lease;
    rhi::SubmissionToken producer;
    const Status acquired = state.Output.Acquire(lease, producer);
    if (acquired != Status::Ready)
    {
        return acquired;
    }
    inoutFrame.mOwner = state.Owner;
    inoutFrame.mGeneration = lease.Generation;
    inoutFrame.mSlot = lease.Slot;
    outImage = {state.Images[lease.Slot], producer, state.Width, state.Height};
    return Status::Ready;
}
Status RasterViewport::Release(ViewportFrame& inoutFrame) noexcept
{
    if (mState.Get() == nullptr || inoutFrame.mOwner != mState->Owner)
    {
        return Status::InvalidHandle;
    }
    const Status released = mState->Output.Release({ .Slot = inoutFrame.mSlot, .Generation = inoutFrame.mGeneration });
    if (released != Status::Ready)
    {
        return released;
    }
    inoutFrame = {};
    return Status::Ready;
}
} // namespace ludus::graphics::renderer
