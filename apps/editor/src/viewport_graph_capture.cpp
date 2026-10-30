#include "internal/viewport_graph_capture.h"

#include "internal/render_scene.h"
#include "internal/viewport_reference_capture.h"

#include <ludus/foundation/base/core.h>

#include <ludus/content/render_graph_format/plan.h>
#include <ludus/graphics/renderer/render_graph_activation.hpp>

#include "renderer_composite.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <cstring>
#include <new>

// Design: docs/architecture/render-graph-frame-capture.md (phase 2).
namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace renderer = graphics::renderer;
namespace format = content::render_graph_format;

/// Why the engine capture rejected the route, in the terms of the viewport's output list.
QString DescribeProblem(renderer::RenderGraphCaptureProblem problem, graphics::rhi::RasterStatus status)
{
    switch (problem)
    {
        case renderer::RenderGraphCaptureProblem::None:
            return {};
        case renderer::RenderGraphCaptureProblem::NoOutput:
            return QStringLiteral("The selected output is not an exported colour texture of this graph.");
        case renderer::RenderGraphCaptureProblem::RasterRoute:
            return QStringLiteral("The selected output is drawn by raster passes, which store display colour rather "
                                  "than linear radiance. Graph capture needs a ray-traced output with an Accumulate.");
        case renderer::RenderGraphCaptureProblem::NoAccumulation:
            return QStringLiteral("The ray pass that draws the selected output shows individual samples. Add an "
                                  "Accumulate after it to capture a converged image.");
        case renderer::RenderGraphCaptureProblem::InvalidSettings:
            return QStringLiteral("The sample count or image size is outside the capture's range.");
        case renderer::RenderGraphCaptureProblem::GraphFailure:
            return QStringLiteral("The render graph failed while capturing (%1).").arg(DescribeRasterStatus(status));
        case renderer::RenderGraphCaptureProblem::ReadbackFailure:
            return QStringLiteral("Reading the capture back failed (%1).").arg(DescribeRasterStatus(status));
        case renderer::RenderGraphCaptureProblem::LatchedPixels:
            return QStringLiteral("Some pixels show the graph's failure colour: a shader failed there, for example "
                                  "by exceeding its query budget. Their counts stop at the failure.");
        case renderer::RenderGraphCaptureProblem::UntiledPackage:
            return QStringLiteral("This capture needs to render each sample in several submissions to stay under "
                                  "the GPU timeout, but the graph's package was cooked before tiled capture. "
                                  "Rebuild the graph and capture again.");
    }
    return {};
}

/// Read a cooked package of at most RenderGraphPreview::MAX_PACKAGE_BYTES.
bool ReadPackageBytes(const QString& path, QByteArray& outBytes)
{
    QFile file(path);
    if (QFileInfo(path).isFile() == false || QFileInfo(path).size() > RenderGraphPreview::MAX_PACKAGE_BYTES ||
        file.open(QIODevice::ReadOnly) == false)
    {
        return false;
    }
    outBytes = file.read(RenderGraphPreview::MAX_PACKAGE_BYTES);
    return true;
}
} // namespace

ViewportGraphCapture::ViewportGraphCapture() noexcept = default;
ViewportGraphCapture::~ViewportGraphCapture() noexcept
{
    Release();
}

