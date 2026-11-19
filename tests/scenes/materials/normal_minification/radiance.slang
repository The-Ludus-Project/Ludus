// ForwardLit exports RGBA8Unorm. Store the top 24 bits of positive binary32
// radiance for readback before any display transform. The white patch is achromatic.
float4 EncodeRadiance(in float radiance)
{
    static const uint BYTE_MASK = 255;
    static const uint BYTE_BITS = 8;
    uint bits = asuint(radiance);
    return float4(
        float3(bits >> (3 * BYTE_BITS), (bits >> (2 * BYTE_BITS)) & BYTE_MASK, (bits >> BYTE_BITS) & BYTE_MASK) /
            float(BYTE_MASK),
        1);
}
