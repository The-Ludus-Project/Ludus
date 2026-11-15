// Analytic subpixel coverage benchmark for the interactive raster viewport's spatial antialiasing (issue 1414).
// The measured results and their limits are documented in docs/architecture/renderer-systems.md
// ("Measured coverage of spatial antialiasing").
//
// The comparator follows Edwin Catmull, "A Hidden-Surface Algorithm with Anti-Aliasing", SIGGRAPH '78, pp. 6-11
// (https://people.csail.mit.edu/ericchan/bib/pdf/p6-catmull.pdf): fine features pop in and out of successive frames
// under point sampling, and the reference pixel value integrates the visible intensities over the pixel. Here each
// projected polygon is clipped against the pixel square (a box filter) and the area hidden by a nearer polygon is
// subtracted.
//
// The three-surface strips apply the distinction of Andreas Schilling, "A New Simple and Efficient Antialiasing with
// Subpixel Masks", SIGGRAPH '91, pp. 133-141, between the true covered area and a sampled approximation, for a pixel
// shared by more than two objects. A stratified point estimate cross-checks the analytic areas.
//
// The corner, staircase and thin-feature regions are reported separately after Alexander Reshetov's MLAA
// retrospective ("Pros and cons") and Jorge Jimenez's slides 18-20, HPG 2017
// (https://www.highperformancegraphics.org/wp-content/uploads/2017/Retrospective/HPG2017_Reshetov_MLAARetrospective.pdf,
// https://www.iryoku.com/research-impact-retrospective-mlaa-from-2009-to-2017/), and Jimenez et al., "SMAA: Enhanced
// Subpixel Morphological Antialiasing", Computer Graphics Forum 31(2), 2012, sections 4.1-4.3 and Fig. 10
// (https://doi.org/10.1111/j.1467-8659.2012.03014.x): shape-aware blending improves edges present in the input, such
// as corners and long diagonal runs, but cannot recover coverage the sample lattice missed.
#include "renderer_composite.h"
#include "renderer_flat.h"
#include "renderer_output.h"
#include "renderer_shadow.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/viewport.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace
{
using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace shaders = ludus::shaders;
using S = rhi::RasterStatus;

constexpr uint32 VIEWPORT_WIDTH = 128;
constexpr uint32 VIEWPORT_HEIGHT = 64;
constexpr usize CHANNELS = 4;
constexpr usize PIXEL_COUNT = usize{VIEWPORT_WIDTH} * VIEWPORT_HEIGHT;
constexpr usize IMAGE_BYTES = PIXEL_COUNT * CHANNELS;
// Every feature moves diagonally, right and down, over one pixel in sixteen equal steps.
constexpr uint32 PHASE_COUNT = 16;
constexpr usize MODE_COUNT = 3;
constexpr StaticArray<rr::SpatialAntialiasing, MODE_COUNT> MODES{
    {rr::SpatialAntialiasing::Off, rr::SpatialAntialiasing::Fast, rr::SpatialAntialiasing::Balanced}};
constexpr StaticArray<const char*, MODE_COUNT> MODE_NAMES{{"off", "fast", "balanced"}};
constexpr usize OFF = 0;
constexpr usize FAST = 1;
constexpr usize BALANCED = 2;
// Feature vertices lie on odd multiples of 1/32 pixel, so no axis-aligned edge meets a pixel centre (a half pixel)
// at any phase (a multiple of 1/16). Sloped edges may pass close to a centre; those centres are excluded from the
// point-sampling check, which tolerates the rasterizer's fixed-point vertex snapping.
constexpr float64 GRID = 1.0 / 32;
constexpr float64 AMBIGUOUS_CENTRE_DISTANCE_PIXELS = 1.0 / 32;
constexpr float64 FAR_DISTANCE_PIXELS = 1e9;
constexpr float64 COVERAGE_EPSILON = 1e-9;
constexpr float64 DISPLAY_MAXIMUM = 255;
constexpr float64 SRGB_LINEAR_LIMIT = .0031308;
constexpr float64 SRGB_ENCODED_LIMIT = .04045;
constexpr float64 SRGB_LINEAR_SLOPE = 12.92;
constexpr float64 SRGB_SCALE = 1.055;
constexpr float64 SRGB_OFFSET = .055;
constexpr float64 SRGB_EXPONENT = 2.4;
// One display step of slack for the GPU's own rounding of the encoded value.
constexpr float64 DISPLAY_ROUNDING_STEPS = 1;
// A subject quadrilateral clipped by a square, then by a rectangle, gains at most one vertex per clipping edge.
constexpr usize MAX_POLYGON_VERTICES = 16;
constexpr uint64 TIMEOUT_TICKS = 10 * time::NANOSECONDS_PER_SECOND;

/// A point in viewport pixel space: x right, y down; pixel (i, j) covers [i, i + 1] x [j, j + 1].
struct PointPixel final
{
    float64 X = 0;
    float64 Y = 0;
};
/// A convex polygon in pixel space, in either winding.
struct Polygon final
{
    StaticArray<PointPixel, MAX_POLYGON_VERTICES> Vertices{};
    usize Count = 0;
};
Polygon MakePolygon(std::initializer_list<PointPixel> vertices)
{
    Polygon polygon;
    for (const PointPixel& vertex : vertices)
    {
        polygon.Vertices[polygon.Count++] = vertex;
    }
    return polygon;
}
Polygon MakeRectangle(float64 left, float64 top, float64 right, float64 bottom)
{
    return MakePolygon({{left, top}, {right, top}, {right, bottom}, {left, bottom}});
}
float64 ComputeCross(PointPixel origin, PointPixel a, PointPixel b)
{
    return (a.X - origin.X) * (b.Y - origin.Y) - (a.Y - origin.Y) * (b.X - origin.X);
}
float64 ComputeSignedArea(const Polygon& polygon)
{
    float64 twiceArea = 0;
    for (usize i = 0; i < polygon.Count; ++i)
    {
        const PointPixel& a = polygon.Vertices[i];
        const PointPixel& b = polygon.Vertices[(i + 1) % polygon.Count];
        twiceArea += a.X * b.Y - b.X * a.Y;
    }
    return twiceArea / 2;
}
float64 ComputeArea(const Polygon& polygon)
{
    return polygon.Count < 3 ? 0 : math::Abs(ComputeSignedArea(polygon));
}
// Sutherland-Hodgman clipping of a polygon against a convex clipper of either winding. The intersection of two
// convex polygons is symmetric, so exchanging the arguments yields the same region.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Polygon IntersectConvex(const Polygon& subject, const Polygon& clipper)
{
    const float64 orientation = ComputeSignedArea(clipper) >= 0 ? 1 : -1;
    Polygon result = subject;
    for (usize edge = 0; edge < clipper.Count && result.Count > 0; ++edge)
    {
        const PointPixel edgeStart = clipper.Vertices[edge];
        const PointPixel edgeEnd = clipper.Vertices[(edge + 1) % clipper.Count];
        const Polygon input = result;
        result.Count = 0;
        for (usize i = 0; i < input.Count; ++i)
        {
            const PointPixel current = input.Vertices[i];
            const PointPixel next = input.Vertices[(i + 1) % input.Count];
            const float64 currentSide = ComputeCross(edgeStart, edgeEnd, current) * orientation;
            const float64 nextSide = ComputeCross(edgeStart, edgeEnd, next) * orientation;
            if (currentSide >= 0)
            {
                LUDUS_ASSERT(result.Count < MAX_POLYGON_VERTICES);
                result.Vertices[result.Count++] = current;
            }
            if ((currentSide >= 0) != (nextSide >= 0))
            {
                const float64 t = currentSide / (currentSide - nextSide);
                LUDUS_ASSERT(result.Count < MAX_POLYGON_VERTICES);
                result.Vertices[result.Count++] = {current.X + (next.X - current.X) * t,
                                                   current.Y + (next.Y - current.Y) * t};
            }
        }
    }
    return result;
}
bool ContainsPoint(const Polygon& polygon, PointPixel point)
{
    const float64 orientation = ComputeSignedArea(polygon) >= 0 ? 1 : -1;
    for (usize i = 0; i < polygon.Count; ++i)
    {
        if (ComputeCross(polygon.Vertices[i], polygon.Vertices[(i + 1) % polygon.Count], point) * orientation < 0)
        {
            return false;
        }
    }
    return true;
}
float64 ComputeEdgeDistance(const Polygon& polygon, PointPixel point)
{
    float64 nearest = FAR_DISTANCE_PIXELS;
    for (usize i = 0; i < polygon.Count; ++i)
    {
        const PointPixel a = polygon.Vertices[i];
        const PointPixel b = polygon.Vertices[(i + 1) % polygon.Count];
        const float64 lengthSquared = (b.X - a.X) * (b.X - a.X) + (b.Y - a.Y) * (b.Y - a.Y);
        const float64 t =
            math::Clamp(((point.X - a.X) * (b.X - a.X) + (point.Y - a.Y) * (b.Y - a.Y)) / lengthSquared, 0.0, 1.0);
        const float64 x = a.X + (b.X - a.X) * t - point.X;
        const float64 y = a.Y + (b.Y - a.Y) * t - point.Y;
        nearest = math::Min(nearest, std::sqrt(x * x + y * y));
    }
    return nearest;
}

/// Linear RGB of a flat unlit material.
struct Color final
{
    float64 Red = 0;
    float64 Green = 0;
    float64 Blue = 0;
};
/// One opaque flat polygon of the fixture. Smaller depth is nearer (conventional depth, strict Less).
struct Feature final
{
    Polygon ShapePixel;
    float32 Depth = 0;
    Color Linear;
};
constexpr usize OBLIQUE = 0;
constexpr usize THIN = 1;
constexpr usize FAR_STRIP = 2;
constexpr usize NEAR_STRIP = 3;
constexpr usize CORNER = 4;
constexpr usize STAIRCASE = 5;
constexpr usize FEATURE_COUNT = 6;
using Features = StaticArray<Feature, FEATURE_COUNT>;
constexpr Color WHITE{1, 1, 1};
constexpr Color RED{1, 0, 0};
constexpr Color GREEN{0, 1, 0};
constexpr Color BACKGROUND{0, 0, 1};
constexpr float32 FEATURE_DEPTH = .5F;
constexpr float32 NEAR_DEPTH = .3F;
constexpr float32 FAR_DEPTH = .6F;
constexpr float32 BACKGROUND_DEPTH = .9F;

// Fixture layout at phase zero, in pixels. Features stay at least five pixels apart, so no filter neighbourhood
// across an edge spans two of them.
Features MakeFeatures()
{
    Features features{};
    // An oblique opaque edge, about 20 degrees from vertical; its other edges lie outside the measured window.
    features[OBLIQUE] = {
        MakePolygon({{6 + GRID, 4 + GRID}, {34 + GRID, 4 + GRID}, {34 + GRID, 58 + GRID}, {26 + GRID, 58 + GRID}}),
        FEATURE_DEPTH,
        WHITE};
    // An isolated thin vertical strip, 10/32 pixel wide: most phases place no pixel centre inside it.
    features[THIN] = {MakeRectangle(40 + 5 * GRID, 8 + GRID, 40 + 15 * GRID, 56 + GRID), FEATURE_DEPTH, WHITE};
    // Two overlapping strips at distinct depths: at phase zero one pixel shows the background (10/32), the visible
    // part of the far strip (8/32) and the near strip (14/32), and the silhouettes overlap by 8/32.
    features[FAR_STRIP] = {MakeRectangle(52 + 5 * GRID, 8 + GRID, 52 + 21 * GRID, 56 + GRID), FAR_DEPTH, RED};
    features[NEAR_STRIP] = {MakeRectangle(52 + 13 * GRID, 8 + GRID, 52 + 27 * GRID, 56 + GRID), NEAR_DEPTH, GREEN};
    // An acute wedge, about 20 degrees at its apex (106, 13).
    features[CORNER] = {MakePolygon({{66 + GRID, 8 + GRID}, {106 + GRID, 13 + GRID}, {66 + GRID, 22 + GRID}}),
                        FEATURE_DEPTH,
                        WHITE};
    // A silhouette rising one pixel in eight: a staircase of eight-pixel runs.
    features[STAIRCASE] = {
        MakePolygon({{66 + GRID, 40 + GRID}, {122 + GRID, 47 + GRID}, {122 + GRID, 60 + GRID}, {66 + GRID, 60 + GRID}}),
        FEATURE_DEPTH,
        WHITE};
    return features;
}
// The features as projected at one phase: the camera translates the image right and down by phase pixels.
Features TranslateFeatures(const Features& features, float64 phase)
{
    Features translated = features;
    for (usize i = 0; i < FEATURE_COUNT; ++i)
    {
        for (usize vertex = 0; vertex < translated[i].ShapePixel.Count; ++vertex)
        {
            translated[i].ShapePixel.Vertices[vertex].X += phase;
            translated[i].ShapePixel.Vertices[vertex].Y += phase;
        }
    }
    return translated;
}

/// Box-filtered visible area of each feature in one pixel; the background takes the rest.
struct PixelCoverage final
{
    StaticArray<float64, FEATURE_COUNT> Visible{};
    float64 Background = 1;
};
PixelCoverage ComputeCoverage(const Features& projected, usize x, usize y)
{
    const Polygon pixel = MakeRectangle(float64(x), float64(y), float64(x + 1), float64(y + 1));
    PixelCoverage coverage;
    for (usize i = 0; i < FEATURE_COUNT; ++i)
    {
        const Polygon inside = IntersectConvex(projected[i].ShapePixel, pixel);
        float64 visible = ComputeArea(inside);
        // Subtract the part of this pixel hidden by each nearer feature. This is exact while no two nearer features
        // overlap each other inside it, which holds for this fixture: only the two strips overlap.
        for (usize j = 0; j < FEATURE_COUNT && visible > 0; ++j)
        {
            if (projected[j].Depth < projected[i].Depth)
            {
                visible -= ComputeArea(IntersectConvex(inside, projected[j].ShapePixel));
            }
        }
        coverage.Visible[i] = math::Max(visible, 0.0);
        coverage.Background -= coverage.Visible[i];
    }
    return coverage;
}
Color ComposeLinear(const Features& projected, const PixelCoverage& coverage)
{
    Color color{BACKGROUND.Red * coverage.Background,
                BACKGROUND.Green * coverage.Background,
                BACKGROUND.Blue * coverage.Background};
    for (usize i = 0; i < FEATURE_COUNT; ++i)
    {
        color.Red += projected[i].Linear.Red * coverage.Visible[i];
        color.Green += projected[i].Linear.Green * coverage.Visible[i];
        color.Blue += projected[i].Linear.Blue * coverage.Visible[i];
    }
    return color;
}
/// Nearest feature at a point, or FEATURE_COUNT for the background.
usize FindNearest(const Features& projected, PointPixel point)
{
    usize nearest = FEATURE_COUNT;
    for (usize i = 0; i < FEATURE_COUNT; ++i)
    {
        if (ContainsPoint(projected[i].ShapePixel, point) == true &&
            (nearest == FEATURE_COUNT || projected[i].Depth < projected[nearest].Depth))
        {
            nearest = i;
        }
    }
    return nearest;
}
bool IsCentreAmbiguous(const Features& projected, PointPixel centre)
{
    for (usize i = 0; i < FEATURE_COUNT; ++i)
    {
        if (ComputeEdgeDistance(projected[i].ShapePixel, centre) < AMBIGUOUS_CENTRE_DISTANCE_PIXELS)
        {
            return true;
        }
    }
    return false;
}

// The documented output conversion of a linear RGBA8 scene value: clip to [0,1] (the attachment), per-channel
// Reinhard, then the sRGB encode, on the 8-bit display scale (unrounded).
float64 ConvertLinearToDisplay(float64 linear)
{
    const float64 clipped = math::Clamp(linear, 0.0, 1.0);
    const float64 toneMapped = clipped / (1 + clipped);
    const float64 encoded = toneMapped <= SRGB_LINEAR_LIMIT
                                ? toneMapped * SRGB_LINEAR_SLOPE
                                : SRGB_SCALE * std::pow(toneMapped, 1 / SRGB_EXPONENT) - SRGB_OFFSET;
    return encoded * DISPLAY_MAXIMUM;
}
// Inverse of ConvertLinearToDisplay below its clip, used to recover a linear contribution from a captured channel.
float64 ConvertDisplayToLinear(uint8 value)
{
    const float64 encoded = float64(value) / DISPLAY_MAXIMUM;
    const float64 toneMapped = encoded <= SRGB_ENCODED_LIMIT
                                   ? encoded / SRGB_LINEAR_SLOPE
                                   : std::pow((encoded + SRGB_OFFSET) / SRGB_SCALE, SRGB_EXPONENT);
    return toneMapped / (1 - toneMapped);
}
float64 GetChannelError(const uint8* captured, const Color& expected)
{
    const float64 red = math::Abs(float64(captured[0]) - ConvertLinearToDisplay(expected.Red));
    const float64 green = math::Abs(float64(captured[1]) - ConvertLinearToDisplay(expected.Green));
    const float64 blue = math::Abs(float64(captured[2]) - ConvertLinearToDisplay(expected.Blue));
    return math::Max(red, math::Max(green, blue));
}

/// Half-open pixel rectangle in which a region's metrics are measured.
struct Window final
{
    usize Left = 0;
    usize Top = 0;
    usize Right = 0;
    usize Bottom = 0;
};
bool IsInside(const Window& window, usize x, usize y)
{
    return x >= window.Left && x < window.Right && y >= window.Top && y < window.Bottom;
}
// Measured windows keep each feature's other edges at least four pixels away, wider than the filter's neighbourhood
// across an edge. The corner window holds the last eight pixels of the wedge.
constexpr Window OBLIQUE_WINDOW{4, 10, 30, 52};
constexpr Window THIN_WINDOW{36, 12, 46, 52};
constexpr Window STRIPS_WINDOW{48, 12, 58, 52};
constexpr Window CORNER_WINDOW{98, 8, 110, 19};
constexpr Window STAIRCASE_WINDOW{72, 36, 116, 52};
// Flat controls: background only, and the interior of the staircase feature.
constexpr Window BACKGROUND_WINDOW{112, 2, 127, 30};
constexpr Window INTERIOR_WINDOW{72, 50, 116, 58};
// Row on which the stratified estimate cross-checks the analytic three-surface areas; every strip row is alike.
constexpr usize STRATIFIED_ROW = 30;
constexpr usize STRATIFIED_SAMPLES = 64;
// A straight edge misplaces at most half a stratum of each 64-sample row; four edges cross a strip pixel.
constexpr float64 STRATIFIED_TOLERANCE = 4.0 / (2 * STRATIFIED_SAMPLES);
// A strip's colour counts as present in a frame when some pixel recovers more than this visible weight of it.
constexpr float64 COMPONENT_PRESENT_WEIGHT = .02;
// Pixels at least half covered by the wedge are inside the corner.
constexpr float64 INSIDE_CORNER_COVERAGE = .5;

/// Error of captured display values against the expected display values, in 8-bit display steps, or of recovered
/// visible weights against the analytic areas.
struct ErrorSummary final
{
    float64 Sum = 0;
    float64 Maximum = 0;
    usize Count = 0;
    void Add(float64 error)
    {
        Sum += error;
        Maximum = math::Max(Maximum, error);
        ++Count;
    }
    [[nodiscard]] float64 GetMean() const
    {
        return Count == 0 ? 0 : Sum / float64(Count);
    }
};
constexpr usize STRIP_COMPONENT_COUNT = 3;
constexpr usize FAR_COMPONENT = 0;
constexpr usize NEAR_COMPONENT = 1;
constexpr usize BACKGROUND_COMPONENT = 2;
/// Results of one antialiasing mode over all phases.
struct ModeReport final
{
    ErrorSummary Oblique;
    ErrorSummary Corner;
    ErrorSummary Staircase;
    ErrorSummary StripsColor;
    StaticArray<ErrorSummary, STRIP_COMPONENT_COUNT> StripsComponent{};
    /// |recovered far + near + background weight - 1|: display-space blending does not preserve the partition.
    ErrorSummary StripsWeightSum;
    float64 FlatMaximum = 0;
    usize ThinVanishedFrames = 0;
    float64 ThinContrastMinimum = FAR_DISTANCE_PIXELS;
    float64 ThinContrastMaximum = 0;
    usize FarMissingFrames = 0;
    usize NearMissingFrames = 0;
    float64 CornerInsideExpected = 0;
    float64 CornerInsideLost = 0;
    usize PointMismatches = 0;
    usize PointChecked = 0;
    bool HasInvalidCoverage = false;
    bool IsControlInBand = false;
    [[nodiscard]] float64 GetCornerInsideLoss() const
    {
        return CornerInsideExpected > 0 ? CornerInsideLost / CornerInsideExpected : 0;
    }
};
const char* GetBackendName(rhi::Backend backend)
{
    switch (backend)
    {
        case rhi::Backend::Vulkan:
            return "vulkan";
        case rhi::Backend::Metal:
            return "metal";
        case rhi::Backend::WebGPU:
            return "webgpu";
        case rhi::Backend::WebGL2:
            return "webgl2";
        case rhi::Backend::Direct3D12:
            return "direct3d12";
    }
    return "unknown";
}
bool IsSampleMismatched(const uint8* captured, const Color& sampled)
{
    return math::Abs(float64(captured[0]) - std::round(ConvertLinearToDisplay(sampled.Red))) > DISPLAY_ROUNDING_STEPS ||
           math::Abs(float64(captured[1]) - std::round(ConvertLinearToDisplay(sampled.Green))) >
               DISPLAY_ROUNDING_STEPS ||
           math::Abs(float64(captured[2]) - std::round(ConvertLinearToDisplay(sampled.Blue))) > DISPLAY_ROUNDING_STEPS;
}

// Accumulate one captured frame into its mode's report.
void MeasureFrame(const Features& projected, bool isPointSampled, const uint8* pixels, ModeReport& inoutReport)
{
    // Expected coverage of every pixel, and which pixels have a fractional box filter (an edge).
    static StaticArray<PixelCoverage, PIXEL_COUNT> coverage{};
    static StaticArray<bool, PIXEL_COUNT> isFractional{};
    for (usize y = 0; y < VIEWPORT_HEIGHT; ++y)
    {
        for (usize x = 0; x < VIEWPORT_WIDTH; ++x)
        {
            const usize index = y * VIEWPORT_WIDTH + x;
            coverage[index] = ComputeCoverage(projected, x, y);
            const float64 background = coverage[index].Background;
            inoutReport.HasInvalidCoverage = inoutReport.HasInvalidCoverage == true || background < -COVERAGE_EPSILON ||
                                             background > 1 + COVERAGE_EPSILON;
            bool isEdge = background > COVERAGE_EPSILON && background < 1 - COVERAGE_EPSILON;
            for (usize i = 0; i < FEATURE_COUNT; ++i)
            {
                const float64 visible = coverage[index].Visible[i];
                isEdge = isEdge == true || (visible > COVERAGE_EPSILON && visible < 1 - COVERAGE_EPSILON);
            }
            isFractional[index] = isEdge;
        }
    }
    // The edge band: fractional pixels and their eight neighbours, which a display-space filter may also change.
    const auto isInBand = [&](usize x, usize y) {
        for (usize j = (y == 0 ? 0 : y - 1); j <= y + 1 && j < VIEWPORT_HEIGHT; ++j)
        {
            for (usize i = (x == 0 ? 0 : x - 1); i <= x + 1 && i < VIEWPORT_WIDTH; ++i)
            {
                if (isFractional[j * VIEWPORT_WIDTH + i] == true)
                {
                    return true;
                }
            }
        }
        return false;
    };
    float64 thinExpected = 0;
    float64 thinCaptured = 0;
    bool hasFar = false;
    bool hasNear = false;
    for (usize y = 0; y < VIEWPORT_HEIGHT; ++y)
    {
        for (usize x = 0; x < VIEWPORT_WIDTH; ++x)
        {
            const usize index = y * VIEWPORT_WIDTH + x;
            const uint8* captured = pixels + index * CHANNELS;
            const Color expected = ComposeLinear(projected, coverage[index]);
            const float64 error = GetChannelError(captured, expected);
            const bool isBand = isInBand(x, y);
            if (isPointSampled == true)
            {
                // Off takes one sample at the pixel centre: it must show the nearest feature there.
                const PointPixel centre{float64(x) + .5, float64(y) + .5};
                if (IsCentreAmbiguous(projected, centre) == false)
                {
                    const usize nearest = FindNearest(projected, centre);
                    ++inoutReport.PointChecked;
                    inoutReport.PointMismatches +=
                        IsSampleMismatched(captured, nearest == FEATURE_COUNT ? BACKGROUND : projected[nearest].Linear)
                            ? 1
                            : 0;
                }
            }
            if (IsInside(BACKGROUND_WINDOW, x, y) == true || IsInside(INTERIOR_WINDOW, x, y) == true)
            {
                inoutReport.IsControlInBand = inoutReport.IsControlInBand == true || isBand == true;
                inoutReport.FlatMaximum = math::Max(inoutReport.FlatMaximum, error);
            }
            if (IsInside(OBLIQUE_WINDOW, x, y) == true && isBand == true)
            {
                inoutReport.Oblique.Add(error);
            }
            if (IsInside(STAIRCASE_WINDOW, x, y) == true && isBand == true)
            {
                inoutReport.Staircase.Add(error);
            }
            if (IsInside(CORNER_WINDOW, x, y) == true)
            {
                if (isBand == true)
                {
                    inoutReport.Corner.Add(error);
                }
                // Contrast against the background lives in the red channel, which the background lacks.
                if (coverage[index].Visible[CORNER] >= INSIDE_CORNER_COVERAGE)
                {
                    const float64 expectedRed = ConvertLinearToDisplay(expected.Red);
                    inoutReport.CornerInsideExpected += expectedRed;
                    inoutReport.CornerInsideLost += math::Max(expectedRed - float64(captured[0]), 0.0);
                }
            }
            if (IsInside(THIN_WINDOW, x, y) == true)
            {
                thinExpected += ConvertLinearToDisplay(expected.Red);
                thinCaptured += float64(captured[0]);
            }
            if (IsInside(STRIPS_WINDOW, x, y) == true)
            {
                // Each surface owns one primary, so each channel recovers one visible weight.
                const float64 farWeight = ConvertDisplayToLinear(captured[0]);
                const float64 nearWeight = ConvertDisplayToLinear(captured[1]);
                const float64 backgroundWeight = ConvertDisplayToLinear(captured[2]);
                hasFar = hasFar == true || farWeight > COMPONENT_PRESENT_WEIGHT;
                hasNear = hasNear == true || nearWeight > COMPONENT_PRESENT_WEIGHT;
                if (isBand == true)
                {
                    inoutReport.StripsColor.Add(error);
                    inoutReport.StripsComponent[FAR_COMPONENT].Add(
                        math::Abs(farWeight - coverage[index].Visible[FAR_STRIP]));
                    inoutReport.StripsComponent[NEAR_COMPONENT].Add(
                        math::Abs(nearWeight - coverage[index].Visible[NEAR_STRIP]));
                    inoutReport.StripsComponent[BACKGROUND_COMPONENT].Add(
                        math::Abs(backgroundWeight - coverage[index].Background));
                    inoutReport.StripsWeightSum.Add(math::Abs(farWeight + nearWeight + backgroundWeight - 1));
                }
            }
        }
    }
    // Every phase has nonzero expected coverage of the thin strip and of both strips' visible parts.
    const float64 thinRatio = thinCaptured / thinExpected;
    inoutReport.ThinContrastMinimum = math::Min(inoutReport.ThinContrastMinimum, thinRatio);
    inoutReport.ThinContrastMaximum = math::Max(inoutReport.ThinContrastMaximum, thinRatio);
    inoutReport.ThinVanishedFrames += thinCaptured > 0 ? 0 : 1;
    inoutReport.FarMissingFrames += hasFar == false ? 1 : 0;
    inoutReport.NearMissingFrames += hasNear == false ? 1 : 0;
}

/// Analytic properties of one phase, independent of any capture.
struct PhaseExpectation final
{
    bool IsThinSampled = false;
    bool IsFarSampled = false;
    bool IsNearSampled = false;
    bool IsOverlapSampled = false;
};
// Cross-check the analytic three-surface areas against a stratified point estimate, and record which surfaces the
// pixel centres of this phase reach.
PhaseExpectation CheckPhase(const Features& projected)
{
    PhaseExpectation expectation;
    for (usize x = STRIPS_WINDOW.Left; x < STRIPS_WINDOW.Right; ++x)
    {
        const PixelCoverage coverage = ComputeCoverage(projected, x, STRATIFIED_ROW);
        float64 farCount = 0;
        float64 nearCount = 0;
        float64 backgroundCount = 0;
        for (usize j = 0; j < STRATIFIED_SAMPLES; ++j)
        {
            for (usize i = 0; i < STRATIFIED_SAMPLES; ++i)
            {
                const PointPixel sample{float64(x) + (float64(i) + .5) / STRATIFIED_SAMPLES,
                                        float64(STRATIFIED_ROW) + (float64(j) + .5) / STRATIFIED_SAMPLES};
                const usize nearest = FindNearest(projected, sample);
                farCount += nearest == FAR_STRIP ? 1 : 0;
                nearCount += nearest == NEAR_STRIP ? 1 : 0;
                backgroundCount += nearest == FEATURE_COUNT ? 1 : 0;
            }
        }
        constexpr float64 SAMPLE_COUNT = float64(STRATIFIED_SAMPLES * STRATIFIED_SAMPLES);
        CAPTURE(x, coverage.Visible[FAR_STRIP], coverage.Visible[NEAR_STRIP], coverage.Background);
        // The three visible weights partition the pixel, and each agrees with its sampled estimate.
        CHECK(coverage.Background >= -COVERAGE_EPSILON);
        CHECK(math::Abs(coverage.Visible[FAR_STRIP] + coverage.Visible[NEAR_STRIP] + coverage.Background - 1) <
              COVERAGE_EPSILON);
        CHECK(math::Abs(coverage.Visible[FAR_STRIP] - farCount / SAMPLE_COUNT) <= STRATIFIED_TOLERANCE);
        CHECK(math::Abs(coverage.Visible[NEAR_STRIP] - nearCount / SAMPLE_COUNT) <= STRATIFIED_TOLERANCE);
        CHECK(math::Abs(coverage.Background - backgroundCount / SAMPLE_COUNT) <= STRATIFIED_TOLERANCE);
        const PointPixel centre{float64(x) + .5, float64(STRATIFIED_ROW) + .5};
        const usize nearest = FindNearest(projected, centre);
        expectation.IsFarSampled = expectation.IsFarSampled == true || nearest == FAR_STRIP;
        expectation.IsNearSampled = expectation.IsNearSampled == true || nearest == NEAR_STRIP;
        expectation.IsOverlapSampled =
            expectation.IsOverlapSampled == true || (ContainsPoint(projected[FAR_STRIP].ShapePixel, centre) == true &&
                                                     ContainsPoint(projected[NEAR_STRIP].ShapePixel, centre) == true);
    }
    for (usize y = THIN_WINDOW.Top; y < THIN_WINDOW.Bottom; ++y)
    {
        for (usize x = THIN_WINDOW.Left; x < THIN_WINDOW.Right; ++x)
        {
            expectation.IsThinSampled =
                expectation.IsThinSampled == true || FindNearest(projected, {float64(x) + .5, float64(y) + .5}) == THIN;
        }
    }
    return expectation;
}

void ReadViewportImage(rhi::DeviceHandle device, const rr::ViewportImage& image, uint8* output)
{
    // Acceptance only: the production viewport never reads its output back.
    rhi::ReadbackTicket ticket;
    S status = rhi::RequestTextureReadback(device,
                                           image.Texture,
                                           {0, 0, 0, image.Width, image.Height},
                                           usize(image.Width) * CHANNELS,
                                           ticket);
    const uint64 start = time::NowTicks();
    while (status == S::Pending && time::NowTicks() - start < TIMEOUT_TICKS)
    {
        status = rhi::GetStatus(device, ticket);
    }
    REQUIRE(status == S::Ready);
    REQUIRE(rhi::CopyReadback(device, ticket, output, IMAGE_BYTES) == S::Ready);
    REQUIRE(rhi::Release(device, ticket) == S::Ready);
}
void PrintReport(const char* backend, usize mode, const ModeReport& report)
{
    std::printf("AACOVERAGE,%s,%ux%u,%s,oblique=%.2f/%.2f,staircase=%.2f/%.2f,corner=%.2f/%.2f,"
                "corner_inside_loss=%.3f,thin_vanished=%zu/%u,thin_contrast=%.3f..%.3f,"
                "strips_color=%.2f/%.2f,strips_far=%.3f/%.3f,strips_near=%.3f/%.3f,"
                "strips_background=%.3f/%.3f,strips_sum=%.3f/%.3f,far_missing=%zu,near_missing=%zu,"
                "flat=%.2f,point_mismatches=%zu/%zu\n",
                backend,
                VIEWPORT_WIDTH,
                VIEWPORT_HEIGHT,
                MODE_NAMES[mode],
                report.Oblique.GetMean(),
                report.Oblique.Maximum,
                report.Staircase.GetMean(),
                report.Staircase.Maximum,
                report.Corner.GetMean(),
                report.Corner.Maximum,
                report.GetCornerInsideLoss(),
                report.ThinVanishedFrames,
                PHASE_COUNT,
                report.ThinContrastMinimum,
                report.ThinContrastMaximum,
                report.StripsColor.GetMean(),
                report.StripsColor.Maximum,
                report.StripsComponent[FAR_COMPONENT].GetMean(),
                report.StripsComponent[FAR_COMPONENT].Maximum,
                report.StripsComponent[NEAR_COMPONENT].GetMean(),
                report.StripsComponent[NEAR_COMPONENT].Maximum,
                report.StripsComponent[BACKGROUND_COMPONENT].GetMean(),
                report.StripsComponent[BACKGROUND_COMPONENT].Maximum,
                report.StripsWeightSum.GetMean(),
                report.StripsWeightSum.Maximum,
                report.FarMissingFrames,
                report.NearMissingFrames,
                report.FlatMaximum,
                report.PointMismatches,
                report.PointChecked);
}
} // namespace

