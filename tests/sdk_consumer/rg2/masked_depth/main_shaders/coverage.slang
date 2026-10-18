// Authored masked coverage shared by the masked color and depth-only techniques.
// Fragments left of the per-draw mesh-space threshold are not covered.
void ApplyCoverage(RasterVarying value)
{
    if (value.PositionMesh.x < mask.Threshold)
    {
        discard;
    }
}
