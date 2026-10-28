#include "internal/reference_comparison.h"
#include "internal/render_scene.h"
#include "internal/texture_inspection.h"
#include "internal/viewport_raster_capture.h"
#include "internal/viewport_reference_capture.h"
#include "viewport_fixture.h"

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>

#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/rhi.h>

#include <ludus/graphics/renderer/reference.hpp>
#include <ludus/graphics/renderer/renderer.hpp>

#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonObject>
#include <QSize>
#include <QString>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace renderer = ludus::graphics::renderer;

namespace
{
constexpr uint32 IMAGE_WIDTH = 2;
constexpr uint32 IMAGE_HEIGHT = 2;
constexpr usize RGBA_CHANNELS = 4;
constexpr usize HALF_BYTES = 2;
/// IEEE binary16 encodings of the values the linear tests use.
constexpr uint16 HALF_ZERO = 0x0000;
constexpr uint16 HALF_ONE = 0x3C00;
constexpr uint16 HALF_TWO = 0x4000;
constexpr int32 BYTE_MAXIMUM = 255;
/// Eight-bit rounding bounds a display-space error by half a level.
constexpr float64 EIGHT_BIT_ROUNDING = 0.5 / 255;
/// That rounding as drawn in the difference image, which saturates at REFERENCE_DIFFERENCE_FULL_SCALE.
constexpr int32 DIFFERENCE_ROUNDING_LEVELS = 2;
constexpr uint32 TEST_SAMPLES = 2;
constexpr uint32 TEST_DEPTH = 3;
constexpr uint32 TEST_LONG_SIDE = 24;
constexpr int64 CAPTURE_TIMEOUT_MILLISECONDS = 120000;
constexpr uint64 TEST_SERVICE_BUDGET_NANOSECONDS = 50'000'000;
constexpr uint32 POLL_MICROSECONDS = 200;

QVector<renderer::ReferencePixel> MakeReference(float32 radiance)
{
    renderer::ReferencePixel pixel;
    pixel.Radiance = {radiance, radiance, radiance};
    pixel.Samples = TEST_SAMPLES;
    return QVector<renderer::ReferencePixel>(static_cast<qsizetype>(IMAGE_WIDTH * IMAGE_HEIGHT), pixel);
}

InspectedTexture MakeRgba8(uint8 value)
{
    InspectedTexture texture;
    texture.Encoding = TexelEncoding::Rgba8Unorm;
    texture.Width = IMAGE_WIDTH;
    texture.Height = IMAGE_HEIGHT;
    texture.Bytes =
        QByteArray(static_cast<qsizetype>(IMAGE_WIDTH * IMAGE_HEIGHT * RGBA_CHANNELS), static_cast<char>(value));
    return texture;
}

InspectedTexture MakeRgba16Float(uint16 halfBits)
{
    InspectedTexture texture;
    texture.Encoding = TexelEncoding::Rgba16Float;
    texture.Width = IMAGE_WIDTH;
    texture.Height = IMAGE_HEIGHT;
    for (usize i = 0; i < IMAGE_WIDTH * IMAGE_HEIGHT * RGBA_CHANNELS; ++i)
    {
        texture.Bytes.append(static_cast<char>(halfBits & 0xFFU));
        texture.Bytes.append(static_cast<char>(halfBits >> 8U));
    }
    REQUIRE(static_cast<usize>(texture.Bytes.size()) == IMAGE_WIDTH * IMAGE_HEIGHT * RGBA_CHANNELS * HALF_BYTES);
    return texture;
}

bool Compare(const QVector<renderer::ReferencePixel>& reference,
             const InspectedTexture& raster,
             ReferenceComparison& outComparison,
             QImage& outDifference,
             QString& outError)
{
    renderer::ReferenceCaptureInfo information;
    information.Width = IMAGE_WIDTH;
    information.Height = IMAGE_HEIGHT;
    information.TargetSamples = TEST_SAMPLES;
    return CompareRasterWithReference({reference.constData(), static_cast<usize>(reference.size())},
                                      information,
                                      false,
                                      raster,
                                      outComparison,
                                      outDifference,
                                      outError);
}

/// One headless device per test; RHI is a process-wide session.
struct DeviceFixture final
{
    rhi::DeviceHandle Device{};
    rhi::SurfaceHandle Surface{};
    bool IsAvailable = false;
    DeviceFixture()
    {
        rhi::Shutdown();
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        rhi::StartupInfo failure;
        IsAvailable = rhi::CreateDevice({}, { .Width = 1, .Height = 1 }, description, Device, Surface, &failure) ==
                      rhi::DeviceStatus::Ready;
        if (IsAvailable == false)
        {
            REQUIRE(failure.Error == rhi::StartupError::AdapterUnavailable);
        }
    }
    ~DeviceFixture()
    {
        if (IsAvailable == true)
        {
            static_cast<void>(rhi::DestroyDevice(Device));
        }
        rhi::Shutdown();
    }
    DeviceFixture(const DeviceFixture&) = delete;
    DeviceFixture& operator=(const DeviceFixture&) = delete;
};
} // namespace

