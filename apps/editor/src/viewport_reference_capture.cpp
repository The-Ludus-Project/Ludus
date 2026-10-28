#include "internal/viewport_reference_capture.h"

#include "internal/preview_gpu_generation.h"
#include "internal/preview_shader_set.h"
#include "internal/preview_shading.h"
#include "internal/render_scene.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/raster.h>
#include <ludus/graphics/rhi/ray.h>

#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/renderer/reference.hpp>
#include <ludus/graphics/renderer/renderer.hpp>

#include <ludus/runtime/resources/scene.h>

#if defined(LUDUS_EDITOR_GPU_PREVIEW) && defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "renderer_reference.h"
#    include "renderer_reference_reduce.h"
#endif
#if defined(LUDUS_EDITOR_REFERENCE_NATIVE_QUERIES)
#    include "ray_query.h"
#endif

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSaveFile>

#include <new>

// Reference capture drives the renderer's RT1 progressive path tracer; see
// docs/architecture/renderer-systems.md#reference-workflow-and-frozen-profile for the
// integrator, its attribution (Pharr, Jakob and Humphreys, PBRT 4e section 13.4; Boksansky
// and Marrs, Ray Tracing Gems II chapter 14) and its declared approximations.
namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace rhi = graphics::rhi;
namespace renderer = graphics::renderer;
/// LUHDR01 header bytes before the pixel records.
constexpr usize REFERENCE_HDR_HEADER_BYTES = 256;
/// LUHDR01 bytes per pixel: three FP32 means and a uint32 committed count.
constexpr usize REFERENCE_HDR_PIXEL_BYTES = 16;
/// Roulette starts after this many scattering events, or at the last one for shallower depths.
constexpr uint32 REFERENCE_ROULETTE_DEPTH = 3;
/// Largest display channel value after encoding.
constexpr float32 DISPLAY_CHANNEL_MAXIMUM = 255;

bool IsAreaProxy(renderer::LightKind kind) noexcept
{
    return kind == renderer::LightKind::AreaOneSided || kind == renderer::LightKind::AreaTwoSided;
}

QString DescribeReferenceFailure(renderer::ReferenceFailure failure)
{
    switch (failure)
    {
        case renderer::ReferenceFailure::InvalidHit:
            return QStringLiteral("invalid traversal hit");
        case renderer::ReferenceFailure::InvalidEmitterPdf:
            return QStringLiteral("invalid emitter density");
        case renderer::ReferenceFailure::InvalidSurfaceParameters:
            return QStringLiteral("surface parameter decoding failed");
        case renderer::ReferenceFailure::InvalidLightSample:
            return QStringLiteral("invalid light sample");
        case renderer::ReferenceFailure::InvalidBsdfEvaluation:
            return QStringLiteral("BSDF evaluation failed");
        case renderer::ReferenceFailure::InvalidDirectContribution:
            return QStringLiteral("invalid direct-light contribution");
        case renderer::ReferenceFailure::InvalidShadowSegment:
            return QStringLiteral("invalid shadow segment");
        case renderer::ReferenceFailure::InvalidBsdfSample:
            return QStringLiteral("BSDF sampling failed");
        case renderer::ReferenceFailure::InvalidThroughput:
            return QStringLiteral("invalid path throughput");
        case renderer::ReferenceFailure::InvalidRayOrigin:
            return QStringLiteral("invalid ray origin");
        case renderer::ReferenceFailure::InvalidRadiance:
            return QStringLiteral("invalid path radiance");
        case renderer::ReferenceFailure::InvalidAccumulation:
            return QStringLiteral("pixel accumulation overflow");
        case renderer::ReferenceFailure::InvalidCameraRay:
            return QStringLiteral("camera ray construction failed");
        case renderer::ReferenceFailure::InvalidSceneLayout:
            return QStringLiteral("incompatible GPU scene layout");
        case renderer::ReferenceFailure::TraversalCapacityExceeded:
            return QStringLiteral("GPU traversal stack capacity exceeded");
        case renderer::ReferenceFailure::None:
            return QStringLiteral("no path validation failure recorded");
    }
    return QStringLiteral("unknown path validation failure");
}

