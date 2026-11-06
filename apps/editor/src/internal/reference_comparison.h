#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>

#include <ludus/graphics/renderer/reference.hpp>
#include <ludus/graphics/renderer/render_graph_capture.hpp>

#include "internal/texture_inspection.h"

#include <QImage>
#include <QJsonObject>
#include <QString>

namespace ludus::editor
{
/// Display-space error above which a pixel counts as visibly different: about 13 of 255 levels.
inline constexpr foundation::float64 REFERENCE_VISIBLE_DISPLAY_ERROR = 0.05;
/// Signed display-luminance difference drawn at full colour in the difference image.
inline constexpr foundation::float32 REFERENCE_DIFFERENCE_FULL_SCALE = 0.25F;
/// Denominator offset of the relative mean squared error, in linear radiance units; keeps dark
/// reference pixels from dominating, following the convention of Rousselle et al.
inline constexpr foundation::float64 REFERENCE_RELATIVE_ERROR_EPSILON = 0.01;
/// PSNR reported for identical display images, in decibels, instead of an infinite value.
inline constexpr foundation::float64 REFERENCE_MAXIMUM_PSNR = 100;

/// Space in which raster and reference values are compared, chosen by the raster output encoding.
enum class ReferenceComparisonSpace : foundation::uint8
{
    /// RGBA8 raster output is the displayed image; the reference passes through the viewport's
    /// display transform (exposure one, the viewport's tone curve, sRGB) before comparison. Values in [0, 1].
    Display,
    /// RGBA16F raster output is scene-linear radiance, compared with the raw reference means.
    Linear,
};

/// Raster-minus-reference error over the pixels that have committed reference samples.
/// Channel errors are averaged over R, G and B; alpha is ignored. The reference's own Monte Carlo
/// noise is included in every statistic, so compare captures with enough samples per pixel.
struct ReferenceComparison final
{
    ReferenceComparisonSpace Space = ReferenceComparisonSpace::Display;
    foundation::uint32 Width = 0;
    foundation::uint32 Height = 0;
    /// Pixels with at least one committed reference sample; the statistics cover only these.
    foundation::uint64 ComparedPixels = 0;
    /// Fewest committed reference samples among the compared pixels.
    foundation::uint32 MinimumReferenceSamples = 0;
    /// Requested samples per pixel; completion requires this count across the whole image.
    foundation::uint32 TargetReferenceSamples = 0;
    /// Fewest samples over the whole reference image, including unsampled pixels.
    foundation::uint32 MinimumImageSamples = 0;
    /// True only when every image pixel has reached the requested count without a capture failure.
    bool IsReferenceComplete = false;
    /// The capture terminated with an error; any metrics are partial diagnostics.
    bool HasReferenceFailed = false;
    /// Mean absolute channel error, in the comparison space.
    foundation::float64 MeanAbsoluteError = 0;
    /// Root mean squared channel error, in the comparison space.
    foundation::float64 RootMeanSquareError = 0;
    /// Largest absolute channel error, in the comparison space.
    foundation::float64 MaximumAbsoluteError = 0;
    /// Mean of raster minus reference Rec. 709 luminance, in the comparison space. Negative
    /// means the raster image is darker, for example where it omits indirect light.
    foundation::float64 MeanSignedLuminanceError = 0;
    /// Display space only: peak signal-to-noise ratio in decibels for a peak of one.
    foundation::float64 PeakSignalToNoiseRatio = 0;
    /// Linear space only: mean of squared error over squared reference plus epsilon, per channel.
    foundation::float64 RelativeMeanSquareError = 0;
    /// Fraction of compared pixels whose largest display-space channel error exceeds
    /// REFERENCE_VISIBLE_DISPLAY_ERROR; linear rasters are display-encoded for this test.
    foundation::float64 VisiblyDifferentFraction = 0;
};

/// Compare one raster output with reference means of the same extent and camera.
/// @param reference Width * Height top-left pixels, borrowed for the call.
/// @param information Capture dimensions and requested sample count, matching reference.
/// @param hasReferenceFailed Whether capture ended with an error, including backend failures.
/// @param raster Read-back raster output; RGBA8 compares in display space, RGBA16F in linear space.
/// @param outDifference Receives a signed display-luminance difference image: red where the raster
/// is brighter, blue where it is darker, black where they agree and grey where no reference sample
/// exists, saturating at REFERENCE_DIFFERENCE_FULL_SCALE.
/// @return False with outError (outputs unchanged) for mismatched extents, an invalid or depth
/// raster, or a reference without any committed sample.
[[nodiscard]] bool
CompareRasterWithReference(foundation::BorrowedView<const graphics::renderer::ReferencePixel> reference,
                           const graphics::renderer::ReferenceCaptureInfo& information,
                           bool hasReferenceFailed,
                           const InspectedTexture& raster,
                           ReferenceComparison& outComparison,
                           QImage& outDifference,
                           QString& outError);

/// Compare a render graph capture's linear means with reference means of the same extent and camera, in linear
/// space. Pixels without graph samples are left uncompared, like pixels without reference samples.
/// @param capture Width * Height top-left pixels from RenderGraphCapture, borrowed for the call.
/// @param captureWidth Capture width in pixels; must equal the reference's.
/// @param captureHeight Capture height in pixels; must equal the reference's.
/// @return False with outError (outputs unchanged) for mismatched extents or no pixel compared.
[[nodiscard]] bool
CompareGraphCaptureWithReference(foundation::BorrowedView<const graphics::renderer::ReferencePixel> reference,
                                 const graphics::renderer::ReferenceCaptureInfo& information,
                                 bool hasReferenceFailed,
                                 foundation::BorrowedView<const graphics::renderer::RenderGraphCapturePixel> capture,
                                 foundation::uint32 captureWidth,
                                 foundation::uint32 captureHeight,
                                 ReferenceComparison& outComparison,
                                 QImage& outDifference,
                                 QString& outError);

/// Compare equal-sized sRGB display images. A nonempty samples view masks uncommitted candidate pixels.
[[nodiscard]] bool
CompareDisplayImages(const QImage& reference,
                     const QImage& candidate,
                     foundation::BorrowedView<const graphics::renderer::RenderGraphCapturePixel> samples,
                     ReferenceComparison& outComparison,
                     QImage& outDifference,
                     QString& outError);

/// Largest accepted |candidate / reference - 1| of a significant column's summed radiance.
inline constexpr foundation::float64 REFERENCE_COLUMN_TOLERANCE = 0.05;
/// A column is significant when its summed reference radiance reaches this fraction of the largest column's.
inline constexpr foundation::float64 REFERENCE_COLUMN_SIGNIFICANCE = 0.01;

/// Column-profile comparison of a linear candidate with a linear reference. Each image column's RGB-mean radiance is
/// summed over the rows both images sampled, so Monte Carlo noise averages out along the column. It suits scenes
/// that vary little along the image's vertical axis, such as the edge-diffraction test scenes, whose oracle
/// references (tests/scenes/diffraction/references) a renderer's diffraction should reproduce column by column.
struct ColumnComparison final
{
    foundation::uint32 SignificantColumns =
        0;                              ///< Columns that hold at least REFERENCE_COLUMN_SIGNIFICANCE of the light.
    foundation::uint32 WorstColumn = 0; ///< The significant column whose ratio is furthest from one.
    foundation::float64 WorstColumnRatio = 1; ///< Candidate over reference sum of that column.
    foundation::float64 TotalRatio = 1;       ///< Candidate over reference sum of every compared pixel.
    /// Every significant column within REFERENCE_COLUMN_TOLERANCE.
    bool IsWithinTolerance = false;
};

/// Compare a render graph capture with linear reference pixels column by column.
/// @param reference Width * Height top-left pixels, borrowed for the call.
/// @param capture Width * Height top-left graph capture pixels, borrowed for the call.
/// @return False with outError (outputs unchanged) for mismatched sizes or a reference without light in any compared
/// pixel.
[[nodiscard]] bool
CompareColumnProfiles(foundation::BorrowedView<const graphics::renderer::ReferencePixel> reference,
                      foundation::BorrowedView<const graphics::renderer::RenderGraphCapturePixel> capture,
                      foundation::uint32 width,
                      foundation::uint32 height,
                      ColumnComparison& outComparison,
                      QString& outError);

/// Machine-readable record of a column comparison, with field names matching ColumnComparison.
[[nodiscard]] QJsonObject MakeColumnComparisonJson(const ColumnComparison& comparison);

/// One-line verdict, for example "Columns pass: worst 0.983 at column 125 of 230 within ±5%".
[[nodiscard]] QString DescribeColumnComparison(const ColumnComparison& comparison);

/// Machine-readable record of a comparison, with field names matching ReferenceComparison.
[[nodiscard]] QJsonObject MakeReferenceComparisonJson(const ReferenceComparison& comparison);

/// One-paragraph summary for the capture window.
[[nodiscard]] QString DescribeReferenceComparison(const ReferenceComparison& comparison);
} // namespace ludus::editor
