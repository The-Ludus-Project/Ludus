#include "radiance.slang"
// Same opaque white patch and packed channels as the moment fit.
float4 PSMain(in Varying input)
{
    SurfaceSample surface = SampleMaterial(input);
    surface.BaseColor = float4(1);
    return EncodeRadiance(ShadeMaterial(surface, input).r);
}