bool ViewportGraphCapture::Load(graphics::rhi::DeviceHandle device,
                                graphics::rhi::SurfaceHandle surface,
                                const ViewportRasterSource& source,
                                const QString& captureProfilePath,
                                const QSize& requestedSize,
                                uint32 samples,
                                QSize& outExtent,
                                QString& outError)
{
    Release();
    mError.clear();
    if (source.PackagePath.isEmpty() == true || requestedSize.isEmpty() == true)
    {
        outError = QStringLiteral("The viewport has no project graph output to capture yet.");
        return false;
    }
    if (source.ShouldForceRaster == true)
    {
        outError = QStringLiteral("Force raster draws the raster fallback, whose output is display colour. Turn it "
                                  "off to capture the render graph.");
        return false;
    }
    if (source.OutputTarget.isEmpty() == false && source.OutputResource == format::ABSENT)
    {
        outError = QStringLiteral("The selected output %1 is not part of the active graph generation.")
                       .arg(source.OutputTarget);
        return false;
    }
    // A capture profile is cooked from the runtime package's generation with the same plan, so the source's
    // output resource and pass indices apply to it unchanged.
    mIsCaptureProfile = captureProfilePath.isEmpty() == false;
    const QString path = mIsCaptureProfile == true ? captureProfilePath : source.PackagePath;
    QByteArray bytes;
    if (ReadPackageBytes(path, bytes) == false)
    {
        outError = mIsCaptureProfile == true
                       ? QStringLiteral("No readable capture profile at %1; capture again to rebuild it.")
                             .arg(QFileInfo(path).fileName())
                       : QStringLiteral("No readable runtime generation at %1; cook the runtime first.")
                             .arg(QFileInfo(path).fileName());
        return false;
    }
    renderer::RenderGraphPackage* packageOrNull = new (std::nothrow) renderer::RenderGraphPackage;
    mPackage.Reset(packageOrNull);
    const BorrowedView<const uint8> view{reinterpret_cast<const uint8*>(bytes.constData()),
                                         static_cast<usize>(bytes.size())};
    if (mPackage.Get() == nullptr ||
        (mIsCaptureProfile == true ? mPackage->DecodeCaptureProfile(view) : mPackage->Decode(view)) !=
            renderer::RenderGraphPackageStatus::Ready)
    {
        // Name the package before Release forgets which kind was decoded.
        outError = QStringLiteral("The %1 %2 could not be decoded for capture.")
                       .arg(mIsCaptureProfile == true ? QStringLiteral("capture profile")
                                                      : QStringLiteral("runtime generation"),
                            QFileInfo(path).fileName());
        Release();
        return false;
    }
    // The programs record is tens of KiB; keep it off the stack. Its artifacts stay borrowed from the package.
    renderer::RenderGraphPrograms* programsOrNull = new (std::nothrow) renderer::RenderGraphPrograms(
        mPackage->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment()));
    mProgramsOrNull.Reset(programsOrNull);
    if (mProgramsOrNull.Get() == nullptr)
    {
        Release();
        outError = QStringLiteral("The capture could not be allocated.");
        return false;
    }
    const renderer::RenderGraphPlan plan = mPackage->GetPlan();
    renderer::RenderGraphCaptureSettings settings;
    settings.TargetSamples = samples;
    settings.PreviewInterval = VIEWPORT_GRAPH_CAPTURE_PREVIEW_INTERVAL;
    settings.Output = source.OutputResource;
    if (settings.Output == format::ABSENT)
    {
        for (uint32 resource = 0; resource < plan.Resources.GetSize(); ++resource)
        {
            if (plan.Resources[resource].IsExported == 1 && plan.Resources[resource].Kind == format::Aspect::Color)
            {
                settings.Output = resource;
                break;
            }
        }
    }
    const QByteArray identity = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    if (static_cast<usize>(identity.size()) == settings.PackageIdentity.GetSize())
    {
        std::memcpy(settings.PackageIdentity.GetData(), identity.constData(), settings.PackageIdentity.GetSize());
    }
    const graphics::rhi::RasterStatus started = mCapture.Start(device,
                                                               surface,
                                                               plan,
                                                               *mProgramsOrNull,
                                                               static_cast<uint32>(requestedSize.width()),
                                                               static_cast<uint32>(requestedSize.height()),
                                                               settings);
    if (started != graphics::rhi::RasterStatus::Ready && started != graphics::rhi::RasterStatus::Pending)
    {
        outError = DescribeProblem(mCapture.GetProblem(), mCapture.GetStatus());
        Release();
        return false;
    }
    const renderer::RenderGraphCaptureInfo info = mCapture.GetInfo();
    outExtent = QSize(static_cast<int32>(info.Width), static_cast<int32>(info.Height));
    return true;
}

