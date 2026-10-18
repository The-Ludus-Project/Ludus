#include "coverage.slang"

float4 PSMain(RasterVarying value)
{
    ApplyCoverage(value);
    return value.Color;
}
