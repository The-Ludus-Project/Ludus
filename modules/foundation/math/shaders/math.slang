#pragma once

// GPU counterparts of FoundationMath operations; Slang owns shader scalar spellings.
static const float PI = 3.14159265358979323846;
float3 NormalizeOrZero(float3 v)
{
    float s = dot(v, v);
    return s > 0 ? v * rsqrt(s) : float3(0);
}
bool HasPositiveComponent(float3 v)
{
    return any(v > 0);
}
// Arctangent within 3 ulp on every backend, measured against float64 over float32 inputs of
// magnitude 1e-6 to 1e6. Vulkan bounds atan only to 4096 ulp; Mesa lowers it to a polynomial
// whose absolute error reaches 3.3e-6 near |x| = 1 on lavapipe 25.2, and differences of such
// angles lose that much in every result. Thanks to Stephen L. Moshier, Cephes Math Library,
// single precision atanf.c, https://www.netlib.org/cephes/ (single.tgz): reduction by
// tan(3 pi / 8) and tan(pi / 8) to |x| <= tan(pi / 8), then its odd degree-9 polynomial.
static const float ARCTANGENT_UPPER_REDUCTION = 2.414213562373095;  // tan(3 pi / 8)
static const float ARCTANGENT_LOWER_REDUCTION = 0.4142135623730950; // tan(pi / 8)
// Odd polynomial coefficients of x^9, x^7, x^5 and x^3; the x coefficient is one.
static const float ARCTANGENT_COEFFICIENT_9 = 8.05374449538e-2;
static const float ARCTANGENT_COEFFICIENT_7 = -1.38776856032e-1;
static const float ARCTANGENT_COEFFICIENT_5 = 1.99777106478e-1;
static const float ARCTANGENT_COEFFICIENT_3 = -3.33329491539e-1;
float ComputeArctangent(float x)
{
    float magnitude = abs(x);
    float offset = 0;
    float reduced = magnitude;
    if (magnitude > ARCTANGENT_UPPER_REDUCTION)
    {
        offset = 0.5 * PI;
        reduced = -1.0 / magnitude;
    }
    else if (magnitude > ARCTANGENT_LOWER_REDUCTION)
    {
        offset = 0.25 * PI;
        reduced = (magnitude - 1.0) / (magnitude + 1.0);
    }
    float square = reduced * reduced;
    float polynomial = ARCTANGENT_COEFFICIENT_9 * square + ARCTANGENT_COEFFICIENT_7;
    polynomial = polynomial * square + ARCTANGENT_COEFFICIENT_5;
    polynomial = polynomial * square + ARCTANGENT_COEFFICIENT_3;
    float result = offset + (polynomial * square * reduced + reduced);
    return x < 0 ? -result : result;
}
// Standard normal quantile. Thanks to Michael J. Wichura, "Algorithm AS 241: The Percentage Points of the Normal
// Distribution", Journal of the Royal Statistical Society, Series C (Applied Statistics) 37(3), 1988, pp. 477-484,
// https://doi.org/10.2307/2347330 : PPND16's three degree-7 rational approximations, with the published coefficients.
// The GPU counterpart of ComputeStandardNormalQuantile(float32) in ludus/foundation/math/scalar.hpp, which evaluates
// the same polynomials in the same Horner order; scalar_tests.cpp checks that overload against the float64 one and
// the normal CDF. Each helper lists its coefficients from the highest degree to the constant term.
float EvaluateQuantileCentralNumerator(float x)
{
    float result = 2.5090809287301226727e+3;
    result = result * x + 3.3430575583588128105e+4;
    result = result * x + 6.7265770927008700853e+4;
    result = result * x + 4.5921953931549871457e+4;
    result = result * x + 1.3731693765509461125e+4;
    result = result * x + 1.9715909503065514427e+3;
    result = result * x + 1.3314166789178437745e+2;
    result = result * x + 3.3871328727963666080e+0;
    return result;
}
float EvaluateQuantileCentralDenominator(float x)
{
    float result = 5.2264952788528545610e+3;
    result = result * x + 2.8729085735721942674e+4;
    result = result * x + 3.9307895800092710610e+4;
    result = result * x + 2.1213794301586595867e+4;
    result = result * x + 5.3941960214247511077e+3;
    result = result * x + 6.8718700749205790830e+2;
    result = result * x + 4.2313330701600911252e+1;
    result = result * x + 1.0;
    return result;
}
float EvaluateQuantileIntermediateNumerator(float x)
{
    float result = 7.74545014278341407640e-4;
    result = result * x + 2.27238449892691845833e-2;
    result = result * x + 2.41780725177450611770e-1;
    result = result * x + 1.27045825245236838258e+0;
    result = result * x + 3.64784832476320460504e+0;
    result = result * x + 5.76949722146069140550e+0;
    result = result * x + 4.63033784615654529590e+0;
    result = result * x + 1.42343711074968357734e+0;
    return result;
}
float EvaluateQuantileIntermediateDenominator(float x)
{
    float result = 1.05075007164441684324e-9;
    result = result * x + 5.47593808499534494600e-4;
    result = result * x + 1.51986665636164571966e-2;
    result = result * x + 1.48103976427480074590e-1;
    result = result * x + 6.89767334985100004550e-1;
    result = result * x + 1.67638483018380384940e+0;
    result = result * x + 2.05319162663775882187e+0;
    result = result * x + 1.0;
    return result;
}
float EvaluateQuantileTailNumerator(float x)
{
    float result = 2.01033439929228813265e-7;
    result = result * x + 2.71155556874348757815e-5;
    result = result * x + 1.24266094738807843860e-3;
    result = result * x + 2.65321895265761230930e-2;
    result = result * x + 2.96560571828504891230e-1;
    result = result * x + 1.78482653991729133580e+0;
    result = result * x + 5.46378491116411436990e+0;
    result = result * x + 6.65790464350110377720e+0;
    return result;
}
float EvaluateQuantileTailDenominator(float x)
{
    float result = 2.04426310338993978564e-15;
    result = result * x + 1.42151175831644588870e-7;
    result = result * x + 1.84631831751005468180e-5;
    result = result * x + 7.86869131145613259100e-4;
    result = result * x + 1.48753612908506148525e-2;
    result = result * x + 1.36929880922735805310e-1;
    result = result * x + 5.99832206555887937690e-1;
    result = result * x + 1.0;
    return result;
}
/// Inverse standard normal CDF: the x with Phi(x) = probability, for probability in the open interval (0, 1). Metal
/// compiles with fast math, so there is no NaN sentinel: 0, 1 and values outside the interval return 0. A caller
/// mapping a [0, 1) random number moves it into the open interval first.
float ComputeStandardNormalQuantile(float probability)
{
    static const float CENTRAL_LIMIT = 0.425;
    static const float CENTRAL_OFFSET = 0.180625; // 0.425^2
    static const float INTERMEDIATE_LIMIT = 5;
    static const float INTERMEDIATE_OFFSET = 1.6;
    if (probability <= 0 || probability >= 1)
        return 0;
    float q = probability - 0.5;
    if (abs(q) <= CENTRAL_LIMIT)
    {
        float r = CENTRAL_OFFSET - q * q;
        return q * EvaluateQuantileCentralNumerator(r) / EvaluateQuantileCentralDenominator(r);
    }
    // The smaller tail probability, so that 1 - probability does not cancel below the median.
    float tail = q < 0 ? probability : 1 - probability;
    float r = sqrt(-log(tail));
    float magnitude;
    if (r <= INTERMEDIATE_LIMIT)
    {
        r -= INTERMEDIATE_OFFSET;
        magnitude = EvaluateQuantileIntermediateNumerator(r) / EvaluateQuantileIntermediateDenominator(r);
    }
    else
    {
        r -= INTERMEDIATE_LIMIT;
        magnitude = EvaluateQuantileTailNumerator(r) / EvaluateQuantileTailDenominator(r);
    }
    return q < 0 ? -magnitude : magnitude;
}
// Matches DecodeSrgb's bounded CPU contract. Thanks to W3C, "CSS Color Module
// Level 4", section 10.2, https://www.w3.org/TR/css-color-4/#predefined-sRGB,
// for the piecewise transfer. GPU pow/division can round encoded white above one;
// preserve exact endpoints and bound only normalized color, never radiance.
float DecodeSrgb(float value)
{
    static const uint INVALID_COLOR_BITS = 0x7fc00000;
    static const float LINEAR_THRESHOLD = .04045;
    static const float LINEAR_SCALE = 12.92;
    static const float POWER_OFFSET = .055;
    static const float POWER_SCALE = 1.055;
    static const float GAMMA = 2.4;
    if (isfinite(value) == false || value < 0 || value > 1)
        return asfloat(INVALID_COLOR_BITS);
    if (value == 0 || value == 1)
        return value;
    float linear = value <= LINEAR_THRESHOLD ? value / LINEAR_SCALE : pow((value + POWER_OFFSET) / POWER_SCALE, GAMMA);
    return clamp(linear, 0, 1);
}
// Exact 32x32->64-bit product from 16-bit limbs, because WGSL has no 64-bit
// integers. Every partial sum stays below 2^32: a limb product is at most
// (2^16-1)^2 = 2^32-2^17+1 and each added carry/middle term is below 2^16.
void MultiplyWide(uint a, uint b, out uint outLow, out uint outHigh)
{
    uint aLow = a & 0xffff;
    uint aHigh = a >> 16;
    uint bLow = b & 0xffff;
    uint bHigh = b >> 16;
    uint partial = aLow * bLow;
    uint lowest = partial & 0xffff;
    uint carry = partial >> 16;
    partial = aHigh * bLow + carry;
    uint middle = partial & 0xffff;
    outHigh = partial >> 16;
    partial = aLow * bHigh + middle;
    outHigh += aHigh * bHigh + (partial >> 16);
    outLow = (partial << 16) | lowest;
}

