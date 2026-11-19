#pragma once

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

namespace ludus::test_support::outline
{
using namespace foundation;

// Vertices 0 and 1 form the upper contour of this closed tetrahedron in every admitted view.
// The two incident faces lie on the same side of its projected edge.
inline constexpr StaticArray POSITIONS_MESH{math::Vector3{-0.8525F, 0.234F, -0.3F},
                                            math::Vector3{6.2775F, 1.456F, -3},
                                            math::Vector3{0.31F, 0.104F, -2.7F},
                                            math::Vector3{1.74F, 0.592F, -2.02F}};
inline constexpr usize TETRAHEDRON_INDEX_COUNT = 12;
inline constexpr StaticArray<uint32, TETRAHEDRON_INDEX_COUNT> INDICES{0, 1, 2, 0, 3, 1, 1, 3, 2, 2, 3, 0};
inline constexpr StaticArray WIDTHS_PIXELS{232, 256};
inline constexpr int32 HEIGHT_PIXELS = 160;
inline constexpr float32 NEAR_METRES = 1;

struct Placement final
{
    const char* Name;
    math::Vector3 OffsetView{};
    bool IsBehind = false;
    bool IsNearClipped = false;
    bool IsRightClipped = false;
};

inline constexpr StaticArray PLACEMENTS{
    Placement{ .Name = "visible", .OffsetView = {0, 0, -2} },
    Placement{ .Name = "near", .OffsetView = {-2.13F, 0, 0}, .IsNearClipped = true },
    Placement{ .Name = "eye", .OffsetView = {-3.1F, 0, 0.7F}, .IsNearClipped = true },
    Placement{ .Name = "behind", .OffsetView = {0, 0, 4}, .IsBehind = true },
    Placement{ .Name = "near and right", .IsNearClipped = true, .IsRightClipped = true },
    Placement{ .Name = "right only", .OffsetView = {0, 0, -0.8F}, .IsRightClipped = true }};

inline math::Matrix4 MakeMeshToClip(const Placement& placement, int32 widthPixels)
{
    math::Matrix4 viewToClip;
    REQUIRE(math::TryPerspectiveReverseZInfinite(math::kPiF / 2,
                                                 static_cast<float32>(widthPixels) / HEIGHT_PIXELS,
                                                 NEAR_METRES,
                                                 viewToClip) == math::MathStatus::Success);
    math::Matrix4 meshToView = math::Matrix4::Identity();
    meshToView.Columns[3] = {placement.OffsetView.X, placement.OffsetView.Y, placement.OffsetView.Z, 1};
    return viewToClip * meshToView;
}

enum class Mark
{
    Clear,
    Casing,
    Stroke
};

// Thanks to Blinn and Newell, "Clipping Using Homogeneous Coordinates", SIGGRAPH '78, pp. 245–251,
// https://doi.org/10.1145/800248.807398. Solve for the two intersections on the original contour,
// independently of the production matrix and line clipper. With 90 degree vertical FOV and infinite
// reverse Z, clip = (x/aspect, y, near, -z). Only near and right can bound this edge's visible interval.
template <typename ReadPixel>
void CheckImage(const Placement& placement, int32 widthPixels, const ReadPixel& readPixel)
{
    CAPTURE(placement.Name, widthPixels);
    // All four vertices have positive view Y. Leave room for the casing at the equator.
    const int32 firstClearRow = placement.IsBehind == true ? 0 : HEIGHT_PIXELS / 2 + 3;
    int32 strayPixels = 0;
    for (int32 y = firstClearRow; y < HEIGHT_PIXELS; ++y)
    {
        for (int32 x = 0; x < widthPixels; ++x)
        {
            strayPixels += readPixel(x, y) == Mark::Clear ? 0 : 1;
        }
    }
    CHECK(strayPixels == 0);
    if (placement.IsBehind == true)
    {
        return;
    }
    const math::Vector3 firstView = POSITIONS_MESH[0] + placement.OffsetView;
    const math::Vector3 secondView = POSITIONS_MESH[1] + placement.OffsetView;
    const float64 firstW = -static_cast<float64>(firstView.Z);
    const float64 secondW = -static_cast<float64>(secondView.Z);
    const float64 aspect = static_cast<float64>(widthPixels) / HEIGHT_PIXELS;
    const float64 firstRight = firstW - static_cast<float64>(firstView.X) / aspect;
    const float64 secondRight = secondW - static_cast<float64>(secondView.X) / aspect;
    const float64 enter = math::Max(0.0, (static_cast<float64>(NEAR_METRES) - firstW) / (secondW - firstW));
    const float64 leave = secondRight < 0 ? firstRight / (firstRight - secondRight) : 1;
    REQUIRE((enter > 0) == placement.IsNearClipped);
    REQUIRE((leave < 1) == placement.IsRightClipped);
    REQUIRE(enter < leave);
    const auto projectPixel = [&](float64 fraction) -> math::Vector2 {
        const float64 w = math::Lerp(firstW, secondW, fraction);
        const float64 x = math::Lerp(static_cast<float64>(firstView.X), static_cast<float64>(secondView.X), fraction);
        const float64 y = math::Lerp(static_cast<float64>(firstView.Y), static_cast<float64>(secondView.Y), fraction);
        return {static_cast<float32>((1 + x / (aspect * w)) * widthPixels * 0.5),
                static_cast<float32>((1 - y / w) * HEIGHT_PIXELS * 0.5)};
    };
    const math::Vector2 firstPixel = projectPixel(enter);
    const math::Vector2 lastPixel = projectPixel(leave);
    // A two-pixel stroke and backend sample coverage allow two pixels from the analytic edge
    // to the nearest stroke pixel centre. Stay inside the surviving segment and away from cap joins.
    constexpr float32 TOLERANCE_PIXELS = 2;
    const auto checkStroke = [&](math::Vector2 probePixel) {
        const int32 column = static_cast<int32>(probePixel.X);
        const int32 row = static_cast<int32>(probePixel.Y);
        CAPTURE(column, row);
        REQUIRE((column >= 2 && column < widthPixels - 2 && row >= 2 && row < HEIGHT_PIXELS - 2));
        bool hasStroke = false;
        for (int32 y = row - 2; y <= row + 2; ++y)
        {
            for (int32 x = column - 2; x <= column + 2; ++x)
            {
                const math::Vector2 centrePixel{static_cast<float32>(x) + 0.5F, static_cast<float32>(y) + 0.5F};
                if (math::Length(centrePixel - probePixel) <= TOLERANCE_PIXELS && readPixel(x, y) == Mark::Stroke)
                {
                    hasStroke = true;
                }
            }
        }
        CHECK(hasStroke == true);
    };
    for (float32 fraction : {0.19F, 0.43F, 0.68F, 0.83F})
    {
        const math::Vector2 probePixel = math::Lerp(firstPixel, lastPixel, fraction);
        checkStroke(probePixel);
        const int32 x = static_cast<int32>(probePixel.X);
        const int32 y = static_cast<int32>(probePixel.Y) - 6;
        REQUIRE(y >= 0);
        CHECK(readPixel(x, y) == Mark::Clear);
    }

    // Edge 1–2 is the lower contour. Its midpoint stays well inside the frustum in every visible placement.
    const math::Vector3 lowerView = (POSITIONS_MESH[1] + POSITIONS_MESH[2]) * 0.5F + placement.OffsetView;
    checkStroke({static_cast<float32>(widthPixels) * 0.5F + HEIGHT_PIXELS * 0.5F * lowerView.X / -lowerView.Z,
                 HEIGHT_PIXELS * 0.5F * (1 - lowerView.Y / -lowerView.Z)});
    if (placement.IsNearClipped == true)
    {
        // On the rejected continuation of this contour, well clear of the new cap and the stroke's end.
        const math::Vector2 directionPixel = lastPixel - firstPixel;
        const math::Vector2 rejectedPixel = firstPixel - directionPixel * (18.0F / math::Length(directionPixel));
        const int32 x = static_cast<int32>(rejectedPixel.X);
        const int32 y = static_cast<int32>(rejectedPixel.Y);
        REQUIRE((x >= 0 && x < widthPixels && y >= 0 && y < HEIGHT_PIXELS));
        CHECK(readPixel(x, y) == Mark::Clear);
    }
}
} // namespace ludus::test_support::outline
