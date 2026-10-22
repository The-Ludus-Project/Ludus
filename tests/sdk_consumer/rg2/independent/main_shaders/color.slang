// Two authored entry points from one source; each pass selects a distinct program.
float4 ColorMain(RasterVarying value)
{
    return float4(1, 0, 0, 1);
}
float4 GBufferMain(RasterVarying value)
{
    const float HALF_WIDTH_PIXEL = 48.0;
    if (value.PositionClip.x < HALF_WIDTH_PIXEL)
    {
        discard;
    }
    return float4(0, 1, 0, 1);
}