// Addressed random v1 for kernels: a bit-exact port of FoundationMath's
// ludus/foundation/math/addressed_random.hpp, not a separate visual hash.
// docs/architecture/randomness.md#gpu-addressed-sampling owns the contract;
// renderer GPU tests compare this code with the CPU reference and pinned
// Random123 known answers. The owner's C++ constants define these values;
// those tests read the shader copies back and compare them.
static const uint RANDOM_ADDRESS_LAYOUT_VERSION = 1;
static const uint RANDOM_BOUNDED_MAPPING_VERSION = 1;
// Largest Dimension in layout v1 (the CPU field is uint16).
static const uint RANDOM_MAX_DIMENSION = 0xffff;
// Retry budget of TrySampleRandomBounded: attempts 0..65535.
static const uint RANDOM_ATTEMPT_COUNT = 0x10000;

// CPU RandomAddress with Scope split into (low32, high32) words. WGSL has no
// uint64, so a 64-bit scope is carried as two explicit words; a 32-bit owner
// identity passes uint2(identity, 0). Dimension must not exceed
// RANDOM_MAX_DIMENSION. Check IsRandomAddressInRange (or use the bounded sampler,
// which checks it) before relying on a computed dimension.
struct RandomAddress
{
    uint2 Scope;
    uint Event;
    uint Dimension;
};

