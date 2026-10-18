float4 PSMain(RasterVarying value)
{
    // Per-draw material values arrive through the generated binding, not a shader permutation.
    return float4(material.Tint * material.Gain, 1);
}
