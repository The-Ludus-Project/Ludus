#include "internal/render_preview.h"
#include "internal/preview_gpu_generation.h"
#include "internal/preview_lighting.h"
#include "internal/preview_shading.h"
#include "internal/render_scene_limits.h"
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/time/time.hpp>

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/memory/shared_ptr.hpp>

#include <ludus/content/scene_format/luscn.h>

#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/measurement.h>
#include <ludus/graphics/rhi/raster.h>
#include <ludus/graphics/rhi/rhi.h>

#include <new>

#include <QElapsedTimer>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace rhi = graphics::rhi;
namespace rr = graphics::renderer;
constexpr int32 NAVIGATION_STATE_VERSION = 2;
/// Minimum interval between statistics overlay refreshes, so the text stays readable.
constexpr int64 STATISTICS_REFRESH_MILLISECONDS = 250;
/// Nanoseconds per millisecond, for GPU timestamp durations.
constexpr float64 NANOSECONDS_PER_MILLISECOND = 1000000.0;
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
bool Accepted(rhi::RasterStatus s) noexcept
{
    return s == rhi::RasterStatus::Ready || s == rhi::RasterStatus::Pending;
}
// Shared by the Qt-shared and offscreen devices: authored lighting needs the
// same budgets whichever way the viewport presents.
rhi::DeviceDescription MakeViewportDeviceDescription() noexcept
{
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Preferred.RayQueries = true;
    // Two retained GPU generations, 80 caster views, and direct-lighting material versions.
    // These are explicit admission budgets, not allocations or inferred native limits.
    description.Storage.ResourcesPerKind = 4096;
    description.Storage.BindingDescriptors = 32768;
    description.Storage.DrawsPerFrame = 32768;
    description.Storage.DrawsPerPlan = 1024;
    description.Storage.ResourcesPerGraph = rr::VISIBILITY_GRAPH_RESOURCE_CAPACITY;
    description.Storage.HostBytes = usize{256} * 1024 * 1024;
    description.Storage.ResourceBytes = uint64{2} * 1024 * 1024 * 1024;
    description.RequiredProfile.Raster.MinBindings = 11;
    description.RequiredProfile.Raster.MinUniformRange = 16384;
    description.RequiredProfile.Raster.MinVertexAttributes = 16;
    description.RequiredProfile.Raster.MinTextureDimension2D = preview_lighting::SHADOW_ATLAS_PIXELS;
    // Optional frame timestamps for the viewport statistics overlay; backends without
    // timestamp queries report Unavailable and the overlay shows "GPU: n/a".
    description.EnableGpuTiming = true;
    return description;
}
#endif
} // namespace
struct RenderPreview::Residency final
{
    foundation::SharedPtr<const resources::SceneDefinition> Definition;
    resources::SceneDefinitionAdapter Adapter;
    resources::Manager Manager;
    resources::Lease Lease;
    ~Residency() noexcept
    {
        (void)Lease.Release();
        auto status = Manager.Shutdown();
        // CPU-only retirement is immediate. Cover the complete coordinator cursor,
        // including scope closure, before destroying the borrowed provider context.
        for (uint32 i = 0; i < 4 && status == resources::Status::Pending; ++i)
        {
            Manager.Service(64);
            status = Manager.Shutdown();
        }
        LUDUS_REQUIRE(status == resources::Status::Ok);
    }
    bool Initialize(const RenderScene& scene) noexcept
    {
        Definition = scene.Definition;
        content::Resource resource;
        const auto id = scene.LogicalId.toUtf8();
        const auto path = QByteArray("revisions/") + scene.Identity.toHex() + QByteArray(".luscn");
        if (resource.Id.Set({id.constData(), static_cast<usize>(id.size())}) == false ||
            resource.Path.Set({path.constData(), static_cast<usize>(path.size())}) == false)
        {
            return false;
        }
        resource.Type = content::Kind::CookedScene;
        content::Catalog catalog;
        if (Adapter.Initialize(resource, *Definition) != resources::Status::Ok ||
            catalog.Put(resource) != content::Status::Ok)
        {
            return false;
        }
        const auto adapter = Adapter.Descriptor();
        resources::AssetHandle asset;
        resources::ScopeHandle scope;
        content::ResourceId name;
        resources::RequestHandle request;
        (void)name.Set("editor-level");
        if (Manager.Initialize(catalog, &adapter, 1) != resources::Status::Ok ||
            Manager.Resolve(resource.Id, resource.Type, asset) != resources::Status::Ok ||
            Manager.OpenScope(name, scope) != resources::Status::Ok ||
            Manager.BeginAcquire(scope, asset, request) != resources::Status::Ok)
        {
            return false;
        }
        Manager.Service();
        Manager.Service();
        Manager.Service();
        const void* payload = nullptr;
        return Manager.TakeLease(request, Lease) == resources::Status::Ok &&
               Manager.Borrow(Lease.Token(), content::Kind::CookedScene, payload) == resources::Status::Ok &&
               payload == Definition.Get();
    }
};
RenderPreview::RenderPreview(QObject* parent) : ViewportFrameSource(parent)
{
    connect(&mFramePump, &ViewportFramePump::WorkRequested, this, &RenderPreview::Tick);
}
RenderPreview::~RenderPreview()
{
    Stop();
}
platform::NativeWindowInfo RenderPreview::WindowInfo() const noexcept
{
    return { .Width = RequestedWidth_, .Height = RequestedHeight_ };
}
QSize RenderPreview::GetDrawableSize() const noexcept
{
    return mDrawableSize;
}
bool RenderPreview::SetScene(const RenderScene& scene)
{
    bool isValid =
        scene.Identity.isEmpty()
            ? (scene.Meshes.isEmpty() && scene.Textures.isEmpty() && scene.Surfaces.isEmpty() &&
               scene.Objects.isEmpty() && scene.Cameras.isEmpty())
            : (scene.Meshes.isEmpty() == false && static_cast<usize>(scene.Meshes.size()) <= MAX_PREVIEW_MESHES &&
               static_cast<usize>(scene.Textures.size()) <= MAX_PREVIEW_TEXTURES &&
               static_cast<usize>(scene.Surfaces.size()) <= MAX_PREVIEW_SURFACES &&
               static_cast<usize>(scene.Objects.size()) <= MAX_PREVIEW_OBJECTS &&
               static_cast<usize>(scene.Cameras.size()) <= MAX_PREVIEW_CAMERAS);
    for (const auto& surface : scene.Surfaces)
    {
        isValid = isValid && surface.Texture >= (scene.Lit ? -1 : 0) && surface.Texture < scene.Textures.size();
    }
    for (const auto& object : scene.Objects)
    {
        isValid = isValid && object.Mesh >= 0 && object.Mesh < scene.Meshes.size() && object.Texture >= -1 &&
                  object.Texture < scene.Textures.size() &&
                  (scene.Lit || object.Texture >= 0 ? object.Surface >= 0 && object.Surface < scene.Surfaces.size()
                                                    : object.Surface == -1);
    }
    if (isValid == false)
    {
        Status_ = QStringLiteral(
            "Preview snapshot exceeds its validated asset/object/material bounds. Previous data retained.");
        Q_EMIT Changed();
        return false;
    }
    if (scene.Identity.isEmpty())
    {
        Stop();
        Scene_ = {};
        Residency_.Reset();
        OrbitCenter_ = {};
        OrbitRadius_ = 1;
        mCameraController = EditorCameraController{};
        RestoredOrbit_ = false;
        Camera_ = -1;
        CameraId_.clear();
        mSelectedId.clear();
        Status_.clear();
        return true;
    }
    if (Scene_.Identity == scene.Identity)
    {
        return true;
    }
    const bool sameResources = preview_lighting::MakeKey(Scene_) == preview_lighting::MakeKey(scene);
    if (Scene_.Identity.isEmpty() && RestoredOrbit_ == false)
    {
        OrbitCenter_ = scene.Center;
        OrbitRadius_ = scene.Radius;
        math::Aabb3 boundsWorld;
        if (scene.TryGetBounds({}, boundsWorld) == true)
        {
            static_cast<void>(mCameraController.TryFrame(boundsWorld,
                                                         static_cast<float32>(RequestedWidth_) /
                                                             static_cast<float32>(RequestedHeight_)));
        }
    }
    foundation::UniquePtr<Residency> residency;
    if (scene.Lit)
    {
        residency.Reset(new (std::nothrow) Residency);
        if (residency.Get() == nullptr || scene.Definition == nullptr || residency->Initialize(scene) == false)
        {
            Status_ = QStringLiteral("Resource-manager admission failed; previous scene retained.");
            Q_EMIT Changed();
            return false;
        }
    }
    // Retire the previous lease and provider through the local owner.
    Residency_.Swap(residency);
    Scene_ = scene;
    if (CameraId_.isEmpty() == false)
    {
        Camera_ = -1;
        for (int32 i = 0; i < Scene_.Cameras.size(); ++i)
        {
            if (Scene_.Cameras[i].Id == CameraId_)
            {
                Camera_ = i;
            }
        }
    }
    Status_ = QStringLiteral("World preview snapshot updated; preparing the renderer.");
    if (Camera_ >= Scene_.Cameras.size())
    {
        Camera_ = -1;
    }
    if (mHasPendingFrame == true)
    {
        static_cast<void>(mResources->Viewport.Release(mPendingFrame));
        mHasPendingFrame = false;
    }
    Frames_ = 0;
    if (OwnsDevice_)
    {
        if (sameResources == true && mResources.Get() != nullptr && mResources->IsPrepared() == true &&
            mHasPreparationFailed == false)
        {
            ViewDirty_ = true;
        }
        else
        {
            restartResources();
        }
    }
    Q_EMIT Changed();
    return true;
}
void RenderPreview::restartResources() noexcept
{
    mNeedsResourceRestart = true;
    mHasPreparationFailed = false;
    mFramePump.Start();
}
bool RenderPreview::beginResourceRestart() noexcept
{
    // Bound staging to two generations. If Qt still samples both, keep the
    // latest requested scene/mode queued until the oldest consumer retires.
    if (mRetiredResources.Get() != nullptr && mResources.Get() != nullptr && mResources->ExternalLeases != 0)
    {
        return false;
    }
    foundation::UniquePtr<PreviewGpuGeneration> candidate;
    candidate.Reset(new (std::nothrow) PreviewGpuGeneration(Device_, Scene_, mShading, mShaders));
    if (candidate.Get() == nullptr)
    {
        fail(QStringLiteral("GPU preview staging allocation failed. Previous view retained."));
        return false;
    }
    if (mHasPendingFrame == true)
    {
        static_cast<void>(mResources->Viewport.Release(mPendingFrame));
    }
    mPendingFrame = {};
    mPendingImage = {};
    mHasPendingFrame = false;
    if (mResources.Get() != nullptr && mResources->ExternalLeases != 0)
    {
        mRetiredResources.Swap(mResources);
    }
    mResources.Swap(candidate);
    mHasViewport = false;
    mHasPreparedView = false;
    mProducer = {};
    mIsAwaitingProducer = false;
    mFramePump.SetProducerPending(false);
    mNeedsResourceRestart = false;
    mPreparationFailure.clear();
    Report_ = {};
    Frames_ = 0;
    ViewDirty_ = true;
    return true;
}
void RenderPreview::SetShaders(const PreviewShaderSet& shaders)
{
    mShaders = shaders;
    if (OwnsDevice_ == true)
    {
        restartResources();
    }
}
bool RenderPreview::rejectShaders()
{
    if (mResources.Get() == nullptr || mResources->Shaders.GetRevision() == mPresentedShaders.GetRevision())
    {
        return false;
    }
    const uint64 rejected = mResources->Shaders.GetRevision();
    const QString reason = mPreparationFailure.isEmpty() == false ? mPreparationFailure
                                                                  : QStringLiteral("GPU pipeline preparation failed.");
    mShaders = mPresentedShaders;
    restartResources();
    Q_EMIT ShadersRejected(rejected, reason);
    return true;
}
bool RenderPreview::Start()
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    if (mIsProjectGraphEnabled == true && mProjectGraphProblem.isEmpty() == false)
    {
        Status_ = mProjectGraphProblem;
        Q_EMIT Changed();
        return false;
    }
    if (Scene_.Identity.isEmpty())
    {
        Status_ = QStringLiteral("Load and validate a World preview first.");
        Q_EMIT Changed();
        return false;
    }
    if (Running())
    {
        mFramePump.Start();
        return true;
    }
    if (rhi::GetStartup().State != rhi::StartupState::Idle)
    {
        Status_ = QStringLiteral("Another authoring surface owns rendering. Close it before opening the 3D preview.");
        Q_EMIT Changed();
        return false;
    }
    mIsStarting = true;
    Status_ = mIsProjectGraphEnabled == true ? QStringLiteral("Preparing the project renderer…")
                                             : QStringLiteral("Preparing the shared Qt GPU device…");
    Q_EMIT DeviceRequested();
    Q_EMIT Changed();
    return mIsStarting == true || OwnsDevice_ == true;
