#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <ludus/content/scene_format/luscn.h>

#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/raster.h>

#include "internal/viewport_tone_curve.h"
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/atmosphere.hpp>
#include <ludus/graphics/renderer/environment.hpp>
#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/renderer/reference.hpp>
#include <ludus/graphics/renderer/renderer.hpp>

#include "internal/render_scene.h"

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>
#if !defined(Q_OS_WASM)
#    include <QFuture>
#endif

namespace ludus::editor
{
class PreviewGpuGeneration;

/// Smallest admitted long side of a reference image, in physical pixels.
inline constexpr foundation::uint32 MINIMUM_REFERENCE_LONG_SIDE = 16;
/// Largest admitted long side; bounds the retained per-pixel accumulation to tens of megabytes.
inline constexpr foundation::uint32 MAXIMUM_REFERENCE_LONG_SIDE = 2048;
/// Largest admitted samples per pixel; the renderer's own limit is far higher.
inline constexpr foundation::uint32 MAXIMUM_REFERENCE_SAMPLES = 65536;
/// Largest scattering depth RT1 admits.
inline constexpr foundation::uint32 MAXIMUM_REFERENCE_DEPTH = 64;
inline constexpr foundation::uint64 REFERENCE_GIBIBYTE = foundation::uint64{1024} * 1024 * 1024;
/// CPU ceiling for retained mesh data, the expanded scene/BVH and peak fingerprint or upload scratch.
/// A full imported Sponza scene needs both the expanded scene and its packed copy during GPU setup.
inline constexpr foundation::usize REFERENCE_CPU_BYTE_BUDGET = static_cast<foundation::usize>(
    sizeof(foundation::usize) > sizeof(foundation::uint32) ? 5 * REFERENCE_GIBIBYTE : REFERENCE_GIBIBYTE);
/// Device ray buffer, acceleration structure and build scratch bytes for native traversal.
inline constexpr foundation::usize REFERENCE_RAY_BYTE_BUDGET = foundation::usize{512} * 1024 * 1024;
/// Packed scene, texel and accumulation buffer bytes for GPU-compute traversal; the device's
/// own resource budget can reject a capture sooner.
inline constexpr foundation::usize REFERENCE_GPU_BYTE_BUDGET = static_cast<foundation::usize>(
    sizeof(foundation::usize) > sizeof(foundation::uint32) ? 3 * REFERENCE_GIBIBYTE : REFERENCE_GIBIBYTE / 2);
/// Pixels one CPU-oracle tile advances; small enough that one tile stays well under a frame.
inline constexpr foundation::uint32 REFERENCE_CPU_TILE_PIXELS = 256;
/// Largest tile RT1 admits for either traversal.
inline constexpr foundation::uint32 MAXIMUM_REFERENCE_TILE_PIXELS = 2048;

/// Default long side of a reference image, in physical pixels.
inline constexpr foundation::uint32 DEFAULT_REFERENCE_LONG_SIDE = 512;
/// Default samples per pixel; enough to judge shading, not to remove noise.
inline constexpr foundation::uint32 DEFAULT_REFERENCE_SAMPLES = 64;
/// Default scattering depth; captures the dominant interreflection of indoor scenes.
inline constexpr foundation::uint32 DEFAULT_REFERENCE_DEPTH = 8;
/// Width of the World Atmosphere's sky image; about one sky-table texel (0.7 degrees) per texel.
inline constexpr foundation::uint32 REFERENCE_ATMOSPHERE_WIDTH = 512;
/// Height of the World Atmosphere's sky image, half its width as the latitude/longitude layout requires.
inline constexpr foundation::uint32 REFERENCE_ATMOSPHERE_HEIGHT = REFERENCE_ATMOSPHERE_WIDTH / 2;

/// Analytic lights supplied to one capture; RT1 admits at most the scene format's light count.
using ReferenceLightArray = foundation::StaticArray<graphics::renderer::DirectLight, content::scene_format::MAX_LIGHTS>;

/// User choices for one capture of the current viewport camera.
struct ViewportReferenceSettings final
{
    /// Long side of the image in physical pixels; the short side follows the viewport aspect.
    foundation::uint32 LongSide = DEFAULT_REFERENCE_LONG_SIDE;
    /// Committed samples per pixel at completion.
    foundation::uint32 Samples = DEFAULT_REFERENCE_SAMPLES;
    /// Maximum scattering events; transport after the last one is truncated.
    foundation::uint32 MaxDepth = DEFAULT_REFERENCE_DEPTH;
    /// Explicit traversal; NativeQueries and GpuCompute never fall back to the CPU oracle.
    graphics::renderer::ReferenceTraversal Traversal = graphics::renderer::ReferenceTraversal::CpuOracle;
    /// Addressed Philox seed; equal inputs and seed reproduce the image.
    foundation::uint64 Seed = 1;
    /// Trace every camera ray through its pixel's centre instead of a jittered point in the pixel, as a render
    /// graph's camera rays do. The image then estimates centre rays rather than pixel footprints, so edges alias
    /// as the graph's do and a comparison measures shading alone. Recorded in the LUHDR01 flags.
    bool ShouldCenterRays = false;
    /// Display transform of CopyDisplayImage and Save's PNG, so the reference looks like the viewport output it is
    /// compared with; ComputeReferenceDisplayTransform finds it. The linear means and the LUHDR01 file never use
    /// it. A non-finite or nonpositive exposure, or an unknown curve, rejects Start.
    ViewportDisplayTransform Display{};
    /// Optional row-major fixed primary rays. Start copies these before asynchronous preparation.
    foundation::BorrowedView<const graphics::renderer::ReferenceProbeRay> ProbeRays = {nullptr, 0};
};

/// Progress of one ViewportReferenceCapture.
enum class ViewportReferencePhase : foundation::uint8
{
    /// Nothing started, or the capture was released.
    Idle,
    /// Building the reference renderer, meshes, textures and materials.
    Preparing,
    /// Progressive samples are accumulating.
    Tracing,
    /// Every pixel reached the requested sample count.
    Complete,
    /// Preparation or tracing failed; GetError explains why. Partial pixels stay readable.
    Failed,
};

/// Image extent with the long side fixed and the short side following the viewport aspect.
/// Returns an empty size when drawable or longSide is not positive.
[[nodiscard]] QSize ComputeReferenceExtent(const QSize& drawable, foundation::uint32 longSide) noexcept;

/// Encode one scene-linear colour the way a viewport output displays it: times the transform's exposure, its
/// tone curve, saturate, then sRGB. The default transform is the raster viewport's own. Returns display values in
/// [0, 1]; non-finite or negative radiance encodes as zero, and so does every colour for a non-finite or
/// nonpositive exposure.
[[nodiscard]] foundation::math::Vector3
EncodeReferenceDisplayColor(foundation::math::Vector3 radiance, const ViewportDisplayTransform& display = {}) noexcept;
/// The display transform a clustered deferred ToneMap applies to view, as RenderGraph does. Its exposure is the
/// pass's authored multiplier, or ComputeExposureMultiplier(view.ExposureValue100) for ExposureMode::Physical,
/// times 2^view.RasterExposureStops; its curve is the pass's. A reference encoded with it looks like that output.
/// @param graphToneMappingExposure The authored ToneMap multiplier of the pass drawing the viewport output
///        (RenderGraphPreview::GetRasterToneMappingExposure). Zero, or any nonpositive or non-finite value, means
///        no ToneMap draws it, so the view's exposure fields do not apply: the result is the default transform.
/// @param graphCurve The curve of that ToneMap.
/// @return The default transform for an output without a ToneMap, and for view exposure fields RenderGraph
///         rejects; otherwise a finite positive exposure and graphCurve.
[[nodiscard]] ViewportDisplayTransform
ComputeReferenceDisplayTransform(const graphics::renderer::ViewDescription& view,
                                 foundation::float32 graphToneMappingExposure,
                                 graphics::renderer::ToneCurve graphCurve) noexcept;
/// One channel of EncodeReferenceDisplayColor for a grey radiance, where all three channels are equal.
[[nodiscard]] foundation::float32 EncodeReferenceDisplayValue(foundation::float32 greyRadiance) noexcept;
/// EncodeReferenceDisplayValue rounded to eight bits.
[[nodiscard]] foundation::uint8 EncodeReferenceDisplayChannel(foundation::float32 greyRadiance) noexcept;

/// Copy the analytic lights of a raster light selection into outLights and return their count.
/// The area proxies the raster viewport derives from emissive surfaces are omitted: RT1 samples
/// those surfaces' triangles itself, so passing the proxies too would count their light twice.
[[nodiscard]] foundation::usize CollectReferenceLights(const graphics::renderer::LightSelection& selection,
                                                       ReferenceLightArray& outLights) noexcept;

/// The World Atmosphere's distant lighting for one capture, resolved by ResolveViewportAtmosphereSky.
struct ViewportAtmosphereSky final
{
    /// Ready when Pixels and SunIlluminance hold the sky; NotLit when the Atmosphere adds nothing.
    graphics::renderer::AtmosphereEnvironmentStatus Status = graphics::renderer::AtmosphereEnvironmentStatus::NotLit;
    /// REFERENCE_ATMOSPHERE_WIDTH x REFERENCE_ATMOSPHERE_HEIGHT radiance texels when Ready.
    foundation::Array<foundation::math::Vector4> Pixels;
    /// The linked Sun's illuminance after atmospheric transmittance when Ready.
    foundation::math::Vector3 SunIlluminance{};
};

/// Resolve an Atmosphere's sky at eyeWorld with graphics::renderer::ResolveAtmosphereEnvironment. Thread-safe
/// and self-contained; a capture runs it off the GUI thread because it takes tens of milliseconds (seconds in
/// debug builds).
[[nodiscard]] ViewportAtmosphereSky
ResolveViewportAtmosphereSky(const graphics::renderer::AtmosphereDescription& atmosphere,
                             foundation::math::Vector3 eyeWorld) noexcept;

/// RT1 reference render of a frozen viewport scene and camera.
///
/// The capture owns a separate renderer on the viewport's device, prepared from the same
/// RenderScene and material inputs as Lit shading, so later comparisons see one scene. It
/// supplies the scene's analytic lights and environment; emissive surfaces are sampled as RT1
/// emitters, never through the raster viewport's area-light proxies. When the World's
/// Atmosphere lights the raster scene at the camera, its sky replaces the imported environment
/// and the linked Sun carries the atmosphere's transmittance (see
/// graphics::renderer::ResolveAtmosphereEnvironment). Primary misses also show the frozen
/// celestial boundary inside the shell. The World's absorbing medium (RenderScene::Medium) is transported
/// through the CPU oracle and GPU compute; Start refuses native ray queries for a scene with a medium.
/// Owner thread only, between frames; the borrowed device must outlive the capture or Release must run first.
class ViewportReferenceCapture final
{
public:
    ViewportReferenceCapture() noexcept;
    ~ViewportReferenceCapture() noexcept;
    ViewportReferenceCapture(const ViewportReferenceCapture&) = delete;
    ViewportReferenceCapture& operator=(const ViewportReferenceCapture&) = delete;

