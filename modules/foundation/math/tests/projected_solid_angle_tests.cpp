// Projected solid angle sampling (Peters 2021) checked against an independent
// float64 oracle. The oracle never calls the production sampler: it clips the
// polygon itself, integrates the cosine with Lambert's edge formula and measures
// how far each sample is from the exact inverse of the distribution function,
// mirroring the backward-error analysis of the paper's section 3.6.

#include "internal/projected_solid_angle.hpp"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/math/projected_solid_angle.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <initializer_list>
#include <limits>

using namespace ludus::foundation::math;
using ludus::foundation::Array;
using ludus::foundation::core::float32;
using ludus::foundation::core::float64;
using ludus::foundation::core::uint32;
using ludus::foundation::core::uint64;
using ludus::foundation::core::usize;

namespace
{
// Room for an input polygon clipped by the horizon and two sector planes.
constexpr uint32 MAX_ORACLE_VERTICES = MAX_PROJECTED_POLYGON_INPUT_VERTICES + 3;
constexpr uint64 RANDOM_SEED = 0x50E7E125ULL;
constexpr uint32 POLYGON_COUNT = 4000;
constexpr uint32 SAMPLES_PER_POLYGON = 16;
// Polygons whose projected solid angle is smaller than this are excluded from relative statistics: their
// relative error is dominated by single-precision rounding of the vertices (paper section 3.6, Fig. 6).
constexpr float64 MINIMUM_STATISTICS_PROJECTED_SOLID_ANGLE = 1.0e-4;

struct OraclePolygon final
{
    Vector3d Positions[MAX_ORACLE_VERTICES]{};
    uint32 Count = 0;
};

[[nodiscard]] Vector3d Widen(Vector3 value) noexcept
{
    return Vector3d{static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}
[[nodiscard]] float64 GetLength(Vector3d value) noexcept
{
    return std::sqrt(Dot(value, value));
}
[[nodiscard]] float64 ComputeCross2(float64 ax, float64 ay, float64 bx, float64 by) noexcept
{
    return ax * by - ay * bx;
}

/// The input polygon in float64, mirrored like the production frame when requested.
[[nodiscard]] OraclePolygon WidenPolygon(const ProjectedPolygonInput& input, bool isMirrored) noexcept
{
    OraclePolygon result;
    result.Count = input.VertexCount;
    for (uint32 i = 0; i < input.VertexCount; ++i)
    {
        result.Positions[i] = Widen(input.PositionsTangent[i]);
        result.Positions[i].Y = isMirrored == true ? -result.Positions[i].Y : result.Positions[i].Y;
    }
    return result;
}

/// Keep the part of a planar polygon where Dot(normal, p) >= 0 (planes pass through the origin).
[[nodiscard]] OraclePolygon ClipByPlane(const OraclePolygon& polygon, Vector3d normal) noexcept
{
    OraclePolygon result;
    for (uint32 i = 0; i < polygon.Count; ++i)
    {
        const Vector3d start = polygon.Positions[i];
        const Vector3d end = polygon.Positions[(i + 1) % polygon.Count];
        const float64 startDistance = Dot(normal, start);
        const float64 endDistance = Dot(normal, end);
        if (startDistance >= 0.0)
        {
            result.Positions[result.Count] = start;
            ++result.Count;
        }
        if ((startDistance > 0.0 && endDistance < 0.0) || (startDistance < 0.0 && endDistance > 0.0))
        {
            const float64 t = startDistance / (startDistance - endDistance);
            result.Positions[result.Count] = start + (end - start) * t;
            ++result.Count;
        }
    }
    if (result.Count < 3)
    {
        result.Count = 0;
    }
    return result;
}

/// Lambert's formula: the cosine-weighted solid angle of a spherical polygon in the upper hemisphere.
[[nodiscard]] float64 ComputeLambertProjectedSolidAngle(const OraclePolygon& polygon) noexcept
{
    float64 sum = 0.0;
    for (uint32 i = 0; i < polygon.Count; ++i)
    {
        const Vector3d start = polygon.Positions[i] * (1.0 / GetLength(polygon.Positions[i]));
        const Vector3d end =
            polygon.Positions[(i + 1) % polygon.Count] * (1.0 / GetLength(polygon.Positions[(i + 1) % polygon.Count]));
        const Vector3d normal = Cross(start, end);
        const float64 normalLength = GetLength(normal);
        if (normalLength > 0.0)
        {
            sum += std::atan2(normalLength, Dot(start, end)) * normal.Z / normalLength;
        }
    }
    return 0.5 * std::abs(sum);
}

[[nodiscard]] float64 ComputeOracleProjectedSolidAngle(const OraclePolygon& polygon) noexcept
{
    return ComputeLambertProjectedSolidAngle(ClipByPlane(polygon, Vector3d{0.0, 0.0, 1.0}));
}

/// Projected solid angle of the polygon within the sector from direction0 counterclockwise to direction1 (< 180°).
// NOLINTBEGIN(bugprone-easily-swappable-parameters): sector start, then end, each as x, y.
[[nodiscard]] float64 ComputeOracleSectorArea(const OraclePolygon& polygon,
                                              float64 direction0X,
                                              float64 direction0Y,
                                              float64 direction1X,
                                              float64 direction1Y) noexcept
{
    OraclePolygon clipped = ClipByPlane(polygon, Vector3d{0.0, 0.0, 1.0});
    clipped = ClipByPlane(clipped, Vector3d{-direction0Y, direction0X, 0.0});
    clipped = ClipByPlane(clipped, Vector3d{direction1Y, -direction1X, 0.0});
    return ComputeLambertProjectedSolidAngle(clipped);
}
// NOLINTEND(bugprone-easily-swappable-parameters)

/// Squared radii of the polygon's inner and outer boundary along one azimuth; each edge plane bounds the squared
/// radius of the lifted point (r cos, r sin, sqrt(1 - r^2)) from below or above.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): direction as x, y; bounds lower, then upper.
void ComputeRadialBounds(const OraclePolygon& polygon,
                         float64 directionX,
                         float64 directionY,
                         float64& outLowerSquared,
                         float64& outUpperSquared) noexcept
{
    outLowerSquared = 0.0;
    outUpperSquared = 1.0;
    for (uint32 i = 0; i < polygon.Count; ++i)
    {
        const Vector3d normal = Cross(polygon.Positions[i], polygon.Positions[(i + 1) % polygon.Count]);
        const float64 a = normal.X * directionX + normal.Y * directionY;
        const float64 b = normal.Z;
        const float64 boundSquared = b * b / (a * a + b * b);
        if (b > 0.0 && a < 0.0)
        {
            outUpperSquared = std::fmin(outUpperSquared, boundSquared);
        }
        else if (b < 0.0 && a > 0.0)
        {
            outLowerSquared = std::fmax(outLowerSquared, boundSquared);
        }
    }
}
// NOLINTEND(bugprone-easily-swappable-parameters)

struct SampleMeasurement final
{
    /// |exact cumulative projected solid angle up to the sample's azimuth - xi0 * A| / A (backward error).
    float64 AzimuthError = 0.0;
    /// The same error before division by A (paper Fig. 6b).
    float64 ScaledAzimuthError = 0.0;
    /// Distance between the sample and the exact float64 inverse of the distribution at the same random numbers.
    float64 ForwardError = 0.0;
    bool IsInside = false;
};

/// Measure one sample against the exact inverse distribution function of the polygon. The sector boundaries are
/// the prepared vertices: they only fix the order in which azimuths are swept, not the distribution.
[[nodiscard]] SampleMeasurement MeasureSample(const ProjectedPolygonInput& input,
                                              const ProjectedSolidAnglePolygon& prepared,
                                              Vector2 randomNumbers,
                                              Vector3 sample) noexcept
{
    SampleMeasurement measurement;
    const OraclePolygon polygon = WidenPolygon(input, prepared.IsMirrored);
    const float64 total = ComputeOracleProjectedSolidAngle(polygon);
    const float64 x = static_cast<float64>(sample.X);
    const float64 y = prepared.IsMirrored == true ? -static_cast<float64>(sample.Y) : static_cast<float64>(sample.Y);
    const float64 z = static_cast<float64>(sample.Z);

    // Containment: every edge plane of the clockwise polygon has the sample on its nonnegative side.
    measurement.IsInside = z >= 0.0;
    for (uint32 i = 0; i < polygon.Count; ++i)
    {
        const Vector3d normal = Cross(polygon.Positions[i], polygon.Positions[(i + 1) % polygon.Count]);
        measurement.IsInside =
            measurement.IsInside == true && Dot(normal, Vector3d{x, y, z}) >= -1.0e-5 * GetLength(normal);
    }

    const bool isCentral = prepared.IsCentralCase == true;
    const uint32 sectorCount = isCentral == true ? prepared.VertexCount : prepared.VertexCount - 1;
    const float64 target = static_cast<float64>(randomNumbers.X) * total;

    // Backward error: cumulative area from the first sector boundary to the sample. A sample just beyond the
    // counterclockwise end, where the inner and outer boundary meet, counts the whole polygon.
    float64 cumulative = 0.0;
    bool isFound = false;
    for (uint32 i = 0; i < sectorCount && isFound == false; ++i)
    {
        const float64 startX = static_cast<float64>(prepared.VerticesXy[i].X);
        const float64 startY = static_cast<float64>(prepared.VerticesXy[i].Y);
        const float64 endX = static_cast<float64>(prepared.VerticesXy[(i + 1) % prepared.VertexCount].X);
        const float64 endY = static_cast<float64>(prepared.VerticesXy[(i + 1) % prepared.VertexCount].Y);
        const bool isAfterStart = ComputeCross2(startX, startY, x, y) >= 0.0;
        if (isAfterStart == true && ComputeCross2(x, y, endX, endY) >= 0.0)
        {
            cumulative += ComputeOracleSectorArea(polygon, startX, startY, x, y);
            isFound = true;
        }
        else if (i == 0 && isAfterStart == false && isCentral == false)
        {
            isFound = true;
        }
        else
        {
            cumulative += ComputeOracleSectorArea(polygon, startX, startY, endX, endY);
        }
    }
    measurement.ScaledAzimuthError = std::abs(cumulative - target);
    measurement.AzimuthError = measurement.ScaledAzimuthError / total;

    // Forward error: invert the exact distribution by bisection over the azimuth of the selected sector.
    float64 remaining = target;
    uint32 sector = 0;
    for (; sector + 1 < sectorCount; ++sector)
    {
        const float64 area = ComputeOracleSectorArea(polygon,
                                                     static_cast<float64>(prepared.VerticesXy[sector].X),
                                                     static_cast<float64>(prepared.VerticesXy[sector].Y),
                                                     static_cast<float64>(prepared.VerticesXy[sector + 1].X),
                                                     static_cast<float64>(prepared.VerticesXy[sector + 1].Y));
        if (remaining < area)
        {
            break;
        }
        remaining -= area;
    }
    const Vector2 start = prepared.VerticesXy[sector];
    const Vector2 end = prepared.VerticesXy[(sector + 1) % prepared.VertexCount];
    const float64 startAngle = std::atan2(static_cast<float64>(start.Y), static_cast<float64>(start.X));
    float64 sweep = std::atan2(static_cast<float64>(end.Y), static_cast<float64>(end.X)) - startAngle;
    sweep = sweep < 0.0 ? sweep + kTau : sweep;
    float64 low = 0.0;
    float64 high = sweep;
    for (uint32 i = 0; i < 80; ++i)
    {
        const float64 middle = 0.5 * (low + high);
        const float64 area = ComputeOracleSectorArea(polygon,
                                                     static_cast<float64>(start.X),
                                                     static_cast<float64>(start.Y),
                                                     std::cos(startAngle + middle),
                                                     std::sin(startAngle + middle));
        low = area < remaining ? middle : low;
        high = area < remaining ? high : middle;
    }
    const float64 azimuth = startAngle + 0.5 * (low + high);
    float64 lowerSquared = 0.0;
    float64 upperSquared = 1.0;
    ComputeRadialBounds(polygon, std::cos(azimuth), std::sin(azimuth), lowerSquared, upperSquared);
    const float64 radiusSquared = lowerSquared + static_cast<float64>(randomNumbers.Y) * (upperSquared - lowerSquared);
    const float64 radius = std::sqrt(radiusSquared);
    const Vector3d exact{radius * std::cos(azimuth), radius * std::sin(azimuth), std::sqrt(1.0 - radiusSquared)};
    measurement.ForwardError = GetLength(exact - Vector3d{x, y, z});
    return measurement;
}

[[nodiscard]] float64 Draw(RandomStream& inoutRandom, float64 low, float64 high) noexcept
{
    return low + (high - low) * static_cast<float64>(inoutRandom.NextFloat01());
}

/// A random convex polygon: points on an ellipse in a random plane, at a random distance and direction, possibly
/// crossing the horizon, containing the zenith or wound counterclockwise.
[[nodiscard]] ProjectedPolygonInput MakeRandomPolygon(RandomStream& inoutRandom) noexcept
{
    ProjectedPolygonInput result;
    uint32 count = 0;
    static_cast<void>(inoutRandom.TryNextBounded(MAX_PROJECTED_POLYGON_INPUT_VERTICES - 2, count));
    result.VertexCount = count + 3;
    const float64 cosine = Draw(inoutRandom, -0.4, 1.0);
    const float64 azimuth = Draw(inoutRandom, 0.0, kTau);
    const float64 sine = std::sqrt(1.0 - cosine * cosine);
    const float64 distance = std::exp(Draw(inoutRandom, std::log(0.05), std::log(20.0)));
    const Vector3d center{sine * std::cos(azimuth) * distance, sine * std::sin(azimuth) * distance, cosine * distance};
    const float64 normalCosine = Draw(inoutRandom, -1.0, 1.0);
    const float64 normalAzimuth = Draw(inoutRandom, 0.0, kTau);
    const float64 normalSine = std::sqrt(1.0 - normalCosine * normalCosine);
    const Vector3d normal{normalSine * std::cos(normalAzimuth), normalSine * std::sin(normalAzimuth), normalCosine};
    const Vector3d helper = std::abs(normal.X) < 0.9 ? Vector3d{1.0, 0.0, 0.0} : Vector3d{0.0, 1.0, 0.0};
    Vector3d tangent = Cross(normal, helper);
    tangent = tangent * (1.0 / GetLength(tangent));
    const Vector3d bitangent = Cross(normal, tangent);
    const float64 radius = distance * std::exp(Draw(inoutRandom, std::log(0.01), std::log(3.0)));
    const float64 secondRadius = radius * Draw(inoutRandom, 0.3, 1.0);
    const float64 phase = Draw(inoutRandom, 0.0, kTau);
    const bool isReversed = inoutRandom.NextFloat01() < 0.5F;
    for (uint32 i = 0; i < result.VertexCount; ++i)
    {
        // Stratified angles keep consecutive vertices apart, so no three are collinear.
        const float64 angle =
            phase + (static_cast<float64>(i) + Draw(inoutRandom, 0.0, 0.8)) * kTau / result.VertexCount;
        const Vector3d position =
            center + tangent * (std::cos(angle) * radius) + bitangent * (std::sin(angle) * secondRadius);
        const uint32 index = isReversed == true ? result.VertexCount - 1 - i : i;
        result.PositionsTangent[index] = Vector3{static_cast<float32>(position.X),
                                                 static_cast<float32>(position.Y),
                                                 static_cast<float32>(position.Z)};
    }
    return result;
}

[[nodiscard]] ProjectedPolygonInput MakePolygon(std::initializer_list<Vector3> positions) noexcept
{
    ProjectedPolygonInput result;
    for (const Vector3& position : positions)
    {
        result.PositionsTangent[result.VertexCount] = position;
        ++result.VertexCount;
    }
    return result;
}

[[nodiscard]] float64 GetPercentile(Array<float64>& inoutValues, float64 fraction) noexcept
{
    ludus::foundation::Sort(inoutValues.GetData(), inoutValues.GetSize(), [](float64 a, float64 b) noexcept {
        return a < b;
    });
    const usize index = static_cast<usize>(fraction * static_cast<float64>(inoutValues.GetSize() - 1));
    return inoutValues[index];
}

/// Prepare, then check the projected solid angle and a stratified grid of samples against the oracle.
void RequireAccurateSampling(const ProjectedPolygonInput& input, float64 tolerance)
{
    ProjectedSolidAnglePolygon prepared;
    REQUIRE(TryPrepareProjectedSolidAnglePolygon(input, prepared) == MathStatus::Success);
    const float64 total = ComputeOracleProjectedSolidAngle(WidenPolygon(input, prepared.IsMirrored));
    REQUIRE(total > 0.0);
    REQUIRE(std::abs(static_cast<float64>(prepared.ProjectedSolidAngle) - total) <= tolerance * total);
    constexpr uint32 GRID = 9;
    for (uint32 i = 0; i < GRID; ++i)
    {
        for (uint32 j = 0; j < GRID; ++j)
        {
            const Vector2 randomNumbers{(static_cast<float32>(i) + 0.5F) / GRID,
                                        (static_cast<float32>(j) + 0.5F) / GRID};
            Vector3 sample;
            REQUIRE(TrySampleProjectedSolidAnglePolygon(prepared, randomNumbers, sample) == MathStatus::Success);
            REQUIRE(std::abs(static_cast<float64>(Length(sample)) - 1.0) <= 1.0e-5);
            const SampleMeasurement measurement = MeasureSample(input, prepared, randomNumbers, sample);
            REQUIRE(measurement.IsInside == true);
            REQUIRE(measurement.AzimuthError <= tolerance);
            REQUIRE(measurement.ForwardError <= tolerance);
        }
    }
}
} // namespace

TEST_CASE("Prepared projected solid angle matches Lambert's edge formula", "[math][projected_solid_angle]")
{
    RandomStream random;
    REQUIRE(random.TryReseed(RANDOM_SEED, 1) == true);
    Array<float64> errors;
    uint32 emptyCount = 0;
    for (uint32 i = 0; i < POLYGON_COUNT; ++i)
    {
        const ProjectedPolygonInput input = MakeRandomPolygon(random);
        ProjectedSolidAnglePolygon prepared;
        REQUIRE(TryPrepareProjectedSolidAnglePolygon(input, prepared) == MathStatus::Success);
        const float64 total = ComputeOracleProjectedSolidAngle(WidenPolygon(input, prepared.IsMirrored));
        const float64 prepared64 = static_cast<float64>(prepared.ProjectedSolidAngle);
        REQUIRE(prepared64 >= 0.0);
        REQUIRE(prepared64 <= kPi + 1.0e-5);
        // Absolute agreement everywhere, relative agreement for visible lights.
        REQUIRE(std::abs(prepared64 - total) <= 1.0e-6 + 1.0e-3 * total);
        if (total < MINIMUM_STATISTICS_PROJECTED_SOLID_ANGLE)
        {
            ++emptyCount;
            continue;
        }
        REQUIRE(errors.TryAdd(std::abs(prepared64 - total) / total) == true);
        float64 sectorSum = 0.0;
        const uint32 sectorCount = prepared.IsCentralCase == true ? prepared.VertexCount : prepared.VertexCount - 1;
        for (uint32 j = 0; j < sectorCount; ++j)
        {
            REQUIRE(prepared.SectorProjectedSolidAngles[j] >= 0.0F);
            sectorSum += static_cast<float64>(prepared.SectorProjectedSolidAngles[j]);
        }
        REQUIRE(std::abs(sectorSum - prepared64) <= 1.0e-5 * prepared64);
    }
    // The random corpus must exercise visible polygons, not only ones below the horizon.
    REQUIRE(errors.GetSize() > POLYGON_COUNT / 2);
    REQUIRE(emptyCount > 0);
    // Sector areas subtract an inner from an outer ellipse area, so tiny lights lose relative precision.
    REQUIRE(GetPercentile(errors, 0.99) <= 1.0e-4);
}

TEST_CASE("Samples invert the exact distribution function within single-precision error",
          "[math][projected_solid_angle]")
{
    RandomStream random;
    REQUIRE(random.TryReseed(RANDOM_SEED, 2) == true);
    Array<float64> azimuthErrors;
    Array<float64> scaledAzimuthErrors;
    Array<float64> forwardErrors;
    uint32 centralCount = 0;
    uint32 decentralCount = 0;
    for (uint32 i = 0; i < POLYGON_COUNT; ++i)
    {
        const ProjectedPolygonInput input = MakeRandomPolygon(random);
        ProjectedSolidAnglePolygon prepared;
        REQUIRE(TryPrepareProjectedSolidAnglePolygon(input, prepared) == MathStatus::Success);
        const float64 total = ComputeOracleProjectedSolidAngle(WidenPolygon(input, prepared.IsMirrored));
        if (total < MINIMUM_STATISTICS_PROJECTED_SOLID_ANGLE)
        {
            continue;
        }
        centralCount += prepared.IsCentralCase == true ? 1 : 0;
        decentralCount += prepared.IsCentralCase == true ? 0 : 1;
        for (uint32 j = 0; j < SAMPLES_PER_POLYGON; ++j)
        {
            const Vector2 randomNumbers{random.NextFloat01(), random.NextFloat01()};
            Vector3 sample;
            REQUIRE(TrySampleProjectedSolidAnglePolygon(prepared, randomNumbers, sample) == MathStatus::Success);
            REQUIRE(sample.Z >= 0.0F);
            REQUIRE(std::abs(static_cast<float64>(Length(sample)) - 1.0) <= 1.0e-5);
            const SampleMeasurement measurement = MeasureSample(input, prepared, randomNumbers, sample);
            REQUIRE(measurement.IsInside == true);
            REQUIRE(azimuthErrors.TryAdd(measurement.AzimuthError) == true);
            REQUIRE(scaledAzimuthErrors.TryAdd(measurement.ScaledAzimuthError) == true);
            REQUIRE(forwardErrors.TryAdd(measurement.ForwardError) == true);
            REQUIRE(EvaluateProjectedSolidAnglePolygonDensity(prepared, sample) ==
                    sample.Z / prepared.ProjectedSolidAngle);
        }
    }
    REQUIRE(centralCount > 0);
    REQUIRE(decentralCount > 0);
    // Paper section 3.6 and Fig. 6: in single precision the backward error grows only for small projected solid
    // angles, while the error scaled by the projected solid angle stays near 1e-7. Measured on this corpus (Apple
    // M5): p99 4.2e-5 and maximum 5.9e-3 relative, p99 1.1e-7 and maximum 9.6e-7 scaled, forward p99 6.8e-6 and
    // maximum 1.8e-2 near the polygon ends where the inner and outer boundary meet.
    REQUIRE(GetPercentile(azimuthErrors, 0.99) <= 1.0e-4);
    REQUIRE(GetPercentile(azimuthErrors, 1.0) <= 2.0e-2);
    REQUIRE(GetPercentile(scaledAzimuthErrors, 0.99) <= 2.0e-7);
    REQUIRE(GetPercentile(scaledAzimuthErrors, 1.0) <= 2.0e-6);
    REQUIRE(GetPercentile(forwardErrors, 0.99) <= 2.0e-5);
    REQUIRE(GetPercentile(forwardErrors, 1.0) <= 5.0e-2);
}

TEST_CASE("Two tangent-line iterations refine the two-quad initialization", "[math][projected_solid_angle]")
{
    // Paper section 3.6: initialization alone has a 99th-percentile backward error near 0.02; after two iterations
    // the residual area error is below 1e-7 almost everywhere. Measured on this corpus (Apple M5): initialization
    // p99 4.8e-2 relative; iterated p99 1.5e-4 relative and 8.2e-8 absolute.
    RandomStream random;
    REQUIRE(random.TryReseed(RANDOM_SEED, 3) == true);
    Array<float64> initializationErrors;
    Array<float64> iteratedErrors;
    Array<float64> iteratedAbsoluteErrors;
    for (uint32 i = 0; i < POLYGON_COUNT; ++i)
    {
        const ProjectedPolygonInput input = MakeRandomPolygon(random);
        ProjectedSolidAnglePolygon prepared;
        REQUIRE(TryPrepareProjectedSolidAnglePolygon(input, prepared) == MathStatus::Success);
        const OraclePolygon polygon = WidenPolygon(input, prepared.IsMirrored);
        if (prepared.IsCentralCase == true || prepared.VertexCount == 0 ||
            ComputeOracleProjectedSolidAngle(polygon) < MINIMUM_STATISTICS_PROJECTED_SOLID_ANGLE)
        {
            continue;
        }
        // Pair every sector with its inner and outer ellipse (paper section 3.7).
        Vector2 innerEllipse = prepared.InnerEllipse0;
        Vector2 outerEllipse = prepared.Ellipses[0];
        for (uint32 sector = 0; sector + 1 < prepared.VertexCount; ++sector)
        {
            if (sector > 0)
            {
                const bool isInner = prepared.EllipseKinds[sector] == ProjectedEllipseKind::Inner;
                innerEllipse = isInner == true ? prepared.Ellipses[sector] : innerEllipse;
                outerEllipse = isInner == true ? outerEllipse : prepared.Ellipses[sector];
            }
            const Vector2 start = prepared.VerticesXy[sector];
            const Vector2 end = prepared.VerticesXy[sector + 1];
            const float64 sectorArea = ComputeOracleSectorArea(polygon,
                                                               static_cast<float64>(start.X),
                                                               static_cast<float64>(start.Y),
                                                               static_cast<float64>(end.X),
                                                               static_cast<float64>(end.Y));
            const float32 preparedSectorArea = prepared.SectorProjectedSolidAngles[sector];
            if (sectorArea < MINIMUM_STATISTICS_PROJECTED_SOLID_ANGLE)
            {
                continue;
            }
            const Vector2 randomNumbers{random.NextFloat01(), random.NextFloat01()};
            for (uint32 iterationCount = 0; iterationCount <= PROJECTED_POLYGON_ITERATION_COUNT;
                 iterationCount += PROJECTED_POLYGON_ITERATION_COUNT)
            {
                const Vector2 sample = internal::SampleSectorBetweenEllipses(randomNumbers,
                                                                             randomNumbers.X * preparedSectorArea,
                                                                             innerEllipse,
                                                                             outerEllipse,
                                                                             start,
                                                                             end,
                                                                             iterationCount);
                const float64 reached = ComputeOracleSectorArea(polygon,
                                                                static_cast<float64>(start.X),
                                                                static_cast<float64>(start.Y),
                                                                static_cast<float64>(sample.X),
                                                                static_cast<float64>(sample.Y));
                const float64 error = std::abs(reached - static_cast<float64>(randomNumbers.X) * sectorArea);
                Array<float64>& errors = iterationCount == 0 ? initializationErrors : iteratedErrors;
                REQUIRE(errors.TryAdd(error / sectorArea) == true);
                if (iterationCount != 0)
                {
                    REQUIRE(iteratedAbsoluteErrors.TryAdd(error) == true);
                }
            }
        }
    }
    REQUIRE(iteratedErrors.GetSize() > 1000);
    const float64 initialization = GetPercentile(initializationErrors, 0.99);
    const float64 iterated = GetPercentile(iteratedErrors, 0.99);
    REQUIRE(initialization >= 1.0e-3);
    REQUIRE(initialization <= 0.1);
    REQUIRE(iterated <= 1.0e-3);
    REQUIRE(iterated * 10.0 <= initialization);
    REQUIRE(GetPercentile(iteratedAbsoluteErrors, 0.99) <= 2.0e-7);
}

TEST_CASE("Boundary configurations of supplement A.2 and section 3.7 sample correctly", "[math][projected_solid_angle]")
{
    SECTION("Central case: the zenith is inside the polygon")
    {
        RequireAccurateSampling(MakePolygon({{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}}), 1.0e-4);
    }
    SECTION("A vertex at the zenith makes two great circles pass through it")
    {
        RequireAccurateSampling(MakePolygon({{0, 0, 1}, {1, 0, 1}, {0, 1, 1}}), 1.0e-4);
    }
    SECTION("An edge whose great circle passes through the zenith")
    {
        RequireAccurateSampling(MakePolygon({{1, 0, 1}, {2, 0, 1}, {2, 1, 1}, {1, 1, 1}}), 1.0e-4);
    }
    SECTION("A light perpendicular to the receiver with edges parallel to the normal (paper Fig. 12h)")
    {
        RequireAccurateSampling(MakePolygon({{1, -1, 0.5F}, {1, -1, 2}, {1, 1, 2}, {1, 1, 0.5F}}), 1.0e-4);
    }
    SECTION("A polygon straddling the horizon is clipped")
    {
        RequireAccurateSampling(MakePolygon({{1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}}), 1.0e-4);
    }
    SECTION("A heptagon close to the shading point")
    {
        ProjectedPolygonInput heptagon;
        heptagon.VertexCount = MAX_PROJECTED_POLYGON_INPUT_VERTICES;
        for (uint32 i = 0; i < heptagon.VertexCount; ++i)
        {
            const float32 angle = static_cast<float32>(i) * kTauF / static_cast<float32>(heptagon.VertexCount);
            heptagon.PositionsTangent[i] = Vector3{0.3F + Cos(angle), Sin(angle), 0.01F};
        }
        RequireAccurateSampling(heptagon, 1.0e-3);
    }
    SECTION("A 1 cm square light 40 m away (paper Fig. 7a)")
    {
        RequireAccurateSampling(
            MakePolygon({{30, 0, 26.46F}, {30.01F, 0, 26.46F}, {30.01F, 0.01F, 26.46F}, {30, 0.01F, 26.46F}}),
            1.0e-2);
    }
}

TEST_CASE("Winding, empty and edge-on polygons", "[math][projected_solid_angle]")
{
    const ProjectedPolygonInput clockwise = MakePolygon({{1, -1, 1}, {2, -1, 1}, {2, 1, 2}, {1, 1, 2}});
    const ProjectedPolygonInput counterclockwise = MakePolygon({{1, 1, 2}, {2, 1, 2}, {2, -1, 1}, {1, -1, 1}});
    ProjectedSolidAnglePolygon first;
    ProjectedSolidAnglePolygon second;
    REQUIRE(TryPrepareProjectedSolidAnglePolygon(clockwise, first) == MathStatus::Success);
    REQUIRE(TryPrepareProjectedSolidAnglePolygon(counterclockwise, second) == MathStatus::Success);
    REQUIRE(first.IsMirrored == false);
    REQUIRE(second.IsMirrored == true);
    REQUIRE(std::abs(first.ProjectedSolidAngle - second.ProjectedSolidAngle) <= 1.0e-6F * first.ProjectedSolidAngle);
    RequireAccurateSampling(counterclockwise, 1.0e-4);

    ProjectedSolidAnglePolygon empty;
    REQUIRE(TryPrepareProjectedSolidAnglePolygon(MakePolygon({{1, 0, -1}, {1, 1, -1}, {0, 1, -2}}), empty) ==
            MathStatus::Success);
    REQUIRE(empty.ProjectedSolidAngle == 0.0F);
    REQUIRE(empty.VertexCount == 0);
    Vector3 unchanged{7, 7, 7};
    REQUIRE(TrySampleProjectedSolidAnglePolygon(empty, Vector2{0.5F, 0.5F}, unchanged) == MathStatus::Degenerate);
    REQUIRE(unchanged == Vector3{7, 7, 7});
    REQUIRE(EvaluateProjectedSolidAnglePolygonDensity(empty, Vector3{0, 0, 1}) == 0.0F);

    // The shading point lies in the polygon's plane: the light covers no solid angle.
    ProjectedSolidAnglePolygon edgeOn;
    REQUIRE(TryPrepareProjectedSolidAnglePolygon(MakePolygon({{1, 0, 1}, {2, 0, 2}, {2, 0, 1}}), edgeOn) ==
            MathStatus::Success);
    REQUIRE(edgeOn.ProjectedSolidAngle == 0.0F);

    // A vertex exactly on the horizon is kept once, not duplicated by clipping.
    const Vector3 touching[MAX_PROJECTED_POLYGON_INPUT_VERTICES] = {{1, 0, 0}, {1, 1, 1}, {0, 1, 1}};
    Vector3 clipped[MAX_PROJECTED_POLYGON_VERTICES]{};
    REQUIRE(internal::ClipPolygonToUpperHemisphere(touching, 3, clipped) == 3);
    const Vector3 crossing[MAX_PROJECTED_POLYGON_INPUT_VERTICES] = {{1, 0, -1}, {1, 1, 1}, {0, 1, 1}};
    REQUIRE(internal::ClipPolygonToUpperHemisphere(crossing, 3, clipped) == 4);
    REQUIRE(clipped[0].Z >= 0.0F);
    REQUIRE(clipped[3].Z == 0.0F);
}

TEST_CASE("The density integrates to one over the polygon", "[math][projected_solid_angle]")
{
    // Independent of the sampler: integrate cos(theta) / A over the hemisphere with stratified uniform directions
    // and the oracle's containment test. The quad lies in the plane z = 1 + 0.2x - 0.1y.
    const ProjectedPolygonInput input =
        MakePolygon({{-0.5F, -1, 1}, {1.5F, -0.5F, 1.35F}, {1, 1, 1.1F}, {-1, 0.5F, 0.75F}});
    ProjectedSolidAnglePolygon prepared;
    REQUIRE(TryPrepareProjectedSolidAnglePolygon(input, prepared) == MathStatus::Success);
    const OraclePolygon polygon = WidenPolygon(input, prepared.IsMirrored);
    constexpr uint32 GRID = 1024;
    float64 integral = 0.0;
    for (uint32 i = 0; i < GRID; ++i)
    {
        for (uint32 j = 0; j < GRID; ++j)
        {
            // Uniform directions on the upper hemisphere have density 1 / (2 pi).
            const float64 z = (static_cast<float64>(i) + 0.5) / GRID;
            const float64 azimuth = (static_cast<float64>(j) + 0.5) / GRID * kTau;
            const float64 radius = std::sqrt(1.0 - z * z);
            const Vector3d direction{radius * std::cos(azimuth), radius * std::sin(azimuth), z};
            bool isInside = true;
            for (uint32 k = 0; k < polygon.Count; ++k)
            {
                isInside =
                    isInside == true &&
                    Dot(Cross(polygon.Positions[k], polygon.Positions[(k + 1) % polygon.Count]), direction) >= 0.0;
            }
            if (isInside == true)
            {
                const Vector3 narrow{static_cast<float32>(direction.X),
                                     static_cast<float32>(prepared.IsMirrored == true ? -direction.Y : direction.Y),
                                     static_cast<float32>(direction.Z)};
                integral += static_cast<float64>(EvaluateProjectedSolidAnglePolygonDensity(prepared, narrow));
            }
        }
    }
    integral *= kTau / (static_cast<float64>(GRID) * GRID);
    REQUIRE(std::abs(integral - 1.0) <= 5.0e-3);
    REQUIRE(EvaluateProjectedSolidAnglePolygonDensity(prepared, Vector3{0, 0, -1}) == 0.0F);
    REQUIRE(
        EvaluateProjectedSolidAnglePolygonDensity(prepared, Vector3{std::numeric_limits<float32>::quiet_NaN(), 0, 1}) ==
        0.0F);
}

TEST_CASE("Sampling is continuous in the first random number", "[math][projected_solid_angle]")
{
    // Paper section 3.2: the mapping is continuous, so stratification carries over. Near the clockwise end the
    // density vanishes and the inverse has an infinite derivative (paper Fig. 13), so steps shrink like a square
    // root rather than linearly. The pentagon lies in the plane z = 0.4 + 0.3x + 0.2y and excludes the zenith.
    const ProjectedPolygonInput input =
        MakePolygon({{1, -1, 0.5F}, {2, -0.5F, 0.9F}, {2, 1, 1.2F}, {1, 1.5F, 1}, {0.5F, 0.5F, 0.65F}});
    ProjectedSolidAnglePolygon prepared;
    REQUIRE(TryPrepareProjectedSolidAnglePolygon(input, prepared) == MathStatus::Success);
    REQUIRE(prepared.IsCentralCase == false);
    float32 largestSteps[2] = {};
    constexpr uint32 STEP_COUNTS[2] = {4096, 32768};
    for (uint32 k = 0; k < 2; ++k)
    {
        Vector3 previous;
        REQUIRE(TrySampleProjectedSolidAnglePolygon(prepared, Vector2{0.0F, 0.5F}, previous) == MathStatus::Success);
        for (uint32 i = 1; i <= STEP_COUNTS[k]; ++i)
        {
            const Vector2 randomNumbers{static_cast<float32>(i) / static_cast<float32>(STEP_COUNTS[k]), 0.5F};
            Vector3 current;
            REQUIRE(TrySampleProjectedSolidAnglePolygon(prepared, randomNumbers, current) == MathStatus::Success);
            largestSteps[k] = Max(largestSteps[k], Length(current - previous));
            previous = current;
        }
    }
    REQUIRE(largestSteps[0] <= 0.05F);
    REQUIRE(largestSteps[1] * 2.0F <= largestSteps[0]);
}

TEST_CASE("Invalid inputs leave outputs unchanged", "[math][projected_solid_angle]")
{
    ProjectedSolidAnglePolygon prepared;
    prepared.ProjectedSolidAngle = 42.0F;
    ProjectedPolygonInput input = MakePolygon({{1, 0, 1}, {1, 1, 1}});
    REQUIRE(TryPrepareProjectedSolidAnglePolygon(input, prepared) == MathStatus::InvalidArgument);
    input.VertexCount = MAX_PROJECTED_POLYGON_INPUT_VERTICES + 1;
    REQUIRE(TryPrepareProjectedSolidAnglePolygon(input, prepared) == MathStatus::InvalidArgument);
    input = MakePolygon({{1, 0, 1}, {1, 1, std::numeric_limits<float32>::infinity()}, {0, 1, 1}});
    REQUIRE(TryPrepareProjectedSolidAnglePolygon(input, prepared) == MathStatus::NonFiniteInput);
    REQUIRE(prepared.ProjectedSolidAngle == 42.0F);

    REQUIRE(TryPrepareProjectedSolidAnglePolygon(MakePolygon({{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}}), prepared) ==
            MathStatus::Success);
    Vector3 unchanged{7, 7, 7};
    REQUIRE(TrySampleProjectedSolidAnglePolygon(prepared, Vector2{-0.1F, 0.5F}, unchanged) ==
            MathStatus::InvalidArgument);
    REQUIRE(TrySampleProjectedSolidAnglePolygon(prepared, Vector2{0.5F, 1.5F}, unchanged) ==
            MathStatus::InvalidArgument);
    REQUIRE(TrySampleProjectedSolidAnglePolygon(prepared,
                                                Vector2{std::numeric_limits<float32>::quiet_NaN(), 0.5F},
                                                unchanged) == MathStatus::NonFiniteInput);
    REQUIRE(unchanged == Vector3{7, 7, 7});
}
