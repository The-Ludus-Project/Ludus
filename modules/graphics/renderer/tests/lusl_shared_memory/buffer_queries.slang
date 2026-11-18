// Slang equivalent of buffer_queries.lusl.
#include "raster_bindings.slang"

LUDUS_SHADER_BINDING(0) StructuredBuffer<float4> values LUDUS_SHADER_TEXTURE_REGISTER(0);
LUDUS_SHADER_BINDING(1) RWStructuredBuffer<uint> results LUDUS_SHADER_STORAGE_REGISTER(1);

[shader("compute")][numthreads(1, 1, 1)] void Main() {
    uint count;
    uint stride;
    values.GetDimensions(count, stride);
    results[0] = count;
    results.GetDimensions(count, stride);
    results[1] = count;
}
