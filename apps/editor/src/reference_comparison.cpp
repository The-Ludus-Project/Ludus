#include "internal/reference_comparison.h"

#include "internal/texture_inspection.h"
#include "internal/viewport_reference_capture.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <QColor>
#include <QJsonObject>
#include <QString>

// The relative mean squared error, squared error over squared reference plus 0.01, follows
// Fabrice Rousselle, Claude Knaus and Matthias Zwicker, "Adaptive Sampling and Reconstruction
// using Greedy Error Minimization", ACM Transactions on Graphics 30(6) (SIGGRAPH Asia 2011),
// where it measures rendering error without letting dark pixels dominate. The other statistics
// are the ordinary image-difference measures.
namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace renderer = graphics::renderer;
constexpr usize RGB_CHANNELS = 3;
constexpr float32 LUMINANCE_RED = 0.2126F;
constexpr float32 LUMINANCE_GREEN = 0.7152F;
constexpr float32 LUMINANCE_BLUE = 0.0722F;
/// Eight-bit channel maximum of the difference image.
constexpr float32 DIFFERENCE_CHANNEL_MAXIMUM = 255;
/// Grey level marking pixels without a committed reference sample.
constexpr int32 UNCOMPARED_GREY = 64;
/// Decibels per decade of power ratio, and the base of that decade.
constexpr float64 DECIBELS_PER_DECADE = 10;
constexpr float64 DECADE_BASE = 10;

using RgbValue = StaticArray<float32, RGB_CHANNELS>;

float32 ComputeLuminance(const RgbValue& value) noexcept
{
    return value[0] * LUMINANCE_RED + value[1] * LUMINANCE_GREEN + value[2] * LUMINANCE_BLUE;
}

int32 ScaleDifference(float32 difference) noexcept
{
    const float32 scaled = math::Clamp(difference / REFERENCE_DIFFERENCE_FULL_SCALE, 0.0F, 1.0F);
    return static_cast<int32>(scaled * DIFFERENCE_CHANNEL_MAXIMUM + .5F);
}

QString GetSpaceName(ReferenceComparisonSpace space)
{
    return space == ReferenceComparisonSpace::Display ? QStringLiteral("display") : QStringLiteral("linear");
}
} // namespace

