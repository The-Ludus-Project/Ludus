// Thanks to NIST, "Secure Hash Standard (SHS)", FIPS PUB 180-4 (2015), sections
// 4.1.2/4.2.2, 5.1.1/5.2.1/5.3.3 and 6.2, for the SHA-256 functions, constants,
// preprocessing and compression algorithm: https://doi.org/10.6028/NIST.FIPS.180-4
// This specification citation identifies the algorithm; it does not establish
// the implementation's original code provenance or imply FIPS validation.
#include <ludus/content/content.h>

#if defined(__aarch64__) && defined(__ARM_FEATURE_SHA2)
#    include <arm_neon.h>
#endif

namespace ludus::content
{
namespace
{
constexpr uint32 K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
uint32 Rotate(uint32 value, uint32 amount) noexcept
{
    return (value >> amount) | (value << (32 - amount));
}
void Block(const uint8* input, uint32* state) noexcept
{
    uint32 w[64]{};
    for (usize i = 0; i < 16; ++i)
    {
        w[i] = (static_cast<uint32>(input[i * 4]) << 24) | (static_cast<uint32>(input[i * 4 + 1]) << 16) |
               (static_cast<uint32>(input[i * 4 + 2]) << 8) | input[i * 4 + 3];
    }
    for (usize i = 16; i < 64; ++i)
    {
        const auto a = w[i - 15], b = w[i - 2];
        w[i] = w[i - 16] + (Rotate(a, 7) ^ Rotate(a, 18) ^ (a >> 3)) + w[i - 7] +
               (Rotate(b, 17) ^ Rotate(b, 19) ^ (b >> 10));
    }
    auto a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
    for (usize i = 0; i < 64; ++i)
    {
        const auto t1 = h + (Rotate(e, 6) ^ Rotate(e, 11) ^ Rotate(e, 25)) + ((e & f) ^ ((~e) & g)) + K[i] + w[i];
        const auto t2 = (Rotate(a, 2) ^ Rotate(a, 13) ^ Rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}
/// Bytes in one SHA-256 message block.
constexpr usize BLOCK_BYTES = 64;
#if defined(__aarch64__) && defined(__ARM_FEATURE_SHA2)
// Thanks to Arm, "Arm C Language Extensions" (ACLE), Cryptographic extensions
// (SHA2 intrinsics): https://arm-software.github.io/acle/. Each SHA256H/SHA256H2
// pair performs four rounds; SHA256SU0/SU1 extend the message schedule. The
// four-round grouping follows Jeffrey Walton's public-domain SHA-Intrinsics
// (https://github.com/noloader/SHA-Intrinsics); this is a separate implementation.
// Selected at compile time only; other targets use the portable Block above.
/// 32-bit words per vector register and per four-round group.
constexpr usize LANE_WORDS = 4;
/// Bytes per vector register.
constexpr usize LANE_BYTES = LANE_WORDS * sizeof(uint32);
/// Message vectors per block: BLOCK_BYTES / LANE_BYTES.
constexpr usize MESSAGE_VECTORS = BLOCK_BYTES / LANE_BYTES;
/// Four-round groups per block (64 rounds).
constexpr usize ROUND_GROUPS = sizeof(K) / sizeof(K[0]) / LANE_WORDS;
/// Groups whose message vector must still be extended for a later group.
constexpr usize EXTENDED_GROUPS = ROUND_GROUPS - MESSAGE_VECTORS;
void CompressBlocks(const uint8* input, usize blockCount, uint32* state) noexcept
{
    uint32x4_t abcd = vld1q_u32(state);
    uint32x4_t efgh = vld1q_u32(state + LANE_WORDS);
    for (usize block = 0; block < blockCount; ++block)
    {
        const uint8* bytes = input + block * BLOCK_BYTES;
        const uint32x4_t savedAbcd = abcd;
        const uint32x4_t savedEfgh = efgh;
        uint32x4_t message[MESSAGE_VECTORS];
        for (usize vector = 0; vector < MESSAGE_VECTORS; ++vector)
        {
            message[vector] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(bytes + vector * LANE_BYTES)));
        }
        for (usize group = 0; group < ROUND_GROUPS; ++group)
        {
            const usize current = group % MESSAGE_VECTORS;
            const uint32x4_t scheduled = vaddq_u32(message[current], vld1q_u32(K + group * LANE_WORDS));
            if (group < EXTENDED_GROUPS)
            {
                message[current] =
                    vsha256su1q_u32(vsha256su0q_u32(message[current], message[(current + 1) % MESSAGE_VECTORS]),
                                    message[(current + 2) % MESSAGE_VECTORS],
                                    message[(current + MESSAGE_VECTORS - 1) % MESSAGE_VECTORS]);
            }
            const uint32x4_t previousAbcd = abcd;
            abcd = vsha256hq_u32(abcd, efgh, scheduled);
            efgh = vsha256h2q_u32(efgh, previousAbcd, scheduled);
        }
        abcd = vaddq_u32(abcd, savedAbcd);
        efgh = vaddq_u32(efgh, savedEfgh);
    }
    vst1q_u32(state, abcd);
    vst1q_u32(state + LANE_WORDS, efgh);
}
#else
void CompressBlocks(const uint8* input, usize blockCount, uint32* state) noexcept
{
    for (usize block = 0; block < blockCount; ++block)
    {
        Block(input + block * BLOCK_BYTES, state);
    }
}
#endif
} // namespace
bool Hasher::Add(std::span<const uint8> data) noexcept
{
    if (data.size() > static_cast<uint64>(-1) / 8 - mBytes)
    {
        return false;
    }
    mBytes += data.size();
    const uint8* input = data.data();
    usize remaining = data.size();
    // Complete a buffered partial block first, then compress whole blocks in place.
    if (mCount != 0)
    {
        const usize space = BLOCK_BYTES - mCount;
        const usize take = remaining < space ? remaining : space;
        for (usize i = 0; i < take; ++i)
        {
            mTail[mCount + i] = input[i];
        }
        mCount += take;
        input += take;
        remaining -= take;
        if (mCount < BLOCK_BYTES)
        {
            return true;
        }
        CompressBlocks(mTail, 1, mState);
        mCount = 0;
    }
    const usize blockCount = remaining / BLOCK_BYTES;
    CompressBlocks(input, blockCount, mState);
    input += blockCount * BLOCK_BYTES;
    remaining -= blockCount * BLOCK_BYTES;
    for (usize i = 0; i < remaining; ++i)
    {
        mTail[i] = input[i];
    }
    mCount = remaining;
    return true;
}
Digest Hasher::Finish() const noexcept
{
    uint32 state[8];
    for (usize i = 0; i < 8; ++i)
    {
        state[i] = mState[i];
    }
    uint8 tail[128]{};
    for (usize i = 0; i < mCount; ++i)
    {
        tail[i] = mTail[i];
    }
    tail[mCount] = 0x80;
    const usize bytes = mCount < 56 ? 64 : 128;
    const uint64 bits = mBytes * 8;
    for (usize i = 0; i < 8; ++i)
    {
        tail[bytes - 1 - i] = static_cast<uint8>(bits >> (i * 8));
    }
    Block(tail, state);
    if (bytes == 128)
    {
        Block(tail + 64, state);
    }
    Digest result;
    for (usize i = 0; i < 32; ++i)
    {
        result.Data[i] = static_cast<uint8>(state[i / 4] >> (24 - (i % 4) * 8));
    }
    return result;
}
Digest Hash(std::span<const uint8> data) noexcept
{
    Hasher hash;
    (void)hash.Add(data);
    return hash.Finish();
}
} // namespace ludus::content
