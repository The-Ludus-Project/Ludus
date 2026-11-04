// Thanks to Shlomi Steinberg, "Slice Sampling: Importance sampling with an
// auxiliary map" (University of Waterloo draft), Sections 4 and 7. This
// bounded fixture uses a reciprocal slope lobe with a monotone coupled map.
// Thanks to Eric Veach, Robust Monte Carlo Methods for Light Transport
// Simulation, Chapters 9–10, https://graphics.stanford.edu/papers/veach_thesis/,
// for the three fixed-endpoint connection strategies and balance MIS.
static const float2 ROUGH_PAIR_LIGHT_WORLD = float2(0.26, -0.18);
static const float ROUGH_PAIR_WIDTH = 0.17;
static const float ROUGH_PAIR_CURVATURE = 0.85;
static const float ROUGH_PAIR_HALF_EXTENT = 0.68;
static const uint SOLVER_ITERATIONS = 16;
static const float SLOPE_TOLERANCE = 0.000004;
static const float ROUGH_PAIR_SUPPORT_BOUNDARY_GUARD = 8 * SLOPE_TOLERANCE;

// Midpoints of 23-bit bins are exactly representable in FP32 and avoid
// both singular endpoints of the Gaussian and cosine-slope transforms.
static const uint OPEN_RANDOM_DISCARDED_BITS = 9;
static const float OPEN_RANDOM_BIN_WIDTH = 1.0 / 8388608;

float ComputeRoughPairOpenUniform(uint word)
{
    return (float(word >> OPEN_RANDOM_DISCARDED_BITS) + 0.5) * OPEN_RANDOM_BIN_WIDTH;
}

struct PairResult
{
    float2 FirstPositionWorld;
    float2 SecondPositionWorld;
    float Density;
    float Radiance;
    float HasFailed;
    float ShiftJacobian;
};

struct CachedPairSample
{
    PairResult Fresh;
    PairResult Shifted;
    float FreshWeight;
    float Radiance;
    float HasFailed;
};

float2 WarpSlope(float2 slope)
{
    return slope * (1 + ROUGH_PAIR_CURVATURE * abs(slope));
}

float2 ComputeWarpDerivative(float2 slope)
{
    return 1 + 2 * ROUGH_PAIR_CURVATURE * abs(slope);
}

float2 UnwarpSlope(float2 value)
{
    return 2 * value / (1 + sqrt(1 + 4 * ROUGH_PAIR_CURVATURE * abs(value)));
}

float ComputeGaussian(float2 value)
{
    float variance = ROUGH_PAIR_WIDTH * ROUGH_PAIR_WIDTH;
    return exp(-dot(value, value) / (2 * variance)) / (2 * PI * variance);
}

float ComputeProjectedDensity(float2 slope)
{
    float denominator = 1 + dot(slope, slope);
    return 1 / (PI * denominator * denominator);
}

float2 ComputePolarVector(float radius, float azimuth)
{
    float angle = 2 * PI * azimuth;
    return radius * float2(cos(angle), sin(angle));
}

float2 DrawGaussian(float2 uniform)
{
    return ROUGH_PAIR_WIDTH * ComputePolarVector(sqrt(-2 * log(uniform.x)), uniform.y);
}

float2 DrawCosineSlope(float2 uniform)
{
    return ComputePolarVector(sqrt(uniform.x / (1 - uniform.x)), uniform.y);
}

