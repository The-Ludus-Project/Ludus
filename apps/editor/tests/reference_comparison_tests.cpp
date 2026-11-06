#include "internal/reference_comparison.h"
#include "internal/reference_hdr_file.h"
#include "internal/render_scene.h"
#include "internal/texture_inspection.h"
#include "internal/viewport_raster_capture.h"
#include "internal/viewport_reference_capture.h"
#include "reference_hdr_test_support.h"
#include "viewport_fixture.h"

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>

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

TEST_CASE("A graph capture compares its linear means with the reference and skips unsampled pixels",
          "[editor][reference-comparison][render-graph-capture]")
{
    const QVector<renderer::ReferencePixel> reference = MakeReference(1);
    renderer::ReferenceCaptureInfo information;
    information.Width = IMAGE_WIDTH;
    information.Height = IMAGE_HEIGHT;
    information.TargetSamples = TEST_SAMPLES;
    constexpr float32 BRIGHTER = 1.5F;
    QVector<renderer::RenderGraphCapturePixel> capture(IMAGE_WIDTH * IMAGE_HEIGHT,
                                                       {{BRIGHTER, BRIGHTER, BRIGHTER}, TEST_SAMPLES});
    // A pixel that latched a failure before its first sample has nothing to compare.
    capture[0].Samples = 0;
    ReferenceComparison comparison;
    QImage difference;
    QString error;
    REQUIRE(CompareGraphCaptureWithReference({reference.constData(), static_cast<usize>(reference.size())},
                                             information,
                                             false,
                                             {capture.constData(), static_cast<usize>(capture.size())},
                                             IMAGE_WIDTH,
                                             IMAGE_HEIGHT,
                                             comparison,
                                             difference,
                                             error) == true);
    CHECK(comparison.Space == ReferenceComparisonSpace::Linear);
    CHECK(comparison.ComparedPixels == IMAGE_WIDTH * IMAGE_HEIGHT - 1);
    CHECK(comparison.MeanAbsoluteError == Catch::Approx(BRIGHTER - 1));
    CHECK(comparison.MeanSignedLuminanceError > 0);
    CHECK(qRed(difference.pixel(0, 0)) == qBlue(difference.pixel(0, 0)));
    CHECK(qRed(difference.pixel(1, 1)) > 0);
    // A capture of another extent is rejected without touching the outputs.
    const ReferenceComparison before = comparison;
    CHECK(CompareGraphCaptureWithReference({reference.constData(), static_cast<usize>(reference.size())},
                                           information,
                                           false,
                                           {capture.constData(), static_cast<usize>(capture.size())},
                                           IMAGE_WIDTH + 1,
                                           IMAGE_HEIGHT,
                                           comparison,
                                           difference,
                                           error) == false);
    CHECK(error.contains(QStringLiteral("graph capture")) == true);
    CHECK(comparison.MeanAbsoluteError == before.MeanAbsoluteError);
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

TEST_CASE("Display comparison uses supplied sRGB values and masks missing graph samples",
          "[editor][reference-comparison]")
{
    QImage baseline(2, 1, QImage::Format_RGB32);
    QImage candidate(2, 1, QImage::Format_RGB32);
    baseline.fill(Qt::red);
    candidate.fill(Qt::blue);
    candidate.setPixelColor(1, 0, Qt::red);
    constexpr usize PIXEL_COUNT = 2;
    const StaticArray<renderer::RenderGraphCapturePixel, PIXEL_COUNT> samples{
        renderer::RenderGraphCapturePixel{},
        renderer::RenderGraphCapturePixel{ .Samples = 1 },
    };
    ReferenceComparison comparison;
    QImage difference;
    QString error;
    REQUIRE(CompareDisplayImages(baseline,
                                 candidate,
                                 {samples.GetData(), samples.GetSize()},
                                 comparison,
                                 difference,
                                 error) == true);
    CHECK(comparison.Space == ReferenceComparisonSpace::Display);
    CHECK(comparison.ComparedPixels == 1);
    CHECK(comparison.MeanAbsoluteError == 0);
    CHECK(difference.pixelColor(0, 0).red() == 64);
}

TEST_CASE("Linear baselines read back from LUHDR01 files with their camera", "[editor][reference-comparison]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    tests::TestReferenceHdr description;
    description.Extent = QSize(5, 3);
    description.Flags = tests::TEST_HDR_CPU_ORACLE_FLAG | tests::TEST_HDR_REVERSE_Z_FLAG | tests::TEST_HDR_OPTICAL_FLAG;
    description.OpticalWavelengthWorld = 0.005F;
    description.WorldToClip.Columns[0] = {1, 2, 3, 4};
    description.WorldToClip.Columns[3] = {5, 6, 7, 8};
    description.CameraPosition = {0, 0, -0.1F};
    description.Grey = 0.25F;
    description.ColumnSlope = 0.5F;
    const QString path = QDir(directory.path()).filePath(QStringLiteral("baseline.luhdr"));
    REQUIRE(tests::WriteTestReferenceHdr(path, tests::MakeTestReferenceHdr(description)) == true);

    ReferenceHdrFile file;
    QString error;
    REQUIRE(ReadReferenceHdrFile(path, file, error) == true);
    CHECK(file.Extent == description.Extent);
    CHECK(file.OpticalWavelengthWorld == 0.005F);
    CHECK(file.View.Depth == renderer::DepthConvention::ReverseZ);
    CHECK(file.View.InfiniteFar == false);
    CHECK(file.View.OrthographicCamera == false);
    CHECK(file.View.WorldToClip.Columns[0] == description.WorldToClip.Columns[0]);
    CHECK(file.View.WorldToClip.Columns[3] == description.WorldToClip.Columns[3]);
    CHECK(file.View.CameraPosition == description.CameraPosition);
    REQUIRE(file.Pixels.GetSize() == 15);
    CHECK(file.Pixels[4].Radiance.X == 0.25F + 0.5F * 4);
    CHECK(file.Pixels[4].Samples == 1);
    CHECK(file.Info.Width == 5);
    CHECK(file.Info.MinimumSamples == 1);
    CHECK(file.Info.Complete == true);
    const QImage display = MakeReferenceHdrDisplayImage(file);
    CHECK(display.size() == description.Extent);

    // Another format, a truncated file and negative radiance are refused with the next step.
    const QString other = QDir(directory.path()).filePath(QStringLiteral("other.luhdr"));
    REQUIRE(tests::WriteTestReferenceHdr(other, QByteArray(300, 'x')) == true);
    CHECK(ReadReferenceHdrFile(other, file, error) == false);
    CHECK(error.contains(QStringLiteral("not a LUHDR01 capture")) == true);
    QByteArray truncated = tests::MakeTestReferenceHdr(description);
    truncated.chop(1);
    REQUIRE(tests::WriteTestReferenceHdr(other, truncated) == true);
    CHECK(ReadReferenceHdrFile(other, file, error) == false);
    CHECK(error.contains(QStringLiteral("truncated")) == true);
    tests::TestReferenceHdr negative = description;
    negative.Grey = -1;
    REQUIRE(tests::WriteTestReferenceHdr(other, tests::MakeTestReferenceHdr(negative)) == true);
    CHECK(ReadReferenceHdrFile(other, file, error) == false);
    CHECK(error.contains(QStringLiteral("negative radiance")) == true);
}

TEST_CASE("Column profiles pass within the tolerance and name the worst column", "[editor][reference-comparison]")
{
    constexpr uint32 WIDTH = 6;
    constexpr uint32 HEIGHT = 4;
    constexpr usize PIXELS = static_cast<usize>(WIDTH) * HEIGHT;
    // Columns 0 to 4 carry light; column 5 holds 0.1% of the brightest column's, below the significance threshold.
    constexpr StaticArray<float32, WIDTH> COLUMN_RADIANCE{{1, 2, 3, 4, 5, 0.005F}};
    Array<renderer::ReferencePixel> reference;
    Array<renderer::RenderGraphCapturePixel> capture;
    REQUIRE(reference.TryResize(PIXELS) == true);
    REQUIRE(capture.TryResize(PIXELS) == true);
    const auto fill = [&](float32 scale, uint32 changedColumn, float32 changedScale) {
        for (usize pixel = 0; pixel < PIXELS; ++pixel)
        {
            const usize column = pixel % WIDTH;
            const float32 value = COLUMN_RADIANCE[column];
            reference[pixel].Radiance = {value, value, value};
            reference[pixel].Samples = 1;
            const float32 factor = column == changedColumn ? changedScale : scale;
            const float32 scaled = value * factor;
            capture[pixel].Mean = {scaled, scaled, scaled};
            capture[pixel].Samples = 1;
        }
    };
    ColumnComparison comparison;
    QString error;
    // Every column 2% bright passes.
    fill(1.02F, WIDTH, 1);
    REQUIRE(CompareColumnProfiles({reference.GetData(), reference.GetSize()},
                                  {capture.GetData(), capture.GetSize()},
                                  WIDTH,
                                  HEIGHT,
                                  comparison,
                                  error) == true);
    CHECK(comparison.IsWithinTolerance == true);
    CHECK(comparison.SignificantColumns == 5);
    CHECK(comparison.WorstColumnRatio == Catch::Approx(1.02).epsilon(1e-5));
    CHECK(comparison.TotalRatio == Catch::Approx(1.02).epsilon(1e-5));
    // One significant column 10% bright fails and is named.
    fill(1, 3, 1.1F);
    REQUIRE(CompareColumnProfiles({reference.GetData(), reference.GetSize()},
                                  {capture.GetData(), capture.GetSize()},
                                  WIDTH,
                                  HEIGHT,
                                  comparison,
                                  error) == true);
    CHECK(comparison.IsWithinTolerance == false);
    CHECK(comparison.WorstColumn == 3);
    CHECK(comparison.WorstColumnRatio == Catch::Approx(1.1).epsilon(1e-5));
    CHECK(DescribeColumnComparison(comparison).startsWith(QStringLiteral("Columns FAIL")) == true);
    // A tenfold error in the insignificant column does not fail the comparison.
    fill(1, 5, 10);
    REQUIRE(CompareColumnProfiles({reference.GetData(), reference.GetSize()},
                                  {capture.GetData(), capture.GetSize()},
                                  WIDTH,
                                  HEIGHT,
                                  comparison,
                                  error) == true);
    CHECK(comparison.IsWithinTolerance == true);
    // Pixels without candidate samples are left out of both sums.
    fill(1, WIDTH, 1);
    capture[0].Samples = 0;
    capture[0].Mean = {100, 100, 100};
    REQUIRE(CompareColumnProfiles({reference.GetData(), reference.GetSize()},
                                  {capture.GetData(), capture.GetSize()},
                                  WIDTH,
                                  HEIGHT,
                                  comparison,
                                  error) == true);
    CHECK(comparison.WorstColumnRatio == Catch::Approx(1));
    // Mismatched sizes are refused.
    CHECK(CompareColumnProfiles({reference.GetData(), reference.GetSize()},
                                {capture.GetData(), PIXELS - 1},
                                WIDTH,
                                HEIGHT,
                                comparison,
                                error) == false);
}
