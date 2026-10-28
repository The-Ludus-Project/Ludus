#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>

#include <ludus/graphics/renderer/reference.hpp>

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

/// Machine-readable record of a comparison, with field names matching ReferenceComparison.
[[nodiscard]] QJsonObject MakeReferenceComparisonJson(const ReferenceComparison& comparison);

/// One-paragraph summary for the capture window.
[[nodiscard]] QString DescribeReferenceComparison(const ReferenceComparison& comparison);
} // namespace ludus::editor
