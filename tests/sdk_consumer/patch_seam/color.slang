// Store the interpolated shading normal and conventional depth alongside the shaded coverage mask.
// UNORM16 depth split across two UNORM8 channels survives ordinary RGBA8 viewport readback.
float4 PSMain(in Varying value)
{
    const float depthMaximum = 65535;
    const float byteRadix = 256;
    const float byteMaximum = byteRadix - 1;
    float depth = round(saturate(value.PositionClip.z) * depthMaximum);
    float highByte = floor(depth / byteRadix);
    float normalX = normalize(value.NormalWorld).x;
    return float4(0.5 + 0.5 * normalX,
                  highByte / byteMaximum,
                  (depth - byteRadix * highByte) / byteMaximum,
                  0.5 + 0.25 * saturate(FragmentMain(value).r));
}