#else
    Status_ = QStringLiteral("3D preview requires the native editor built with LUDUS_EDITOR_RENDER_PREVIEW and pinned "
                             "shader tools. World data remains editable.");
    Q_EMIT Changed();
    return false;
#endif
}
bool RenderPreview::AdmitDevice(const rhi::NativeDeviceDescription& native)
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mIsStarting == false || OwnsDevice_ == true)
    {
        return false;
    }
    const rhi::DeviceDescription description = MakeViewportDeviceDescription();
    const rhi::DeviceStatus started = rhi::CreateNativeDevice({ .Name = "Ludus shared editor viewport", .Version = 1 },
                                                              native,
                                                              description,
                                                              8,
                                                              8,
                                                              Device_,
                                                              Surface_);
    if (started != rhi::DeviceStatus::Ready)
    {
        Status_ =
            QStringLiteral("The Qt GPU context does not support native viewport sharing. Editing remains available.");
        Q_EMIT Changed();
        Q_EMIT Failed(Status_);
        return false;
    }
    mIsStarting = false;
    OwnsDevice_ = true;
    ++mDeviceSerial;
    resetStatistics();
    Width_ = Height_ = 0;
    Frames_ = 0;
    restartResources();
    mFramePump.Start();
    Q_EMIT Changed();
    return true;
#else
    static_cast<void>(native);
    return false;
#endif
}
bool RenderPreview::AdmitOffscreenDevice()
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mIsStarting == false || OwnsDevice_ == true)
    {
        return false;
    }
    const rhi::DeviceDescription description = MakeViewportDeviceDescription();
    // Headless target: the viewport reads the display image back, so no window surface is needed.
    const platform::NativeWindowInfo headless{ .Width = 1, .Height = 1 };
    const rhi::DeviceStatus started = rhi::CreateDevice({ .Name = "Ludus offscreen editor viewport", .Version = 1 },
                                                        headless,
                                                        description,
                                                        Device_,
                                                        Surface_);
    if (started != rhi::DeviceStatus::Ready && started != rhi::DeviceStatus::Pending)
    {
        Status_ = QStringLiteral("No GPU device meets the offscreen viewport's raster profile and budgets. "
                                 "Editing remains available.");
        Q_EMIT Changed();
        Q_EMIT Failed(Status_);
        return false;
    }
    mIsStarting = false;
    OwnsDevice_ = true;
    ++mDeviceSerial;
    resetStatistics();
    Width_ = Height_ = 0;
    Frames_ = 0;
    restartResources();
    mFramePump.Start();
    return true;
