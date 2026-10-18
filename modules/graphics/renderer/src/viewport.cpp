#include <ludus/graphics/renderer/viewport.hpp>

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
    rhi::TextureHandle Images[2]{};
    rhi::SubmissionToken Producers[2]{};
    Presentation Display{};
    static constexpr usize DISPLAY_IMAGE_COUNT = 2;
    StaticArray<Presentation, DISPLAY_IMAGE_COUNT> SurfaceDisplays{};
    StaticArray<bool, DISPLAY_IMAGE_COUNT> HasSurfaceDisplay{};
    bool ShouldPresentToSurface = false;
    ViewMapping Mapping;
    PreparedView View{};
    PreparedView PendingView{};
    bool HasPendingView = false;
    uint64 Owner = 0;
    uint64 Generations[2]{1, 1};
    uint32 Width = 0;
    uint32 Height = 0;
    uint32 Latest = 0;
    bool Leased[2]{};
    bool Written[2]{};
    bool Submitted[2]{};
    bool DisplayCreated = false;
    bool FrameOpen = false;
    uint32 Producing = 0;
    rhi::SurfaceHandle ProducingSurface{};
    Status RecordingStatus = Status::Ready;
    Status Finish(rhi::SubmissionToken& completion) noexcept
    {
        const Status submitted = rhi::EndFrame(Device, completion);
        const Status producer = rhi::GetStatus(Device, completion);
        if (producer != Status::InvalidHandle)
        {
            Producers[Producing] = completion;
            Submitted[Producing] = true;
        }
        rhi::FrameInfo frame;
        FrameOpen = rhi::GetFrameInfo(Device, ProducingSurface, frame) == rhi::DeviceStatus::Ready;
        if (FrameOpen)
        {
            return submitted;
        }
        if (RecordingStatus != Status::Ready)
        {
            return RecordingStatus;
        }
        if (submitted == Status::Ready)
        {
            Latest = Producing;
            Written[Producing] = true;
        }
        return submitted;
    }
    float32 Exposure = 1;
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
                                  bool shouldPresentToSurface) noexcept
{
    if (mState)
    {
        return Status::InvalidState;
    }
    if (width == 0 || height == 0 || math::IsFinite(exposure) == false || exposure < .000001F || exposure > 1000 ||
        (colorMode != ViewportColorMode::PreferHdr && colorMode != ViewportColorMode::Linear8))
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
    Status status = rhi::CreateTexture(device, texture, {}, candidate->SceneColor);
    if (Accepted(status) == false)
    {
        return status;
    }
    texture.Format = rhi::RasterFormat::Rgba8Unorm;
    for (rhi::TextureHandle& image : candidate->Images)
    {
        status = rhi::CreateTexture(device, texture, {}, image);
        if (Accepted(status) == false)
        {
            return status;
        }
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
        const Status created = state.SceneRenderer->PrepareLightingPresentation(
            state.SceneColor,
            state.Mapping,
            { .Exposure = state.Exposure, .ToneMap = true, .Encode = true },
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
    for (uint32 slot = 0; slot < 2; ++slot)
    {
        if (state.Submitted[slot] == false)
        {
            continue;
        }
        const Status producer = rhi::GetStatus(state.Device, state.Producers[slot]);
        if (producer != Status::Ready)
        {
            return producer;
        }
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
    if (mState && mState->FrameOpen)
    {
        return mState->Finish(completion);
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
    uint32 index = (state.Latest + 1) % 2;
    if (state.Leased[index] || state.Generations[index] == ~uint64{0})
    {
        index = 1 - index;
    }
    if (state.Leased[index])
    {
        return Status::NotReady;
    }
    if (state.Generations[index] == ~uint64{0})
    {
        return Status::IdentityExhausted;
    }
    const rhi::DeviceStatus started = rhi::BeginFrame(state.Device, surface);
    switch (started)
    {
        case rhi::DeviceStatus::Ready:
            break;
        case rhi::DeviceStatus::Pending:
            return Status::Pending;
        case rhi::DeviceStatus::Skipped:
        case rhi::DeviceStatus::NotReady:
            return Status::NotReady;
        case rhi::DeviceStatus::InvalidHandle:
            return Status::InvalidHandle;
        case rhi::DeviceStatus::InvalidDescription:
            return Status::InvalidDescription;
        case rhi::DeviceStatus::Unsupported:
            return Status::Unsupported;
        case rhi::DeviceStatus::DeviceLost:
            return Status::DeviceLost;
        case rhi::DeviceStatus::Failed:
            return Status::Failed;
        case rhi::DeviceStatus::InvalidState:
            return Status::InvalidState;
        case rhi::DeviceStatus::IdentityExhausted:
            return Status::IdentityExhausted;
    }
    state.Written[index] = false;
    state.FrameOpen = true;
    state.Producing = index;
    state.ProducingSurface = surface;
    state.RecordingStatus = state.SceneRenderer->DrawView(state.View, { .Color = state.SceneColor });
    if (state.RecordingStatus == Status::Ready)
    {
        state.RecordingStatus =
            state.SceneRenderer->DrawLightingPresentation(state.Display, { .Color = state.Images[index] });
    }
    if (state.RecordingStatus == Status::Ready && state.ShouldPresentToSurface == true)
    {
        state.RecordingStatus = state.SceneRenderer->DrawPresentation(state.SurfaceDisplays[index]);
    }
    return state.Finish(completion);
}

Status RasterViewport::Acquire(ViewportFrame& frame, ViewportImage& image) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    if (frame.Owner != 0)
    {
        return Status::InvalidDescription;
    }
    State& state = *mState;
    const uint32 index = state.Latest;
    if (state.Written[index] == false || state.Leased[index])
    {
        return Status::NotReady;
    }
    const Status completed = rhi::GetStatus(state.Device, state.Producers[index]);
    if (completed != Status::Ready)
    {
        return completed;
    }
    if (state.Generations[index] == ~uint64{0})
    {
        return Status::IdentityExhausted;
    }
    state.Leased[index] = true;
    frame.Owner = state.Owner;
    frame.Generation = state.Generations[index];
    frame.Slot = index;
    image = {state.Images[index], state.Producers[index], state.Width, state.Height};
    return Status::Ready;
}
Status RasterViewport::Release(ViewportFrame& frame) noexcept
{
    if (mState.Get() == nullptr || frame.Owner != mState->Owner || frame.Slot >= 2 ||
        mState->Leased[frame.Slot] == false || frame.Generation != mState->Generations[frame.Slot])
    {
        return Status::InvalidHandle;
    }
    mState->Leased[frame.Slot] = false;
    ++mState->Generations[frame.Slot];
    frame = {};
    return Status::Ready;
}
} // namespace ludus::graphics::renderer