namespace
{
/// Compare reference means with another image of the same extent. readColor(x, y, outColor) supplies the other
/// image's stored colour: display values in [0, 1] for ReferenceComparisonSpace::Display, linear radiance for
/// Linear. It returns false for a pixel that has no value to compare.
template <typename ReadReference, typename ReadColor>
bool CompareWithReference(const ReadReference& readReference,
                          const renderer::ReferenceCaptureInfo& information,
                          bool hasReferenceFailed,
                          ReferenceComparisonSpace space,
                          bool isReferenceDisplayEncoded,
                          const ReadColor& readColor,
                          ReferenceComparison& outComparison,
                          QImage& outDifference,
                          QString& outError)
{
    const uint32 width = information.Width;
    const uint32 height = information.Height;
    const usize count = static_cast<usize>(width) * height;
    ReferenceComparison comparison;
    comparison.Space = space;
    comparison.Width = width;
    comparison.Height = height;
    comparison.TargetReferenceSamples = information.TargetSamples;
    comparison.HasReferenceFailed = hasReferenceFailed == true || information.InvalidContributions != 0;
    QImage difference(static_cast<int32>(width), static_cast<int32>(height), QImage::Format_RGB32);
    if (difference.isNull() == true)
    {
        outError = QStringLiteral("Could not allocate the difference image.");
        return false;
    }
    float64 absoluteSum = 0;
    float64 squaredSum = 0;
    float64 relativeSum = 0;
    float64 signedLuminanceSum = 0;
    uint64 visiblyDifferent = 0;
    uint32 minimumSamples = ~uint32{0};
    for (uint32 y = 0; y < height; ++y)
    {
        QRgb* row = reinterpret_cast<QRgb*>(difference.scanLine(static_cast<int32>(y)));
        for (uint32 x = 0; x < width; ++x)
        {
            const renderer::ReferencePixel& pixel = readReference(x, y);
            math::Vector3 storedColor{};
            if (pixel.Samples == 0 || readColor(x, y, storedColor) == false)
            {
                row[x] = qRgb(UNCOMPARED_GREY, UNCOMPARED_GREY, UNCOMPARED_GREY);
                continue;
            }
            const RgbValue referenceLinear{pixel.Radiance.X, pixel.Radiance.Y, pixel.Radiance.Z};
            RgbValue rasterValue{};
            RgbValue referenceValue{};
            RgbValue rasterDisplay{};
            RgbValue referenceDisplay{};
            const math::Vector3 referenceEncoded =
                isReferenceDisplayEncoded == true ? pixel.Radiance : EncodeReferenceDisplayColor(pixel.Radiance);
            const math::Vector3 rasterEncoded = EncodeReferenceDisplayColor(storedColor);
            for (usize channel = 0; channel < RGB_CHANNELS; ++channel)
            {
                const float32 stored = channel == 0 ? storedColor.X : channel == 1 ? storedColor.Y : storedColor.Z;
                referenceDisplay[channel] = channel == 0   ? referenceEncoded.X
                                            : channel == 1 ? referenceEncoded.Y
                                                           : referenceEncoded.Z;
                rasterDisplay[channel] = comparison.Space == ReferenceComparisonSpace::Linear
                                             ? (channel == 0   ? rasterEncoded.X
                                                : channel == 1 ? rasterEncoded.Y
                                                               : rasterEncoded.Z)
                                             : math::Clamp(stored, 0.0F, 1.0F);
                rasterValue[channel] =
                    comparison.Space == ReferenceComparisonSpace::Linear ? stored : rasterDisplay[channel];
                referenceValue[channel] = comparison.Space == ReferenceComparisonSpace::Linear
                                              ? referenceLinear[channel]
                                              : referenceDisplay[channel];
            }
            float64 largestDisplayError = 0;
            for (usize channel = 0; channel < RGB_CHANNELS; ++channel)
            {
                const float64 error =
                    static_cast<float64>(rasterValue[channel]) - static_cast<float64>(referenceValue[channel]);
                absoluteSum += math::Abs(error);
                squaredSum += error * error;
                comparison.MaximumAbsoluteError = math::Max(comparison.MaximumAbsoluteError, math::Abs(error));
                const float64 referenceChannel = static_cast<float64>(referenceValue[channel]);
                relativeSum += error * error / (referenceChannel * referenceChannel + REFERENCE_RELATIVE_ERROR_EPSILON);
                largestDisplayError =
                    math::Max(largestDisplayError,
                              math::Abs(static_cast<float64>(rasterDisplay[channel] - referenceDisplay[channel])));
            }
            signedLuminanceSum += static_cast<float64>(ComputeLuminance(rasterValue)) -
                                  static_cast<float64>(ComputeLuminance(referenceValue));
            if (largestDisplayError > REFERENCE_VISIBLE_DISPLAY_ERROR)
            {
                ++visiblyDifferent;
            }
            const float32 displayDifference = ComputeLuminance(rasterDisplay) - ComputeLuminance(referenceDisplay);
            row[x] = qRgb(ScaleDifference(displayDifference), 0, ScaleDifference(-displayDifference));
            minimumSamples = math::Min(minimumSamples, pixel.Samples);
            ++comparison.ComparedPixels;
        }
    }
    if (comparison.ComparedPixels == 0)
    {
        outError = QStringLiteral("No reference pixel has a committed sample on both sides yet.");
        return false;
    }
    const float64 pixels = static_cast<float64>(comparison.ComparedPixels);
    const float64 channels = pixels * static_cast<float64>(RGB_CHANNELS);
    comparison.MinimumReferenceSamples = minimumSamples;
    comparison.MinimumImageSamples = comparison.ComparedPixels == count ? minimumSamples : 0;
    comparison.IsReferenceComplete = comparison.HasReferenceFailed == false && information.TargetSamples != 0 &&
                                     comparison.MinimumImageSamples >= information.TargetSamples;
    comparison.MeanAbsoluteError = absoluteSum / channels;
    const float64 meanSquare = squaredSum / channels;
    comparison.RootMeanSquareError = math::Sqrt(meanSquare);
    comparison.MeanSignedLuminanceError = signedLuminanceSum / pixels;
    comparison.VisiblyDifferentFraction = static_cast<float64>(visiblyDifferent) / pixels;
    if (comparison.Space == ReferenceComparisonSpace::Display)
    {
        comparison.PeakSignalToNoiseRatio =
            meanSquare > 0 ? math::Min(REFERENCE_MAXIMUM_PSNR,
                                       -DECIBELS_PER_DECADE * math::Log(meanSquare) / math::Log(DECADE_BASE))
                           : REFERENCE_MAXIMUM_PSNR;
    }
    else
    {
        comparison.RelativeMeanSquareError = relativeSum / channels;
    }
    outComparison = comparison;
    outDifference = Move(difference);
    return true;
}
} // namespace