#else
    return false;
#endif
}
bool RenderPreview::AdmitBrowserDevice(const char* canvasSelector)
{
#if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
    if (mIsStarting == false || OwnsDevice_ == true || canvasSelector == nullptr)
    {
        return false;
    }
    rhi::DeviceDescription description = MakeViewportDeviceDescription();
    // Keep Qt's widget canvas independent, without changing the current GL context.
    description.Selection = rhi::BackendSelection::WebGPU;
    const platform::NativeWindowInfo window
    {
        .System = platform::WindowSystem::WebCanvas,
        .Width = RequestedWidth_,
        .Height = RequestedHeight_,
        .CanvasSelector = canvasSelector,
    };
    const rhi::DeviceStatus started = rhi::CreateDevice({ .Name = "Ludus browser editor viewport", .Version = 1 },
                                                        window,
                                                        description,
                                                        Device_,
                                                        Surface_);
    if (started != rhi::DeviceStatus::Ready && started != rhi::DeviceStatus::Pending)
    {
        mIsStarting = false;
        Status_ = QStringLiteral(
            "WebGPU viewport unavailable. Use a browser with WebGPU enabled. World edits remain available.");
        Q_EMIT Changed();
        Q_EMIT Failed(Status_);
        return false;
    }
    mIsStarting = false;
    OwnsDevice_ = true;
    ++mDeviceSerial;
    resetStatistics();
    Width_ = 0;
    Height_ = 0;
    Frames_ = 0;
    restartResources();
    mFramePump.Start();
    return true;
#else
    static_cast<void>(canvasSelector);
    return false;
#endif
}
void RenderPreview::Stop() noexcept
{
    const bool wasRunning = Running();
    mIsStarting = false;
    mFramePump.Stop();
    if (OwnsDevice_)
    {
        // Borrowers such as the render-graph preview release their resources first.
        Q_EMIT DeviceStopping();
        mHasTimingAssociation = false;
        mIsTimingArmed = false;
        mHasPassTimingAssociation = false;
        mIsPassTimingArmed = false;
        resetFrames();
        mResources.Reset();
        mRetiredResources.Reset();
        (void)rhi::DestroyDevice(Device_);
    }
    OwnsDevice_ = false;
    Device_ = {};
    Surface_ = {};
    Width_ = Height_ = 0;
    Frames_ = 0;
    if (wasRunning)
    {
        Q_EMIT Stopped();
    }
}
void RenderPreview::SetForceRaster(bool shouldForceRaster)
{
    if (mShouldForceRaster == shouldForceRaster)
    {
        return;
    }
    mShouldForceRaster = shouldForceRaster;
    Q_EMIT RendererPreferenceChanged(shouldForceRaster);
    Q_EMIT NavigationChanged();
}
void RenderPreview::SetStatisticsVisible(bool isVisible)
{
    if (isVisible == mIsStatisticsVisible)
    {
        return;
    }
    mIsStatisticsVisible = isVisible;
    resetStatistics();
    Q_EMIT NavigationChanged();
    Q_EMIT StatisticsChanged();
}
void RenderPreview::resetStatistics() noexcept
{
    mStatistics.GpuMilliseconds.Reset();
    mStatistics.CpuMilliseconds.Reset();
    mStatistics.GpuState = ViewportGpuTimeState::Waiting;
    mStatisticsRefresh.invalidate();
}
bool RenderPreview::CanProfileGpu() const noexcept
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    return OwnsDevice_ == true && mStatistics.GpuState != ViewportGpuTimeState::Unavailable;
#else
    return false;
#endif
}
bool RenderPreview::SetGpuProfilingEnabled(bool isEnabled) noexcept
{
    mIsGpuProfilingEnabled = isEnabled;
    if (isEnabled == true)
    {
        if (CanProfileGpu() == false)
        {
            mIsGpuProfilingEnabled = false;
            return false;
        }
        serviceGpuTiming(true);
        if (mStatistics.GpuState == ViewportGpuTimeState::Unavailable ||
            (mIsTimingArmed == false && mHasTimingAssociation == false))
        {
            mIsGpuProfilingEnabled = false;
            return false;
        }
    }
    return true;
}
bool RenderPreview::HasGpuProfilingPending(uint64 throughTicks) const noexcept
{
    return HasGpuFrameProfilingPending(throughTicks) == true || HasGpuPassProfilingPending(throughTicks) == true;
}
void RenderPreview::PollGpuProfiling() noexcept
{
    serviceGpuTiming(false);
}
void RenderPreview::PrepareGpuProfileFrame() noexcept
{
    serviceGpuTiming(true);
    mPassTimingCandidateTicks = mIsPassTimingArmed == true ? foundation::time::NowTicks() : 0;
    mTimingCandidateTicks = mIsTimingArmed == true ? foundation::time::NowTicks() : 0;
}
void RenderPreview::NotifyGpuProfileSubmission() noexcept
{
    ++mSubmittedFrameSequence;
    if (mIsGpuProfilingEnabled == true)
    {
        Q_EMIT GpuProfileSubmission();
    }
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    rhi::GpuPassTimings passes;
    if (mIsPassTimingArmed == true && rhi::GetGpuPassTimings(Device_, passes) == rhi::RasterStatus::Ready &&
        passes.State != rhi::GpuTimingState::Armed && passes.State != rhi::GpuTimingState::Empty &&
        passes.State != rhi::GpuTimingState::Unavailable)
    {
        mPassTimingFrameId = mSubmittedFrameSequence;
        mPassTimingSubmissionTicks = mPassTimingCandidateTicks;
        mPassTimingDeviceSession = mDeviceSerial;
        mHasPassTimingAssociation = true;
        mIsPassTimingArmed = false;
        serviceGpuPassTimings(false);
    }
    rhi::GpuTiming timing;
    if (mIsTimingArmed == true && rhi::GetGpuTiming(Device_, timing) == rhi::RasterStatus::Ready &&
        timing.State != rhi::GpuTimingState::Armed && timing.State != rhi::GpuTimingState::Empty &&
        timing.State != rhi::GpuTimingState::Unavailable)
    {
        mTimingFrameId = mSubmittedFrameSequence;
        mTimingSubmissionTicks = mTimingCandidateTicks;
        mTimingDeviceSession = mDeviceSerial;
        mHasTimingAssociation = true;
        mIsTimingArmed = false;
        serviceGpuTiming(false);
    }
#endif
}
bool RenderPreview::CanProfileGpuPasses() const noexcept
{
    return mIsPassTimingSupported;
}
bool RenderPreview::HasGpuFrameProfilingPending(uint64 throughTicks) const noexcept
{
    return mHasTimingAssociation == true && mTimingSubmissionTicks <= throughTicks;
}
bool RenderPreview::HasGpuPassProfilingPending(uint64 throughTicks) const noexcept
{
    return mHasPassTimingAssociation == true && mPassTimingSubmissionTicks <= throughTicks;
}
void RenderPreview::serviceGpuPassTimings(bool shouldArm) noexcept
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    if (OwnsDevice_ == false ||
        (mIsGpuProfilingEnabled == false && mHasPassTimingAssociation == false && mIsPassTimingArmed == false))
    {
        return;
    }
    rhi::GpuPassTimings batch;
    if (rhi::GetGpuPassTimings(Device_, batch) != rhi::RasterStatus::Ready ||
        batch.State == rhi::GpuTimingState::Pending || batch.State == rhi::GpuTimingState::Armed)
    {
        return;
    }
    if (mHasPassTimingAssociation == true)
    {
        mHasPassTimingAssociation = false;
        Q_EMIT GpuPassProfileBatch(mPassTimingFrameId, mPassTimingDeviceSession, mPassTimingSubmissionTicks, batch);
    }
    if (shouldArm == true && mIsGpuProfilingEnabled == true)
    {
        const rhi::RasterStatus status = rhi::ArmGpuPassTimings(Device_);
        mIsPassTimingArmed = status == rhi::RasterStatus::Ready;
        mIsPassTimingSupported = mIsPassTimingArmed;
    }
