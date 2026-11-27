#pragma once
// Authored masked coverage shared by the masked visibility and depth-only fragments.
// Fragments left of the per-draw mesh-space threshold are not covered.
void ApplyCoverage(RasterVarying value)
{
    if (value.PositionMesh.x < mask.Threshold)
    {
        discard;
    }
}
