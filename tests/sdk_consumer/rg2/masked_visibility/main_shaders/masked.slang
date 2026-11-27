#include "coverage.slang"

// Masked items write their identity only where their coverage keeps the fragment.
float4 WriteMaskedVisibility(RasterVarying input)
{
    ApplyCoverage(input);
    return EncodeVisibility(uint(round(input.DrawInstance.y)), GetRasterPrimitiveIndex(input.Primitive));
}

void WriteMaskedDepth(RasterVarying value)
{
    // The same coverage as the masked visibility fragment.
    ApplyCoverage(value);
}
