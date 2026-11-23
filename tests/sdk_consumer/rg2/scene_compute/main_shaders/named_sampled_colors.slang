float3 WriteSampledColors(SceneContext scene,
                          RayLaunch launch,
                          inout RayQueryContext inoutQueries,
                          WriteSampledColorsResources resources)
{
    resources.Diffraction[launch.PositionPixel] = float4(1, 0, 0, 1);
    resources.Weights[launch.PositionPixel] = float4(0, 1, 0, 1);
    resources.Mask[launch.PositionPixel] = float4(0, 0, 1, 1);
    return float3(0);
}

float3 ReadSampledColors(SceneContext scene,
                         RayLaunch launch,
                         inout RayQueryContext inoutQueries,
                         ReadSampledColorsResources resources)
{
    const int3 texelPixel = int3(launch.PositionPixel, 0);
    return float3(resources.Diffraction.Load(texelPixel).r,
                  resources.Weights.Load(texelPixel).g,
                  resources.Mask.Load(texelPixel).b);
}
