// GetEmitterBounds check: column x below GetEmitterCount recomputes emitter x's bounds from
// GetEmitterTriangle and GetMaterial and compares them with the precomputed record.
// Green = 1 when they agree and red = 1 when they do not. Column GetEmitterCount reads one past
// the last emitter: blue = 1 when that read reports IsValid=false.
// Float32 sums on the host may differ from this recomputation by a few units in the last place.
static const float POSITION_TOLERANCE = 1e-4f; // Relative to max(1, radius), in world units.
static const float COSINE_TOLERANCE = 1e-5f;

float3 ComputeCenterWorld(SceneContext scene, uint emitterIndex, uint triangleCount)
{
    const TriangleData first = GetEmitterTriangle(scene, emitterIndex, 0);
    float3 minimumWorld = first.Position0World;
    float3 maximumWorld = first.Position0World;
    for (uint i = 0; i < triangleCount; ++i)
    {
        const TriangleData triangle = GetEmitterTriangle(scene, emitterIndex, i);
        minimumWorld =
            min(minimumWorld, min(triangle.Position0World, min(triangle.Position1World, triangle.Position2World)));
        maximumWorld =
            max(maximumWorld, max(triangle.Position0World, max(triangle.Position1World, triangle.Position2World)));
    }
    return (minimumWorld + maximumWorld) * 0.5f;
}

bool AreBoundsConsistent(SceneContext scene, uint emitterIndex)
{
    const EmitterBounds bounds = GetEmitterBounds(scene, emitterIndex);
    const uint triangleCount = uint(GetEmitter(scene, emitterIndex).y);
    if (bounds.IsValid == false || triangleCount == 0 || abs(length(bounds.NormalAxisWorld) - 1) > COSINE_TOLERANCE)
    {
        return false;
    }
    const float3 centerWorld = ComputeCenterWorld(scene, emitterIndex, triangleCount);
    float radiusWorld = 0;
    float minimumCosine = 1;
    bool isEmissionTwoSided =
        GetMaterial(scene, GetEmitterTriangle(scene, emitterIndex, 0).MaterialIndex).IsEmissionTwoSided;
    for (uint i = 0; i < triangleCount; ++i)
    {
        const TriangleData triangle = GetEmitterTriangle(scene, emitterIndex, i);
        radiusWorld = max(radiusWorld, length(triangle.Position0World - centerWorld));
        radiusWorld = max(radiusWorld, length(triangle.Position1World - centerWorld));
        radiusWorld = max(radiusWorld, length(triangle.Position2World - centerWorld));
        minimumCosine = min(minimumCosine, dot(normalize(triangle.GeometricNormalWorld), bounds.NormalAxisWorld));
    }
    const float scale = max(1.0f, radiusWorld) * POSITION_TOLERANCE;
    // -1 is the documented whole-sphere cone; it bounds every normal without matching the minimum.
    const bool isConeTight =
        bounds.CosConeHalfAngle == -1 || abs(minimumCosine - bounds.CosConeHalfAngle) <= COSINE_TOLERANCE;
    return all(abs(centerWorld - bounds.CenterWorld) <= scale) && abs(radiusWorld - bounds.RadiusWorld) <= scale &&
           minimumCosine >= bounds.CosConeHalfAngle - COSINE_TOLERANCE && isConeTight == true &&
           isEmissionTwoSided == bounds.IsEmissionTwoSided;
}

float3 ShadeEmitterBounds(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    const uint emitterCount = GetEmitterCount(scene);
    const uint column = launch.PositionPixel.x;
    if (column < emitterCount)
    {
        const bool isConsistent = AreBoundsConsistent(scene, column);
        return float3(isConsistent == true ? 0 : 1, isConsistent == true ? 1 : 0, 0);
    }
    if (column == emitterCount)
    {
        return float3(0, 0, GetEmitterBounds(scene, column).IsValid == false ? 1 : 0);
    }
    return float3(0);
}
