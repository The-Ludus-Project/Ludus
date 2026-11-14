#include "r10.slang"

Mrt CoverageMain(Varying input)
{
    const float QUARTER_WIDTH = 0.25;
    float coverage = input.uv.x < QUARTER_WIDTH ? 0 : input.uv.x < 1 - QUARTER_WIDTH ? 0.5 : 1;
    Mrt output;
    output.first = float4(1, 0, 0, coverage);
    output.second = float4(0, 1, 0, 1);
    return output;
}
