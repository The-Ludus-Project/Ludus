#pragma once
/// Default shader-local state shared by a ray pipeline's callbacks.
/// Ray generation initializes this value before dispatching hit or miss callbacks.
/// Supply a manifest payload source when additional project-owned fields are needed.
struct ScenePayload
{
    float3 Radiance; ///< Linear RGB radiance written by the selected hit or miss callback.
};