bool ViewportGraphCapture::Begin(const RenderScene& scene, const renderer::ViewDescription& view, QString& outError)
{
    if (mProgramsOrNull.Get() == nullptr || mExtractionOrNull.Get() != nullptr)
    {
        outError = QStringLiteral("Load the render graph before beginning its capture.");
        return false;
    }
    RenderGraphExtraction* extractionOrNull = new (std::nothrow) RenderGraphExtraction;
    mExtractionOrNull.Reset(extractionOrNull);
    // A fixed nonzero revision identifies the frozen extraction and seeds the graph's sampling.
    constexpr uint64 CAPTURE_REVISION = 1;
    QString error;
    if (mExtractionOrNull.Get() == nullptr ||
        MakeRenderGraphExtraction(scene, view, CAPTURE_REVISION, *mExtractionOrNull, error) == false)
    {
        outError = QStringLiteral("The scene cannot be prepared for the graph capture: %1").arg(error);
        Release();
        return false;
    }
    return true;
}

renderer::RenderGraphCapturePhase ViewportGraphCapture::Service() noexcept
{
    renderer::RenderGraphCapturePhase phase = mCapture.Service();
    if (phase == renderer::RenderGraphCapturePhase::AwaitingScene && mExtractionOrNull.Get() != nullptr)
    {
        // As in the viewport, the one editor view feeds every camera of the plan, and authored lights join
        // the shader inputs.
        const RenderGraphExtraction& extraction = *mExtractionOrNull;
        const QVector<renderer::ViewDescription> cameras(
            static_cast<qsizetype>(mPackage->GetPlan().Description.CameraCount),
            extraction.View);
        renderer::SceneShaderInputs shaderInputs = extraction.ShaderInputs;
        shaderInputs.Lights = {extraction.Scene.AuthoredLights.Lights, extraction.Scene.AuthoredLights.Count};
        const graphics::rhi::RasterStatus submitted =
            mCapture.SubmitScene({extraction.Meshes.constData(), static_cast<usize>(extraction.Meshes.size())},
                                 {extraction.Items.constData(), static_cast<usize>(extraction.Items.size())},
                                 {cameras.constData(), static_cast<usize>(cameras.size())},
                                 extraction.Revision,
                                 &shaderInputs);
        phase = mCapture.GetPhase();
        if (submitted != graphics::rhi::RasterStatus::Ready && submitted != graphics::rhi::RasterStatus::Pending)
        {
            fail(DescribeProblem(mCapture.GetProblem(), mCapture.GetStatus()));
            return phase;
        }
    }
    if (phase == renderer::RenderGraphCapturePhase::Failed && mError.isEmpty() == true)
    {
        fail(DescribeProblem(mCapture.GetProblem(), mCapture.GetStatus()));
    }
    // The private graph copied the scene; release the borrowed World as soon as rendering began.
    if (phase == renderer::RenderGraphCapturePhase::Rendering)
    {
        mExtractionOrNull.Reset();
    }
    return phase;
}

void ViewportGraphCapture::Stop() noexcept
{
    mCapture.Stop();
}

void ViewportGraphCapture::Release() noexcept
{
    mCapture.Release();
    mExtractionOrNull.Reset();
    mProgramsOrNull.Reset();
    mPackage.Reset();
    mIsCaptureProfile = false;
}

bool ViewportGraphCapture::ReadGenerationIdentity(const ViewportRasterSource& source,
                                                  QByteArray& outIdentity,
                                                  QString& outError)
{
    QByteArray bytes;
    if (source.PackagePath.isEmpty() == true || ReadPackageBytes(source.PackagePath, bytes) == false)
    {
        outError = QStringLiteral("The viewport has no readable runtime generation to capture yet.");
        return false;
    }
    renderer::RenderGraphPackage* packageOrNull = new (std::nothrow) renderer::RenderGraphPackage;
    const core::UniquePtr<renderer::RenderGraphPackage> package{packageOrNull};
    if (package.Get() == nullptr ||
        package->Decode({reinterpret_cast<const uint8*>(bytes.constData()), static_cast<usize>(bytes.size())}) !=
            renderer::RenderGraphPackageStatus::Ready)
    {
        outError = QStringLiteral("The runtime generation %1 could not be decoded for capture.")
                       .arg(QFileInfo(source.PackagePath).fileName());
        return false;
    }
    const BorrowedView<const uint8> identity = package->GetGenerationIdentity();
    outIdentity =
        QByteArray(reinterpret_cast<const char*>(identity.GetDataOrNull()), static_cast<qsizetype>(identity.GetSize()));
    return true;
}