bool SolvePair(float2 firstSeed,
               float2 secondSeed,
               float2 receiverWorld,
               float2 lightWorld,
               out float2 outFirstWorld,
               out float2 outSecondWorld)
{
    // Eliminate the two positions given their separation. The remaining
    // equation is strictly increasing and bracketed between zero and the
    // separation of the uncoupled roots. Safeguard Newton within this bracket.
    float2 uncoupled = receiverWorld - lightWorld + UnwarpSlope(firstSeed) - UnwarpSlope(secondSeed);
    float2 lower = min(uncoupled, 0);
    float2 upper = max(uncoupled, 0);
    float2 middleSlope = uncoupled / 3;
    for (uint iteration = 0; iteration < SOLVER_ITERATIONS; ++iteration)
    {
        float2 middleWarped = WarpSlope(middleSlope);
        float2 firstSlope = UnwarpSlope(firstSeed - middleWarped);
        float2 secondSlope = UnwarpSlope(secondSeed + middleWarped);
        outFirstWorld = receiverWorld + firstSlope;
        outSecondWorld = lightWorld + secondSlope;
        float2 residual = middleSlope - (outFirstWorld - outSecondWorld);
        bool2 isConverged = abs(residual) <= SLOPE_TOLERANCE;
        if (all(isConverged))
        {
            return true;
        }
        lower = select(residual < 0, middleSlope, lower);
        upper = select(residual > 0, middleSlope, upper);
        float2 derivative = 1 + ComputeWarpDerivative(middleSlope) *
                                    (1 / ComputeWarpDerivative(firstSlope) + 1 / ComputeWarpDerivative(secondSlope));
        float2 candidate = middleSlope - residual / derivative;
        bool2 isInterior = (candidate > lower) & (candidate < upper);
        middleSlope = select(isConverged, middleSlope, select(isInterior, candidate, (lower + upper) * 0.5));
    }
    return false;
}

bool IsOnPatch(float2 positionWorld)
{
    return all(abs(positionWorld) <= ROUGH_PAIR_HALF_EXTENT);
}

float ComputePairJacobian(float2 cameraSlope, float2 lightSlope, float2 middleSlope)
{
    float2 middleDerivative = ComputeWarpDerivative(middleSlope);
    float2 cameraDerivative = ComputeWarpDerivative(cameraSlope);
    float2 lightDerivative = ComputeWarpDerivative(lightSlope);
    float2 determinant = cameraDerivative * lightDerivative + middleDerivative * (cameraDerivative + lightDerivative);
    return determinant.x * determinant.y;
}

float3 EvaluatePair(float2 firstWorld, float2 secondWorld, float2 receiverWorld)
{
    float2 cameraSlope = firstWorld - receiverWorld;
    float2 lightSlope = secondWorld - ROUGH_PAIR_LIGHT_WORLD;
    float2 middleSlope = firstWorld - secondWorld;
    float2 middleWarped = WarpSlope(middleSlope);
    float firstLobe = ComputeGaussian(WarpSlope(cameraSlope) + middleWarped);
    float secondLobe = ComputeGaussian(WarpSlope(lightSlope) - middleWarped);
    float cameraDensity = ComputeProjectedDensity(cameraSlope);
    float lightDensity = ComputeProjectedDensity(lightSlope);
    float middleDensity = ComputeProjectedDensity(middleSlope);
    float2 middleDerivative = ComputeWarpDerivative(middleSlope);
    float physical = firstLobe * secondLobe * cameraDensity * lightDensity * middleDensity * PI * PI * PI;
    float sliceDensity = firstLobe * secondLobe * ComputePairJacobian(cameraSlope, lightSlope, middleSlope);
    float bdptDensity = (cameraDensity * lightDensity + middleDerivative.x * middleDerivative.y *
                                                            (cameraDensity * firstLobe + lightDensity * secondLobe)) /
                        3;
    return float3(physical, sliceDensity, bdptDensity);
}

