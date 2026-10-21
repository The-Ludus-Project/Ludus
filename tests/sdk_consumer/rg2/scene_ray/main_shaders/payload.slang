#pragma once
// Shader-local user data. The renderer does not interpret these fields.
struct ScenePayload
{
    float3 Radiance;
    // Reference answers supplied by raygen for callback sampling acceptance.
    float3 ExpectedRandom;
    float ExpectedMissRandom;
    uint ExpectedLightIndex;
};