renderer::RenderGraphCapturePhase ViewportGraphCapture::GetPhase() const noexcept
{
    return mCapture.GetPhase();
}

bool ViewportGraphCapture::IsRunning() const noexcept
{
    const renderer::RenderGraphCapturePhase phase = mCapture.GetPhase();
    return phase != renderer::RenderGraphCapturePhase::Idle && phase != renderer::RenderGraphCapturePhase::Complete &&
           phase != renderer::RenderGraphCapturePhase::Stopped && phase != renderer::RenderGraphCapturePhase::Failed;
}

renderer::RenderGraphCaptureInfo ViewportGraphCapture::GetInfo() const noexcept
{
    return mCapture.GetInfo();
}

BorrowedView<const renderer::RenderGraphCapturePixel> ViewportGraphCapture::GetPixels() const noexcept
{
    return mCapture.GetPixels();
}

QImage ViewportGraphCapture::MakeDisplayImage() const
{
    const renderer::RenderGraphCaptureInfo info = mCapture.GetInfo();
    const BorrowedView<const renderer::RenderGraphCapturePixel> pixels = mCapture.GetPixels();
    if (pixels.GetSize() != usize{info.Width} * info.Height || pixels.IsEmpty() == true)
    {
        return {};
    }
    QImage image(static_cast<int32>(info.Width), static_cast<int32>(info.Height), QImage::Format_RGB32);
    if (image.isNull() == true)
    {
        return {};
    }
    constexpr float32 CHANNEL_MAXIMUM = 255;
    for (uint32 y = 0; y < info.Height; ++y)
    {
        QRgb* row = reinterpret_cast<QRgb*>(image.scanLine(static_cast<int32>(y)));
        for (uint32 x = 0; x < info.Width; ++x)
        {
            const math::Vector3 display = EncodeReferenceDisplayColor(pixels[usize{y} * info.Width + x].Mean);
            row[x] = qRgb(qRound(display.X * CHANNEL_MAXIMUM),
                          qRound(display.Y * CHANNEL_MAXIMUM),
                          qRound(display.Z * CHANNEL_MAXIMUM));
        }
    }
    return image;
}

QString ViewportGraphCapture::GetDisplayImagePath(const QString& capturePath)
{
    const QFileInfo info(capturePath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral(".png"));
}

bool ViewportGraphCapture::Save(const QString& capturePath, QString& outError) const
{
    Array<uint8> bytes;
    if (mCapture.Encode(bytes) != graphics::rhi::RasterStatus::Ready)
    {
        outError = QStringLiteral("The render graph capture has no read-back pixels to save yet.");
        return false;
    }
    const QImage image = MakeDisplayImage();
    QSaveFile capture(capturePath);
    if (capture.open(QIODevice::WriteOnly) == false ||
        capture.write(reinterpret_cast<const char*>(bytes.GetData()), static_cast<qint64>(bytes.GetSize())) !=
            static_cast<qint64>(bytes.GetSize()) ||
        capture.commit() == false)
    {
        outError = QStringLiteral("Could not write %1: %2").arg(capturePath, capture.errorString());
        return false;
    }
    const QString pngPath = GetDisplayImagePath(capturePath);
    QSaveFile png(pngPath);
    if (png.open(QIODevice::WriteOnly) == false || image.save(&png, "PNG") == false || png.commit() == false)
    {
        outError = QStringLiteral("Could not write %1: %2").arg(pngPath, png.errorString());
        return false;
    }
    return true;
}

void ViewportGraphCapture::fail(const QString& message) noexcept
{
    mError = message;
}
} // namespace ludus::editor
