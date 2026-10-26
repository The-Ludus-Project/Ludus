#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <ludus/content/scene_format/luscn.h>

#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/raster.h>

#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/renderer/reference.hpp>
#include <ludus/graphics/renderer/renderer.hpp>

#include "internal/render_scene.h"

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>

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
/// Renderer-owned CPU bytes for retained geometry, textures and accumulation of one capture.
inline constexpr foundation::usize REFERENCE_CPU_BYTE_BUDGET = foundation::usize{1024} * 1024 * 1024;
/// Device ray buffer, acceleration structure and build scratch bytes for native traversal.
inline constexpr foundation::usize REFERENCE_RAY_BYTE_BUDGET = foundation::usize{512} * 1024 * 1024;
/// Packed scene, texel and accumulation buffer bytes for GPU-compute traversal; the device's
/// own resource budget can reject a capture sooner.
inline constexpr foundation::usize REFERENCE_GPU_BYTE_BUDGET = foundation::usize{512} * 1024 * 1024;
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

/// Encode one scene-linear radiance the way the raster viewport displays it: exposure one,
/// per-channel Reinhard, then sRGB. Returns a display value in [0, 1]; non-finite or negative
/// radiance encodes as zero.
[[nodiscard]] foundation::float32 EncodeReferenceDisplayValue(foundation::float32 radiance) noexcept;
/// EncodeReferenceDisplayValue rounded to eight bits.
[[nodiscard]] foundation::uint8 EncodeReferenceDisplayChannel(foundation::float32 radiance) noexcept;

/// Copy the analytic lights of a raster light selection into outLights and return their count.
/// The area proxies the raster viewport derives from emissive surfaces are omitted: RT1 samples
/// those surfaces' triangles itself, so passing the proxies too would count their light twice.
[[nodiscard]] foundation::usize CollectReferenceLights(const graphics::renderer::LightSelection& selection,
                                                       ReferenceLightArray& outLights) noexcept;

/// RT1 reference render of a frozen viewport scene and camera.
///
/// The capture owns a separate renderer on the viewport's device, prepared from the same
/// RenderScene and material inputs as Lit shading, so later comparisons see one scene. It
/// supplies the scene's analytic lights and environment; emissive surfaces are sampled as RT1
/// emitters, never through the raster viewport's area-light proxies. Owner thread only, between
/// frames; the borrowed device must outlive the capture or Release must run first.
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
    /// Copy progress and profile evidence; false before tracing has begun.
    [[nodiscard]] bool GetInfo(graphics::renderer::ReferenceCaptureInfo& outInfo) const noexcept;
    /// Copy the current means and display-encode them as EncodeReferenceDisplayChannel does.
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
    foundation::UniquePtr<PreviewGpuGeneration> mGeneration;
    graphics::rhi::SurfaceHandle mSurface{};
    graphics::renderer::ReferenceCaptureDescription mDescription{};
    graphics::renderer::ReferenceCapture mCapture{};
    ReferenceLightArray mLights{};
    foundation::usize mLightCount = 0;
    QSize mExtent;
    QString mError;
    ViewportReferencePhase mPhase = ViewportReferencePhase::Idle;
    bool mHasCapture = false;
};
} // namespace ludus::editor
