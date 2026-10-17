#include "internal/render_preview.h"
#include "internal/preview_bake.h"
#include "internal/preview_gpu_generation.h"
#include "internal/preview_lighting.h"
#include "internal/preview_shading.h"
#include "internal/static_lighting_job.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <ludus/content/scene_format/luscn.h>

#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>
#include <ludus/graphics/rhi/rhi.h>

#include <new>

#include <QElapsedTimer>
#include <QStringList>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace rhi = graphics::rhi;
namespace rr = graphics::renderer;
constexpr int32 NAVIGATION_STATE_VERSION = 2;
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
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
    // Two retained GPU generations, 80 caster views, and direct/baked material versions.
    // These are explicit admission budgets, not allocations or inferred native limits.
    description.Storage.ResourcesPerKind = 4096;
    description.Storage.BindingDescriptors = 32768;
    description.Storage.DrawsPerFrame = 32768;
    description.Storage.DrawsPerPlan = 1024;
    description.Storage.HostBytes = usize{256} * 1024 * 1024;
    description.Storage.ResourceBytes = uint64{2} * 1024 * 1024 * 1024;
    description.RequiredProfile.Raster.MinBindings = 11;
    description.RequiredProfile.Raster.MinUniformRange = 16384;
    description.RequiredProfile.Raster.MinVertexAttributes = 16;
    description.RequiredProfile.Raster.MinTextureDimension2D = preview_lighting::SHADOW_ATLAS_PIXELS;
    return description;
}
#endif
} // namespace
struct RenderPreview::Residency final
{
    QSharedPointer<resources::SceneDefinition> Definition;
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
        if (!resource.Id.Set({id.constData(), static_cast<usize>(id.size())}) ||
            !resource.Path.Set({path.constData(), static_cast<usize>(path.size())}))
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
               payload == Definition.data();
    }
};
RenderPreview::RenderPreview(QObject* parent) : QObject(parent)
{
    Timer_.setInterval(16);
    connect(&Timer_, &QTimer::timeout, this, &RenderPreview::Tick);
}
RenderPreview::~RenderPreview()
{
    Stop();
}
platform::NativeWindowInfo RenderPreview::WindowInfo() const noexcept
{
    return { .Width = RequestedWidth_, .Height = RequestedHeight_ };
}
bool RenderPreview::SetScene(const RenderScene& scene)
{
    bool valid = scene.Identity.isEmpty()
                     ? (scene.Meshes.isEmpty() && scene.Textures.isEmpty() && scene.Surfaces.isEmpty() &&
                        scene.Objects.isEmpty() && scene.Cameras.isEmpty())
                     : (!scene.Meshes.isEmpty() && scene.Meshes.size() <= 6 && scene.Textures.size() <= 8 &&
                        scene.Surfaces.size() <= 8 && scene.Objects.size() <= 128 && scene.Cameras.size() <= 128);
    for (const auto& surface : scene.Surfaces)
    {
        valid = valid && surface.Texture >= (scene.Lit ? -1 : 0) && surface.Texture < scene.Textures.size();
    }
    for (const auto& object : scene.Objects)
    {
        valid = valid && object.Mesh >= 0 && object.Mesh < scene.Meshes.size() && object.Texture >= -1 &&
                object.Texture < scene.Textures.size() &&
                (scene.Lit || object.Texture >= 0 ? object.Surface >= 0 && object.Surface < scene.Surfaces.size()
                                                  : object.Surface == -1);
    }
    if (!valid)
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
    if (Scene_.Identity.isEmpty() && !RestoredOrbit_)
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
        if (!residency || !scene.Definition || !residency->Initialize(scene))
        {
            Status_ = QStringLiteral("Resource-manager admission failed; previous scene retained.");
            Q_EMIT Changed();
            return false;
        }
    }
    // Retire the previous lease and provider through the local owner.
    Residency_.Swap(residency);
    Scene_ = scene;
    if (!CameraId_.isEmpty())
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
    Timer_.start();
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
    candidate.Reset(new (std::nothrow) PreviewGpuGeneration(Device_, Scene_, mShading));
    if (candidate.Get() == nullptr)
    {
        fail(QStringLiteral("GPU preview staging allocation failed. Previous view retained."));
        return false;
    }
    // Cancel and drain the obsolete bake now: the preview's JobSystem runs one bake
    // at a time, and a retired generation can outlive this call.
    if (mResources.Get() != nullptr)
    {
        mResources->BakeWorker.Reset();
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
    mNeedsResourceRestart = false;
    mPreparationFailure.clear();
    Report_ = {};
    Frames_ = 0;
    ViewDirty_ = true;
    return true;
}
bool RenderPreview::Start()
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (Scene_.Identity.isEmpty())
    {
        Status_ = QStringLiteral("Load and validate a World preview first.");
        Q_EMIT Changed();
        return false;
    }
    Timer_.setInterval(16);
    if (Running())
    {
        Timer_.start();
        return true;
    }
    if (rhi::GetStartup().State != rhi::StartupState::Idle)
    {
        Status_ = QStringLiteral("Another authoring surface owns rendering. Close it before opening the 3D preview.");
        Q_EMIT Changed();
        return false;
    }
    mIsStarting = true;
    Status_ = QStringLiteral("Preparing the shared Qt GPU device…");
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
    Width_ = Height_ = 0;
    Frames_ = 0;
    restartResources();
    Timer_.start();
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
        Status_ = QStringLiteral("No Vulkan device meets the offscreen viewport's raster profile and budgets. "
                                 "Editing remains available.");
        Q_EMIT Changed();
        Q_EMIT Failed(Status_);
        return false;
    }
    mIsStarting = false;
    OwnsDevice_ = true;
    ++mDeviceSerial;
    Width_ = Height_ = 0;
    Frames_ = 0;
    restartResources();
    Timer_.start();
    return true;