bool CompareRasterWithReference(BorrowedView<const renderer::ReferencePixel> reference,
                                const renderer::ReferenceCaptureInfo& information,
                                bool hasReferenceFailed,
                                const InspectedTexture& raster,
                                ReferenceComparison& outComparison,
                                QImage& outDifference,
                                QString& outError)
{
    const uint32 width = information.Width;
    const uint32 height = information.Height;
    const usize count = static_cast<usize>(width) * height;
    if (width == 0 || height == 0 || reference.GetSize() != count || raster.IsValid() == false ||
        raster.Width != width || raster.Height != height)
    {
        outError = QStringLiteral("The raster image (%1 × %2) does not match the reference extent (%3 × %4).")
                       .arg(raster.Width)
                       .arg(raster.Height)
                       .arg(width)
                       .arg(height);
        return false;
    }
    if (raster.Encoding == TexelEncoding::Depth32Float)
    {
        outError = QStringLiteral("The viewport output is a depth texture; choose a colour output to compare.");
        return false;
    }
    const auto readTexel = [&raster](uint32 x, uint32 y, math::Vector3& outColor) {
        TexelValue texel;
        if (ReadTexel(raster, x, y, texel) == false)
        {
            return false;
        }
        outColor = {texel.Channels[0], texel.Channels[1], texel.Channels[2]};
        return true;
    };
    const auto readReference = [&reference, width](uint32 x, uint32 y) -> const renderer::ReferencePixel& {
        return reference[static_cast<usize>(y) * width + x];
    };
    return CompareWithReference(readReference,
                                information,
                                hasReferenceFailed,
                                raster.Encoding == TexelEncoding::Rgba16Float ? ReferenceComparisonSpace::Linear
                                                                              : ReferenceComparisonSpace::Display,
                                false,
                                readTexel,
                                outComparison,
                                outDifference,
                                outError);
}

bool CompareGraphCaptureWithReference(BorrowedView<const renderer::ReferencePixel> reference,
                                      const renderer::ReferenceCaptureInfo& information,
                                      bool hasReferenceFailed,
                                      BorrowedView<const renderer::RenderGraphCapturePixel> capture,
                                      uint32 captureWidth,
                                      uint32 captureHeight,
                                      ReferenceComparison& outComparison,
                                      QImage& outDifference,
                                      QString& outError)
{
    const uint32 width = information.Width;
    const uint32 height = information.Height;
    const usize count = static_cast<usize>(width) * height;
    if (width == 0 || height == 0 || reference.GetSize() != count || capture.GetSize() != count ||
        captureWidth != width || captureHeight != height)
    {
        outError = QStringLiteral("The graph capture (%1 × %2) does not match the reference extent (%3 × %4).")
                       .arg(captureWidth)
                       .arg(captureHeight)
                       .arg(width)
                       .arg(height);
        return false;
    }
    // Linear means compare with the reference's means directly; a pixel without graph samples is uncompared.
    const auto readMean = [&capture, width](uint32 x, uint32 y, math::Vector3& outColor) {
        const renderer::RenderGraphCapturePixel& pixel = capture[static_cast<usize>(y) * width + x];
        outColor = pixel.Mean;
        return pixel.Samples != 0;
    };
    const auto readReference = [&reference, width](uint32 x, uint32 y) -> const renderer::ReferencePixel& {
        return reference[static_cast<usize>(y) * width + x];
    };
    return CompareWithReference(readReference,
                                information,
                                hasReferenceFailed,
                                ReferenceComparisonSpace::Linear,
                                false,
                                readMean,
                                outComparison,
                                outDifference,
                                outError);
}

