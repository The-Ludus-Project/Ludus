#pragma once
#include "core.slang"

// Backprojection and the pinhole camera surface-to-pixel Jacobian (section 4.2.1).
// Recover the near-plane basis from the public inverse camera transform, avoiding
// assumptions about FOV/aspect, perspective skew or an engine-private uniform layout.
bool TryProjectFgPhoton(RayLaunch launch, float3 positionWorld, float3 normalWorld,
                         out uint outPixel, out float outJacobian)
{
    outPixel = 0;
    outJacobian = 0;
    float4 centerH = mul(launch.CameraClipToWorld, float4(0, 0, 0, 1));
    float4 rightH = mul(launch.CameraClipToWorld, float4(1, 0, 0, 1));
    float4 upH = mul(launch.CameraClipToWorld, float4(0, 1, 0, 1));
    if (centerH.w == 0 || rightH.w == 0 || upH.w == 0) return false;
    float3 centerWorld = centerH.xyz / centerH.w;
    float3 rightWorld = rightH.xyz / rightH.w - centerWorld;
    float3 upWorld = upH.xyz / upH.w - centerWorld;
    float3 planeNormalWorld = normalize(cross(rightWorld, upWorld));
    float3 offsetWorld = positionWorld - launch.CameraPositionWorld;
    float3 planeOffsetWorld = centerWorld - launch.CameraPositionWorld;
    float planeDistanceWorld = abs(dot(planeOffsetWorld, planeNormalWorld));
    float distanceWorld = length(offsetWorld);
    float denominator = dot(offsetWorld, planeNormalWorld);
    float planeRatio = dot(planeOffsetWorld, planeNormalWorld) / denominator;
    if (launch.IsOrthographic == true || distanceWorld <= 0 || planeRatio <= 0 || planeRatio > 1) return false;
    float3 projectedWorld = launch.CameraPositionWorld + planeRatio * offsetWorld - centerWorld;
    float rr = dot(rightWorld, rightWorld);
    float uu = dot(upWorld, upWorld);
    float ru = dot(rightWorld, upWorld);
    float determinant = rr * uu - ru * ru;
    if (determinant <= 0) return false;
    float2 positionClip = float2(dot(projectedWorld, rightWorld) * uu - dot(projectedWorld, upWorld) * ru,
                                 dot(projectedWorld, upWorld) * rr - dot(projectedWorld, rightWorld) * ru) / determinant;
    if (any(positionClip < -1) == true || any(positionClip >= 1) == true) return false;
    float2 positionUv = float2(positionClip.x + 1, 1 - positionClip.y) * 0.5;
    uint2 positionPixel = min(uint2(positionUv * float2(launch.ExtentPixel)), launch.ExtentPixel - 1);
    outPixel = positionPixel.y * launch.ExtentPixel.x + positionPixel.x;
    float cosine = abs(denominator) / distanceWorld;
    float pixelAreaWorld = 4 * length(cross(rightWorld, upWorld)) / float(GetFgPixelCount(launch));
    float pixelSolidAngle = pixelAreaWorld * cosine * cosine * cosine / (planeDistanceWorld * planeDistanceWorld);
    outJacobian = max(0, dot(normalWorld, -offsetWorld / distanceWorld)) / (distanceWorld * distanceWorld * pixelSolidAngle);
    return isfinite(outJacobian) == true && outJacobian > 0;
}