#else
    return false;
#endif
}
void RenderPreview::Stop() noexcept
{
    const bool wasRunning = Running();
    mIsStarting = false;
    Timer_.stop();
    if (OwnsDevice_)
    {
        // Borrowers such as the render-graph preview release their resources first.
        Q_EMIT DeviceStopping();
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
void RenderPreview::SetShading(PreviewShading shading)
{
    if ((shading != PreviewShading::Solid && shading != PreviewShading::Albedo && shading != PreviewShading::Lit) ||
        shading == mShading)
    {
        return;
    }
    mShading = shading;
    if (OwnsDevice_ == true)
    {
        restartResources();
        Frames_ = 0;
        Timer_.start();
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
    if (Running() == true)
    {
        Timer_.start();
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
void RenderPreview::Resize(int32 width, int32 height)
{
    if (width <= 0 || height <= 0)
    {
        return;
    }
    const float32 scale = math::Min(1.0F, 1024.0F / static_cast<float32>(math::Max(width, height)));
    RequestedWidth_ = math::Max(uint32{1}, static_cast<uint32>(static_cast<float32>(width) * scale));
    RequestedHeight_ = math::Max(uint32{1}, static_cast<uint32>(static_cast<float32>(height) * scale));
    if (Running() == true)
    {
        Timer_.start();
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
void RenderPreview::Orbit(float32 deltaX, float32 deltaY)
{
    if ((deltaX == 0 && deltaY == 0) || beginNavigation() == false)
    {
        return;
    }
    if (mCameraController.TryRotate(deltaX, deltaY, true) == true)
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
    if (mCameraController.TryRotate(deltaX, deltaY, false) == true)
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
            {QStringLiteral("shading"),
             mShading == PreviewShading::Lit
                 ? QStringLiteral("lit")
                 : (mShading == PreviewShading::Albedo ? QStringLiteral("albedo") : QStringLiteral("solid"))},
            {QStringLiteral("x"), OrbitCenter_.X},
            {QStringLiteral("y"), OrbitCenter_.Y},
            {QStringLiteral("z"), OrbitCenter_.Z},
            {QStringLiteral("radius"), OrbitRadius_},
            {QStringLiteral("navigationVersion"), NAVIGATION_STATE_VERSION},
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
bool RenderPreview::DescribeView(uint32 width, uint32 height, rr::ViewDescription& outView, QString& outError) const
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
    return Scene_.View(options, outView, outError);
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
    SetShading(state.value(QStringLiteral("shading")).toString() == QStringLiteral("lit")
                   ? PreviewShading::Lit
                   : (state.value(QStringLiteral("shading")).toString() == QStringLiteral("albedo")
                          ? PreviewShading::Albedo
                          : PreviewShading::Solid));
    navigationChanged();
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
    }
    else if (mRetiredResources.Get() != nullptr &&
             mRetiredResources->Viewport.Release(inoutFrame) == rhi::RasterStatus::Ready)
    {
        LUDUS_REQUIRE(mRetiredResources->ExternalLeases != 0);
        --mRetiredResources->ExternalLeases;
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
        Timer_.stop();
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
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
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
    if (ViewDirty_ == true)
    {
        rr::ViewDescription description;
        PreviewViewOptions options;
        options.Camera = Camera_;
        options.OrbitDegrees = {Yaw_, Pitch_};
        options.Distance = OrbitRadius_ * Zoom_;
        options.Width = Width_;
        options.Height = Height_;
        options.Reference = Reference_;
        options.OrbitCenter = &OrbitCenter_;
        options.OrbitRadius = OrbitRadius_;
        options.EditorCameraOrNull = &mCameraController.GetSample();
        if (Scene_.View(options, description, mPreparationFailure) == false)
        {
            return false;
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
    return Accepted(pollViewport());
#else
    return false;
#endif
}
void RenderPreview::serviceStaticLighting()
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    PreviewGpuGeneration& resources = *mResources;
    if (resources.HasBakeFailed == true || resources.IsBakePublished == true || Frames_ == 0 ||
        (resources.IsBakeCancelled == true && resources.BakeWorker.Get() == nullptr))
    {
        return;
    }
    const usize count = resources.Source.Definition->Objects.GetSize();
    if (resources.HasBakeStarted == false)
    {
        rr::StaticLightingBakeDescription description;
        description.SceneRevision = resources.Lighting.SceneRevision;
        description.DependencyRevision = resources.Lighting.DependencyRevision;
        description.Surfaces = {resources.Lighting.Surfaces.GetData(), count};
        if (mHasBakeJobs == false)
        {
            mHasBakeJobs = mBakeJobs.Initialize({ .WorkerCount = STATIC_LIGHTING_WORKER_COUNT }) ==
                           foundation::threading::JobStatus::Ok;
        }
        resources.BakeWorker.Reset(new (std::nothrow) StaticLightingJob(resources.Source, description));
        resources.HasBakeStarted = true;
        if (mHasBakeJobs == false || resources.BakeWorker.Get() == nullptr ||
            resources.BakeWorker->Start(mBakeJobs) != rhi::RasterStatus::Pending)
        {
            resources.BakeWorker.Reset();
            resources.HasBakeFailed = true;
        }
        markBakeChanged();
        return;
    }
    if (resources.BakeWorker.Get() != nullptr)
    {
        updateBakeNotice();
        const rhi::RasterStatus status = resources.BakeWorker->Poll();
        if (status == rhi::RasterStatus::Pending)
        {
            return;
        }
        if (resources.IsBakeCancelled == true)
        {
            // A cancelled bake is discarded even if its last range finished first.
            resources.BakeWorker.Reset();
            markBakeChanged();
            return;
        }
        if (status != rhi::RasterStatus::Ready)
        {
            resources.HasBakeFailed = true;
            markBakeChanged();
            return;
        }
        resources.Bake = resources.BakeWorker->TakeBake();
        resources.BakeWorker.Reset();
    }
    const auto failBake = [this, &resources]() {
        resources.HasBakeFailed = true;
        markBakeChanged();
    };
    while (resources.BakeUploadCursor < count)
    {
        rr::StaticLightingChart chart;
        const usize index = resources.BakeUploadCursor;
        const resources::SceneObject& object = resources.Source.Definition->Objects[index];
        if (resources.Bake->GetChart(index, chart) != rhi::RasterStatus::Ready || chart.SourceId != object.SourceId ||
            chart.SceneRevision != resources.Lighting.SceneRevision ||
            chart.DependencyRevision != resources.Lighting.DependencyRevision || chart.Transform != object.Transform)
        {
            failBake();
            return;
        }
        rhi::TextureDescription description;
        description.Width = chart.Width;
        description.Height = chart.Height;
        description.Format = rhi::RasterFormat::Rgba8Unorm;
        const rhi::RasterStatus status =
            rhi::CreateTexture(Device_,
                               description,
                               {{chart.Pixels.GetDataOrNull(), chart.Pixels.GetSize()}, usize{chart.Width} * 4},
                               resources.BakeTextures[index]);
        if (Accepted(status) == false)
        {
            failBake();
            return;
        }
        ++resources.BakeUploadCursor;
    }
    for (usize i = 0; i < count; ++i)
    {
        const rhi::RasterStatus status = rhi::GetStatus(Device_, resources.BakeTextures[i]);
        if (status == rhi::RasterStatus::Pending)
        {
            return;
        }
        if (status != rhi::RasterStatus::Ready)
        {
            failBake();
            return;
        }
    }
    while (resources.BakeMaterialCursor < count)
    {
        const usize index = resources.BakeMaterialCursor;
        rr::StaticLightingChart chart;
        if (resources.Bake->GetChart(index, chart) != rhi::RasterStatus::Ready)
        {
            failBake();
            return;
        }
        rr::BakedLighting bake;
        bake.Texture = resources.BakeTextures[index];
        bake.Scale = chart.Scale;
        bake.UvTransform = chart.UvTransform;
        bake.SourceId = chart.SourceId;
        bake.SceneRevision = chart.SceneRevision;
        bake.DependencyRevision = chart.DependencyRevision;
        bake.Transform = chart.Transform;
        bake.AreTriangleChartsEnabled = true;
        bake.HasStaticAreaDirect = true;
        const rhi::RasterStatus status =
            resources.CreateLightingMaterial(index, &bake, true, resources.BakedMaterials[index]);
        if (Accepted(status) == false)
        {
            failBake();
            return;
        }
        ++resources.BakeMaterialCursor;
    }
    for (usize i = 0; i < count; ++i)
    {
        const rhi::RasterStatus status = resources.Renderer.GetStatus(resources.BakedMaterials[i]);
        if (status == rhi::RasterStatus::Pending)
        {
            return;
        }
        if (status != rhi::RasterStatus::Ready)
        {
            failBake();
            return;
        }
    }
    if (resources.HasBakeSnapshot == false)
    {
        StaticArray<rr::SceneItem, content::scene_format::MAX_OBJECTS> items{};
        for (usize i = 0; i < count; ++i)
        {
            const resources::SceneObject& source = resources.Source.Definition->Objects[i];
            items[i].Geometry = resources.GetMesh(source.Mesh);
            items[i].Transform = source.Transform;
            items[i].SourceId = source.SourceId;
            items[i].Surface = resources.BakedMaterials[i];
            items[i].Material.Color = {1, 1, 1, 1};
            items[i].BakeSceneRevision = resources.Lighting.SceneRevision;
            items[i].BakeDependencyRevision = resources.Lighting.DependencyRevision;
        }
        if (Accepted(resources.Renderer.CreateSnapshot(items.GetData(), count, resources.BakedSnapshot)) == false)
        {
            failBake();
            return;
        }
        resources.HasBakeSnapshot = true;
    }
    const rhi::RasterStatus updated = resources.Viewport.Update(resources.BakedSnapshot, Description_, Report_);
    if (updated == rhi::RasterStatus::NotReady || updated == rhi::RasterStatus::Pending)
    {
        return;
    }
    if (updated != rhi::RasterStatus::Ready)
    {
        failBake();
        return;
    }
    resources.PublishBakedSnapshot();
    markBakeChanged();
#endif
}

void RenderPreview::updateBakeNotice()
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    PreviewGpuGeneration& resources = *mResources;
    if (resources.HasBakeNotice == true || resources.BakeWorker.Get() == nullptr)
    {
        return;
    }
    rr::StaticLightingBakeDiagnostics diagnostics;
    if (resources.BakeWorker->GetDiagnostics(diagnostics) != rhi::RasterStatus::Ready)
    {
        return;
    }
    PreviewBakeNotice notice;
    notice.SkippedTriangleCount = diagnostics.SkippedTriangleCount;
    notice.AffectedObjectCount = diagnostics.AffectedObjectCount;
    QStringList names;
    for (int32 i = 0; i < resources.Source.Objects.size(); ++i)
    {
        uint32 skipped = 0;
        if (resources.BakeWorker->GetSkippedTriangleCount(static_cast<usize>(i), skipped) == rhi::RasterStatus::Ready &&
            skipped != 0)
        {
            const PreviewObject& object = resources.Source.Objects[i];
            names.append(QStringLiteral("%1 (%2)").arg(object.Name.isEmpty() ? object.Id : object.Name).arg(skipped));
        }
    }
    notice.AffectedObjects = names.join(QStringLiteral(", "));
    resources.BakeNotice = foundation::Move(notice);
    resources.HasBakeNotice = true;
    markBakeChanged();
#endif
}
void RenderPreview::markBakeChanged()
{
    FreshReport_ = true;
    Q_EMIT BakeChanged();
}
PreviewBakeState RenderPreview::GetBakeState() const noexcept
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mResources.Get() == nullptr || Scene_.Lit == false || mShading != PreviewShading::Lit)
    {
        return PreviewBakeState::Idle;
    }
    const PreviewGpuGeneration& resources = *mResources;
    if (resources.IsBakePublished == true)
    {
        return PreviewBakeState::Baked;
    }
    if (resources.IsBakeCancelled == true)
    {
        return PreviewBakeState::Cancelled;
    }
    if (resources.HasBakeFailed == true)
    {
        return PreviewBakeState::Failed;
    }
    return resources.BakeWorker.Get() != nullptr ? PreviewBakeState::Running : PreviewBakeState::Idle;
#else
    return PreviewBakeState::Idle;
#endif
}
bool RenderPreview::GetBakeNotice(PreviewBakeNotice& outNotice) const
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mResources.Get() == nullptr || mResources->HasBakeNotice == false ||
        mResources->BakeNotice.SkippedTriangleCount == 0)
    {
        return false;
    }
    outNotice = mResources->BakeNotice;
    return true;
#else
    static_cast<void>(outNotice);
    return false;
#endif
}
void RenderPreview::CancelBake() noexcept
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (GetBakeState() != PreviewBakeState::Running)
    {
        return;
    }
    // Ranges stop at their next sample boundary; Tick discards the result when Poll resolves.
    mResources->BakeWorker->RequestInterruption();
    mResources->IsBakeCancelled = true;
    markBakeChanged();
#endif
}
bool RenderPreview::RestartBake() noexcept
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (GetBakeState() != PreviewBakeState::Cancelled || mResources->BakeWorker.Get() != nullptr)
    {
        return false;
    }
    // Cancellation happens before any chart upload, so only the CPU bake restarts.
    PreviewGpuGeneration& resources = *mResources;
    resources.IsBakeCancelled = false;
    resources.HasBakeStarted = false;
    resources.HasBakeNotice = false;
    resources.BakeNotice = {};
    markBakeChanged();
    if (Running() == true)
    {
        Timer_.start();
    }
    return true;
#else
    return false;
#endif
}
rhi::RasterStatus RenderPreview::pollViewport()
{
    const rhi::RasterStatus status = mResources->Viewport.Poll();
    if (status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending ||
        status == rhi::RasterStatus::DeviceLost || mResources->HasBakeSnapshot == false ||
        mResources->IsBakePublished == true || mResources->HasBakeFailed == true)
    {
        return status;
    }
    // A delayed candidate failure must not replace the existing direct view.
    // RasterViewport has discarded the rejected pending view before returning.
    mResources->HasBakeFailed = true;
    FreshReport_ = true;
    return mResources->Viewport.Update(mResources->GetSnapshot(), Description_, Report_);
}

void RenderPreview::Tick()
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (OwnsDevice_ == false || mHasPreparationFailed == true)
    {
        return;
    }
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
        mIsAwaitingProducer = true;
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
        ++Frames_;
        const uint64 ownerFrame = Frames_;
        Q_EMIT FrameReady();
        // A receiver can stop/replace the preview at this transaction boundary.
        if (OwnsDevice_ == false || Frames_ != ownerFrame)
        {
            return;
        }
    }
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
            usesLitMaterials() == true ? rr::ViewportColorMode::PreferHdr : rr::ViewportColorMode::Linear8);
        if (Accepted(created) == false)
        {
            fail(QStringLiteral("GPU viewport allocation failed. World edits are preserved."));
            return;
        }
        mHasViewport = true;
        ViewDirty_ = true;
    }
    if (mHasPreparedView == true && Scene_.Lit == true && mShading == PreviewShading::Lit)
    {
        serviceStaticLighting();
    }
    if (prepare() == false)
    {
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
    const rhi::RasterStatus configured = pollViewport();
    if (configured == rhi::RasterStatus::Pending)
    {
        return;
    }
    if (configured != rhi::RasterStatus::Ready)
    {
        fail(QStringLiteral("GPU viewport setup failed. World edits are preserved."));
        return;
    }
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
    mIsAwaitingProducer = true;
    if (FreshReport_ == true || Frames_ % 60 == 0)
    {
        FreshReport_ = false;
        Status_ =
            QStringLiteral("GPU raster viewport · %1×%2 · visible %3 · draws %4%5")
                .arg(Width_)
                .arg(Height_)
                .arg(Report_.Visible)
                .arg(Report_.Draws)
                .arg(Scene_.Lit == true
                         ? (mShading == PreviewShading::Lit
                                ? QStringLiteral(" · Lit: %1 lights / %2 planar area proxies / %3 shadow faces · %4")
                                      .arg(mResources->Lighting.Lights.Count)
                                      .arg(mResources->Lighting.AreaCount)
                                      .arg(mResources->Lighting.ShadowCount)
                                      .arg((mResources->IsBakePublished == true
                                                ? QStringLiteral("static diffuse baked")
                                                : (mResources->IsBakeCancelled == true
                                                       ? QStringLiteral("bake cancelled; direct lighting retained")
                                                       : (mResources->HasBakeFailed == true
                                                              ? QStringLiteral("bake failed; direct lighting retained")
                                                              : QStringLiteral("direct lighting; bake %1%")
                                                                    .arg(mResources->BakeWorker.Get() != nullptr
                                                                             ? mResources->BakeWorker
                                                                                   ->GetProgressPercent()
                                                                             : 0)))) +
                                           (mResources->HasBakeNotice == true &&
                                                    mResources->BakeNotice.SkippedTriangleCount != 0
                                                ? QStringLiteral(" · skipped %1 zero-area triangles in %2 %3")
                                                      .arg(mResources->BakeNotice.SkippedTriangleCount)
                                                      .arg(mResources->BakeNotice.AffectedObjectCount)
                                                      .arg(mResources->BakeNotice.AffectedObjectCount == 1
                                                               ? QStringLiteral("object")
                                                               : QStringLiteral("objects"))
                                                : QString()))
                                : (mShading == PreviewShading::Solid
                                       ? QStringLiteral(" · Solid: neutral preview shading")
                                       : QStringLiteral(" · Albedo: unlit surface colors")))
                         : (mShading == PreviewShading::Solid ? QStringLiteral(" · Solid: neutral preview shading")
                                                              : QStringLiteral(" · Albedo: unlit surface colors")));
        Q_EMIT Changed();
    }
#endif
}
} // namespace ludus::editor
