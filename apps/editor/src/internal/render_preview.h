#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/viewport_frame.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/gpu_scope.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/native_interop.h>
#include <ludus/graphics/rhi/raster.h>
#include <ludus/platform/native_window.h>

#include "internal/browser_viewport_backend.h"
#include "internal/editor_camera_controller.h"
#include "internal/preview_shader_set.h"
#include "internal/preview_shading.h"
#include "internal/ray_path_overlay.h"
#include "internal/render_scene.h"
#include "internal/viewport_antialiasing.h"
#include "internal/viewport_frame_pump.h"
#include "internal/viewport_frame_source.h"
#include "internal/viewport_size.h"
#include "internal/viewport_statistics.h"
#include "internal/viewport_walk_collision.h"
#include "internal/world_surface_bake.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QSize>

namespace ludus::editor
{
class PreviewGpuGeneration;
// Main-thread renderer/device owner. Never destroys another preview's session.
// Its frames reach PreviewViewport through the ViewportFrameSource contract.
class RenderPreview final : public ViewportFrameSource
{
    Q_OBJECT
public:
    explicit RenderPreview(QObject* parent = nullptr);
    ~RenderPreview() override;
    // Reject malformed private snapshots before touching fixed GPU handle arrays.
    [[nodiscard]] bool SetScene(const RenderScene& scene);
    /// Update lighting inputs without changing the World extraction or its geometry resources.
    void SetProbeGrid(const graphics::renderer::GiProbeGrid& grid) noexcept;
    void SetSurfaceBakeWorldPath(const QString& path);
    [[nodiscard]] bool SaveSurfaceBake();
    [[nodiscard]] QSharedPointer<const WorldSurfaceBake> GetSurfaceBake() const;
    [[nodiscard]] QString GetSurfaceBakeFailure() const;
    [[nodiscard]] bool Start();
    [[nodiscard]] bool AdmitDevice(const graphics::rhi::NativeDeviceDescription& native);
    /// Start an engine-owned headless device for viewports that cannot share Qt's device.
    /// Frames are then copied to the CPU by the viewport (see OffscreenFrameReader).
    /// @return False when no preview start is pending or the device cannot start.
    [[nodiscard]] bool AdmitOffscreenDevice();
    /// One browser viewport startup attempt on a connected, separately owned canvas.
    struct BrowserDeviceRequest final
    {
        /// CSS selector of a canvas no graphics API has claimed yet; must outlive Stop.
        const char* CanvasSelectorOrNull = nullptr;
        BrowserViewportApi Api = BrowserViewportApi::WebGpu;
        /// Active-API note for the statistics overlay, for example why WebGL 2 is used.
        QString ApiNote;
    };
    /// Start an engine WebGPU or WebGL 2 device for the request's canvas. Pending startup is serviced by
    /// Tick. A startup failure, now or later, never shows an error: it stops the device, keeps the reason
    /// in GetBrowserStartupFailure and (when asynchronous) emits BrowserApiFailed, so the viewport can try
    /// another API or call ReportBrowserUnavailable. Either way the start stays pending (Running() remains
    /// true) until the viewport admits another API or reports; a synchronous failure returns false.
    /// Failures after the device reached Ready are reported as ordinary failures.
    [[nodiscard]] bool AdmitBrowserDevice(const BrowserDeviceRequest& request);
    /// Reason the last browser startup attempt failed, for example "no compatible GPU adapter".
    [[nodiscard]] LUDUS_INLINE const QString& GetBrowserStartupFailure() const noexcept
    {
        return mBrowserStartupFailure;
    }
    /// End a browser start that no API could satisfy: shows message and emits Failed. An operation waiting
    /// for the first frame, such as a World scene import, rolls back with message as its cause.
    void ReportBrowserUnavailable(const QString& message);
    void Stop() noexcept;
    /// Select a view; orbitDegrees contains yaw/pitch in degrees and zoom is the legacy radius scale.
    void Navigate(foundation::int32 camera,
                  foundation::math::Vector2 orbitDegrees,
                  foundation::float32 zoom,
                  bool isReference);
    void FrameAll() noexcept;
    /// True when the current scene carries a level start view (see PreviewStartView).
    [[nodiscard]] LUDUS_INLINE bool HasStartView() const noexcept
    {
        return Scene_.HasStartView;
    }
    /// Move the free editor camera to the scene's start view, detaching an authored camera like
    /// other navigation. The field of view is clamped to the editor's admitted range. Without a
    /// start view, or for a degenerate one, the camera is preserved and false is returned.
    /// A scene's first load already opens at its start view unless navigation was restored.
    bool GoToStartView();
    /// Describe the visible camera as a start view: its eye, a target at the focus distance along
    /// the view, its up direction and its vertical field of view. Orthographic views have no
    /// field of view and fail; failure preserves outView.
    [[nodiscard]] bool TryDescribeStartView(PreviewStartView& outView) const noexcept;
    void SetSelection(const QString& id);
    [[nodiscard]] bool FrameSelection() noexcept;
    /// Frame the visible mesh bounds of a World entity by stable ID.
    [[nodiscard]] bool FrameEntity(const QString& id) noexcept;
    /// Fit the given world-space bounds in the free editor camera.
    [[nodiscard]] bool FrameBounds(const foundation::math::Aabb3& boundsWorld) noexcept;
    /// Use a transient first-person camera while retaining the editor's navigation and scene.
    [[nodiscard]] bool BeginFirstPerson(const gameplay::camera::CameraSample& spawn, QString& outError);
    // NOLINTBEGIN(bugprone-easily-swappable-parameters): elapsed seconds then speed, as in Fly; the units differ.
    /// Move and apply gravity to the transient player. Controls use only X and Z.
    void AdvanceFirstPerson(foundation::math::Vector3 movementControls,
                            foundation::float32 deltaSeconds,
                            foundation::float32 speed,
                            bool shouldJump);
    // NOLINTEND(bugprone-easily-swappable-parameters)
    /// Restore the normal editor view; safe to call when no session is active.
    void EndFirstPerson();
    [[nodiscard]] LUDUS_INLINE bool IsExploringFirstPerson() const noexcept
    {
        return mIsExploringFirstPerson;
    }
    void Look(foundation::float32 deltaX, foundation::float32 deltaY);
    void Fly(foundation::math::Vector3 movementControls, foundation::float32 deltaSeconds, foundation::float32 speed);
    [[nodiscard]] foundation::float32 GetFocusDistance() const noexcept;
    /// Persist the local viewport renderer preference; emits RendererPreferenceChanged on change.
    void SetForceRaster(bool shouldForceRaster);
    [[nodiscard]] LUDUS_INLINE bool ShouldForceRaster() const noexcept
    {
        return mShouldForceRaster;
    }
    /// Spatial antialiasing of the displayed frame (FXAA in the output pass). Changing it rebuilds the GPU viewport
    /// on the next frame; every shading mode honours it. Persisted with the navigation state.
    void SetAntialiasing(graphics::renderer::SpatialAntialiasing antialiasing);
    [[nodiscard]] LUDUS_INLINE graphics::renderer::SpatialAntialiasing GetAntialiasing() const noexcept
    {
        return mAntialiasing;
    }
    void SetShading(PreviewShading shading);
    [[nodiscard]] LUDUS_INLINE PreviewShading GetShading() const noexcept
    {
        return mShading;
    }
    /// Show or hide the viewport statistics overlay. While visible, each submitted frame adds a
    /// CPU sample and the GPU timestamp probe is re-armed only after its previous sample was read
    /// (never waiting for the GPU). Showing restarts the rolling averages. Persisted by Navigation.
    void SetStatisticsVisible(bool isVisible);
    /// Recording demand is independent of Stats visibility; owner-thread, no GPU waits.
    [[nodiscard]] bool SetGpuProfilingEnabled(bool isEnabled) noexcept;
    [[nodiscard]] bool CanProfileGpu() const noexcept;
    [[nodiscard]] bool HasGpuProfilingPending(foundation::uint64 throughTicks = ~foundation::uint64{0}) const noexcept;
    /// Poll without arming work; used to finalize recordings even when the viewport is hidden.
    void PollGpuProfiling() noexcept;
    /// Shared sampler boundaries for the built-in and delegated graph producers.
    [[nodiscard]] bool CanProfileGpuPasses() const noexcept;
    [[nodiscard]] bool HasGpuFrameProfilingPending(foundation::uint64 throughTicks = ~foundation::uint64{
                                                       0}) const noexcept;
    [[nodiscard]] bool HasGpuPassProfilingPending(foundation::uint64 throughTicks = ~foundation::uint64{
                                                      0}) const noexcept;
    void PrepareGpuProfileFrame() noexcept;
    void NotifyGpuProfileSubmission() noexcept;
    /// Next successful submission identity shared by CPU viewport spans and GPU samples.
    [[nodiscard]] LUDUS_INLINE foundation::uint64 GetNextProfileFrameId() const noexcept
    {
        return mSubmittedFrameSequence + 1;
    }
    [[nodiscard]] LUDUS_INLINE bool IsStatisticsVisible() const noexcept
    {
        return mIsStatisticsVisible;
    }
    /// Smoothed measurements of the presented frames; meaningful while statistics are visible.
    [[nodiscard]] LUDUS_INLINE const ViewportStatistics& GetStatistics() const noexcept
    {
        return mStatistics;
    }
    /// Draw with shaders from the next generation on. The live generation keeps drawing until the
    /// replacement presents its first frame (the existing generation swap, so GPU work that still uses
    /// the old pipelines completes first). A set with a new revision that fails preparation is
    /// rejected: the last presented set is restored and ShadersRejected reports why.
    void SetShaders(const PreviewShaderSet& shaders);
    /// Most recently requested shader set (not necessarily presented yet).
    [[nodiscard]] LUDUS_INLINE const PreviewShaderSet& GetShaders() const noexcept
    {
        return mShaders;
    }
    void Resize(foundation::int32 width, foundation::int32 height) override;
    /// Track whether a viewport can display new frames.
    void SetPresentationVisible(bool isVisible) noexcept;
    [[nodiscard]] bool ShouldRenderPresentation() const noexcept;
    /// A scene import needs a candidate frame even while its viewport is hidden.
    void SetNeedsValidationFrame(bool isNeeded) noexcept;
    /// Pair each active capture on this device with a matching end call.
    void BeginGpuCapture() noexcept;
    void EndGpuCapture() noexcept;
    [[nodiscard]] bool HasActiveGpuCapture() const noexcept;
    [[nodiscard]] bool HasOpenViewportSubmission() const noexcept;
    /// Lowest admitted mouse rotation multiplier, relative to the default angular speed.
    static constexpr foundation::float32 MINIMUM_LOOK_SENSITIVITY = 0.1F;
    /// Highest admitted mouse rotation multiplier.
    static constexpr foundation::float32 MAXIMUM_LOOK_SENSITIVITY = 4;
    /// Set the multiplier for right-drag look and Alt/Option orbit. Invalid values preserve it.
    /// GUI thread only; changing sensitivity preserves the camera pose and selected camera.
    void SetLookSensitivity(foundation::float32 sensitivity);
    /// Current mouse rotation multiplier; independent of keyboard fly speed.
    [[nodiscard]] LUDUS_INLINE foundation::float32 GetLookSensitivity() const noexcept
    {
        return mLookSensitivity;
    }
    /// Lowest admitted keyboard fly speed multiplier, relative to the viewport's focus-distance speed.
    static constexpr foundation::float32 MINIMUM_FLY_SPEED = 0.01F;
    /// Highest admitted keyboard fly speed multiplier.
    static constexpr foundation::float32 MAXIMUM_FLY_SPEED = 100;
    /// Factor one wheel notch applies to the fly speed while flying. Three notches about double
    /// the speed, and a notch back undoes a notch forward.
    static constexpr foundation::float32 FLY_SPEED_NOTCH_FACTOR = 1.25F;
    /// Set the multiplier for WASD/QE flight while the right mouse button is held, relative to the
    /// viewport's focus-distance speed. Nonfinite or out-of-range values preserve it. Persisted by
    /// Navigation; GUI thread only; changing it preserves the camera pose and selected camera.
    void SetFlySpeed(foundation::float32 multiplier);
    /// Scale the fly speed by FLY_SPEED_NOTCH_FACTOR raised to notches (negative notches slow down),
    /// clamped to the admitted range. False, with the speed preserved, for zero or nonfinite notches
    /// and when the speed is already at the limit it was pushed towards.
    bool ScaleFlySpeed(foundation::float32 notches);
    /// Current fly speed multiplier; 1 by default. Independent of look sensitivity.
    [[nodiscard]] LUDUS_INLINE foundation::float32 GetFlySpeed() const noexcept
    {
        return mFlySpeed;
    }
    /// Adjust the native viewport's tone-mapped raster output, in stops. The value belongs to this view session.
    /// Invalid values preserve it. Zero resets the preview to the graph's authored exposure.
    void SetRasterExposureStops(foundation::float32 stops);
    [[nodiscard]] LUDUS_INLINE foundation::float32 GetRasterExposureStops() const noexcept
    {
        return mRasterExposureStops;
    }
    /// Select the viewport toolbar's size preset by index (below VIEWPORT_SIZE_PRESET_COUNT);
    /// other values preserve it. The viewport letterboxes its frame to the preset and requests
    /// the drawable from ComputeViewportDrawableSize. Persisted by Navigation; GUI thread only.
    void SetViewportSize(foundation::usize presetIndex);
    /// Current viewport size preset index; 0 (Fill) by default.
    [[nodiscard]] LUDUS_INLINE foundation::usize GetViewportSize() const noexcept
    {
        return mViewportSizeIndex;
    }
    /// Look along a world axis through an orthographic editor lens, keeping the pivot and the
    /// visible height there. Detaches an authored camera like other navigation; the eye backs
    /// off outside the scene bounds so nothing is clipped. Failure preserves the camera.
    void SetAxisView(EditorAxisView view);
    /// Switch the editor camera between perspective and orthographic projection, keeping its
    /// orientation, pivot and visible height. Failure, or no change, preserves the camera.
    void SetOrthographic(bool isOrthographic);
    /// Set the editor camera's perspective vertical field of view, in degrees between
    /// MINIMUM_EDITOR_VERTICAL_FOV_DEGREES and MAXIMUM_EDITOR_VERTICAL_FOV_DEGREES. Other
    /// values, or an unchanged angle, preserve the camera. Persisted by Navigation.
    void SetVerticalFovDegrees(foundation::float32 degrees);
    /// Camera the viewport shows: the first-person view, selected authored camera, or editor camera.
    [[nodiscard]] const gameplay::camera::CameraSample& GetVisibleCameraSample() const noexcept;
    void Orbit(foundation::float32 deltaX, foundation::float32 deltaY);
    void Pan(foundation::float32 deltaX, foundation::float32 deltaY, foundation::float32 heightLogical = 480);
    void Zoom(foundation::float32 steps);
    void SetFocusDistance(foundation::float32 distance);
    [[nodiscard]] QJsonObject Navigation() const;
    /// Exact world bounds of every object in Scene(), computed once when the scene is published.
    /// False when the scene has no finite geometry; outBoundsWorld is then unchanged.
    [[nodiscard]] bool TryGetSceneBounds(foundation::math::Aabb3& outBoundsWorld) const noexcept;
    /// Build the current camera or orbit view for an explicit physical size, without a device.
    /// Software viewports call this after NavigationChanged. Failure preserves outView.
    [[nodiscard]] bool DescribeView(foundation::uint32 width,
                                    foundation::uint32 height,
                                    graphics::renderer::ViewDescription& outView,
                                    QString& outError) const;
    /// DescribeView for the render-graph runtime profile, which admits only finite projections: the
    /// same camera, with an infinite perspective lens replaced by a finite one whose far plane
    /// encloses the scene's geometry, in the viewport's reverse-Z depth. RenderGraph keeps reverse-Z
    /// where its passes allow, so positions rebuilt from depth stay precise far from the camera.
    /// Needs no device. Failure (including a scene without geometry bounds) preserves outView.
    [[nodiscard]] bool DescribeGraphView(foundation::uint32 width,
                                         foundation::uint32 height,
                                         graphics::renderer::ViewDescription& outView,
                                         QString& outError) const;
    /// DescribeGraphView for consumers that admit only DepthConvention::Conventional (the reference
    /// tracer and raster captures compared with it): clip z is also rewritten as w - z.
    /// Needs no device. Failure (including a scene without geometry bounds) preserves outView.
    [[nodiscard]] bool DescribeConventionalView(foundation::uint32 width,
                                                foundation::uint32 height,
                                                graphics::renderer::ViewDescription& outView,
                                                QString& outError) const;
    /// Restore saved viewport preferences on the GUI thread. An empty state resets preferences
    /// without suppressing automatic framing of the first subsequently loaded scene.
    void RestoreNavigation(const QJsonObject& state);
    /// Delegate GPU frame production to the project graph; this owner keeps scene/navigation
    /// and device lifetime, without preparing or drawing a second built-in scene.
    void UseProjectGraph() noexcept;
    /// Set the current delegated renderer diagnostic on the GUI thread. A nonempty problem
    /// refuses Start and fails pending scene preparation without destroying the last frame
    /// or device. Clearing the problem permits an explicit retry.
    void SetProjectGraphProblem(const QString& problem);
    /// Publish completion of the current World extraction from the delegated graph producer.
    /// Ignored while inspecting ray paths, when this preview produces its own frames.
    void NotifyProjectFrame();
    /// Draw this viewport's built-in raster scene from the free editor camera even while
    /// UseProjectGraph delegates frame production, for ray path inspection; false returns frame
    /// production to the project graph. Raster navigation needs no ray hardware. The scene target
    /// becomes linear RGBA8 while inspecting, so the overlay can compose into it. GUI thread only.
    void SetRayPathInspection(bool isInspecting);
    [[nodiscard]] LUDUS_INLINE bool IsRayPathInspection() const noexcept
    {
        return mIsInspectingRayPaths;
    }
    /// Draw this viewport's built-in raster scene in shading even while UseProjectGraph delegates
    /// frame production, for the graph viewport's debug views (albedo, normals, depth, wireframe and
    /// the other material and lighting quantities). Repeated calls only change the shading. Until
    /// HideBuiltInView, Navigation keeps saving the shading from before the first call, so a debug
    /// view is not persisted. Without a project graph this preview always draws. GUI thread only.
    void ShowBuiltInView(PreviewShading shading);
    /// Return frame production to the project graph and restore the shading from before
    /// ShowBuiltInView; nothing happens while no built-in view is shown. GUI thread only.
    void HideBuiltInView();
    [[nodiscard]] LUDUS_INLINE bool IsBuiltInViewShown() const noexcept
    {
        return mIsBuiltInViewShown;
    }
    /// World-space lines composed into built-in frames while inspecting, depth-tested against
    /// that frame's scene depth unless overlay.IsXRay. The lines are projected with the current
    /// camera every time it changes; an empty overlay draws nothing. Native GPU preview only.
    void SetRayPathOverlay(const RayPathOverlay& overlay);
    /// Overlay lines composed into the latest submitted built-in frame; zero without an overlay.
    [[nodiscard]] foundation::usize GetPresentedRayPathLines() const noexcept;
    void Tick();
    [[nodiscard]] LUDUS_INLINE bool Running() const noexcept
    {
        return OwnsDevice_ || mIsStarting;
    }
    [[nodiscard]] LUDUS_INLINE foundation::uint64 Frames() const noexcept
    {
        return Frames_;
    }
    [[nodiscard]] LUDUS_INLINE QString Status() const
    {
        return Status_;
    }
    [[nodiscard]] LUDUS_INLINE const RenderScene& Scene() const noexcept
    {
        return Scene_;
    }
    [[nodiscard]] LUDUS_INLINE graphics::renderer::ViewDescription ViewDescription() const noexcept
    {
        return Description_;
    }
    [[nodiscard]] LUDUS_INLINE graphics::renderer::ViewReport Report() const noexcept
    {
        return Report_;
    }
    [[nodiscard]] LUDUS_INLINE bool FrameAvailable() const noexcept
    {
        return mHasFrame;
    }
    [[nodiscard]] platform::NativeWindowInfo WindowInfo() const noexcept;
    /// Drawable view size before the edit-preview rendering cap, in physical pixels.
    [[nodiscard]] QSize GetDrawableSize() const noexcept;
    [[nodiscard]] bool TakeFrame(graphics::renderer::ViewportFrame& outFrame,
                                 graphics::renderer::ViewportImage& outImage) noexcept override;
    void ReleaseFrame(graphics::renderer::ViewportFrame& inoutFrame) noexcept override;
    /// Borrowed handle of the owned device; null while no device is admitted.
    [[nodiscard]] LUDUS_INLINE graphics::rhi::DeviceHandle GetDevice() const noexcept
    {
        return Device_;
    }
    /// Whether a device is admitted and owned (not merely requested).
    [[nodiscard]] LUDUS_INLINE bool HasDevice() const noexcept
    {
        return OwnsDevice_;
    }
    /// Borrowed headless surface of the owned device; null while no device is admitted.
    [[nodiscard]] LUDUS_INLINE graphics::rhi::SurfaceHandle GetSurface() const noexcept
    {
        return Surface_;
    }
    /// Increments each time a device is admitted, so borrowers can rebind after a restart.
    [[nodiscard]] LUDUS_INLINE foundation::uint64 GetDeviceSerial() const noexcept
    {
        return mDeviceSerial;
    }
    [[nodiscard]] graphics::rhi::RasterStatus PinDevice(graphics::rhi::NativeDeviceLease& outLease) noexcept;
    [[nodiscard]] LUDUS_INLINE bool HasPendingFrame() const noexcept override
    {
        return mHasPendingFrame;
    }
Q_SIGNALS:
    /// Drawable reference changed, in physical pixels before the preview rendering cap.
    /// Emitted on the GUI thread even when no device or rendered frame is ready.
    void DrawableSizeChanged(const QSize& size);
    void Changed();
    void NavigationChanged();
    void RasterExposureChanged();
    void RenderAdmissionChanged();
    void RendererPreferenceChanged(bool shouldForceRaster);
    void DeviceRequested();
    /// Emitted before the owned device is destroyed; borrowers must release device resources.
    void DeviceStopping();
    /// A browser device failed before it reached Ready; the device is stopped, the start stays pending
    /// and no error is shown. The receiver must admit another API or call ReportBrowserUnavailable.
    void BrowserApiFailed(ludus::editor::BrowserViewportApi api, const QString& reason);
    void Stopped();
    void Failed(const QString& error);
    /// The built-in view shown over a project graph (ShowBuiltInView or ray path inspection) cannot be
    /// prepared. The device, the project graph and its last image stay alive; this preview draws nothing
    /// until the view is hidden or another shading is chosen. Unlike Failed, World edits are unaffected.
    void BuiltInViewFailed(const QString& error);
    /// Rate-limited notification that GetStatistics changed while the overlay is visible.
    void StatisticsChanged();
    void GpuProfileSubmission();
    /// Direct GUI-thread notification; batch is borrowed only for the signal delivery.
    void GpuPassProfileBatch(foundation::uint64 frameId,
                             foundation::uint64 deviceSession,
                             foundation::uint64 submissionTicks,
                             const graphics::rhi::GpuPassTimings& batch);
    void GpuProfileSample(foundation::uint64 frameId,
                          foundation::uint64 deviceSession,
                          foundation::uint64 submissionTicks,
                          foundation::uint64 durationNanoseconds,
                          bool isValid);
    /// A frame drawn with the shader set of this revision was presented for the first time.
    void ShadersApplied(foundation::uint64 revision);
    /// The shader set of this revision could not prepare GPU pipelines; the last presented set is
    /// drawn again. reason is the preparation diagnostic.
    void ShadersRejected(foundation::uint64 revision, const QString& reason);

private:
    [[nodiscard]] PreviewViewOptions makeViewOptions(foundation::uint32 width,
                                                     foundation::uint32 height) const noexcept;
    /// Shared body of DescribeGraphView and DescribeConventionalView; failure preserves outView.
    [[nodiscard]] bool describeFiniteView(foundation::uint32 width,
                                          foundation::uint32 height,
                                          bool shouldUseConventionalDepth,
                                          graphics::renderer::ViewDescription& outView,
                                          QString& outError) const;
    [[nodiscard]] bool beginNavigation();
    void navigationChanged();
    void invalidateView();
    EditorCameraController mCameraController;
    EditorCameraController mFirstPersonCamera;
    ViewportWalkCollision mFirstPersonCollision;
    bool mIsExploringFirstPerson = false;
    foundation::float32 mLookSensitivity = 1;
    foundation::float32 mFlySpeed = 1;
    foundation::float32 mRasterExposureStops = 0;
    /// Viewport size preset index; see SetViewportSize.
    foundation::usize mViewportSizeIndex = 0;
    QString mSelectedId;
    [[nodiscard]] LUDUS_INLINE bool usesLitMaterials() const noexcept
    {
        return (Scene_.Lit == true && mShading != PreviewShading::Albedo) || IsPreviewDebugShading(mShading) == true;
    }
    PreviewShading mShading = PreviewShading::Solid;
    bool mShouldForceRaster = false;
    graphics::renderer::SpatialAntialiasing mAntialiasing = DEFAULT_VIEWPORT_ANTIALIASING;
    /// The GPU viewport was built with an older antialiasing choice and is rebuilt on the next frame.
    bool mIsViewportStale = false;
    /// Read a completed GPU timing sample and re-arm the probe; call only between frames.
    void serviceGpuTiming(bool shouldArm) noexcept;
    bool mIsGpuProfilingEnabled = false;
    void serviceGpuPassTimings(bool shouldArm) noexcept;
    /// Fold one completed per-pass batch into the overlay's per-scope averages.
    void recordPassStatistics(const graphics::rhi::GpuPassTimings& batch);
    bool mIsPassTimingSupported = false;
    bool mIsPassTimingArmed = false;
    bool mHasPassTimingAssociation = false;
    foundation::uint64 mPassTimingFrameId = 0;
    foundation::uint64 mPassTimingSubmissionTicks = 0;
    foundation::uint64 mPassTimingDeviceSession = 0;
    foundation::uint64 mPassTimingCandidateTicks = 0;
    bool mIsTimingArmed = false;
    bool mHasTimingAssociation = false;
    foundation::uint64 mTimingFrameId = 0;
    foundation::uint64 mTimingSubmissionTicks = 0;
    foundation::uint64 mTimingDeviceSession = 0;
    foundation::uint64 mTimingCandidateTicks = 0;
    foundation::uint64 mSubmittedFrameSequence = 0;
    /// Record the CPU time of a submitted frame and publish the overlay at its refresh rate.
    void recordFrameStatistics(const QElapsedTimer& frameTimer);
    void resetStatistics() noexcept;
    bool mIsStatisticsVisible = false;
    ViewportStatistics mStatistics;
    bool mIsProjectGraphEnabled = false;
    QString mProjectGraphProblem;
    QElapsedTimer mStatisticsRefresh;
    foundation::UniquePtr<PreviewGpuGeneration> mResources;
    foundation::UniquePtr<PreviewGpuGeneration> mRetiredResources;
    bool mNeedsResourceRestart = false;
    bool mHasPreparationFailed = false;
    [[nodiscard]] bool beginResourceRestart() noexcept;
    /// Restore the last presented shader set after a generation built from mShaders failed.
    /// Returns false when that generation already drew with the last presented set.
    [[nodiscard]] bool rejectShaders();
    PreviewShaderSet mShaders;
    /// Shader set of the most recently presented frame; the fallback for a rejected set.
    PreviewShaderSet mPresentedShaders;
    struct Residency;
    foundation::UniquePtr<Residency> Residency_;
    graphics::renderer::ViewportFrame mPendingFrame;
    graphics::renderer::ViewportImage mPendingImage;
    graphics::rhi::SubmissionToken mProducer;
    bool mHasPendingFrame = false;
    bool mHasFrame = false;
    bool mIsPresentationVisible = true;
    bool mIsValidationFrameNeeded = false;
    bool mIsRasterSubmissionOpen = false;
    foundation::usize mActiveGpuCaptures = 0;
    bool mHasViewport = false;
    bool mHasPreparedView = false;
    bool mIsAwaitingProducer = false;
    [[nodiscard]] bool prepare();
    void restartResources() noexcept;
    bool mIsInspectingRayPaths = false;
    bool mIsBuiltInViewShown = false;
    /// Shading restored by HideBuiltInView and saved by Navigation while a built-in view is shown.
    PreviewShading mShadingBeforeBuiltInView = PreviewShading::Solid;
    /// Whether the project graph, not this preview, produces the presented frames.
    [[nodiscard]] LUDUS_INLINE bool isDelegatingToProjectGraph() const noexcept
    {
        return mIsProjectGraphEnabled == true && mIsInspectingRayPaths == false && mIsBuiltInViewShown == false;
    }
    RayPathOverlay mRayPathOverlay;
    foundation::uint64 mRayPathOverlayRevision = 0;
    /// Overlay renderer, list and prepared versions on the owned device; null until first used.
    struct RayPathOverlayResources;
    foundation::UniquePtr<RayPathOverlayResources> mRayPathOverlayResources;
    /// Prepare and attach the overlay version for the current camera before a built-in frame.
    void serviceRayPathOverlay() noexcept;
    /// Detach and release every overlay version; resetRenderer also drops the renderer before device loss.
    void releaseRayPathOverlay(bool shouldResetRenderer) noexcept;
    RenderScene Scene_;
    WorldSurfaceBakeState mSurfaceBake;
    /// TryGetBounds({}) of Scene_, replaced with it in SetScene.
    foundation::math::Aabb3 mSceneBoundsWorld{};
    bool mHasSceneBounds = false;
    static constexpr foundation::int32 DEFAULT_DRAWABLE_WIDTH = 640;
    static constexpr foundation::int32 DEFAULT_DRAWABLE_HEIGHT = 480;
    QSize mDrawableSize{DEFAULT_DRAWABLE_WIDTH, DEFAULT_DRAWABLE_HEIGHT};
    foundation::uint32 RequestedWidth_ = 640, RequestedHeight_ = 480;
    bool RestoredOrbit_ = false;
    QString CameraId_;
    void resetFrames() noexcept;
    void fail(const QString& message);
    /// Generation-scoped failure: over a project graph only the built-in view stops (BuiltInViewFailed);
    /// otherwise this is fail().
    void failGeneration(const QString& message);
    /// Forget a built-in view failure once frame production returns to the project graph.
    void clearBuiltInViewFailure() noexcept;