TEST_CASE("Display comparison of the reference's own display encoding has only rounding error",
          "[editor][reference-comparison]")
{
    const QVector<renderer::ReferencePixel> reference = MakeReference(1);
    ReferenceComparison comparison;
    QImage difference;
    QString error;
    REQUIRE(Compare(reference, MakeRgba8(EncodeReferenceDisplayChannel(1)), comparison, difference, error) == true);
    CHECK(comparison.Space == ReferenceComparisonSpace::Display);
    CHECK(comparison.ComparedPixels == IMAGE_WIDTH * IMAGE_HEIGHT);
    CHECK(comparison.MinimumReferenceSamples == TEST_SAMPLES);
    CHECK(comparison.MaximumAbsoluteError <= EIGHT_BIT_ROUNDING);
    CHECK(comparison.VisiblyDifferentFraction == 0);
    CHECK(comparison.PeakSignalToNoiseRatio > 50);
    CHECK(difference.size() == QSize(IMAGE_WIDTH, IMAGE_HEIGHT));
    // Half an eight-bit level, scaled to the difference image's full-scale range, rounds to two.
    CHECK(qRed(difference.pixel(0, 0)) + qBlue(difference.pixel(0, 0)) <= DIFFERENCE_ROUNDING_LEVELS);
}

TEST_CASE("A darker raster reports negative luminance bias and a blue difference", "[editor][reference-comparison]")
{
    const QVector<renderer::ReferencePixel> reference = MakeReference(1);
    ReferenceComparison comparison;
    QImage difference;
    QString error;
    REQUIRE(Compare(reference, MakeRgba8(0), comparison, difference, error) == true);
    const float64 expected = static_cast<float64>(EncodeReferenceDisplayValue(1));
    CHECK(comparison.MeanAbsoluteError == Catch::Approx(expected));
    CHECK(comparison.RootMeanSquareError == Catch::Approx(expected));
    CHECK(comparison.MaximumAbsoluteError == Catch::Approx(expected));
    CHECK(comparison.MeanSignedLuminanceError == Catch::Approx(-expected));
    CHECK(comparison.VisiblyDifferentFraction == 1);
    CHECK(qBlue(difference.pixel(1, 1)) == BYTE_MAXIMUM);
    CHECK(qRed(difference.pixel(1, 1)) == 0);
    const QJsonObject json = MakeReferenceComparisonJson(comparison);
    CHECK(json.value(QStringLiteral("space")).toString() == QStringLiteral("display"));
    CHECK(json.contains(QStringLiteral("peakSignalToNoiseRatio")) == true);
    CHECK(DescribeReferenceComparison(comparison).contains(QStringLiteral("darker")) == true);
}

TEST_CASE("Linear comparison uses raw radiance and relative MSE for RGBA16F output", "[editor][reference-comparison]")
{
    const QVector<renderer::ReferencePixel> reference = MakeReference(1);
    ReferenceComparison comparison;
    QImage difference;
    QString error;
    REQUIRE(Compare(reference, MakeRgba16Float(HALF_ONE), comparison, difference, error) == true);
    CHECK(comparison.Space == ReferenceComparisonSpace::Linear);
    CHECK(comparison.MeanAbsoluteError == 0);
    CHECK(comparison.RelativeMeanSquareError == 0);

    REQUIRE(Compare(reference, MakeRgba16Float(HALF_TWO), comparison, difference, error) == true);
    CHECK(comparison.MeanAbsoluteError == Catch::Approx(1));
    CHECK(comparison.MeanSignedLuminanceError == Catch::Approx(1));
    CHECK(comparison.RelativeMeanSquareError == Catch::Approx(1 / (1 + REFERENCE_RELATIVE_ERROR_EPSILON)));
    // The viewport's tone curve compresses highlights: 1 and 2 display 0.94 and 0.98, closer than the visible
    // threshold even though the linear error is one, so the difference image is faint but not empty.
    CHECK(comparison.VisiblyDifferentFraction == 0);
    CHECK(qRed(difference.pixel(0, 0)) > 0);
    CHECK(MakeReferenceComparisonJson(comparison).contains(QStringLiteral("relativeMeanSquareError")) == true);
}

