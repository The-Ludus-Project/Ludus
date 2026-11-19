#pragma once

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::graphics::renderer::testing
{
using namespace foundation;
struct Panel
{
    math::Vector3 CenterWorld;
    math::Vector3 HalfUWorld;
    math::Vector3 HalfVWorld;
};
enum class Shutter
{
    Open,
    Narrow,
    Closed,
    RoofRemoved
};
constexpr uint32 PANEL_COUNT = 10;
constexpr uint32 SHUTTER_INDEX = PANEL_COUNT - 2;
constexpr uint32 ROOF_INDEX = PANEL_COUNT - 1;
using Panels = StaticArray<Panel, PANEL_COUNT>;

inline Panels MakePanels()
{
    return {{{{0, -.25F, 0}, {9000, 0, 0}, {0, 0, -9000}}, // landscape
             {{75, 15, 60}, {70, 0, 0}, {0, 15, 0}},       // distant ridge across the opening
             {{0, 0, 0}, {15, 0, 0}, {0, 0, -15}},
             {{-3, 1.25F, 0}, {0, 0, 4}, {0, 1.25F, 0}},
             {{3, 1.25F, 0}, {0, 0, 4}, {0, 1.25F, 0}},
             {{0, 1.25F, -4}, {3, 0, 0}, {0, 1.25F, 0}},
             {{-2.25F, 1.25F, 4}, {.75F, 0, 0}, {0, 1.25F, 0}},
             {{2.25F, 1.25F, 4}, {.75F, 0, 0}, {0, 1.25F, 0}},
             {{0, 1.25F, 4}, {1.5F, 0, 0}, {0, 1.25F, 0}},
             {{0, 2.5F, 0}, {3, 0, 0}, {0, 0, -4}}}};
}

inline float32 GetShutterTranslation(Shutter shutter)
{
    if (shutter == Shutter::Narrow)
    {
        return 0.35F;
    }
    return shutter == Shutter::Closed ? 0 : 3;
}

} // namespace ludus::graphics::renderer::testing