QString DescribeStatus(rhi::RasterStatus status)
{
    switch (status)
    {
        case rhi::RasterStatus::Failed:
            return QStringLiteral("the reference path or backend validation failed");
        case rhi::RasterStatus::Unsupported:
            return QStringLiteral("the scene uses content outside the reference profile (for example a "
                                  "shading normal facing away from its triangle, an unretained texture "
                                  "format, or a masked material with native traversal)");
        case rhi::RasterStatus::CapacityExceeded:
            return QStringLiteral("the scene or image exceeds the reference memory budget");
        case rhi::RasterStatus::OutOfMemory:
            return QStringLiteral("memory allocation failed");
        case rhi::RasterStatus::InvalidDescription:
            return QStringLiteral("the camera or capture settings are invalid");
        default:
            return QStringLiteral("status %1").arg(static_cast<uint32>(status));
    }
}

#if defined(LUDUS_EDITOR_GPU_PREVIEW)
bool ResolveTileAndShader(rhi::DeviceHandle device,
                          renderer::ReferenceTraversal traversal,
                          PreviewReferenceSetup& outSetup,
                          QString& outError)
{
    outSetup.Options.Traversal = traversal;
    outSetup.Options.CpuByteBudget = REFERENCE_CPU_BYTE_BUDGET;
    if (traversal == renderer::ReferenceTraversal::CpuOracle)
    {
        outSetup.Options.TilePixels = REFERENCE_CPU_TILE_PIXELS;
        return true;
    }
    if (traversal == renderer::ReferenceTraversal::GpuCompute)
    {
#    if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
        rhi::ComputeCapabilities capabilities;
        if (rhi::GetComputeCapabilities(device, capabilities) != rhi::RasterStatus::Ready)
        {
            outError = QStringLiteral("GPU compute is unavailable on this device; choose the CPU oracle.");
            return false;
        }
        // Every dispatch advances the whole image; the tile size only satisfies option validation.
        outSetup.Options.TilePixels = REFERENCE_CPU_TILE_PIXELS;
        outSetup.Options.GpuByteBudget = REFERENCE_GPU_BYTE_BUDGET;
        outSetup.TraceShader = ludus::shaders::renderer_reference::Compute();
        outSetup.ReduceShader = ludus::shaders::renderer_reference_reduce::Compute();
        return true;
#    else
        outError = QStringLiteral("This editor build has no GPU compute reference kernel; choose the CPU oracle.");
        return false;
#    endif
    }
#    if defined(LUDUS_EDITOR_REFERENCE_NATIVE_QUERIES)
    rhi::RayCapabilities capabilities;
    if (rhi::GetRayCapabilities(device, capabilities) != rhi::RasterStatus::Ready || capabilities.MaxRays < 2)
    {
        outError = QStringLiteral("Native ray queries are unavailable on this device; choose the CPU oracle.");
        return false;
    }
    // RT1 needs room for one closest and one visibility query per tile pixel.
    outSetup.Options.TilePixels = capabilities.MaxRays / 2 < MAXIMUM_REFERENCE_TILE_PIXELS
                                      ? capabilities.MaxRays / 2
                                      : MAXIMUM_REFERENCE_TILE_PIXELS;
    outSetup.Options.RayByteBudget = REFERENCE_RAY_BYTE_BUDGET;
    outSetup.QueryShader = ludus::shaders::ray_query::Query();
    return true;
#    else
    static_cast<void>(device);
    outError = QStringLiteral("This editor build has no native ray query shader; choose the CPU oracle.");
    return false;
#    endif
}
#endif
} // namespace

QSize ComputeReferenceExtent(const QSize& drawable, uint32 longSide) noexcept
{
    if (drawable.width() <= 0 || drawable.height() <= 0 || longSide == 0)
    {
        return {};
    }
    const float64 aspect = static_cast<float64>(drawable.width()) / static_cast<float64>(drawable.height());
    const float64 side = static_cast<float64>(longSide);
    if (aspect >= 1)
    {
        return {static_cast<int32>(longSide), math::Max(1, static_cast<int32>(side / aspect + .5))};
    }
    return {math::Max(1, static_cast<int32>(side * aspect + .5)), static_cast<int32>(longSide)};
}