bool CompareDisplayImages(const QImage& reference,
                          const QImage& candidate,
                          BorrowedView<const renderer::RenderGraphCapturePixel> samples,
                          ReferenceComparison& outComparison,
                          QImage& outDifference,
                          QString& outError)
{
    if (reference.isNull() == true || reference.size() != candidate.size())
    {
        outError = QStringLiteral("Export both display images at the same resolution.");
        return false;
    }
    renderer::ReferenceCaptureInfo information;
    information.Width = static_cast<uint32>(reference.width());
    information.Height = static_cast<uint32>(reference.height());
    information.TargetSamples = 1;
    if (samples.IsEmpty() == false && samples.GetSize() != usize{information.Width} * information.Height)
    {
        outError = QStringLiteral("The candidate sample counts do not match the display image.");
        return false;
    }
    const auto readReference = [&reference](uint32 x, uint32 y) {
        const QColor color = reference.pixelColor(static_cast<int32>(x), static_cast<int32>(y));
        return renderer::ReferencePixel{ .Radiance = {color.redF(), color.greenF(), color.blueF()}, .Samples = 1 };
    };
    const auto readCandidate =
        [&candidate, samples, width = information.Width](uint32 x, uint32 y, math::Vector3& outColor) {
            if (samples.IsEmpty() == false && samples[usize{y} * width + x].Samples == 0)
            {
                return false;
            }
            const QColor color = candidate.pixelColor(static_cast<int32>(x), static_cast<int32>(y));
            outColor = {color.redF(), color.greenF(), color.blueF()};
            return true;
        };
    return CompareWithReference(readReference,
                                information,
                                false,
                                ReferenceComparisonSpace::Display,
                                true,
                                readCandidate,
                                outComparison,
                                outDifference,
                                outError);
}

namespace
{
/// The mean of a colour's R, G and B, in float64.
[[nodiscard]] float64 ComputeChannelMean(math::Vector3 colour) noexcept
{
    constexpr float64 CHANNELS = 3;
    return (static_cast<float64>(colour.X) + static_cast<float64>(colour.Y) + static_cast<float64>(colour.Z)) /
           CHANNELS;
}
} // namespace