PairResult SamplePair(bool isSlice, float4 uniforms, float split, float2 receiverWorld)
{
    PairResult result = {};
    if (isSlice)
    {
        if (SolvePair(DrawGaussian(uniforms.xy),
                      DrawGaussian(uniforms.zw),
                      receiverWorld,
                      ROUGH_PAIR_LIGHT_WORLD,
                      result.FirstPositionWorld,
                      result.SecondPositionWorld) == false)
        {
            result.HasFailed = 1;
            return result;
        }
    }
    else if (split < 1.0 / 3)
    {
        result.FirstPositionWorld = receiverWorld + DrawCosineSlope(uniforms.xy);
        result.SecondPositionWorld = ROUGH_PAIR_LIGHT_WORLD + DrawCosineSlope(uniforms.zw);
    }
    else if (split < 2.0 / 3)
    {
        result.FirstPositionWorld = receiverWorld + DrawCosineSlope(uniforms.xy);
        result.SecondPositionWorld =
            result.FirstPositionWorld -
            UnwarpSlope(DrawGaussian(uniforms.zw) - WarpSlope(result.FirstPositionWorld - receiverWorld));
    }
    else
    {
        result.SecondPositionWorld = ROUGH_PAIR_LIGHT_WORLD + DrawCosineSlope(uniforms.zw);
        result.FirstPositionWorld =
            result.SecondPositionWorld -
            UnwarpSlope(DrawGaussian(uniforms.xy) - WarpSlope(result.SecondPositionWorld - ROUGH_PAIR_LIGHT_WORLD));
    }
    if (IsOnPatch(result.FirstPositionWorld) && IsOnPatch(result.SecondPositionWorld))
    {
        float3 values = EvaluatePair(result.FirstPositionWorld, result.SecondPositionWorld, receiverWorld);
        result.Density = isSlice ? values.y : values.z;
        result.Radiance = values.x / result.Density;
    }
    return result;
}

// Thanks to Lin et al., Generalized Resampled Importance Sampling:
// Foundations of ReSTIR (SIGGRAPH 2022), for the partial-bijection support
// contract. Here one source proposal is pushed forward explicitly and
// combined with fresh target support by ordinary balance MIS.
float4 ReplayPairSeed(float2 firstWorld, float2 secondWorld, float2 receiverWorld)
{
    float2 middle = WarpSlope(firstWorld - secondWorld);
    return float4(WarpSlope(firstWorld - receiverWorld) + middle,
                  WarpSlope(secondWorld - ROUGH_PAIR_LIGHT_WORLD) - middle);
}

bool MapPair(float2 firstWorld,
             float2 secondWorld,
             float2 sourceWorld,
             float2 targetWorld,
             out float2 outFirstWorld,
             out float2 outSecondWorld)
{
    float4 seed = ReplayPairSeed(firstWorld, secondWorld, sourceWorld);
    return SolvePair(seed.xy, seed.zw, targetWorld, ROUGH_PAIR_LIGHT_WORLD, outFirstWorld, outSecondWorld);
}

PairResult ShiftPairSample(PairResult source, float2 sourceWorld, float2 targetWorld)
{
    PairResult result = source;
    if (source.HasFailed > 0 || source.Density == 0)
    {
        return result;
    }

    float2 firstWorld;
    float2 secondWorld;
    if (MapPair(source.FirstPositionWorld,
                source.SecondPositionWorld,
                sourceWorld,
                targetWorld,
                firstWorld,
                secondWorld) == false)
    {
        result.HasFailed = 1;
        return result;
    }
    if (IsOnPatch(firstWorld) == false || IsOnPatch(secondWorld) == false)
    {
        result.Density = 0;
        result.Radiance = 0;
        return result;
    }
    float sourceJacobian = ComputePairJacobian(source.FirstPositionWorld - sourceWorld,
                                               source.SecondPositionWorld - ROUGH_PAIR_LIGHT_WORLD,
                                               source.FirstPositionWorld - source.SecondPositionWorld);
    float targetJacobian =
        ComputePairJacobian(firstWorld - targetWorld, secondWorld - ROUGH_PAIR_LIGHT_WORLD, firstWorld - secondWorld);
    result.ShiftJacobian = sourceJacobian / targetJacobian;
    result.Density /= result.ShiftJacobian;
    result.FirstPositionWorld = firstWorld;
    result.SecondPositionWorld = secondWorld;
    result.Radiance = EvaluatePair(firstWorld, secondWorld, targetWorld).x / result.Density;
    return result;
}

