// Opaque items write their identity everywhere they are visible.
float4 WriteOpaqueVisibility(RasterVarying input)
{
    return EncodeVisibility(uint(round(input.DrawInstance.y)), GetRasterPrimitiveIndex(input.Primitive));
}

void WriteOpaqueDepth(RasterVarying value)
{
    // Authored opaque coverage: no discard or custom depth.
}