#else
    static_cast<void>(shouldArm);
#endif
}
void RenderPreview::serviceGpuTiming(bool shouldArm) noexcept
{
    serviceGpuPassTimings(shouldArm);
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    if (mIsStatisticsVisible == false && mIsGpuProfilingEnabled == false && mHasTimingAssociation == false &&
        mIsTimingArmed == false)
    {
        return;
    }
    if (OwnsDevice_ == false)
    {
        mHasTimingAssociation = false;
        mIsTimingArmed = false;
        mHasPassTimingAssociation = false;
        mIsPassTimingArmed = false;
        return;
    }
    rhi::GpuTiming timing;
    if (rhi::GetGpuTiming(Device_, timing) != rhi::RasterStatus::Ready)
    {
        return;
    }
    if (timing.State == rhi::GpuTimingState::Pending || timing.State == rhi::GpuTimingState::Armed)
    {
        return;
    }
    if (mHasTimingAssociation == true)
    {
        constexpr float64 MAX_DURATION_NANOSECONDS = 60'000'000'000;
        const bool isValid = timing.State == rhi::GpuTimingState::Available && timing.DurationNanoseconds >= 0 &&
                             timing.DurationNanoseconds < MAX_DURATION_NANOSECONDS;
        if (isValid == true && mIsStatisticsVisible == true)
        {
            mStatistics.GpuMilliseconds.Add(timing.DurationNanoseconds / NANOSECONDS_PER_MILLISECOND);
            mStatistics.GpuState = ViewportGpuTimeState::Available;
        }
        const uint64 duration = isValid == true ? static_cast<uint64>(timing.DurationNanoseconds + 0.5) : 0;
        mHasTimingAssociation = false;
        Q_EMIT GpuProfileSample(mTimingFrameId, mTimingDeviceSession, mTimingSubmissionTicks, duration, isValid);
    }
    if (timing.State == rhi::GpuTimingState::Unavailable)
    {
        mStatistics.GpuState = ViewportGpuTimeState::Unavailable;
        return;
    }
    if (shouldArm == true && (mIsStatisticsVisible == true || mIsGpuProfilingEnabled == true))
    {
        const rhi::RasterStatus armed = rhi::ArmGpuTiming(Device_);
        mIsTimingArmed = armed == rhi::RasterStatus::Ready;
        if (armed == rhi::RasterStatus::Unsupported)
        {
            mStatistics.GpuState = ViewportGpuTimeState::Unavailable;
        }
    }
#else
    static_cast<void>(shouldArm);
    mStatistics.GpuState = ViewportGpuTimeState::Unavailable;
#endif
}
void RenderPreview::recordFrameStatistics(const QElapsedTimer& frameTimer)
{
    if (mIsStatisticsVisible == false)
    {
        return;
    }
    mStatistics.CpuMilliseconds.Add(static_cast<float64>(frameTimer.nsecsElapsed()) / NANOSECONDS_PER_MILLISECOND);
    mStatistics.Width = Width_;
    mStatistics.Height = Height_;
    mStatistics.Draws = Report_.Draws;
    if (mStatisticsRefresh.isValid() == false || mStatisticsRefresh.elapsed() >= STATISTICS_REFRESH_MILLISECONDS)
    {
        mStatisticsRefresh.start();
        Q_EMIT StatisticsChanged();
    }
}
void RenderPreview::SetShading(PreviewShading shading)
{
    if (static_cast<usize>(shading) >= PREVIEW_SHADING_COUNT || shading == mShading)
    {
        return;
    }
    mShading = shading;
    if (OwnsDevice_ == true)
    {
        restartResources();
        Frames_ = 0;
        mFramePump.Start();
    }
    Q_EMIT NavigationChanged();
    Q_EMIT Changed();
}
bool RenderPreview::beginNavigation()
{
    if (Camera_ >= 0 && Camera_ < Scene_.Cameras.size())
    {
        const gameplay::camera::CameraSample& sample = Scene_.Cameras[Camera_].Sample;
        const float32 distance = GetFocusDistance();
        if (mCameraController.TryAdopt(sample, distance) == false)
        {
            return false;
        }
    }
    Camera_ = -1;
    CameraId_.clear();
    return true;
}
void RenderPreview::navigationChanged()
{
    const math::Vector3d pivotWorld = mCameraController.GetPivotWorld();
    OrbitCenter_ = {static_cast<float32>(pivotWorld.X),
                    static_cast<float32>(pivotWorld.Y),
                    static_cast<float32>(pivotWorld.Z)};
    Yaw_ = mCameraController.GetYawDegrees();
    Pitch_ = mCameraController.GetPitchDegrees();
    Zoom_ = mCameraController.GetDistance() / math::Max(OrbitRadius_, 0.000001F);
    RestoredOrbit_ = true;
    ViewDirty_ = true;
    // Thanks to The Qt Company, "QTimer Class", start() documentation:
    // https://doc.qt.io/qt-6/qtimer.html#start
    // QTimer::start restarts an active timer. Preserve its deadline so continuous
    // mouse/fly input cannot postpone every frame until navigation stops.
    if (Running() == true && mFramePump.IsActive() == false)
    {
        mFramePump.Start();
    }
    Q_EMIT NavigationChanged();
}
void RenderPreview::SetSelection(const QString& id)
{
    mSelectedId = id;
}
bool RenderPreview::FrameSelection() noexcept
{
    math::Aabb3 boundsWorld;
    if (mSelectedId.isEmpty() == true || Scene_.TryGetBounds(mSelectedId, boundsWorld) == false ||
        beginNavigation() == false ||
        mCameraController.TryFrame(boundsWorld,
                                   static_cast<float32>(RequestedWidth_) / static_cast<float32>(RequestedHeight_)) ==
            false)
    {
        return false;
    }
    navigationChanged();
    return true;
}
void RenderPreview::FrameAll() noexcept
{
    math::Aabb3 boundsWorld;
    if (Scene_.TryGetBounds({}, boundsWorld) == false || beginNavigation() == false ||
        mCameraController.TryFrame(boundsWorld,
                                   static_cast<float32>(RequestedWidth_) / static_cast<float32>(RequestedHeight_)) ==
            false)
    {
        return;
    }
    OrbitRadius_ = Scene_.Radius;
    navigationChanged();
}
void RenderPreview::Navigate(int32 camera, math::Vector2 orbitDegrees, float32 zoom, bool isReference)
{
    if (camera < -1 || camera >= Scene_.Cameras.size())
    {
        return;
    }
    const bool isDetaching = Camera_ >= 0 && camera == -1;
    if (camera == -1)
    {
        if (beginNavigation() == false)
        {
            return;
        }
        // Detaching adopts the visible pose/lens, including roll; stale orbit sliders never move it.
        if (isDetaching == false && (Yaw_ != orbitDegrees.X || Pitch_ != orbitDegrees.Y || Zoom_ != zoom))
        {
            if (mCameraController.TrySetOrbit(mCameraController.GetPivotWorld(), orbitDegrees, zoom * OrbitRadius_) ==
                false)
            {
                return;
            }
        }
    }
    Camera_ = camera;
    CameraId_ = camera >= 0 ? Scene_.Cameras[camera].Id : QString();
    Reference_ = isReference;
    navigationChanged();
}
void RenderPreview::SetPresentationVisible(bool isVisible) noexcept
{
    mIsPresentationVisible = isVisible;
}
void RenderPreview::Resize(int32 width, int32 height)
{
    const QSize size(width, height);
    if (mDrawableSize != size)
    {
        mDrawableSize = size;
        Q_EMIT DrawableSizeChanged(size);
    }
    if (width <= 0 || height <= 0)
    {
        return;
    }
    const float32 scale = math::Min(1.0F, 1024.0F / static_cast<float32>(math::Max(width, height)));
    RequestedWidth_ = math::Max(uint32{1}, static_cast<uint32>(static_cast<float32>(width) * scale));
    RequestedHeight_ = math::Max(uint32{1}, static_cast<uint32>(static_cast<float32>(height) * scale));
    if (Running() == true && mFramePump.IsActive() == false)
    {
        mFramePump.Start();
    }
}
float32 RenderPreview::GetFocusDistance() const noexcept
{
    if (Camera_ >= 0 && Camera_ < Scene_.Cameras.size())
    {
        const gameplay::camera::CameraSample& sample = Scene_.Cameras[Camera_].Sample;
        math::Aabb3 boundsWorld;
        if (mSelectedId.isEmpty() == false && Scene_.TryGetBounds(mSelectedId, boundsWorld) == true)
        {
            const math::Vector3 centerWorld = (boundsWorld.Min + boundsWorld.Max) * 0.5F;
            const math::Vector3d offsetWorld = math::Vector3d{static_cast<float64>(centerWorld.X),
                                                              static_cast<float64>(centerWorld.Y),
                                                              static_cast<float64>(centerWorld.Z)} -
                                               sample.Pose.Position;
            const math::Vector3 forwardWorld = math::Rotate(sample.Pose.Orientation, {0, 0, -1});
            const float64 distance = math::Dot(offsetWorld,
                                               math::Vector3d{static_cast<float64>(forwardWorld.X),
                                                              static_cast<float64>(forwardWorld.Y),
                                                              static_cast<float64>(forwardWorld.Z)});
            if (distance > 0.000001 && distance <= 10000000)
            {
                return static_cast<float32>(distance);
            }
        }
        // A distant room or floor must not make the first fly step leap across the scene.
        return math::Clamp(mCameraController.GetDistance(), 0.01F, 10.0F);
    }
    return mCameraController.GetDistance();
}
void RenderPreview::SetLookSensitivity(float32 sensitivity)
{
    if (math::IsFinite(sensitivity) == false || sensitivity < MINIMUM_LOOK_SENSITIVITY ||
        sensitivity > MAXIMUM_LOOK_SENSITIVITY || sensitivity == mLookSensitivity)
    {
        return;
    }
    mLookSensitivity = sensitivity;
    Q_EMIT NavigationChanged();
}
void RenderPreview::SetViewportSize(usize presetIndex)
{
    if (presetIndex >= VIEWPORT_SIZE_PRESET_COUNT || presetIndex == mViewportSizeIndex)
    {
        return;
    }
    mViewportSizeIndex = presetIndex;
    Q_EMIT NavigationChanged();
}
void RenderPreview::Orbit(float32 deltaX, float32 deltaY)
{
    if ((deltaX == 0 && deltaY == 0) || beginNavigation() == false)
    {
        return;
    }
    if (mCameraController.TryRotate(deltaX * mLookSensitivity, deltaY * mLookSensitivity, true) == true)
    {
        navigationChanged();
    }
}
void RenderPreview::Look(float32 deltaX, float32 deltaY)
{
    if ((deltaX == 0 && deltaY == 0) || beginNavigation() == false)
    {
        return;
    }
    if (mCameraController.TryRotate(deltaX * mLookSensitivity, deltaY * mLookSensitivity, false) == true)
    {
        navigationChanged();
    }
}
void RenderPreview::Pan(float32 deltaX, float32 deltaY, float32 heightLogical)
{
    if ((deltaX == 0 && deltaY == 0) || beginNavigation() == false)
    {
        return;
    }
    if (mCameraController.TryPan(deltaX,
                                 deltaY,
                                 heightLogical,
                                 static_cast<float32>(RequestedWidth_) / static_cast<float32>(RequestedHeight_)) ==
        true)
    {
        navigationChanged();
    }
}
void RenderPreview::Fly(math::Vector3 movementControls, float32 deltaSeconds, float32 speed)
{
    if (math::IsFinite(deltaSeconds) == false || deltaSeconds <= 0 || math::IsFinite(speed) == false || speed <= 0 ||
        math::LengthSquared(movementControls) == 0 || beginNavigation() == false)
    {
        return;
    }
    if (math::TryNormalize(movementControls, movementControls) != math::MathStatus::Success)
    {
        return;
    }
    const math::Quaternion orientation = mCameraController.GetSample().Pose.Orientation;
    math::Vector3 movementWorld = math::Rotate(orientation, {movementControls.X, 0, movementControls.Z}) +
                                  math::Vector3{0, movementControls.Y, 0};
    if (math::TryNormalize(movementWorld, movementWorld) != math::MathStatus::Success)
    {
        return;
    }
    if (mCameraController.TryFly(math::Rotate(math::Conjugate(orientation), movementWorld),
                                 math::Min(deltaSeconds, 0.1F) * speed) == true)
    {
        navigationChanged();
    }
}
void RenderPreview::SetAxisView(EditorAxisView view)
{
    math::Aabb3 boundsWorld;
    const bool hasBounds = Scene_.TryGetBounds({}, boundsWorld);
    if (beginNavigation() == true &&
        mCameraController.TrySetAxisView(view, hasBounds == true ? &boundsWorld : nullptr) == true)
    {
        navigationChanged();
    }
}
void RenderPreview::SetOrthographic(bool isOrthographic)
{
    math::Aabb3 boundsWorld;
    const bool hasBounds = Scene_.TryGetBounds({}, boundsWorld);
    if (beginNavigation() == true &&
        mCameraController.TrySetOrthographic(isOrthographic, hasBounds == true ? &boundsWorld : nullptr) == true)
    {
        navigationChanged();
    }
}
void RenderPreview::SetVerticalFovDegrees(float32 degrees)
{
    if (math::IsFinite(degrees) == false ||
        math::DegreesToRadians(degrees) == GetVisibleCameraSample().Lens.VerticalFov || beginNavigation() == false)
    {
        return;
    }
    if (mCameraController.TrySetVerticalFov(math::DegreesToRadians(degrees)) == true)
    {
        navigationChanged();
    }
}
const gameplay::camera::CameraSample& RenderPreview::GetVisibleCameraSample() const noexcept
{
    if (Camera_ >= 0 && Camera_ < Scene_.Cameras.size())
    {
        return Scene_.Cameras[Camera_].Sample;
    }
    return mCameraController.GetSample();
}
void RenderPreview::Zoom(float32 steps)
{
    if (steps == 0 || beginNavigation() == false)
    {
        return;
    }
    if (mCameraController.TryDolly(steps) == true)
    {
        navigationChanged();
    }
}
void RenderPreview::SetFocusDistance(float32 distance)
{
    if (beginNavigation() == true && mCameraController.TrySetDistance(distance) == true)
    {
        navigationChanged();
    }
}
QJsonObject RenderPreview::Navigation() const
{
    return {{QStringLiteral("yaw"), Yaw_},
            {QStringLiteral("pitch"), Pitch_},
            {QStringLiteral("zoom"), Zoom_},
            {QStringLiteral("camera"), CameraId_},
            {QStringLiteral("reference"), Reference_},
            {QStringLiteral("shading"), GetPreviewShadingKey(mShading)},
            {QStringLiteral("statistics"), mIsStatisticsVisible},
            {QStringLiteral("forceRaster"), mShouldForceRaster},
            {QStringLiteral("x"), OrbitCenter_.X},
            {QStringLiteral("y"), OrbitCenter_.Y},
            {QStringLiteral("z"), OrbitCenter_.Z},
            {QStringLiteral("radius"), OrbitRadius_},
            {QStringLiteral("navigationVersion"), NAVIGATION_STATE_VERSION},
            {QStringLiteral("lookSensitivity"), mLookSensitivity},
            {QStringLiteral("viewportSize"), GetViewportSizeKey(mViewportSizeIndex)},
            {QStringLiteral("distance"), mCameraController.GetDistance()},
            {QStringLiteral("eyeX"), mCameraController.GetSample().Pose.Position.X},
            {QStringLiteral("eyeY"), mCameraController.GetSample().Pose.Position.Y},
            {QStringLiteral("eyeZ"), mCameraController.GetSample().Pose.Position.Z},
            {QStringLiteral("rotationX"), mCameraController.GetSample().Pose.Orientation.X},
            {QStringLiteral("rotationY"), mCameraController.GetSample().Pose.Orientation.Y},
            {QStringLiteral("rotationZ"), mCameraController.GetSample().Pose.Orientation.Z},
            {QStringLiteral("rotationW"), mCameraController.GetSample().Pose.Orientation.W},
            {QStringLiteral("projection"), static_cast<int32>(mCameraController.GetSample().Lens.Projection)},
            {QStringLiteral("fov"), mCameraController.GetSample().Lens.VerticalFov},
            {QStringLiteral("span"), mCameraController.GetSample().Lens.VerticalSpan},
            {QStringLiteral("minimumWidth"), mCameraController.GetSample().Lens.MinimumVisibleWidth},
            {QStringLiteral("near"), mCameraController.GetSample().Lens.NearPlane},
            {QStringLiteral("far"), mCameraController.GetSample().Lens.FarPlane}};
}
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — width then height matches ViewDescription.
PreviewViewOptions RenderPreview::makeViewOptions(uint32 width, uint32 height) const noexcept
{
    PreviewViewOptions options;
    options.Camera = Camera_;
    options.OrbitDegrees = {Yaw_, Pitch_};
    options.Distance = OrbitRadius_ * Zoom_;
    options.Width = width;
    options.Height = height;
    options.Reference = Reference_;
    options.OrbitCenter = &OrbitCenter_;
    options.OrbitRadius = OrbitRadius_;
    options.EditorCameraOrNull = &mCameraController.GetSample();
    return options;
}
bool RenderPreview::DescribeView(uint32 width, uint32 height, rr::ViewDescription& outView, QString& outError) const
{
    rr::ViewDescription view;
    if (Scene_.View(makeViewOptions(width, height), view, outError) == false)
    {
        return false;
    }
    view.WorldGrid = { .IsVisible = true, .FocusDistance = GetFocusDistance(), .Width = width, .Height = height };
    outView = view;
    return true;
}
bool RenderPreview::DescribeConventionalView(uint32 width,
                                             uint32 height,
                                             rr::ViewDescription& outView,
                                             QString& outError) const
{
    PreviewViewOptions options = makeViewOptions(width, height);
    options.ShouldUseConventionalDepth = true;
    rr::ViewDescription view;
    if (Scene_.View(options, view, outError) == false)
    {
        return false;
    }
    view.WorldGrid = { .IsVisible = true, .FocusDistance = GetFocusDistance(), .Width = width, .Height = height };
    outView = view;
    return true;
}
void RenderPreview::RestoreNavigation(const QJsonObject& state)
{
    const auto number = [&state](const QString& key, float64 fallback) {
        const float64 value = state.value(key).toDouble(fallback);
        return math::IsFinite(value) == true ? value : fallback;
    };
    const math::Vector3d pivotWorld{number(QStringLiteral("x"), static_cast<float64>(Scene_.Center.X)),
                                    number(QStringLiteral("y"), static_cast<float64>(Scene_.Center.Y)),
                                    number(QStringLiteral("z"), static_cast<float64>(Scene_.Center.Z))};
    float32 orbitRadius = static_cast<float32>(number(QStringLiteral("radius"), static_cast<float64>(Scene_.Radius)));
    EditorCameraController controller = mCameraController;
    if (orbitRadius <= 0 || math::IsFinite(orbitRadius) == false)
    {
        orbitRadius = 1;
    }
    if (state.value(QStringLiteral("navigationVersion")).toInt() == NAVIGATION_STATE_VERSION)
    {
        gameplay::camera::CameraSample sample;
        sample.Pose.Position = {number(QStringLiteral("eyeX"), 0),
                                number(QStringLiteral("eyeY"), 0),
                                number(QStringLiteral("eyeZ"), 3)};
        sample.Pose.Orientation = {static_cast<float32>(number(QStringLiteral("rotationX"), 0)),
                                   static_cast<float32>(number(QStringLiteral("rotationY"), 0)),
                                   static_cast<float32>(number(QStringLiteral("rotationZ"), 0)),
                                   static_cast<float32>(number(QStringLiteral("rotationW"), 1))};
        const int32 projection = state.value(QStringLiteral("projection")).toInt(-1);
        if (projection < static_cast<int32>(gameplay::camera::CameraProjection::PerspectiveFinite) ||
            projection > static_cast<int32>(gameplay::camera::CameraProjection::OrthographicFinite))
        {
            return;
        }
        sample.Lens.Projection = static_cast<gameplay::camera::CameraProjection>(projection);
        sample.Lens.VerticalFov = static_cast<float32>(number(QStringLiteral("fov"), 1));
        sample.Lens.VerticalSpan = static_cast<float32>(number(QStringLiteral("span"), 14));
        sample.Lens.MinimumVisibleWidth = static_cast<float32>(number(QStringLiteral("minimumWidth"), 0));
        sample.Lens.NearPlane = static_cast<float32>(number(QStringLiteral("near"), 0.01));
        sample.Lens.FarPlane = static_cast<float32>(number(QStringLiteral("far"), 100));
        if (controller.TryAdopt(sample, static_cast<float32>(number(QStringLiteral("distance"), 3))) == false)
        {
            return;
        }
    }
    else
    {
        if (controller.TrySetOrbit(pivotWorld,
                                   {static_cast<float32>(number(QStringLiteral("yaw"), 25)),
                                    static_cast<float32>(number(QStringLiteral("pitch"), 20))},
                                   static_cast<float32>(number(QStringLiteral("zoom"), 3)) * orbitRadius) == false)
        {
            return;
        }
    }
    mCameraController = controller;
    OrbitRadius_ = orbitRadius;
    Camera_ = -1;
    CameraId_ = state.value(QStringLiteral("camera")).toString().left(256);
    Reference_ = state.value(QStringLiteral("reference")).toBool();
    PreviewShading shading = PreviewShading::Solid;
    static_cast<void>(TryParsePreviewShading(state.value(QStringLiteral("shading")).toString(), shading));
    SetShading(shading);
    SetStatisticsVisible(state.value(QStringLiteral("statistics")).toBool(false));
    SetForceRaster(state.value(QStringLiteral("forceRaster")).toBool(false));
    SetLookSensitivity(static_cast<float32>(number(QStringLiteral("lookSensitivity"), 1)));
    usize viewportSize = 0;
    static_cast<void>(TryParseViewportSizeKey(state.value(QStringLiteral("viewportSize")).toString(), viewportSize));
    SetViewportSize(viewportSize);
    navigationChanged();
    // An empty project reset supplies no saved pose to preserve when the first scene arrives.
    RestoredOrbit_ = state.isEmpty() == false;
}
void RenderPreview::resetFrames() noexcept
{
    Q_EMIT ResettingFrames();
    if (mHasPendingFrame == true)
    {
        static_cast<void>(mResources->Viewport.Release(mPendingFrame));
    }
    mPendingFrame = {};
    mPendingImage = {};
    mHasPendingFrame = false;
    mHasFrame = false;
    if (mResources.Get() != nullptr)
    {
        mResources->Viewport.Reset();
    }
    mRetiredResources.Reset();
    mHasViewport = false;
    mHasPreparedView = false;
    mProducer = {};
    mIsAwaitingProducer = false;
    mFramePump.SetProducerPending(false);
}
bool RenderPreview::TakeFrame(rr::ViewportFrame& outFrame, rr::ViewportImage& outImage) noexcept
{
    if (mHasPendingFrame == false)
    {
        return false;
    }
    ++mResources->ExternalLeases;
    outFrame = mPendingFrame;
    outImage = mPendingImage;
    mPendingFrame = {};
    mPendingImage = {};
    mHasPendingFrame = false;
    return true;
}
void RenderPreview::ReleaseFrame(rr::ViewportFrame& inoutFrame) noexcept
{
    if (mResources.Get() != nullptr && mResources->Viewport.Release(inoutFrame) == rhi::RasterStatus::Ready)
    {
        LUDUS_REQUIRE(mResources->ExternalLeases != 0);
        --mResources->ExternalLeases;
        mFramePump.Wake();
    }
    else if (mRetiredResources.Get() != nullptr &&
             mRetiredResources->Viewport.Release(inoutFrame) == rhi::RasterStatus::Ready)
    {
        LUDUS_REQUIRE(mRetiredResources->ExternalLeases != 0);
        --mRetiredResources->ExternalLeases;
        mFramePump.Wake();
        if (mRetiredResources->ExternalLeases == 0)
        {
            mRetiredResources.Reset();
        }
    }
}
rhi::RasterStatus RenderPreview::PinDevice(rhi::NativeDeviceLease& outLease) noexcept
{
    return rhi::AcquireNativeDevice(Device_, outLease);
}
void RenderPreview::fail(const QString& message)
{
    if ((mRetiredResources.Get() != nullptr && mRetiredResources->ExternalLeases != 0) ||
        (mResources.Get() != nullptr && mResources->ExternalLeases != 0))
    {
        mFramePump.Stop();
        mHasPreparationFailed = true;
        mNeedsResourceRestart = false;
    }
    else
    {
        Stop();
    }
    Status_ = message;
    Q_EMIT Changed();
    Q_EMIT Failed(message);
}
bool RenderPreview::prepare()
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    const PreviewPreparationStatus status = mResources->Prepare(Surface_, mPreparationFailure);
    if (status == PreviewPreparationStatus::Failed)
    {
        return false;
    }
    if (status == PreviewPreparationStatus::Pending)
    {
        return true;
    }
    if (Width_ == 0 || Height_ == 0 || mHasViewport == false)
    {
        return true;
    }
    const rhi::RasterStatus viewportStatus = mResources->Viewport.Poll();
    if (viewportStatus == rhi::RasterStatus::Pending)
    {
        return true;
    }
    if (viewportStatus != rhi::RasterStatus::Ready)
    {
        mPreparationFailure = QStringLiteral("GPU viewport image preparation failed.");
        return false;
    }
    if (ViewDirty_ == true)
    {
        rr::ViewDescription description;
        if (DescribeView(Width_, Height_, description, mPreparationFailure) == false)
        {
            return false;
        }
        if (mShading == PreviewShading::Depth)
        {
            const gameplay::camera::CameraLens& lens = Camera_ >= 0 && Camera_ < Scene_.Cameras.size()
                                                           ? Scene_.Cameras[Camera_].Sample.Lens
                                                           : mCameraController.GetSample().Lens;
            math::Aabb3 boundsWorld;
            const bool hasBounds = Scene_.TryGetBounds({}, boundsWorld);
            description.DebugDepthRange = ComputeDebugDepthRange(description,
                                                                 lens.NearPlane,
                                                                 lens.FarPlane,
                                                                 hasBounds == true ? &boundsWorld : nullptr);
        }
        Description_ = description;
        if (Accepted(mResources->Viewport.Update(mResources->GetSnapshot(), description, Report_)) == false)
        {
            return false;
        }
        ViewDirty_ = false;
        FreshReport_ = true;
        mHasPreparedView = true;
    }
    return Accepted(mResources->Viewport.Poll());
