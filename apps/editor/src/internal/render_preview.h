#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/viewport.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/native_interop.h>
#include <ludus/graphics/rhi/raster.h>
#include <ludus/platform/native_window.h>

#include "internal/editor_camera_controller.h"
#include "internal/preview_shading.h"
#include "internal/render_scene.h"

#include <QJsonObject>
#include <QObject>
#include <QTimer>

namespace ludus::editor
{
class PreviewGpuGeneration;
// Main-thread renderer/device owner. Never destroys another preview's session.
class RenderPreview final : public QObject
{
    Q_OBJECT
public:
    explicit RenderPreview(QObject* parent = nullptr);
    ~RenderPreview() override;
    // Reject malformed private snapshots before touching fixed GPU handle arrays.
    [[nodiscard]] bool SetScene(const RenderScene& scene);
    [[nodiscard]] bool Start();
    [[nodiscard]] bool AdmitDevice(const graphics::rhi::NativeDeviceDescription& native);
    /// Start an engine-owned headless device for viewports that cannot share Qt's device.
    /// Frames are then copied to the CPU by the viewport (see OffscreenFrameReader).
    /// @return False when no preview start is pending or the device cannot start.
    [[nodiscard]] bool AdmitOffscreenDevice();
    /// Start an engine WebGPU device for a connected, separately owned browser canvas.
    /// The selector and DOM canvas must outlive Stop; Pending startup is serviced by Tick.
    [[nodiscard]] bool AdmitBrowserDevice(const char* canvasSelector);
    void Stop() noexcept;
    /// Select a view; orbitDegrees contains yaw/pitch in degrees and zoom is the legacy radius scale.
    void Navigate(foundation::int32 camera,
                  foundation::math::Vector2 orbitDegrees,
                  foundation::float32 zoom,
                  bool isReference);
    void FrameAll() noexcept;
    void SetSelection(const QString& id);
    [[nodiscard]] bool FrameSelection() noexcept;
    void Look(foundation::float32 deltaX, foundation::float32 deltaY);
    void Fly(foundation::math::Vector3 movementControls, foundation::float32 deltaSeconds, foundation::float32 speed);
    [[nodiscard]] foundation::float32 GetFocusDistance() const noexcept;
    void SetShading(PreviewShading shading);
    [[nodiscard]] LUDUS_INLINE PreviewShading GetShading() const noexcept
    {
        return mShading;
    }
    void Resize(foundation::int32 width, foundation::int32 height);
    /// Suppress repeated GPU submissions for hidden browser surfaces; allow the first
    /// candidate frame so import validation can complete while its tab is hidden.
    void SetPresentationVisible(bool isVisible) noexcept;
    void Orbit(foundation::float32 deltaX, foundation::float32 deltaY);
    void Pan(foundation::float32 deltaX, foundation::float32 deltaY, foundation::float32 heightLogical = 480);
    void Zoom(foundation::float32 steps);
    void SetFocusDistance(foundation::float32 distance);
    [[nodiscard]] QJsonObject Navigation() const;
    /// Build the current camera or orbit view for an explicit physical size, without a device.
    /// Software viewports call this after NavigationChanged. Failure preserves outView.
    [[nodiscard]] bool DescribeView(foundation::uint32 width,
                                    foundation::uint32 height,
                                    graphics::renderer::ViewDescription& outView,
                                    QString& outError) const;
    /// DescribeView for consumers that admit only DepthConvention::Conventional (the render-graph
    /// runtime profile): the same camera, with an infinite perspective lens replaced by a finite
    /// one whose far plane encloses the scene's geometry, and clip z rewritten as w - z.
    /// Needs no device. Failure (including a scene without geometry bounds) preserves outView.
    [[nodiscard]] bool DescribeConventionalView(foundation::uint32 width,
                                                foundation::uint32 height,
                                                graphics::renderer::ViewDescription& outView,
                                                QString& outError) const;
    void RestoreNavigation(const QJsonObject& state);
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
    [[nodiscard]] bool TakeFrame(graphics::renderer::ViewportFrame& outFrame,
                                 graphics::renderer::ViewportImage& outImage) noexcept;
    void ReleaseFrame(graphics::renderer::ViewportFrame& inoutFrame) noexcept;
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
    [[nodiscard]] LUDUS_INLINE bool HasPendingFrame() const noexcept
    {
        return mHasPendingFrame;
    }
Q_SIGNALS:
    void Changed();
    void NavigationChanged();
    void FrameReady();
    void DeviceRequested();
    void ResettingFrames();
    /// Emitted before the owned device is destroyed; borrowers must release device resources.
    void DeviceStopping();
    void Stopped();
    void Failed(const QString& error);

private:
    [[nodiscard]] PreviewViewOptions makeViewOptions(foundation::uint32 width,
                                                     foundation::uint32 height) const noexcept;
    [[nodiscard]] bool beginNavigation();
    void navigationChanged();
    EditorCameraController mCameraController;
    QString mSelectedId;
    [[nodiscard]] LUDUS_INLINE bool usesLitMaterials() const noexcept
    {
        return Scene_.Lit == true && mShading != PreviewShading::Albedo;
    }
    PreviewShading mShading = PreviewShading::Solid;
    foundation::UniquePtr<PreviewGpuGeneration> mResources;
    foundation::UniquePtr<PreviewGpuGeneration> mRetiredResources;
    bool mNeedsResourceRestart = false;
    bool mHasPreparationFailed = false;
    [[nodiscard]] bool beginResourceRestart() noexcept;
    struct Residency;
    foundation::UniquePtr<Residency> Residency_;
    graphics::renderer::ViewportFrame mPendingFrame;
    graphics::renderer::ViewportImage mPendingImage;
    graphics::rhi::SubmissionToken mProducer;
    bool mHasPendingFrame = false;
    bool mHasFrame = false;
    bool mIsPresentationVisible = true;
    bool mHasViewport = false;
    bool mHasPreparedView = false;
    bool mIsAwaitingProducer = false;
    [[nodiscard]] bool prepare();
    void restartResources() noexcept;
    RenderScene Scene_;
    foundation::uint32 RequestedWidth_ = 640, RequestedHeight_ = 480;
    bool RestoredOrbit_ = false;
    QString CameraId_;
    void resetFrames() noexcept;
    void fail(const QString& message);

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
    QTimer Timer_;
    QString Status_;
    /// Reason recorded by the preparation stage that rejected the current generation, if any.
    QString mPreparationFailure;
};
} // namespace ludus::editor
