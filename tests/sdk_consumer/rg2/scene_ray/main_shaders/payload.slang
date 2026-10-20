#pragma once
// Shader-local user data. The renderer does not interpret these fields.
struct ScenePayload
{
    float3 Radiance;
    RayLaunch Launch;
};
