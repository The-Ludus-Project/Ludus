// Thanks to Tristan Lorach, "vkFX: Effect(ive) approach for Vulkan API" (2017), slides 29–33,
// https://www.slideshare.net/slideshow/vkfx-effective-approach-for-vulkan-api/78873976
// for composing shader behavior across passes. These fixtures share a typed Slang helper
// through explicit color and depth entry points.
// Authored masked coverage shared by the masked color and depth-only techniques.
// Fragments left of the per-draw mesh-space threshold are not covered.
void ApplyCoverage(RasterVarying value)
{
    if (value.PositionMesh.x < mask.Threshold)
    {
        discard;
    }
}
