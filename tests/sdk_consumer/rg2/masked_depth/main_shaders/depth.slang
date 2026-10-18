#include "coverage.slang"

void DepthMain(RasterVarying value)
{
    // Separately authored depth-only entry with the same coverage as the color technique.
    ApplyCoverage(value);
}
