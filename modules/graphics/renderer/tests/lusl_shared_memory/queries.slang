// Slang equivalent of queries.lusl.
#include "raster_bindings.slang"

LUDUS_SHADER_BINDING(0) Texture2D<float4> image LUDUS_SHADER_TEXTURE_REGISTER(0);
LUDUS_SHADER_BINDING(1) RWStructuredBuffer<uint> results LUDUS_SHADER_STORAGE_REGISTER(1);

[shader("compute")][numthreads(1, 1, 1)] void Main() {
    uint width;
    uint height;
    image.GetDimensions(width, height);
    results[0] = width;
    results[1] = height;
}