TEST_CASE("Pixels without reference samples are excluded and shown grey", "[editor][reference-comparison]")
{
    QVector<renderer::ReferencePixel> reference = MakeReference(1);
    reference[0].Samples = 0;
    reference[0].Radiance = {};
    ReferenceComparison comparison;
    QImage difference;
    QString error;
    REQUIRE(Compare(reference, MakeRgba16Float(HALF_ONE), comparison, difference, error) == true);
    CHECK(comparison.ComparedPixels == IMAGE_WIDTH * IMAGE_HEIGHT - 1);
    CHECK(comparison.IsReferenceComplete == false);
    CHECK(comparison.MinimumImageSamples == 0);
    CHECK(DescribeReferenceComparison(comparison).contains(QStringLiteral("INCOMPLETE")) == true);
    CHECK(DescribeReferenceComparison(comparison).contains(QStringLiteral("3/4 pixels sampled (75.0%)")) == true);
    CHECK(MakeReferenceComparisonJson(comparison).value(QStringLiteral("referenceComplete")).toBool() == false);
    CHECK(comparison.MeanAbsoluteError == 0);
    CHECK(qRed(difference.pixel(0, 0)) == qBlue(difference.pixel(0, 0)));
    CHECK(qRed(difference.pixel(0, 0)) > 0);

    for (renderer::ReferencePixel& pixel : reference)
    {
        pixel.Samples = 0;
    }
    const ReferenceComparison previous = comparison;
    CHECK(Compare(reference, MakeRgba16Float(HALF_ZERO), comparison, difference, error) == false);
    CHECK(error.contains(QStringLiteral("No reference pixel")) == true);
    CHECK(comparison.ComparedPixels == previous.ComparedPixels);
}

TEST_CASE("Comparison rejects mismatched extents and depth outputs", "[editor][reference-comparison]")
{
    const QVector<renderer::ReferencePixel> reference = MakeReference(1);
    ReferenceComparison comparison;
    QImage difference;
    QString error;
    InspectedTexture wide = MakeRgba8(0);
    wide.Width = IMAGE_WIDTH * 2;
    wide.Bytes.append(wide.Bytes);
    CHECK(Compare(reference, wide, comparison, difference, error) == false);
    CHECK(error.contains(QStringLiteral("does not match")) == true);
    InspectedTexture depth;
    depth.Encoding = TexelEncoding::Depth32Float;
    depth.Width = IMAGE_WIDTH;
    depth.Height = IMAGE_HEIGHT;
    depth.Bytes = QByteArray(static_cast<qsizetype>(IMAGE_WIDTH * IMAGE_HEIGHT * sizeof(float32)), '\0');
    CHECK(Compare(reference, depth, comparison, difference, error) == false);
    CHECK(error.contains(QStringLiteral("depth")) == true);
    CHECK(difference.isNull() == true);
}

TEST_CASE("The viewport's forward-lit graph is captured and compared with a reference at its extent",
          "[editor][reference-comparison][gpu]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    QTemporaryDir root;
    REQUIRE(root.isValid() == true);
    RenderScene scene;
    tests::MakeLitScene(root.path(), scene);
    ViewportRasterSource source;
    source.PackagePath = QStringLiteral(LUDUS_EDITOR_FORWARD_GRAPH_FIXTURE);
    const QSize requested = ComputeReferenceExtent(QSize(640, 480), TEST_LONG_SIDE);
    ViewportRasterCapture raster;
    QSize extent;
    QString error;
    REQUIRE(raster.Load(device.Device, device.Surface, source, requested, extent, error) == true);
    // The template renders at the viewport size, so its output keeps the requested extent.
    CHECK(extent == requested);
    PreviewViewOptions options;
    options.Width = static_cast<uint32>(extent.width());
    options.Height = static_cast<uint32>(extent.height());
    options.ShouldUseConventionalDepth = true;
    renderer::ViewDescription conventional;
    REQUIRE(scene.View(options, conventional, error) == true);
    REQUIRE(raster.Begin(scene, conventional, error) == true);
    QElapsedTimer timer;
    timer.start();
    while (raster.Service() == ViewportRasterPhase::Rendering && timer.elapsed() < CAPTURE_TIMEOUT_MILLISECONDS)
    {
        QThread::usleep(POLL_MICROSECONDS);
    }
    INFO(raster.GetError().toStdString());
    REQUIRE(raster.GetPhase() == ViewportRasterPhase::Complete);
    CHECK(raster.GetImage().size() == extent);
    CHECK(raster.GetTexture().Width == static_cast<uint32>(extent.width()));
    CHECK(raster.GetTexture().Height == static_cast<uint32>(extent.height()));
    CHECK(raster.GetFrameCount() >= 1);
    CHECK(raster.WasStillAccumulating() == false);

    options.ShouldUseConventionalDepth = false;
    renderer::ViewDescription view;
    REQUIRE(scene.View(options, view, error) == true);
    ViewportReferenceSettings settings;
    settings.LongSide = TEST_LONG_SIDE;
    settings.Samples = TEST_SAMPLES;
    settings.MaxDepth = TEST_DEPTH;
    ViewportReferenceCapture reference;
    REQUIRE(reference.Start(device.Device, device.Surface, scene, view, extent, settings, error) == true);
    timer.restart();
    ViewportReferencePhase phase = reference.GetPhase();
    while ((phase == ViewportReferencePhase::Preparing || phase == ViewportReferencePhase::Tracing) &&
           timer.elapsed() < CAPTURE_TIMEOUT_MILLISECONDS)
    {
        phase = reference.Service(TEST_SERVICE_BUDGET_NANOSECONDS);
    }
    INFO(reference.GetError().toStdString());
    REQUIRE(phase == ViewportReferencePhase::Complete);
    Array<renderer::ReferencePixel> pixels;
    REQUIRE(reference.CopyPixels(pixels, error) == true);
    ReferenceComparison comparison;
    QImage difference;
    renderer::ReferenceCaptureInfo information;
    REQUIRE(reference.GetInfo(information) == true);
    REQUIRE(CompareRasterWithReference({pixels.GetData(), pixels.GetSize()},
                                       information,
                                       false,
                                       raster.GetTexture(),
                                       comparison,
                                       difference,
                                       error) == true);
    CHECK(comparison.ComparedPixels == static_cast<uint64>(extent.width()) * static_cast<uint64>(extent.height()));
    CHECK(comparison.MinimumReferenceSamples == TEST_SAMPLES);
    CHECK(comparison.MeanAbsoluteError >= 0);
    CHECK(comparison.MaximumAbsoluteError <= 1);
    CHECK(difference.size() == extent);
    reference.Release();
    raster.Release();
    CHECK(raster.GetPhase() == ViewportRasterPhase::Complete);
}

