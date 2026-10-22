#pragma once

#include <ludus/foundation/base/types.h>

namespace ludus::editor
{
/// Per-camera-sample query admission for the package's shared scene-ray technique.
/// Bounds come from the shared build service's scene_interface policy at configure time.
struct RenderGraphSceneQueryBudgets final
{
    static const foundation::int32 MIN_SURFACE_QUERIES;
    static const foundation::int32 MAX_SURFACE_QUERIES;
    static const foundation::int32 MIN_VISIBILITY_QUERIES;
    static const foundation::int32 MAX_VISIBILITY_QUERIES;

    foundation::int32 SurfaceQueries = MIN_SURFACE_QUERIES;
    foundation::int32 VisibilityQueries = 1;
};
} // namespace ludus::editor