usize CollectReferenceLights(const renderer::LightSelection& selection, ReferenceLightArray& outLights) noexcept
{
    usize count = 0;
    for (uint32 i = 0; i < selection.Count && count < outLights.GetSize(); ++i)
    {
        const renderer::DirectLight& light = selection.Lights[i];
        if (IsAreaProxy(light.Kind) == false)
        {
            outLights[count++] = light;
        }
    }
    return count;
}

math::Vector3 EncodeReferenceDisplayColor(math::Vector3 radiance) noexcept
{
    // Matches renderer_output.slang at exposure one: the viewport's tone curve, saturate, then sRGB.
    const auto sanitize = [](float32 value) { return math::IsFinite(value) == true ? math::Max(value, 0.0F) : 0.0F; };
    const math::Vector3 display =
        renderer::ApplyToneCurve(VIEWPORT_TONE_CURVE,
                                 {sanitize(radiance.X), sanitize(radiance.Y), sanitize(radiance.Z)});
    return {math::EncodeSrgb(math::Clamp(display.X, 0.0F, 1.0F)),
            math::EncodeSrgb(math::Clamp(display.Y, 0.0F, 1.0F)),
            math::EncodeSrgb(math::Clamp(display.Z, 0.0F, 1.0F))};
}

float32 EncodeReferenceDisplayValue(float32 greyRadiance) noexcept
{
    return EncodeReferenceDisplayColor({greyRadiance, greyRadiance, greyRadiance}).X;
}

uint8 EncodeReferenceDisplayChannel(float32 greyRadiance) noexcept
{
    return static_cast<uint8>(EncodeReferenceDisplayValue(greyRadiance) * DISPLAY_CHANNEL_MAXIMUM + .5F);
}

ViewportReferenceCapture::ViewportReferenceCapture() noexcept = default;
ViewportReferenceCapture::~ViewportReferenceCapture() noexcept
{
    Release();
}

bool ViewportReferenceCapture::Start(rhi::DeviceHandle device,
                                     rhi::SurfaceHandle surface,
                                     const RenderScene& scene,
                                     const renderer::ViewDescription& view,
                                     const QSize& extent,
                                     const ViewportReferenceSettings& settings,
                                     QString& outError)
{
    Release();
    mError.clear();
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    if (extent.width() <= 0 || extent.height() <= 0 || settings.LongSide < MINIMUM_REFERENCE_LONG_SIDE ||
        settings.LongSide > MAXIMUM_REFERENCE_LONG_SIDE || settings.Samples == 0 ||
        settings.Samples > MAXIMUM_REFERENCE_SAMPLES || settings.MaxDepth == 0 ||
        settings.MaxDepth > MAXIMUM_REFERENCE_DEPTH)
    {
        outError = QStringLiteral("Reference capture settings are outside their admitted ranges.");
        return false;
    }
    if (scene.Lit == false || scene.Definition == nullptr)
    {
        outError = QStringLiteral("Reference capture needs a lit scene: add a light or an emissive surface.");
        return false;
    }
    PreviewReferenceSetup setup;
    if (ResolveTileAndShader(device, settings.Traversal, setup, outError) == false)
    {
        return false;
    }
    renderer::ReferenceCaptureDescription description;
    description.WorldToClip = view.WorldToClip;
    description.CameraPosition = view.CameraPosition;
    description.OrthographicCamera = view.OrthographicCamera;
    description.ReverseZ = view.Depth == renderer::DepthConvention::ReverseZ;
    description.InfiniteFar = view.InfiniteFar;
    description.Width = static_cast<uint32>(extent.width());
    description.Height = static_cast<uint32>(extent.height());
    description.TargetSamples = settings.Samples;
    description.MaxDepth = settings.MaxDepth;
    description.RouletteDepth = math::Min(REFERENCE_ROULETTE_DEPTH, settings.MaxDepth);
    description.Seed = settings.Seed;
    mGeneration.Reset(new (std::nothrow)
                          PreviewGpuGeneration(device, scene, PreviewShading::Lit, PreviewShaderSet{}, &setup));
    if (mGeneration.Get() == nullptr)
    {
        outError = QStringLiteral("Reference renderer allocation failed.");
        return false;
    }
    mDescription = description;
    mExtent = extent;
    mSurface = surface;
    mPhase = ViewportReferencePhase::Preparing;
    return true;
#else
    static_cast<void>(device);
    static_cast<void>(surface);
    static_cast<void>(scene);
    static_cast<void>(view);
    static_cast<void>(extent);
    static_cast<void>(settings);
    outError = QStringLiteral("This editor build has no GPU preview renderer for reference capture.");
    return false;
#endif
}

