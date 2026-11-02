#include "reflection_map.slang"

// Thanks to Tizian Zeltner, Iliyan Georgiev and Wenzel Jakob, "Specular Manifold Sampling for Rendering
// High-Frequency Caustics and Glints", SIGGRAPH 2020, sections 4.1-4.5,
// https://doi.org/10.1145/3386569.3392408. Use offset-normal solves, smoothed LEAN initialization and
// repeated independent re-hit trials. This port uses slope constraints and analytic derivatives;
// it estimates ITS OWN Newton basin mass, rather than borrowing probabilities from the original solver.

bool SolveReflection(float3 receiverWorld,
                     float3 lightWorld,
                     float2 target,
                     float2 initialUv,
                     bool isSmoothed,
                     out float2 outCoordinatesUv)
{
    float2 coordinatesUv = initialUv;
    float beta = 1;
    outCoordinatesUv = float2(0);
    for (uint iteration = 0; iteration < SOLVER_ITERATIONS; ++iteration)
    {
        ReflectionMap map = EvaluateReflectionMap(receiverWorld, lightWorld, coordinatesUv, isSmoothed);
        if (map.IsValid == false)
            return false;
        float2 difference = map.Residual - target;
        if (length(difference) < SOLVER_TOLERANCE)
        {
            outCoordinatesUv = coordinatesUv;
            return true;
        }
        float determinant = ComputeDeterminant(map.DerivativeU, map.DerivativeV);
        if (abs(determinant) < MINIMUM_MAP_DETERMINANT)
            return false;
        float2 step =
            float2(ComputeDeterminant(difference, map.DerivativeV), ComputeDeterminant(map.DerivativeU, difference)) /
            determinant;
        float2 candidateUv = coordinatesUv - beta * step;
        if (any(candidateUv < 0) || any(candidateUv >= 1))
        {
            beta *= 0.5;
            continue;
        }
        coordinatesUv = candidateUv;
        beta = min(1, 2 * beta);
    }
    return false;
}

bool SolveTwoStage(float3 receiverWorld,
                   float3 lightWorld,
                   float2 target,
                   float2 initialUv,
                   float2 normalDraw,
                   out float2 outCoordinatesUv)
{
    float2 mean = SceneData[SMOOTH_SLOPE_RECORD].xy;
    float3 cholesky = SceneData[SMOOTH_COVARIANCE_RECORD].xyz;
    float2 smoothTarget = -0.5 * (mean - target) -
                          float2(cholesky.x * normalDraw.x, cholesky.y * normalDraw.x + cholesky.z * normalDraw.y);
    float2 smoothUv;
    if (SolveReflection(receiverWorld, lightWorld, smoothTarget, initialUv, true, smoothUv) == false)
    {
        outCoordinatesUv = float2(0);
        return false;
    }
    return SolveReflection(receiverWorld, lightWorld, target, smoothUv, false, outCoordinatesUv);
}

FIGURE17_PARITY_ENTRY
void ValidateFigure17Solver(uint3 threadIndex : SV_DispatchThreadID)
{
    uint count;
    uint stride;
    Inputs.GetDimensions(count, stride);
    uint index = threadIndex.x;
    if (index >= count / INPUT_RECORD_COUNT)
        return;
    uint record = index * INPUT_RECORD_COUNT;
    float4 first = Inputs[record];
    float3 receiverWorld = Inputs[record + 1].xyz;
    float3 lightWorld = Inputs[record + LIGHT_INPUT_RECORD].xyz;
    float4 initial = Inputs[record + RAY_ORIGIN_INPUT_RECORD];
    float2 coordinatesUv;
    bool hasRoot = SolveTwoStage(receiverWorld, lightWorld, first.xy, initial.xy, initial.zw, coordinatesUv);
    Results[index * OUTPUT_RECORD_COUNT] = float4(coordinatesUv, hasRoot == true ? 1 : 0, 0);
    Results[index * OUTPUT_RECORD_COUNT + 1] = float4(0);
    Results[index * OUTPUT_RECORD_COUNT + 2] = float4(0);
}
