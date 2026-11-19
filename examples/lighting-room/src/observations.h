#pragma once

#include <ludus/foundation/math/vector.hpp>

namespace lighting_room
{
namespace math = ludus::foundation::math;
using ludus::foundation::usize;
/// Centroid of triangle (0, 1, 2) of the built-in unit plane, measured from its centre.
constexpr ludus::foundation::float32 TRIANGLE_CENTROID_OFFSET = 1.0F / 6.0F;
constexpr math::Vector3 BOUNCE_VIEW_OFFSET{0, 0, .12F};
// A point inside the rough patch between the editor's XZ construction-grid lines.
constexpr math::Vector3 ROUGH_PATCH_OFFSET{.027F, 0, .033F};
constexpr math::Vector3 PEAK_VIEW_OFFSET{0, 2, .01F};
constexpr math::Vector3 OBLIQUE_VIEW_OFFSET{0, 1, -2};
constexpr math::Vector3 OVERVIEW_EYE{-.5F, 2, 3.5F};
constexpr math::Vector3 NEUTRAL_COLOR{.8F, .8F, .8F};
constexpr math::Vector3 RECEIVER_PATH[] =
    {{.25F, 1, 0}, {.45F, 1, 0}, {.55F, 1, 0}, {.75F, 1, 0}, {.55F, .75F, 0}, {1.25F, 1, 0}};
constexpr usize PATH_COUNT = sizeof(RECEIVER_PATH) / sizeof(math::Vector3);
enum class Patch
{
    Overview,
    Bounce,
    Rough,
    Moving
};
struct Observation final
{
    const char* Name;
    Patch Target;
    bool HasLiveLight = false;
    bool HasNeutralPanel = false;
    bool HasBlocker = false;
    bool IsOblique = false;
    usize PathIndex = 0;
    bool HasSelectedLight = true;
};
constexpr Observation OBSERVATIONS[] = {
    { .Name = "overview", .Target = Patch::Overview, .HasLiveLight = true },
    { .Name = "saturated", .Target = Patch::Bounce },
    { .Name = "neutral", .Target = Patch::Bounce, .HasNeutralPanel = true },
    { .Name = "blocked", .Target = Patch::Bounce, .HasBlocker = true },
    { .Name = "bounce-live", .Target = Patch::Bounce, .HasLiveLight = true },
    { .Name = "peak", .Target = Patch::Rough },
    { .Name = "oblique", .Target = Patch::Rough, .IsOblique = true },
    { .Name = "rough-live", .Target = Patch::Rough, .HasLiveLight = true },
    { .Name = "live-only", .Target = Patch::Rough, .HasLiveLight = true, .HasSelectedLight = false },
    { .Name = "dark", .Target = Patch::Rough, .HasSelectedLight = false },
    { .Name = "lit-side", .Target = Patch::Moving },
    { .Name = "opening-left", .Target = Patch::Moving, .PathIndex = 1 },
    { .Name = "opening-right", .Target = Patch::Moving, .PathIndex = 2 },
    { .Name = "dark-side", .Target = Patch::Moving, .PathIndex = 3 },
    { .Name = "divider", .Target = Patch::Moving, .PathIndex = 4 },
    { .Name = "outside", .Target = Patch::Moving, .PathIndex = 5 },
    { .Name = "outside-live", .Target = Patch::Moving, .HasLiveLight = true, .PathIndex = 5 },
};
constexpr usize OBSERVATION_COUNT = sizeof(OBSERVATIONS) / sizeof(Observation);
} // namespace lighting_room
