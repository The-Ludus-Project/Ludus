RasterVarying TransformGeometry(RendererStatic input)
{
    RasterVarying result;
    float4 positionMesh = float4(input.PositionMesh, 1);
    result.PositionClip = float4(dot(input.MeshToClipRow0, positionMesh),
                                 dot(input.MeshToClipRow1, positionMesh),
                                 dot(input.MeshToClipRow2, positionMesh),
                                 dot(input.MeshToClipRow3, positionMesh));
    result.Color = input.Color;
    result.Primitive = input.Primitive;
    result.DrawInstance = input.DrawInstance;
    return result;
}
