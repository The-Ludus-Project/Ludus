// Slang equivalent of atomics.lusl. Atomic<T> storage, as every portable target (WGSL included)
// requires for atomic operations.
#include "raster_bindings.slang"

static const uint BIN_COUNT = 4;
static const uint WORD_BITS = 32;
static const uint TICKETS = 0;
static const uint MAXIMUM = 1;
static const uint MINIMUM = 2;
static const uint OR_BITS = 3;
static const uint XOR_IDS = 4;
static const uint AND_BITS = 5;
static const uint STORED = 6;
static const uint CLAIMED = 7;
static const uint WINNERS = 8;
static const uint SWAPPED = 9;
static const uint SWAPPED_SUM = 10;
static const uint BINS = 11;
static const uint CLAIM_VALUE = 9;
static const uint SWAP_VALUE = 5;
static const uint STORE_VALUE = 1;

LUDUS_SHADER_BINDING(0) RWStructuredBuffer<Atomic<uint>> counters LUDUS_SHADER_STORAGE_REGISTER(0);
LUDUS_SHADER_BINDING(1) RWStructuredBuffer<uint> results LUDUS_SHADER_STORAGE_REGISTER(1);
LUDUS_SHADER_BINDING(2) RWStructuredBuffer<Atomic<int>> balances LUDUS_SHADER_STORAGE_REGISTER(2);
groupshared Atomic<uint> bins[BIN_COUNT];

[shader("compute")][numthreads(64, 1, 1)] void Main(uint3 id : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
    if (groupIndex < BIN_COUNT)
    {
        bins[groupIndex].store(0);
    }
    GroupMemoryBarrierWithGroupSync();
    const uint ticket = counters[TICKETS].add(1);
    results[ticket] = ticket + 1;
    counters[MAXIMUM].max(id.x);
    counters[MINIMUM].min(id.x);
    const uint bit = 1u << (id.x % WORD_BITS);
    counters[OR_BITS].or (bit);
    counters[XOR_IDS].xor (id.x);
    counters[AND_BITS].and (~bit);
    counters[STORED].compareExchange(0, STORE_VALUE);
    if (counters[CLAIMED].compareExchange(0, CLAIM_VALUE) == 0)
    {
        counters[WINNERS].add(1);
    }
    counters[SWAPPED_SUM].add(counters[SWAPPED].exchange(SWAP_VALUE));
    balances[0].add(-1);
    balances[1].min(-int(id.x));
    bins[groupIndex % BIN_COUNT].add(1);
    GroupMemoryBarrierWithGroupSync();
    if (groupIndex < BIN_COUNT)
    {
        counters[BINS + groupIndex].add(bins[groupIndex].load());
    }
}
