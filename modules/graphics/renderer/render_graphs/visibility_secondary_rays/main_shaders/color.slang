float4 ShadeColor(VisibilityRasterVarying input)
{
    return EncodeVisibility(uint(round(input.DrawInstance.y)), GetRasterPrimitiveIndex(input.Primitive));
}
