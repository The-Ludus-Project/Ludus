#include "math.slang"
#include "random_parity_layout.h"
#include "raster_bindings.slang"

// Test-only kernel for the native renderer test (gpu_random_tests.cpp) and the
// WebGPU browser fixture (random_parity.cpp): evaluates FoundationMath's shader
// addressed-random port for host-supplied cases so the host can compare every
// word with the CPU reference. random_parity_layout.h owns the case ABI and
// random_parity_corpus.h the cases.

LUDUS_SHADER_BINDING(0) StructuredBuffer<uint4> Cases;
LUDUS_SHADER_BINDING(1) RWStructuredBuffer<uint4> Results;

// Rejects every attempt in the budget and would accept one extra attempt, so an
// off-by-one loop reports success instead of exhaustion.
struct ExhaustingAttemptSource : IRandomAttemptSource
{
    uint Draw(uint attempt)
    {
        return attempt >= RANDOM_ATTEMPT_COUNT ? 1 : 0;
    }
};

// Accepts only the final attempt of the budget.
struct LastAttemptSource : IRandomAttemptSource
{
    uint Draw(uint attempt)
    {
        return attempt == RANDOM_ATTEMPT_COUNT - 1 ? 1 : 0;
    }
};

struct ConstantAttemptSource : IRandomAttemptSource
{
    uint Value;
    uint Draw(uint attempt)
    {
        return Value;
    }
};

uint4 BoundedResult(bool isSampled, uint value)
{
    return uint4(value, isSampled ? LUDUS_RANDOM_PARITY_IS_SAMPLED : 0, 0, 0);
}

uint4 EvaluateCase(uint4 header, uint4 operand)
{
    uint2 key = header.yz;
    uint bound = header.w;
    uint value;
    switch (header.x)
    {
        case LUDUS_RANDOM_PARITY_CASE_PHILOX:
            return Philox4x32(operand, key);
        case LUDUS_RANDOM_PARITY_CASE_SAMPLE: {
            RandomAddress address = MakeRandomAddress(operand.xy, operand.z, operand.w);
            bool isSampled = TrySampleRandomBounded(key, address, bound, value);
            return uint4(SampleRandomUInt32(key, address),
                         asuint(SampleRandomFloat01(key, address)),
                         value,
                         (isSampled ? LUDUS_RANDOM_PARITY_IS_SAMPLED : 0) |
                             (IsRandomAddressInRange(address) ? LUDUS_RANDOM_PARITY_IS_IN_RANGE : 0));
        }
        case LUDUS_RANDOM_PARITY_CASE_MULTIPLY: {
            uint productLow;
            uint productHigh;
            MultiplyWide(operand.x, operand.y, productLow, productHigh);
            return uint4(productLow, productHigh, 0, 0);
        }
        case LUDUS_RANDOM_PARITY_CASE_CONSTANTS:
            return uint4(RANDOM_ADDRESS_LAYOUT_VERSION,
                         RANDOM_BOUNDED_MAPPING_VERSION,
                         RANDOM_MAX_DIMENSION,
                         RANDOM_ATTEMPT_COUNT);
        case LUDUS_RANDOM_PARITY_CASE_FORCED_EXHAUSTION: {
            ExhaustingAttemptSource source;
            bool isSampled = TrySampleRandomBoundedFrom(source, bound, value);
            return BoundedResult(isSampled, value);
        }
        case LUDUS_RANDOM_PARITY_CASE_FORCED_LAST_ATTEMPT: {
            LastAttemptSource source;
            bool isSampled = TrySampleRandomBoundedFrom(source, bound, value);
            return BoundedResult(isSampled, value);
        }
        case LUDUS_RANDOM_PARITY_CASE_FORCED_CONSTANT: {
            ConstantAttemptSource source;
            source.Value = operand.x;
            bool isSampled = TrySampleRandomBoundedFrom(source, bound, value);
            return BoundedResult(isSampled, value);
        }
        case LUDUS_RANDOM_PARITY_CASE_FLOAT:
            return uint4(asuint(RandomFloat01(operand.x)), 0, 0, 0);
        default:
            return uint4(0);
    }
}

[shader("compute")][numthreads(LUDUS_RANDOM_PARITY_GROUP_SIZE, 1, 1)] void
ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint index = dispatchThreadId.x;
    Results[index] = EvaluateCase(Cases[index * LUDUS_RANDOM_PARITY_WORDS_PER_CASE],
                                  Cases[index * LUDUS_RANDOM_PARITY_WORDS_PER_CASE + 1]);
}
