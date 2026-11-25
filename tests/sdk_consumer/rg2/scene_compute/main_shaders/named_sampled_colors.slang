float3 WriteSampledColors(SceneContext scene,
                          RayLaunch launch,
                          inout RayQueryContext inoutQueries,
                          WriteSampledColorsResources resources)
{
    // Only the first dispatch writes outside the launch grid. Weights uses Load so it marks prior dispatches.
    if (all(launch.PositionPixel == 0) == true && resources.Weights[launch.PositionPixel].g == 0)
    {
        uint width;
        uint height;
        resources.Diffraction.GetDimensions(width, height);
        resources.Diffraction[uint2(width - 1, height - 1)] = float4(1, 0, 0, 1);
    }
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