RandomAddress MakeRandomAddress(uint2 scope, uint event, uint dimension)
{
    RandomAddress address;
    address.Scope = scope;
    address.Event = event;
    address.Dimension = dimension;
    return address;
}

bool IsRandomAddressInRange(RandomAddress address)
{
    return address.Dimension <= RANDOM_MAX_DIMENSION;
}

// Thanks to John K. Salmon et al., "Parallel Random Numbers: As Easy as 1, 2, 3",
// SC11 (2011), section 4.3, Philox4x32-10; constants and round order match the
// CPU reference, which pins Random123 revision 9545ff6413f258be2f04c1d319d99aaef7521150.
// https://www.thesalmons.org/john/random123/papers/random123sc11.pdf
// key.x/key.y are the low/high words of the CPU RandomKey value.
uint4 Philox4x32(uint4 counter, uint2 key)
{
    for (uint roundIndex = 0; roundIndex < 10; ++roundIndex)
    {
        uint product0Low;
        uint product0High;
        uint product1Low;
        uint product1High;
        MultiplyWide(0xd2511f53, counter.x, product0Low, product0High);
        MultiplyWide(0xcd9e8d57, counter.z, product1Low, product1High);
        counter = uint4(product1High ^ counter.y ^ key.x, product1Low, product0High ^ counter.w ^ key.y, product0Low);
        key += uint2(0x9e3779b9, 0xbb67ae85);
    }
    return counter;
}

// Layout v1 packing. Masking the dimension to 16 bits makes an out-of-range
// dimension alias dimension % 65536, exactly like CPU uint16 narrowing; it can
// never spill into the attempt bits [16,31] and alias another address's retry.
uint4 PackRandomCounter(RandomAddress address, uint attempt)
{
    return uint4(address.Scope.x,
                 address.Scope.y,
                 address.Event,
                 ((address.Dimension & RANDOM_MAX_DIMENSION) >> 2) | (attempt << 16));
}

// CPU SampleUInt32: attempt zero, lane Dimension & 3.
uint SampleRandomUInt32(uint2 key, RandomAddress address)
{
    return Philox4x32(PackRandomCounter(address, 0), key)[address.Dimension & 3];
}

// CPU SampleFloat01 and PCG v1 float conversion: high 24 bits times 2^-24, [0,1).
float RandomFloat01(uint bits)
{
    return float(bits >> 8) * (1.0 / 16777216.0);
}

float SampleRandomFloat01(uint2 key, RandomAddress address)
{
    return RandomFloat01(SampleRandomUInt32(key, address));
}

// Raw word for one rejection attempt; lets tests force rejection and exhaustion
// through the same loop that production sampling uses.
interface IRandomAttemptSource
{
    uint Draw(uint attempt);
};

