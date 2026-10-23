// Callback data for the Ray/Color render-graph profile.
#ifndef LUDUS_SCENE_RAY_INTERFACE
struct Ray
{
    float3 OriginWorld;
    float MinDistanceWorld;
    float3 DirectionWorld;
    float MaxDistanceWorld;
};
struct RayLaunch
{
    uint2 PositionPixel;
    uint2 ExtentPixel;
    Ray CameraRay;
};
#endif
struct RayHit
{
    Ray IncomingRay;
    float3 PositionWorld;
    float DistanceWorld;
    float3 NormalWorld;
    uint Primitive;
    float2 Barycentric;
    float4 Color;
};
