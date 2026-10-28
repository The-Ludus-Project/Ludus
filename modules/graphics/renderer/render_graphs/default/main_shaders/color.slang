SurfaceSample MaterialMain(in Varying value)
{
    return SampleMaterial(value);
}

float4 ForwardMain(in Varying value)
{
    return ShadeMaterial(MaterialMain(value), value);
}