TEST_CASE("Raster capture reports a missing render graph package", "[editor][reference-comparison][gpu]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    ViewportRasterCapture raster;
    ViewportRasterSource source;
    QSize extent;
    QString error;
    CHECK(raster.Load(device.Device, device.Surface, source, QSize(8, 8), extent, error) == false);
    CHECK(error.contains(QStringLiteral("no project graph")) == true);
    source.PackagePath = QDir(QDir::tempPath()).filePath(QStringLiteral("missing-ludus-graph.lurgrt"));
    error.clear();
    CHECK(raster.Load(device.Device, device.Surface, source, QSize(8, 8), extent, error) == false);
    CHECK(error.isEmpty() == false);
    CHECK(raster.GetPhase() == ViewportRasterPhase::Idle);
    CHECK(extent.isEmpty() == true);
}

TEST_CASE("Comparison labels full-coverage under-sampling and failures as partial diagnostics",
          "[editor][reference-comparison]")
{
    const QVector<renderer::ReferencePixel> reference = MakeReference(1);
    renderer::ReferenceCaptureInfo information;
    information.Width = IMAGE_WIDTH;
    information.Height = IMAGE_HEIGHT;
    information.TargetSamples = TEST_SAMPLES + 1;
    ReferenceComparison comparison;
    QImage difference;
    QString error;
    REQUIRE(CompareRasterWithReference({reference.constData(), static_cast<usize>(reference.size())},
                                       information,
                                       false,
                                       MakeRgba16Float(HALF_ONE),
                                       comparison,
                                       difference,
                                       error) == true);
    CHECK(comparison.IsReferenceComplete == false);
    CHECK(comparison.MinimumImageSamples == TEST_SAMPLES);
    CHECK(DescribeReferenceComparison(comparison).contains(QStringLiteral("INCOMPLETE")) == true);
    CHECK(DescribeReferenceComparison(comparison).contains(QStringLiteral("Partial diagnostic metrics only")) == true);
    information.TargetSamples = TEST_SAMPLES;
    REQUIRE(CompareRasterWithReference({reference.constData(), static_cast<usize>(reference.size())},
                                       information,
                                       true,
                                       MakeRgba16Float(HALF_ONE),
                                       comparison,
                                       difference,
                                       error) == true);
    CHECK(comparison.IsReferenceComplete == false);
    CHECK(comparison.HasReferenceFailed == true);
    CHECK(DescribeReferenceComparison(comparison).contains(QStringLiteral("FAILED / INCOMPLETE")) == true);
    CHECK(MakeReferenceComparisonJson(comparison).value(QStringLiteral("referenceFailed")).toBool() == true);
    REQUIRE(CompareRasterWithReference({reference.constData(), static_cast<usize>(reference.size())},
                                       information,
                                       false,
                                       MakeRgba16Float(HALF_ONE),
                                       comparison,
                                       difference,
                                       error) == true);
    CHECK(comparison.IsReferenceComplete == true);
    CHECK(comparison.HasReferenceFailed == false);
    CHECK(DescribeReferenceComparison(comparison).contains(QStringLiteral("Reference complete")) == true);
    CHECK(DescribeReferenceComparison(comparison).contains(QStringLiteral("Partial diagnostic")) == false);
}