bool ViewportReferenceCapture::fail(const QString& message) noexcept
{
    mError = message;
    mPhase = ViewportReferencePhase::Failed;
    return false;
}

bool ViewportReferenceCapture::beginCapture() noexcept
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    PreviewGpuGeneration& generation = *mGeneration;
    mLightCount = CollectReferenceLights(generation.Lighting.Lights, mLights);
    mDescription.Lights = mLightCount == 0 ? nullptr : mLights.GetData();
    mDescription.LightCount = mLightCount;
    if (generation.HasEnvironment == true)
    {
        mDescription.EnvironmentTextureOrNull = &generation.Environment;
    }
    else
    {
        mDescription.Environment = generation.Source.Definition->Environment.Rgb;
    }
    const rhi::RasterStatus status =
        generation.Renderer.BeginReferenceCapture(generation.GetSnapshot(), mDescription, mCapture);
    if (status != rhi::RasterStatus::Ready && status != rhi::RasterStatus::Pending)
    {
        return fail(QStringLiteral("Reference capture could not begin: %1.").arg(DescribeStatus(status)));
    }
    mHasCapture = true;
    mPhase = ViewportReferencePhase::Tracing;
    return true;
#else
    return false;
#endif
}

ViewportReferencePhase ViewportReferenceCapture::Service(uint64 budgetNanoseconds) noexcept
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    QElapsedTimer timer;
    timer.start();
    const auto isWithinBudget = [&timer, budgetNanoseconds]() noexcept {
        return static_cast<uint64>(timer.nsecsElapsed()) < budgetNanoseconds;
    };
    while (mPhase == ViewportReferencePhase::Preparing && isWithinBudget() == true)
    {
        QString error;
        const PreviewPreparationStatus status = mGeneration->Prepare(mSurface, error);
        if (status == PreviewPreparationStatus::Failed)
        {
            static_cast<void>(fail(error));
            break;
        }
        if (status == PreviewPreparationStatus::Ready)
        {
            static_cast<void>(beginCapture());
        }
    }
    while (mPhase == ViewportReferencePhase::Tracing && isWithinBudget() == true)
    {
        const rhi::RasterStatus status = mGeneration->Renderer.AdvanceReferenceCapture(mCapture, mSurface);
        if (status == rhi::RasterStatus::Ready)
        {
            mPhase = ViewportReferencePhase::Complete;
        }
        else if (status != rhi::RasterStatus::Pending)
        {
            renderer::ReferenceCaptureInfo info;
            QString location;
            if (mGeneration->Renderer.GetReferenceCaptureInfo(mCapture, info) == rhi::RasterStatus::Ready &&
                info.InvalidContributions != 0)
            {
                location = QStringLiteral(" at pixel (%1, %2), sample %3, depth %4 (%5)")
                               .arg(info.FailurePixel % mDescription.Width)
                               .arg(info.FailurePixel / mDescription.Width)
                               .arg(info.FailureSample)
                               .arg(info.FailureDepth)
                               .arg(DescribeReferenceFailure(info.Failure));
            }
            static_cast<void>(
                fail(QStringLiteral("Reference tracing failed%1: %2.").arg(location, DescribeStatus(status))));
        }
    }
#else
    static_cast<void>(budgetNanoseconds);
#endif
    return mPhase;
}

void ViewportReferenceCapture::Release() noexcept
{
    if (mGeneration.Get() != nullptr && mHasCapture == true)
    {
        static_cast<void>(mGeneration->Renderer.Release(mCapture));
    }
    mHasCapture = false;
    mGeneration.Reset();
    mLightCount = 0;
    mPhase = ViewportReferencePhase::Idle;
}

bool ViewportReferenceCapture::GetInfo(renderer::ReferenceCaptureInfo& outInfo) const noexcept
{
    return mHasCapture == true &&
           mGeneration->Renderer.GetReferenceCaptureInfo(mCapture, outInfo) == rhi::RasterStatus::Ready;
}

