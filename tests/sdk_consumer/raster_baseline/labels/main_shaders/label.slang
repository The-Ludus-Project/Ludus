// Categorical label export for the saved World label baseline.
// Writes the renderer's exact visibility identity: RGB is the mesh-local triangle and A is the
// extraction instance plus one, with zero reserved for the background. The bytes are labels,
// not colour: the pass is opaque, single-sample and unfiltered, and nothing tone maps it.
float4 WriteLabel(VisibilityRasterVarying input)
{
    return EncodeVisibility(uint(round(input.DrawInstance.y)), GetRasterPrimitiveIndex(input.Primitive));
}