#else
    return false;
#endif
}
void RenderPreview::UseProjectGraph() noexcept
{
    mIsProjectGraphEnabled = true;
}
void RenderPreview::SetProjectGraphProblem(const QString& problem)
{
    if (mIsProjectGraphEnabled == false || mProjectGraphProblem == problem)
    {
        return;
    }
    mProjectGraphProblem = problem;
    if (problem.isEmpty() == false)
    {
        Status_ = problem;
        Q_EMIT Changed();
        if (Running() == true)
        {
            Q_EMIT Failed(problem);
        }
    }
}
void RenderPreview::NotifyProjectFrame()
{
    if (mIsProjectGraphEnabled == true && OwnsDevice_ == true)
    {
        mHasFrame = true;
        ++Frames_;
        Status_ = QStringLiteral("World viewport updated.");
        const uint64 presentedFrame = Frames_;
        Q_EMIT FrameReady();
        if (OwnsDevice_ == true && Frames_ == presentedFrame)
        {
            Q_EMIT Changed();
        }
    }
}
void RenderPreview::Tick()
{
    LUDUS_PROFILE_SCOPE(EditorViewportTick);
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    if (OwnsDevice_ == false || mHasPreparationFailed == true)
    {
        return;
    }
    if (mIsProjectGraphEnabled == true)
    {
        static_cast<void>(rhi::PollLifetime(Device_));
        return;
    }
    QElapsedTimer frameTimer;
    frameTimer.start();
    rhi::DeviceInfo device;
    const rhi::DeviceStatus ready = rhi::GetDeviceInfo(Device_, device);
    if (ready == rhi::DeviceStatus::Pending)
    {
        return;
    }
    if (ready == rhi::DeviceStatus::Ready && device.FrameOpen == true && mResources.Get() != nullptr &&
        mResources->IsShadowFrameOpen == true)
    {
        const rhi::RasterStatus ended = rhi::EndFrame(Device_, mResources->ShadowProducer);
        if (ended == rhi::RasterStatus::Ready)
        {
            mResources->IsShadowFrameOpen = false;
            mResources->LightingStage = PreviewLightingStage::Materials;
        }
        else if (ended != rhi::RasterStatus::Pending)
        {
            fail(QStringLiteral("Shadow submission failed; previous frame retained."));
        }
        return;
    }
    if (ready == rhi::DeviceStatus::Ready && device.FrameOpen == true)
    {
        // RasterViewport retains recording when completion admission is full.
        // Retry that owned frame before lifetime service or new setup work.
        mProducer = {};
        const rhi::RasterStatus retried = mResources->Viewport.Render(Surface_, mProducer);
        rhi::DeviceInfo after;
        if (rhi::GetDeviceInfo(Device_, after) == rhi::DeviceStatus::Ready && after.FrameOpen == true)
        {
            return;
        }
        if (retried != rhi::RasterStatus::Ready)
        {
            fail(QStringLiteral("GPU viewport submission failed. World edits are preserved."));
            return;
        }
        NotifyGpuProfileSubmission();
        mIsAwaitingProducer = true;
        mFramePump.SetProducerPending(true);
        return;
    }
    if (ready != rhi::DeviceStatus::Ready || rhi::PollLifetime(Device_) != rhi::RasterStatus::Ready)
    {
        fail(QStringLiteral("GPU device is unavailable. World edits are preserved."));
        return;
    }
    if (mIsAwaitingProducer == true)
    {
        const rhi::RasterStatus completion = rhi::GetStatus(Device_, mProducer);
        if (completion == rhi::RasterStatus::Pending)
        {
            return;
        }
        if (completion != rhi::RasterStatus::Ready)
        {
            fail(QStringLiteral("GPU viewport submission failed. World edits are preserved."));
            return;
        }
        if (mHasPendingFrame == true)
        {
            static_cast<void>(mResources->Viewport.Release(mPendingFrame));
            mHasPendingFrame = false;
        }
        if (mResources->Viewport.Acquire(mPendingFrame, mPendingImage) != rhi::RasterStatus::Ready)
        {
            fail(QStringLiteral("GPU viewport image is unavailable. World edits are preserved."));
            return;
        }
        mHasPendingFrame = true;
        mHasFrame = true;
        mIsAwaitingProducer = false;
        mFramePump.SetProducerPending(false);
        ++Frames_;
        const uint64 ownerFrame = Frames_;
        const bool hasNewShaders = mResources->Shaders.GetRevision() != mPresentedShaders.GetRevision();
        if (hasNewShaders == true)
        {
            mPresentedShaders = mResources->Shaders;
        }
        Q_EMIT FrameReady();
        // A receiver can stop/replace the preview at this transaction boundary.
        if (OwnsDevice_ == false || Frames_ != ownerFrame)
        {
            return;
        }
        if (hasNewShaders == true)
        {
            Q_EMIT ShadersApplied(mPresentedShaders.GetRevision());
            if (OwnsDevice_ == false || Frames_ != ownerFrame)
            {
                return;
            }
        }
        // Completion polling only publishes this image. The consumer returns an old
        // image to wake the next submission; an absent consumer stays on fallback cadence.
        return;
    }
#    if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
    if (rhi::SetFrameTarget(Device_, Surface_, { .Width = RequestedWidth_, .Height = RequestedHeight_ }) !=
        rhi::DeviceStatus::Ready)
    {
        fail(QStringLiteral("WebGPU viewport resize failed. World edits are preserved."));
        return;
    }
#    endif
    if (mNeedsResourceRestart == true && beginResourceRestart() == false)
    {
        return;
    }
    if (mResources->IsPrepared() == true &&
        (mHasViewport == false || Width_ != RequestedWidth_ || Height_ != RequestedHeight_))
    {
        if (mHasViewport == true)
        {
            resetFrames();
        }
        Width_ = RequestedWidth_;
        Height_ = RequestedHeight_;
        const rhi::RasterStatus created = mResources->Viewport.Initialize(
            mResources->Renderer,
            Device_,
            Width_,
            Height_,
            1,
            usesLitMaterials() == true ? rr::ViewportColorMode::PreferHdr : rr::ViewportColorMode::Linear8,
#    if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
            true
#    else
            false
#    endif
        );
        if (Accepted(created) == false)
        {
            fail(QStringLiteral("GPU viewport allocation failed. World edits are preserved."));
            return;
        }
        mHasViewport = true;
        ViewDirty_ = true;
    }
    if (prepare() == false)
    {
        // A replacement shader set that cannot build pipelines falls back to the presented set.
        if (rejectShaders() == true)
        {
            return;
        }
        // Keep a stage's specific reason; the generic text covers unexplained failures.
        fail(
            mPreparationFailure.isEmpty() == false
                ? mPreparationFailure + QStringLiteral(" World edits are preserved.")
                : QStringLiteral("GPU renderer stopped or exceeded its supported profile. World edits are preserved."));
        return;
    }
    if (mHasPreparedView == false || mHasViewport == false)
    {
        return;
    }
    const rhi::RasterStatus configured = mResources->Viewport.Poll();
    if (configured == rhi::RasterStatus::Pending)
    {
        return;
    }
    if (configured != rhi::RasterStatus::Ready)
    {
        fail(QStringLiteral("GPU viewport setup failed. World edits are preserved."));
        return;
    }
    if (mIsPresentationVisible == false && Frames_ != 0)
    {
        return;
    }
#    if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
    // Shadow preparation may have recorded an offscreen-only frame in this tick.
    if (rhi::SetFrameTarget(Device_, Surface_, { .Width = RequestedWidth_, .Height = RequestedHeight_ }) !=
        rhi::DeviceStatus::Ready)
    {
        fail(QStringLiteral("WebGPU viewport target restoration failed. World edits are preserved."));
        return;
    }
#    endif
    // Arm immediately before the viewport frame so the probe encloses it, not a shadow submission.
    PrepareGpuProfileFrame();
    mProducer = {};
    const rhi::RasterStatus rendered = mResources->Viewport.Render(Surface_, mProducer);
    if (rendered == rhi::RasterStatus::NotReady || rendered == rhi::RasterStatus::Pending)
    {
        return;
    }
    if (rendered != rhi::RasterStatus::Ready)
    {
        fail(QStringLiteral("GPU viewport draw failed (%1). World edits are preserved.")
                 .arg(static_cast<uint32>(rendered)));
        return;
    }
    NotifyGpuProfileSubmission();
    mIsAwaitingProducer = true;
    mFramePump.SetProducerPending(true);
    recordFrameStatistics(frameTimer);
    if (FreshReport_ == true || Frames_ % 60 == 0)
    {
        FreshReport_ = false;
        Status_ =
            QStringLiteral("GPU raster viewport · %1×%2 · visible %3 · draws %4%5")
                .arg(Width_)
                .arg(Height_)
                .arg(Report_.Visible)
                .arg(Report_.Draws)
                .arg(IsPreviewDebugShading(mShading) == true ? GetPreviewShadingStatus(mShading)
                     : Scene_.Lit == true
                         ? (mShading == PreviewShading::Lit
                                ? QStringLiteral(" · Lit: %1 lights / %2 planar area proxies / %3 shadow faces · %4")
                                      .arg(mResources->Lighting.Lights.Count)
                                      .arg(mResources->Lighting.AreaCount)
                                      .arg(mResources->Lighting.ShadowCount)
                                      .arg(QStringLiteral("direct lighting"))
                                : (mShading == PreviewShading::Solid ? GetPreviewShadingStatus(PreviewShading::Solid)
                                                                     : GetPreviewShadingStatus(PreviewShading::Albedo)))
                         : (mShading == PreviewShading::Solid ? GetPreviewShadingStatus(PreviewShading::Solid)
                                                              : GetPreviewShadingStatus(PreviewShading::Albedo)));
        Q_EMIT Changed();
    }
#endif
}
} // namespace ludus::editor
