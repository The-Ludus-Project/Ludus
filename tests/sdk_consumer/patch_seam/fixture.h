#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/geometry/tessellation.hpp>
#include <ludus/graphics/renderer/renderer.hpp>

#include <filesystem>

namespace ludus::qa::patch
{
using namespace foundation;
namespace geom = foundation::geometry;
enum class Shape
{
    Parabola,
    Crease,
    Unequal,
    UnequalCrease,
    Plane,
    Sparse,
    Bulge,
    Mixed,
};
struct Case final
{
    const char* Name;
    Shape Surface;
    bool IsReordered;
};
inline constexpr Case CASES[] = {
    {"smooth", Shape::Parabola, false},
    {"crease", Shape::Crease, false},
    {"unequal", Shape::Unequal, false},
    {"unequal-reversed", Shape::Unequal, true},
    {"unequal-crease", Shape::UnequalCrease, false},
    {"unequal-crease-reversed", Shape::UnequalCrease, true},
    {"plane", Shape::Plane, false},
    {"sparse", Shape::Sparse, false},
    {"bulge", Shape::Bulge, false},
    {"mixed", Shape::Mixed, false},
};
inline constexpr float64 ERRORS[] = {0.035, 0.006};
inline constexpr uint32 EXTENTS[] = {96, 192};
// Two grazing directions and a closer perspective view with nonuniform model scale.
struct Camera final
{
    float32 Side;
    float32 Slope;
    float32 Distance;
    math::Vector3 Scale;
};
inline constexpr Camera CAMERAS[] = {{1, 0.38F, 3, {1, 1, 1}},
                                     {-1, 0.38F, 3, {1, 1, 1}},
                                     {1, 0.32F, 3.2F, {1.4F, 1, 1.5F}},
                                     {1, 0.08F, 2.24F, {1.4F, 1, 1.5F}}};
usize GetCameraCount(Shape shape);
graphics::renderer::ViewDescription MakeView(const Camera& camera);
bool BuildFixture(const Case& fixture,
                  float64 error,
                  geom::SurfaceMesh& outMesh,
                  const std::filesystem::path& directory);
} // namespace ludus::qa::patch