struct AddressedRandomAttemptSource : IRandomAttemptSource
{
    uint2 Key;
    RandomAddress Address;
    uint Draw(uint attempt)
    {
        return Philox4x32(PackRandomCounter(Address, attempt), Key)[Address.Dimension & 3];
    }
};

// Thanks to Daniel Lemire, "Fast Random Integer Generation in an Interval",
// ACM TOMACS 29(1), 2019, Algorithm 5 (https://arxiv.org/abs/1805.10941), as in
// the CPU mapper. Returns false for bound zero or after RANDOM_ATTEMPT_COUNT
// rejected attempts, and outValue is then zero; callers must branch on the result
// rather than treat any value as a sentinel.
bool TrySampleRandomBoundedFrom<Source : IRandomAttemptSource>(Source source, uint bound, out uint outValue)
{
    outValue = 0;
    if (bound == 0)
        return false;
    uint threshold = (0u - bound) % bound;
    for (uint attempt = 0; attempt < RANDOM_ATTEMPT_COUNT; ++attempt)
    {
        uint productLow;
        uint productHigh;
        MultiplyWide(source.Draw(attempt), bound, productLow, productHigh);
        if (productLow >= threshold)
        {
            outValue = productHigh;
            return true;
        }
    }
    return false;
}

// CPU TrySampleBounded in [0,bound). Also returns false for an address outside
// layout v1, so a computed dimension cannot wrap silently here.
bool TrySampleRandomBounded(uint2 key, RandomAddress address, uint bound, out uint outValue)
{
    outValue = 0;
    if (IsRandomAddressInRange(address) == false)
        return false;
    AddressedRandomAttemptSource source;
    source.Key = key;
    source.Address = address;
    return TrySampleRandomBoundedFrom(source, bound, outValue);
}

// UV-derived frame used by renderer raster and ray profiles; see
// docs/architecture/renderer-systems.md#material-and-lighting-fidelity.
// Orthonormalizes against the supplied unit normal, preserving mirrored UV sign.
// Numerically degenerate frames return the original normal without perturbation.
// Input vectors use Slang copy-in semantics; local edge normalization does not
// modify the caller. Do not use inout to express read-only input or avoid copies.
// Thanks to the Slang project, Language Reference, "Declarations", Parameters:
// https://docs.shader-slang.org/en/stable/external/slang/docs/language-reference/declarations.html#parameters
float3 ApplyNormalWorld(in float3 normalWorld,
                        in float3 normalTangent,
                        in float3 edgeUWorld,
                        in float3 edgeVWorld,
                        in float2 edgeUUv,
                        in float2 edgeVUv)
{
    // Normalize edge magnitudes before solving; this makes admission independent
    // of pixel footprint, triangle size and uniform UV tiling scale.
    float uvMagnitude = max(max(abs(edgeUUv.x), abs(edgeUUv.y)), max(abs(edgeVUv.x), abs(edgeVUv.y)));
    float worldMagnitude = max(max(max(abs(edgeUWorld.x), abs(edgeUWorld.y)), abs(edgeUWorld.z)),
                               max(max(abs(edgeVWorld.x), abs(edgeVWorld.y)), abs(edgeVWorld.z)));
    if (uvMagnitude <= 1e-30 || worldMagnitude <= 1e-30)
        return normalWorld;
    edgeUUv /= uvMagnitude;
    edgeVUv /= uvMagnitude;
    edgeUWorld /= worldMagnitude;
    edgeVWorld /= worldMagnitude;
    float determinant = edgeUUv.x * edgeVUv.y - edgeUUv.y * edgeVUv.x;
    if (abs(determinant) <= 1e-6 * max(abs(edgeUUv.x * edgeVUv.y), abs(edgeUUv.y * edgeVUv.x)))
        return normalWorld;
    float3 tangentWorld = (edgeUWorld * edgeVUv.y - edgeVWorld * edgeUUv.y) / determinant;
    float3 bitangentWorld = (edgeVWorld * edgeUUv.x - edgeUWorld * edgeVUv.x) / determinant;
    tangentWorld -= normalWorld * dot(normalWorld, tangentWorld);
    if (dot(tangentWorld, tangentWorld) <= 1e-20)
        return normalWorld;
    tangentWorld = NormalizeOrZero(tangentWorld);
    float handedness = dot(cross(normalWorld, tangentWorld), bitangentWorld) < 0 ? -1 : 1;
    bitangentWorld = cross(normalWorld, tangentWorld) * handedness;
    return NormalizeOrZero(tangentWorld * normalTangent.x + bitangentWorld * normalTangent.y +
                           normalWorld * normalTangent.z);
}