    /// Freeze scene and camera and begin preparation. view must describe width x height with the
    /// viewport's camera. Any earlier capture is released first. Failure leaves the capture Failed
    /// (or Idle for invalid arguments) and sets outError.
    [[nodiscard]] bool Start(graphics::rhi::DeviceHandle device,
                             graphics::rhi::SurfaceHandle surface,
                             const RenderScene& scene,
                             const graphics::renderer::ViewDescription& view,
                             const QSize& extent,
                             const ViewportReferenceSettings& settings,
                             QString& outError);
    /// Advance preparation or tracing until budgetNanoseconds elapse or no immediate work remains.
    /// Never waits for the GPU. Returns the resulting phase.
    ViewportReferencePhase Service(foundation::uint64 budgetNanoseconds) noexcept;
    /// Cancel outstanding work and release the renderer and its device resources. Idempotent.
    void Release() noexcept;

    [[nodiscard]] LUDUS_INLINE ViewportReferencePhase GetPhase() const noexcept
    {
        return mPhase;
    }
    [[nodiscard]] LUDUS_INLINE const QString& GetError() const noexcept
    {
        return mError;
    }
    [[nodiscard]] LUDUS_INLINE QSize GetExtent() const noexcept
    {
        return mExtent;
    }
    /// Analytic lights supplied to the capture; area proxies of emissive surfaces are excluded.
    [[nodiscard]] LUDUS_INLINE foundation::usize GetLightCount() const noexcept
    {
        return mLightCount;
    }
    /// True when the World Atmosphere's sky lights this capture in place of the imported environment.
    [[nodiscard]] LUDUS_INLINE bool HasAtmosphereSky() const noexcept
    {
        return mHasAtmosphereSky;
    }
    /// True when the last successfully started capture traces pixel centres
    /// (ViewportReferenceSettings::ShouldCenterRays); false before any capture starts.
    [[nodiscard]] LUDUS_INLINE bool AreRaysCentered() const noexcept
    {
        return mDescription.CenterRays;
    }
    /// ViewportReferenceSettings::Display of the last successfully started capture; the default before any.
    [[nodiscard]] LUDUS_INLINE const ViewportDisplayTransform& GetDisplayTransform() const noexcept
    {
        return mDisplay;
    }
    /// Copy progress and profile evidence; false before tracing has begun.
    [[nodiscard]] bool GetInfo(graphics::renderer::ReferenceCaptureInfo& outInfo) const noexcept;
    /// Copy the current means and display-encode them with EncodeReferenceDisplayColor and GetDisplayTransform.
    /// Pixels without committed samples are black. False before tracing has begun.
    [[nodiscard]] bool CopyDisplayImage(QImage& outImage, QString& outError) const;
    /// Copy the current raw linear means and committed counts, top-left row order.
    /// False before tracing has begun; outPixels is unchanged on failure.
    [[nodiscard]] bool CopyPixels(foundation::Array<graphics::renderer::ReferencePixel>& outPixels,
                                  QString& outError) const;
    /// Encode the current LUHDR01 capture, including partial progress with exact counts.
    [[nodiscard]] bool EncodeHdr(QByteArray& outBytes, QString& outError) const;
    /// Atomically write the LUHDR01 capture to hdrPath and its display image beside it as PNG.
    [[nodiscard]] bool Save(const QString& hdrPath, QString& outError) const;
    /// PNG path written beside a LUHDR01 path by Save.
    [[nodiscard]] static QString GetDisplayImagePath(const QString& hdrPath);

private:
    [[nodiscard]] bool fail(const QString& message) noexcept;
    [[nodiscard]] bool beginCapture() noexcept;
    /// Collect the resolved Atmosphere sky and upload it once. Ready when absent or uploaded, Pending while
    /// resolving or uploading. Any other status has already failed the capture with an explanation.
    [[nodiscard]] graphics::rhi::RasterStatus prepareAtmosphereSky() noexcept;
    foundation::UniquePtr<PreviewGpuGeneration> mGeneration;
    graphics::rhi::SurfaceHandle mSurface{};
    graphics::renderer::ReferenceCaptureDescription mDescription{};
    foundation::Array<graphics::renderer::ReferenceProbeRay> mProbeRays;
    graphics::renderer::ReferenceCapture mCapture{};
    /// The frozen scene's absorbing medium, borrowing the generation's boundary corners.
    graphics::renderer::ReferenceMediumDescription mMedium{};
    ReferenceLightArray mLights{};
#if !defined(Q_OS_WASM)
    /// Background resolution of the Atmosphere sky. Dropping it lets the job finish on its own copies.
    QFuture<ViewportAtmosphereSky> mAtmosphereJob;
#endif
    /// Resolved Atmosphere sky; its pixels are released once uploaded.
    ViewportAtmosphereSky mAtmosphereSky;
    graphics::renderer::EnvironmentMap mAtmosphereEnvironment;
    /// Frozen sky and linked Sun identity for this capture.
    graphics::renderer::AtmosphereDescription mAtmosphere;
    bool mIsAtmosphereResolving = false;
    bool mHasAtmosphereSky = false;
    bool mIsAtmosphereUploadStarted = false;
    foundation::usize mLightCount = 0;
    ViewportDisplayTransform mDisplay{};
    QSize mExtent;
    QString mError;
    ViewportReferencePhase mPhase = ViewportReferencePhase::Idle;
    bool mHasCapture = false;
};
} // namespace ludus::editor