TEST_CASE("Interactive raster viewport antialiasing is measured against analytic subpixel coverage",
          "[renderer][gpu][viewport][antialiasing]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    const rhi::DeviceStatus started =
        rhi::CreateDevice({}, { .Width = VIEWPORT_WIDTH, .Height = VIEWPORT_HEIGHT }, description, device, surface);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    // One mesh per feature and one for the background.
    constexpr usize ITEM_COUNT = FEATURE_COUNT + 1;
    rr::Limits limits;
    limits.Meshes = uint32(ITEM_COUNT);
    rr::Renderer renderer;
    REQUIRE(renderer.Initialize(device, shaders::renderer_flat::Vertex(), shaders::renderer_flat::Fragment(), limits) ==
            S::Ready);
    REQUIRE(renderer.InitializeViews(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment()) ==
            S::Ready);
    REQUIRE(renderer.InitializeLighting(shaders::renderer_output::Vertex(),
                                        shaders::renderer_output::Fragment(),
                                        shaders::renderer_shadow::Fragment()) == S::Ready);

    // Clip-space meshes (the camera is identity at phase zero): one fan per convex feature, wound counterclockwise
    // in clip space, and a background quad that covers the viewport at every phase.
    const Features features = MakeFeatures();
    constexpr float64 BACKGROUND_MARGIN_PIXELS = 2;
    constexpr usize MAX_FAN_INDICES = 3 * MAX_POLYGON_VERTICES;
    StaticArray<rr::Mesh, ITEM_COUNT> meshes{};
    StaticArray<rr::SceneItem, ITEM_COUNT> items{};
    for (usize item = 0; item < ITEM_COUNT; ++item)
    {
        const bool isBackground = item == FEATURE_COUNT;
        const Polygon shape = isBackground == true ? MakeRectangle(-BACKGROUND_MARGIN_PIXELS,
                                                                   -BACKGROUND_MARGIN_PIXELS,
                                                                   VIEWPORT_WIDTH + BACKGROUND_MARGIN_PIXELS,
                                                                   VIEWPORT_HEIGHT + BACKGROUND_MARGIN_PIXELS)
                                                   : features[item].ShapePixel;
        const float32 depth = isBackground == true ? BACKGROUND_DEPTH : features[item].Depth;
        const Color color = isBackground == true ? BACKGROUND : features[item].Linear;
        StaticArray<math::Vector3, MAX_POLYGON_VERTICES> positionsClip{};
        for (usize i = 0; i < shape.Count; ++i)
        {
            positionsClip[i] = {float32(2 * shape.Vertices[i].X / VIEWPORT_WIDTH - 1),
                                float32(1 - 2 * shape.Vertices[i].Y / VIEWPORT_HEIGHT),
                                depth};
        }
        // Pixel space has y down, so a polygon counterclockwise in clip space has negative pixel-space area.
        const bool isReversed = ComputeSignedArea(shape) > 0;
        StaticArray<uint32, MAX_FAN_INDICES> indices{};
        usize indexCount = 0;
        for (usize i = 1; i + 1 < shape.Count; ++i)
        {
            indices[indexCount++] = 0;
            indices[indexCount++] = uint32(isReversed == true ? i + 1 : i);
            indices[indexCount++] = uint32(isReversed == true ? i : i + 1);
        }
        REQUIRE(renderer.CreateMesh({positionsClip.GetData(), shape.Count, indices.GetData(), indexCount},
                                    meshes[item]) == S::Ready);
        items[item].Geometry = meshes[item];
        items[item].SourceId = item + 1;
        items[item].Material.Color = {float32(color.Red), float32(color.Green), float32(color.Blue), 1};
    }
    rr::Snapshot world;
    REQUIRE(renderer.CreateSnapshot(items.GetData(), ITEM_COUNT, world) == S::Ready);

    // Analytic expectations of each phase: which surfaces any pixel centre reaches.
    usize thinSampledPhases = 0;
    usize farSampledPhases = 0;
    usize nearSampledPhases = 0;
    usize overlapMissedPhases = 0;
    for (uint32 phaseIndex = 0; phaseIndex < PHASE_COUNT; ++phaseIndex)
    {
        const float64 phase = float64(phaseIndex) / PHASE_COUNT;
        CAPTURE(phase);
        const PhaseExpectation expectation = CheckPhase(TranslateFeatures(features, phase));
        thinSampledPhases += expectation.IsThinSampled == true ? 1 : 0;
        farSampledPhases += expectation.IsFarSampled == true ? 1 : 0;
        nearSampledPhases += expectation.IsNearSampled == true ? 1 : 0;
        overlapMissedPhases += expectation.IsOverlapSampled == true ? 0 : 1;
    }
    // The sweep covers both the dropout of the thin strip and phases whose centre samples miss the overlap.
    CHECK(thinSampledPhases > 0);
    CHECK(thinSampledPhases < PHASE_COUNT);
    CHECK(overlapMissedPhases > 0);
    CHECK(farSampledPhases < PHASE_COUNT);

    StaticArray<ModeReport, MODE_COUNT> reports{};
    static StaticArray<uint8, IMAGE_BYTES> pixels{};
    for (usize mode = 0; mode < MODE_COUNT; ++mode)
    {
        rr::RasterViewport viewport;
        REQUIRE(viewport.Initialize(renderer,
                                    device,
                                    VIEWPORT_WIDTH,
                                    VIEWPORT_HEIGHT,
                                    1,
                                    rr::ViewportColorMode::Linear8,
                                    false,
                                    rr::ToneCurve::Reinhard,
                                    false,
                                    MODES[mode]) == S::Ready);
        for (uint32 phaseIndex = 0; phaseIndex < PHASE_COUNT; ++phaseIndex)
        {
            const float64 phase = float64(phaseIndex) / PHASE_COUNT;
            rr::ViewDescription camera;
            camera.WorldToClip.Columns[3].X = float32(2 * phase / VIEWPORT_WIDTH);
            camera.WorldToClip.Columns[3].Y = float32(-2 * phase / VIEWPORT_HEIGHT);
            rr::ViewReport viewReport;
            S status = viewport.Update(world, camera, viewReport);
            const uint64 start = time::NowTicks();
            while (status == S::Pending && time::NowTicks() - start < TIMEOUT_TICKS)
            {
                status = viewport.Poll();
            }
            REQUIRE(status == S::Ready);
            rhi::SubmissionToken producer;
            REQUIRE(viewport.Render(surface, producer) == S::Ready);
            rr::ViewportFrame frame;
            rr::ViewportImage image;
            const uint64 acquireStart = time::NowTicks();
            do
            {
                status = viewport.Acquire(frame, image);
            } while (status == S::Pending && time::NowTicks() - acquireStart < TIMEOUT_TICKS);
            REQUIRE(status == S::Ready);
            REQUIRE(image.Width == VIEWPORT_WIDTH);
            REQUIRE(image.Height == VIEWPORT_HEIGHT);
            ReadViewportImage(device, image, pixels.GetData());
            REQUIRE(viewport.Release(frame) == S::Ready);
            MeasureFrame(TranslateFeatures(features, phase), mode == OFF, pixels.GetData(), reports[mode]);
        }
        viewport.Reset();
    }

    const char* backend = GetBackendName(rhi::GetStartup().SelectedBackend);
    const bool shouldPrint = std::getenv("LUDUS_RENDERER_COVERAGE_REPORT") != nullptr;
    for (usize mode = 0; mode < MODE_COUNT; ++mode)
    {
        if (shouldPrint == true)
        {
            PrintReport(backend, mode, reports[mode]);
        }
        const ModeReport& report = reports[mode];
        CAPTURE(backend, MODE_NAMES[mode]);
        CHECK(report.HasInvalidCoverage == false);
        CHECK(report.IsControlInBand == false);
        // Flat regions keep their exact display value in every mode.
        CHECK(report.FlatMaximum <= DISPLAY_ROUNDING_STEPS);
        // Dropout is a property of the sample lattice: a feature no centre sample reaches is missing from the
        // display image, and a display-space filter has nothing to blend. All modes share the analytic count.
        CHECK(report.ThinVanishedFrames == PHASE_COUNT - thinSampledPhases);
        CHECK(report.FarMissingFrames == PHASE_COUNT - farSampledPhases);
        CHECK(report.NearMissingFrames == PHASE_COUNT - nearSampledPhases);
    }
    // Off is exactly point sampled at pixel centres.
    const ModeReport& off = reports[OFF];
    CHECK(off.PointChecked > 0);
    CHECK(off.PointMismatches == 0);
    CHECK(off.GetCornerInsideLoss() == 0);
    // Regression bounds from the Metal measurement recorded in renderer-systems.md, with about a quarter of headroom
    // for rounding differences between backends. Ratios compare a filtered mode with Off on the same backend.
    constexpr float64 MAXIMUM_OBLIQUE_ERROR_RATIO = .9;          // measured .82 Fast, .83 Balanced
    constexpr float64 MAXIMUM_STAIRCASE_ERROR_RATIO = .95;       // measured .86 Fast, .89 Balanced
    constexpr float64 MAXIMUM_CORNER_ERROR_RATIO = 1.15;         // measured .96 Fast, 1.06 Balanced
    constexpr float64 MAXIMUM_CORNER_INSIDE_LOSS = .2;           // measured .100 Fast, .157 Balanced
    constexpr float64 MAXIMUM_FAR_COMPONENT_ERROR = .07;         // measured .056 both
    constexpr float64 MAXIMUM_NEAR_COMPONENT_ERROR = .125;       // measured .080 Fast, .097 Balanced
    constexpr float64 MAXIMUM_BACKGROUND_COMPONENT_ERROR = .225; // measured .169 Fast, .180 Balanced
    constexpr float64 MAXIMUM_WEIGHT_SUM_ERROR = .27;            // measured .176 Fast, .215 Balanced
    for (const usize mode : {FAST, BALANCED})
    {
        const ModeReport& report = reports[mode];
        CAPTURE(backend, MODE_NAMES[mode]);
        // Edges present in the input move toward their box-filtered values, and no filtered edge pixel is as far
        // off as Off's worst.
        CHECK(report.Oblique.GetMean() <= off.Oblique.GetMean() * MAXIMUM_OBLIQUE_ERROR_RATIO);
        CHECK(report.Oblique.Maximum < off.Oblique.Maximum);
        CHECK(report.Staircase.GetMean() <= off.Staircase.GetMean() * MAXIMUM_STAIRCASE_ERROR_RATIO);
        CHECK(report.Staircase.Maximum < off.Staircase.Maximum);
        // The acute corner does not improve, and the filter softens contrast inside it.
        CHECK(report.Corner.GetMean() <= off.Corner.GetMean() * MAXIMUM_CORNER_ERROR_RATIO);
        CHECK(report.GetCornerInsideLoss() > 0);
        CHECK(report.GetCornerInsideLoss() <= MAXIMUM_CORNER_INSIDE_LOSS);
        // Blending toward a neighbour recovers part of the strips' visible weights, but in display space the
        // recovered weights of the three surfaces no longer sum to one.
        CHECK(report.StripsComponent[FAR_COMPONENT].GetMean() < off.StripsComponent[FAR_COMPONENT].GetMean());
        CHECK(report.StripsComponent[FAR_COMPONENT].GetMean() <= MAXIMUM_FAR_COMPONENT_ERROR);
        CHECK(report.StripsComponent[NEAR_COMPONENT].GetMean() < off.StripsComponent[NEAR_COMPONENT].GetMean());
        CHECK(report.StripsComponent[NEAR_COMPONENT].GetMean() <= MAXIMUM_NEAR_COMPONENT_ERROR);
        CHECK(report.StripsComponent[BACKGROUND_COMPONENT].GetMean() <= MAXIMUM_BACKGROUND_COMPONENT_ERROR);
        CHECK(report.StripsWeightSum.GetMean() > off.StripsWeightSum.GetMean());
        CHECK(report.StripsWeightSum.GetMean() <= MAXIMUM_WEIGHT_SUM_ERROR);
    }
    // Balanced walks up to sixteen pixels along an edge and Fast four, so only Balanced reaches both ends of the
    // staircase's eight-pixel runs from every pixel of a run.
    CHECK(reports[BALANCED].Staircase.Maximum < reports[FAST].Staircase.Maximum);
    for (usize item = 0; item < ITEM_COUNT; ++item)
    {
        REQUIRE(renderer.Release(meshes[item]) == S::Ready);
    }
    REQUIRE(renderer.Release(world) == S::Ready);
}