bool CompareColumnProfiles(BorrowedView<const renderer::ReferencePixel> reference,
                           BorrowedView<const renderer::RenderGraphCapturePixel> capture,
                           uint32 width,
                           uint32 height,
                           ColumnComparison& outComparison,
                           QString& outError)
{
    const usize pixels = static_cast<usize>(width) * height;
    if (width == 0 || reference.GetSize() != pixels || capture.GetSize() != pixels)
    {
        outError = QStringLiteral("The baseline and the capture differ in size. Render the candidate at the baseline's "
                                  "extent.");
        return false;
    }
    // First pass: the largest column, which sets the significance threshold.
    float64 largest = 0;
    for (uint32 column = 0; column < width; ++column)
    {
        float64 sum = 0;
        for (uint32 row = 0; row < height; ++row)
        {
            const usize pixel = static_cast<usize>(row) * width + column;
            if (reference[pixel].Samples > 0 && capture[pixel].Samples > 0)
            {
                sum += ComputeChannelMean(reference[pixel].Radiance);
            }
        }
        largest = math::Max(largest, sum);
    }
    if (largest <= 0)
    {
        outError = QStringLiteral("No pixel sampled by both images carries baseline light. Check that the baseline "
                                  "and the candidate show the same scene.");
        return false;
    }
    ColumnComparison comparison;
    float64 referenceTotal = 0;
    float64 captureTotal = 0;
    float64 worstError = -1;
    for (uint32 column = 0; column < width; ++column)
    {
        float64 referenceSum = 0;
        float64 captureSum = 0;
        for (uint32 row = 0; row < height; ++row)
        {
            const usize pixel = static_cast<usize>(row) * width + column;
            if (reference[pixel].Samples == 0 || capture[pixel].Samples == 0)
            {
                continue;
            }
            referenceSum += ComputeChannelMean(reference[pixel].Radiance);
            captureSum += ComputeChannelMean(capture[pixel].Mean);
        }
        referenceTotal += referenceSum;
        captureTotal += captureSum;
        if (referenceSum < REFERENCE_COLUMN_SIGNIFICANCE * largest)
        {
            continue;
        }
        ++comparison.SignificantColumns;
        const float64 ratio = captureSum / referenceSum;
        if (math::Abs(ratio - 1) > worstError)
        {
            worstError = math::Abs(ratio - 1);
            comparison.WorstColumn = column;
            comparison.WorstColumnRatio = ratio;
        }
    }
    comparison.TotalRatio = captureTotal / referenceTotal;
    comparison.IsWithinTolerance = worstError <= REFERENCE_COLUMN_TOLERANCE;
    outComparison = comparison;
    return true;
}

QJsonObject MakeColumnComparisonJson(const ColumnComparison& comparison)
{
    return QJsonObject{
        {QStringLiteral("significantColumns"), static_cast<qint64>(comparison.SignificantColumns)},
        {QStringLiteral("worstColumn"), static_cast<qint64>(comparison.WorstColumn)},
        {QStringLiteral("worstColumnRatio"), comparison.WorstColumnRatio},
        {QStringLiteral("totalRatio"), comparison.TotalRatio},
        {QStringLiteral("isWithinTolerance"), comparison.IsWithinTolerance},
        {QStringLiteral("tolerance"), REFERENCE_COLUMN_TOLERANCE},
        {QStringLiteral("significance"), REFERENCE_COLUMN_SIGNIFICANCE},
    };
}

QString DescribeColumnComparison(const ColumnComparison& comparison)
{
    constexpr int32 RATIO_DIGITS = 3;
    constexpr float64 PERCENT = 100;
    return QStringLiteral("Columns %1: worst %2 at column %3 of %4 significant, within ±%5% passes · total %6")
        .arg(comparison.IsWithinTolerance == true ? QStringLiteral("pass") : QStringLiteral("FAIL"))
        .arg(comparison.WorstColumnRatio, 0, 'f', RATIO_DIGITS)
        .arg(comparison.WorstColumn)
        .arg(comparison.SignificantColumns)
        .arg(REFERENCE_COLUMN_TOLERANCE * PERCENT)
        .arg(comparison.TotalRatio, 0, 'f', RATIO_DIGITS);
}

QJsonObject MakeReferenceComparisonJson(const ReferenceComparison& comparison)
{
    QJsonObject json{
        {QStringLiteral("space"), GetSpaceName(comparison.Space)},
        {QStringLiteral("width"), static_cast<qint64>(comparison.Width)},
        {QStringLiteral("height"), static_cast<qint64>(comparison.Height)},
        {QStringLiteral("comparedPixels"), static_cast<qint64>(comparison.ComparedPixels)},
        {QStringLiteral("minimumReferenceSamples"), static_cast<qint64>(comparison.MinimumReferenceSamples)},
        {QStringLiteral("targetReferenceSamples"), static_cast<qint64>(comparison.TargetReferenceSamples)},
        {QStringLiteral("minimumImageSamples"), static_cast<qint64>(comparison.MinimumImageSamples)},
        {QStringLiteral("referenceComplete"), comparison.IsReferenceComplete},
        {QStringLiteral("referenceFailed"), comparison.HasReferenceFailed},
        {QStringLiteral("totalPixels"), static_cast<qint64>(comparison.Width) * comparison.Height},
        {QStringLiteral("meanAbsoluteError"), comparison.MeanAbsoluteError},
        {QStringLiteral("rootMeanSquareError"), comparison.RootMeanSquareError},
        {QStringLiteral("maximumAbsoluteError"), comparison.MaximumAbsoluteError},
        {QStringLiteral("meanSignedLuminanceError"), comparison.MeanSignedLuminanceError},
        {QStringLiteral("visiblyDifferentFraction"), comparison.VisiblyDifferentFraction},
        {QStringLiteral("visibleDisplayErrorThreshold"), REFERENCE_VISIBLE_DISPLAY_ERROR},
    };
    if (comparison.Space == ReferenceComparisonSpace::Display)
    {
        json.insert(QStringLiteral("peakSignalToNoiseRatio"), comparison.PeakSignalToNoiseRatio);
    }
    else
    {
        json.insert(QStringLiteral("relativeMeanSquareError"), comparison.RelativeMeanSquareError);
        json.insert(QStringLiteral("relativeErrorEpsilon"), REFERENCE_RELATIVE_ERROR_EPSILON);
    }
    return json;
}

