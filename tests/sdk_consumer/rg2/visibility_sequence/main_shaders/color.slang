float4 ShadeColor(RasterVarying input)
{
    return EncodeVisibility(uint(round(input.DrawInstance.y)), GetRasterPrimitiveIndex(input.Primitive));
}
