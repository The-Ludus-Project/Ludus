float3 ReadDiffraction(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    const float3 diffraction = GraphColorInput.Load(int3(launch.PositionPixel, 0)).rgb;
    return diffraction.bgr;
}