    graphics::rhi::DeviceHandle Device_{};
    graphics::rhi::SurfaceHandle Surface_{};
    graphics::renderer::ViewReport Report_{};
    graphics::renderer::ViewDescription Description_{};
    foundation::math::Vector3 OrbitCenter_{};
    foundation::float32 OrbitRadius_ = 1;
    foundation::uint32 Width_ = 0, Height_ = 0;
    foundation::uint64 Frames_ = 0;
    foundation::int32 Camera_ = -1;
    foundation::float32 Yaw_ = 25, Pitch_ = 20, Zoom_ = 3;
    bool Reference_ = false;
    bool ViewDirty_ = true;
    bool FreshReport_ = true;
    bool OwnsDevice_ = false;
    bool mIsStarting = false;
    foundation::uint64 mDeviceSerial = 0;
    ViewportFramePump mFramePump;
    QString Status_;
    /// Browser viewport API of the current device and whether it has reached Ready at least once.
    BrowserViewportApi mBrowserApi = BrowserViewportApi::WebGpu;
    bool mHasBrowserDeviceStarted = false;
    QString mBrowserApiNote;
    QString mBrowserStartupFailure;
    /// Reason recorded by the preparation stage that rejected the current generation, if any.
    QString mPreparationFailure;
};
} // namespace ludus::editor
