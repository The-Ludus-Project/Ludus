// PCG32 XSH-RR output / bounded / float mapping (design §11). The algorithm is
// adapted from the PCG reference (Apache-2.0; see random.hpp provenance note).
// No <cmath> needed; all integer arithmetic is modulo-2^64 unsigned.

#include <ludus/foundation/math/random.hpp>

namespace ludus::foundation::math
{
uint32 RandomStream::NextUInt32() noexcept
{
    // Compute the output from the OLD state, then advance (PCG order).
    const uint64 oldState = mState;
    stepState();
    // XSH-RR: xorshift ((old>>18)^old)>>27 narrowed to uint32, rotate right by
    // old>>59. Shift counts are chosen so there is never a shift by 32.
    const uint32 xorShifted = static_cast<uint32>(((oldState >> 18u) ^ oldState) >> 27u);
    const uint32 rot = static_cast<uint32>(oldState >> 59u);
    // Rotate right by `rot` in [0,31]. The (-rot & 31) form avoids a shift by 32
    // when rot==0.
    return (xorShifted >> rot) | (xorShifted << ((0u - rot) & 31u));
}

MathStatus RandomStream::TryNextBounded(uint32 bound, uint32& out) noexcept
{
    if (bound == 0u)
    {
        return MathStatus::InvalidArgument; // stream unchanged, out unchanged
    }
    // threshold = (2^32 - bound) % bound = (-bound) % bound in uint32.
    const uint32 threshold = (0u - bound) % bound;
    for (;;)
    {
        const uint32 r = NextUInt32();
        if (r >= threshold)
        {
            out = r % bound;
            return MathStatus::Success;
        }
    }
}

float32 RandomStream::NextFloat01() noexcept
{
    // High 24 bits * 2^-24 -> [0,1). Exactly representable scale.
    const uint32 bits = NextUInt32() >> 8u; // top 24 bits
    return static_cast<float32>(bits) * (1.0f / 16777216.0f);
}
} // namespace ludus::foundation::math