bool ViewportReferenceCapture::CopyPixels(Array<renderer::ReferencePixel>& outPixels, QString& outError) const
{
    if (mHasCapture == false)
    {
        outError = QStringLiteral("No reference samples exist yet.");
        return false;
    }
    const usize count = static_cast<usize>(mDescription.Width) * mDescription.Height;
    Array<renderer::ReferencePixel> pixels;
    if (pixels.TryResize(count) == false)
    {
        outError = QStringLiteral("Could not allocate the reference image copy.");
        return false;
    }
    const rhi::RasterStatus status = mGeneration->Renderer.CopyReferencePixels(mCapture, pixels.GetData(), count);
    if (status != rhi::RasterStatus::Ready)
    {
        outError = QStringLiteral("Could not copy reference pixels: %1.").arg(DescribeStatus(status));
        return false;
    }
    outPixels = Move(pixels);
    return true;
}

bool ViewportReferenceCapture::CopyDisplayImage(QImage& outImage, QString& outError) const
{
    Array<renderer::ReferencePixel> pixels;
    if (CopyPixels(pixels, outError) == false)
    {
        return false;
    }
    QImage image(static_cast<int32>(mDescription.Width), static_cast<int32>(mDescription.Height), QImage::Format_RGB32);
    if (image.isNull() == true)
    {
        outError = QStringLiteral("Could not allocate the reference display image.");
        return false;
    }
    for (uint32 y = 0; y < mDescription.Height; ++y)
    {
        QRgb* row = reinterpret_cast<QRgb*>(image.scanLine(static_cast<int32>(y)));
        for (uint32 x = 0; x < mDescription.Width; ++x)
        {
            const renderer::ReferencePixel& pixel = pixels[static_cast<usize>(y) * mDescription.Width + x];
            const math::Vector3 display = EncodeReferenceDisplayColor(pixel.Radiance);
            row[x] = qRgb(static_cast<uint8>(display.X * DISPLAY_CHANNEL_MAXIMUM + .5F),
                          static_cast<uint8>(display.Y * DISPLAY_CHANNEL_MAXIMUM + .5F),
                          static_cast<uint8>(display.Z * DISPLAY_CHANNEL_MAXIMUM + .5F));
        }
    }
    outImage = image;
    return true;
}

bool ViewportReferenceCapture::EncodeHdr(QByteArray& outBytes, QString& outError) const
{
    if (mHasCapture == false)
    {
        outError = QStringLiteral("No reference samples exist yet.");
        return false;
    }
    const usize capacity = REFERENCE_HDR_HEADER_BYTES +
                           static_cast<usize>(mDescription.Width) * mDescription.Height * REFERENCE_HDR_PIXEL_BYTES;
    QByteArray bytes(static_cast<qsizetype>(capacity), Qt::Uninitialized);
    usize written = 0;
    const rhi::RasterStatus status =
        mGeneration->Renderer.EncodeReferenceHdr(mCapture, reinterpret_cast<uint8*>(bytes.data()), capacity, written);
    if (status != rhi::RasterStatus::Ready)
    {
        outError = QStringLiteral("Could not encode the reference capture: %1.").arg(DescribeStatus(status));
        return false;
    }
    bytes.truncate(static_cast<qsizetype>(written));
    outBytes = bytes;
    return true;
}

QString ViewportReferenceCapture::GetDisplayImagePath(const QString& hdrPath)
{
    const QFileInfo info(hdrPath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral(".png"));
}

bool ViewportReferenceCapture::Save(const QString& hdrPath, QString& outError) const
{
    QByteArray bytes;
    QImage image;
    if (EncodeHdr(bytes, outError) == false || CopyDisplayImage(image, outError) == false)
    {
        return false;
    }
    QSaveFile hdr(hdrPath);
    if (hdr.open(QIODevice::WriteOnly) == false || hdr.write(bytes) != bytes.size() || hdr.commit() == false)
    {
        outError = QStringLiteral("Could not write %1: %2").arg(hdrPath, hdr.errorString());
        return false;
    }
    const QString pngPath = GetDisplayImagePath(hdrPath);
    QSaveFile png(pngPath);
    if (png.open(QIODevice::WriteOnly) == false || image.save(&png, "PNG") == false || png.commit() == false)
    {
        outError = QStringLiteral("Could not write %1: %2").arg(pngPath, png.errorString());
        return false;
    }
    return true;
}
} // namespace ludus::editor
