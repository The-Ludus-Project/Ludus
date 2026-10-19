// Default render graph geometry: transform PositionMesh by the per-instance
// MeshToClip rows and pass the flat instance color through.
RasterVarying VSMain(RendererStatic value)
{
    RasterVarying result;
    float4 positionMesh = float4(value.PositionMesh, 1);
    result.PositionClip = float4(dot(value.MeshToClipRow0, positionMesh),
                                 dot(value.MeshToClipRow1, positionMesh),
                                 dot(value.MeshToClipRow2, positionMesh),
                                 dot(value.MeshToClipRow3, positionMesh));
    result.Color = value.Color;
    return result;
}