float ComputeFreshMisWeight(PairResult fresh, float2 targetWorld, float2 sourceWorld, out bool outHasFailed)
{
    outHasFailed = false;
    if (fresh.HasFailed > 0 || fresh.Density == 0)
    {
        return 1;
    }
    float2 firstWorld;
    float2 secondWorld;
    outHasFailed = MapPair(fresh.FirstPositionWorld,
                           fresh.SecondPositionWorld,
                           targetWorld,
                           sourceWorld,
                           firstWorld,
                           secondWorld) == false;
    if (outHasFailed == true)
    {
        return 0;
    }
    return IsOnPatch(firstWorld) && IsOnPatch(secondWorld) ? 0.5 : 1;
}

float2 EvaluateSeparationResidual(float4 seed, float2 warpedSeparation, float2 receiverWorld)
{
    float2 firstWorld = receiverWorld + UnwarpSlope(seed.xy - warpedSeparation);
    float2 secondWorld = ROUGH_PAIR_LIGHT_WORLD + UnwarpSlope(seed.zw + warpedSeparation);
    return UnwarpSlope(warpedSeparation) - (firstWorld - secondWorld);
}

float ComputeCachedFreshWeight(PairResult fresh, float2 targetWorld, float2 sourceWorld, out bool outHasFailed)
{
    outHasFailed = false;
    if (fresh.HasFailed > 0 || fresh.Density == 0)
    {
        return 1;
    }
    float4 seed = ReplayPairSeed(fresh.FirstPositionWorld, fresh.SecondPositionWorld, targetWorld);
    float2 extent = ROUGH_PAIR_HALF_EXTENT;
    // In the source chart, the first vertex decreases as separation grows and
    // the second increases. The four patch edges bound warped separation.
    float2 lower =
        max(seed.xy - WarpSlope(extent - sourceWorld), WarpSlope(-extent - ROUGH_PAIR_LIGHT_WORLD) - seed.zw);
    float2 upper =
        min(seed.xy - WarpSlope(-extent - sourceWorld), WarpSlope(extent - ROUGH_PAIR_LIGHT_WORLD) - seed.zw);
    float2 lowerResidual = EvaluateSeparationResidual(seed, lower, sourceWorld);
    float2 upperResidual = EvaluateSeparationResidual(seed, upper, sourceWorld);
    // Use the ordinary inverse near a boundary, where FP32 roundoff can
    // otherwise disagree with the sampled position's patch membership.
    if (any(abs(upper - lower) <= ROUGH_PAIR_SUPPORT_BOUNDARY_GUARD) ||
        any(abs(lowerResidual) <= ROUGH_PAIR_SUPPORT_BOUNDARY_GUARD) ||
        any(abs(upperResidual) <= ROUGH_PAIR_SUPPORT_BOUNDARY_GUARD))
    {
        return ComputeFreshMisWeight(fresh, targetWorld, sourceWorld, outHasFailed);
    }
    return all(lower <= upper) && all(lowerResidual <= 0) && all(upperResidual >= 0) ? 0.5 : 1;
}

CachedPairSample CombineCachedRoughPair(PairResult fresh, PairResult source, float2 receiverWorld, float2 sourceWorld)
{
    CachedPairSample result = {};
    result.Fresh = fresh;
    result.Shifted = ShiftPairSample(source, sourceWorld, receiverWorld);
    bool hasFailed;
    result.FreshWeight = ComputeCachedFreshWeight(fresh, receiverWorld, sourceWorld, hasFailed);
    result.HasFailed = max(fresh.HasFailed, max(result.Shifted.HasFailed, hasFailed ? 1 : 0));
    result.Radiance = fresh.Radiance * result.FreshWeight + result.Shifted.Radiance * 0.5;
    return result;
}
