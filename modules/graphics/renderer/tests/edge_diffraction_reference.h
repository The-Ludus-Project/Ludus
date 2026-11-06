#pragma once

// Reference images and capture comparison for the edge-diffraction test scenes. The edge-diffraction oracle
// (edge_diffraction_oracle.h) renders a scene file's exact reference image from the file's camera into the LUHDR01
// layout that the reference renderer saves (Renderer::EncodeReferenceHdr; scripts/python/capture_hdr.py decodes it),
// so the editor, scripts/compare-render and this comparison read references and captures alike. The comparison
// averages each image column over every row before comparing, because the scenes vary little along y: Monte Carlo
// noise then averages out and a column's ratio measures the renderer's diffraction alone. docs/architecture/
// scene-source-format.md#diffraction-references describes the workflow.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/vector.hpp>

#include "edge_diffraction_oracle.h"

#include <span>

namespace ludus::graphics::renderer::testing
{
/// Bytes of a LUHDR01 header.
inline constexpr foundation::usize LUHDR_HEADER_BYTES = 256;
/// Bytes of one LUHDR01 pixel record: float32 R, G, B means and a uint32 sample count.
inline constexpr foundation::usize LUHDR_PIXEL_BYTES = 16;
/// LUHDR01 flags this module reads and writes.
inline constexpr foundation::uint32 LUHDR_CPU_ORACLE_FLAG = 1;
inline constexpr foundation::uint32 LUHDR_CENTER_RAYS_FLAG = 2;
inline constexpr foundation::uint32 LUHDR_REVERSE_Z_FLAG = 8;
inline constexpr foundation::uint32 LUHDR_OPTICAL_FLAG = 256;

/// A linear HDR image in the LUHDR01 layout, with the header fields a comparison needs.
struct DiffractionCapture final
{
    foundation::uint32 Width = 0;
    foundation::uint32 Height = 0;
    foundation::uint32 Flags = 0;
    foundation::float32 OpticalWavelengthWorld = 0;  ///< Zero for geometric transport.
    foundation::math::Matrix4 WorldToClip{};         ///< The camera's world-to-clip matrix.
    foundation::math::Vector3 CameraPositionWorld{}; ///< The camera's centre of projection.
    foundation::Array<foundation::float32> Radiance; ///< Width * Height RGB triples, row-major from the top row.
    foundation::Array<foundation::uint32> Samples;   ///< Committed samples per pixel; zero marks a missing pixel.
};

/// Decodes a LUHDR01 file. Returns false, leaving outCapture unchanged, for any other layout or a size mismatch.
[[nodiscard]] bool TryDecodeLuhdr(std::span<const foundation::uint8> bytes, DiffractionCapture& outCapture) noexcept;

/// Encodes a capture as LUHDR01: one committed sample per pixel with samples, the uniform-area light sampler and the
/// current lighting profile. Returns false, leaving outBytes unchanged, when the capture is inconsistent.
[[nodiscard]] bool TryEncodeLuhdr(const DiffractionCapture& capture,
                                  foundation::Array<foundation::uint8>& outBytes) noexcept;

/// An image's size in pixels.
struct DiffractionExtent final
{
    foundation::uint32 Width = 0;
    foundation::uint32 Height = 0;
};

/// Renders the scene's reference image from its camera: each pixel's centre ray finds its wall
/// point, whose radiance the settings integrate. The image is extent.Width x extent.Height. The camera matrix is the
/// authored camera's reverse-Z perspective with a 1 cm near and 1000 m far plane, as the editor's headless reference
/// capture builds it. Geometric references carry no wavelength; the others are marked optical at the settings'
/// wavelength. workerCount threads share the rows (0 renders on the calling thread). Returns false, leaving outCapture
/// unchanged, for invalid inputs or a model that does not apply to the scene.
[[nodiscard]] bool TryRenderDiffractionReference(const EdgeDiffractionScene& scene,
                                                 const DiffractionSettings& settings,
                                                 DiffractionExtent extent,
                                                 foundation::uint32 workerCount,
                                                 DiffractionCapture& outCapture) noexcept;

/// What a comparison accepts.
struct DiffractionTolerance final
{
    /// The largest accepted |capture / reference - 1| of a significant column's summed radiance.
    foundation::float64 ColumnRatio = 0.05;
    /// A column is significant when its summed reference radiance is at least this fraction of the largest column's.
    foundation::float64 SignificantFraction = 0.01;
    /// The largest accepted relative difference of the cameras' matrices and positions.
    foundation::float64 Camera = 1e-4;
};

/// The outcome of comparing a capture with a reference of the same extent.
struct DiffractionComparison final
{
    bool HasMatchingCamera = false;
    bool HasMatchingWavelength = false;
    foundation::uint32 ComparedPixels = 0;     ///< Pixels with samples in both images.
    foundation::uint32 SignificantColumns = 0; ///< Columns the column ratio checks.
    foundation::uint32 WorstColumn = 0;        ///< The significant column with the largest ratio error.
    foundation::float64 WorstColumnRatio = 1;  ///< capture / reference of that column.
    foundation::float64 TotalRatio = 1;        ///< capture / reference over all compared pixels.
    foundation::float64 RelativeL1 = 0;        ///< Sum of |capture - reference| over the reference's sum, per pixel.
    bool IsPassing = false; ///< Matching camera and wavelength, and every significant column within the tolerance.
};

/// Compares a capture with a reference: per-column sums of the RGB mean over pixels both images sampled. Returns false,
/// leaving outComparison unchanged, when the extents differ or no pixel is comparable.
[[nodiscard]] bool TryCompareDiffractionCapture(const DiffractionCapture& reference,
                                                const DiffractionCapture& capture,
                                                const DiffractionTolerance& tolerance,
                                                DiffractionComparison& outComparison) noexcept;

/// Writes the column profile as comma-separated text, one line per column: the column, the wall x of its centre-row
/// pixel (row Height / 2; empty when it misses the wall), the reference's centre-row radiance and its mean over the
/// column's compared pixels, the same two for the capture when one is given, and the ratio of the column sums. Empty
/// fields mark missing values. Returns false, leaving outText unchanged, when the extents differ.
[[nodiscard]] bool TryWriteDiffractionProfile(const EdgeDiffractionScene& scene,
                                              const DiffractionCapture& reference,
                                              const DiffractionCapture* captureOrNull,
                                              foundation::Array<foundation::uint8>& outText) noexcept;
} // namespace ludus::graphics::renderer::testing