QString DescribeReferenceComparison(const ReferenceComparison& comparison)
{
    constexpr float64 PERCENT = 100;
    // Decimal places shown; errors are display units or radiance near one.
    constexpr int32 ERROR_DECIMALS = 4;
    constexpr int32 PSNR_DECIMALS = 2;
    constexpr int32 RELATIVE_ERROR_DECIMALS = 5;
    const QString bias =
        comparison.MeanSignedLuminanceError < 0 ? QStringLiteral("darker") : QStringLiteral("brighter");
    const uint64 totalPixels = static_cast<uint64>(comparison.Width) * comparison.Height;
    const QString state = comparison.HasReferenceFailed == true    ? QStringLiteral("FAILED / INCOMPLETE")
                          : comparison.IsReferenceComplete == true ? QStringLiteral("complete")
                                                                   : QStringLiteral("INCOMPLETE");
    QString text =
        QStringLiteral("Reference %1: %2/%3 pixels sampled (%4%); %5/%6 samples per pixel across the full image. ")
            .arg(state)
            .arg(comparison.ComparedPixels)
            .arg(totalPixels)
            .arg(totalPixels == 0
                     ? 0
                     : PERCENT * static_cast<float64>(comparison.ComparedPixels) / static_cast<float64>(totalPixels),
                 0,
                 'f',
                 1)
            .arg(comparison.MinimumImageSamples)
            .arg(comparison.TargetReferenceSamples);
    if (comparison.IsReferenceComplete == false)
    {
        text += QStringLiteral("Partial diagnostic metrics only. ");
    }
    text += QStringLiteral("Raster vs reference (%1 space, %2 pixels, ≥%3 samples each): "
                           "MAE %4, RMSE %5, max %6. Raster is %7 on average by %8 luminance. "
                           "%9% of pixels differ visibly.")
                .arg(GetSpaceName(comparison.Space))
                .arg(comparison.ComparedPixels)
                .arg(comparison.MinimumReferenceSamples)
                .arg(comparison.MeanAbsoluteError, 0, 'f', ERROR_DECIMALS)
                .arg(comparison.RootMeanSquareError, 0, 'f', ERROR_DECIMALS)
                .arg(comparison.MaximumAbsoluteError, 0, 'f', ERROR_DECIMALS)
                .arg(bias)
                .arg(math::Abs(comparison.MeanSignedLuminanceError), 0, 'f', ERROR_DECIMALS)
                .arg(comparison.VisiblyDifferentFraction * PERCENT, 0, 'f', 1);
    if (comparison.Space == ReferenceComparisonSpace::Display)
    {
        text += QStringLiteral(" PSNR %1 dB.").arg(comparison.PeakSignalToNoiseRatio, 0, 'f', PSNR_DECIMALS);
    }
    else
    {
        text += QStringLiteral(" relMSE %1.").arg(comparison.RelativeMeanSquareError, 0, 'f', RELATIVE_ERROR_DECIMALS);
    }
    return text;
}
} // namespace ludus::editor
