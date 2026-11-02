#pragma once
#include "path_evaluation.slang"
#include "projection.slang"

struct FgCausticReservoir
{
    FgPhoton Photon;
    float Weight;
    uint Confidence;
    uint SampleIndex;
    uint Version;
    uint4 Identity;
};
uint GetFgCausticAddress(RayLaunch launch, bool isHistory)
{
    uint bank = (launch.SampleIndex + (isHistory == true ? 1 : 0)) % 2;
    return (GetFgPixel(launch) * 2 + bank) * FG_CAUSTIC_WORDS;
}
FgCausticReservoir LoadFgCaustic(RayLaunch launch, bool isHistory)
{
    uint address = GetFgCausticAddress(launch, isHistory);
    FgCausticReservoir result;
    result.Photon = LoadFgPhoton(true, address);
    uint4 header = GraphBuffer1[address + FG_PHOTON_WORDS];
    result.Weight = asfloat(header.x);
    result.Confidence = header.y;
    result.SampleIndex = header.z;
    result.Version = header.w;
    result.Identity = GraphBuffer1[address + FG_PHOTON_WORDS + 1];
    return result;
}
void StoreFgCaustic(RayLaunch launch, FgCausticReservoir reservoir)
{
    uint address = GetFgCausticAddress(launch, false);
    StoreFgPhoton(true, address, reservoir.Photon);
    GraphBuffer1[address + FG_PHOTON_WORDS] = uint4(asuint(reservoir.Weight), reservoir.Confidence, reservoir.SampleIndex, reservoir.Version);
    GraphBuffer1[address + FG_PHOTON_WORDS + 1] = reservoir.Identity;
}
[noinline] float3 EvaluateFgCaustic(SceneContext scene, RayLaunch launch, FgPhoton photon, inout RayQueryContext queries)
{
    if (photon.Triangle == FG_INVALID || (photon.Flags & FG_CAUSTIC_FLAG) == 0) return float3(0);
    SurfaceHit hit = GetSurfaceHit(scene, photon.Triangle, photon.Barycentric);
    FgPoint point;
    float3 directionWorld = normalize(launch.CameraPositionWorld - hit.PositionWorld);
    if (TryMakeFgPoint(scene, hit, directionWorld, queries, point) == false) return float3(0);
    uint pixel;
    float jacobian;
    if (TryProjectFgPhoton(launch, hit.PositionWorld, point.Material.ShadingNormalWorld, pixel, jacobian) == false ||
        pixel != GetFgPixel(launch)) return float3(0);
    // Visibility was tested by the photon pass. Geometry, camera and material changes
    // reset the generation; a static temporal sample retains that visibility result.
    return EvaluateFgBsdf(point, photon.IncomingWorld) * photon.Flux * jacobian;
}
bool HasFgCausticCapacity(RayLaunch launch)
{
    uint count;
    uint stride;
    GraphBuffer1.GetDimensions(count, stride);
    return GetFgPixelCount(launch) <= count / FG_CAUSTIC_WORDS / 2;
}
