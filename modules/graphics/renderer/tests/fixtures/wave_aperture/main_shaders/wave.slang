#pragma once
// Thanks to Shlomi Steinberg and Matt Pharr, "Wave Tracing: Generalizing The Path Integral To Wave Optics",
// Computer Graphics Forum 45(2), e70322, 2026, https://doi.org/10.1111/cgf.70322 : Sections 3.1 and 4.2,
// Eqs. (2), (16), (20), (21). This scalar thin-screen specialization integrates coherent Fresnel fields.
// It is not the paper's multi-region cone tracer. See the canonical render-graph authoring guide.

struct ScalarWaveAperture
{
    float WavelengthWorld;
    float2 SigmaWorldSquared;
    float2 HalfExtentWorld;
    uint Resolution;
};

/// Eq. (20), plus Eq. (21)'s correlation. Output xy are position variances; zw are covariances.
float4 ComputeWaveMoments(float2 sigmaWorldSquared, float correlationWorldSquared)
{
    return float4(0.5 * (sigmaWorldSquared + correlationWorldSquared * correlationWorldSquared / sigmaWorldSquared),
                  0.5 * correlationWorldSquared / sigmaWorldSquared);
}

float2 GetWaveCellPosition(ScalarWaveAperture aperture, uint2 cell)
{
    return ((float2(cell) + 0.5) * (2.0 / float(aperture.Resolution)) - 1) * aperture.HalfExtentWorld;
}

/// Both phase gradients stay below pi/2 per cell. Discontinuous masks require a separate refinement study.
bool CanResolveWavePhase(ScalarWaveAperture aperture, float3 receiverApertureWorld)
{
    float2 stepWorld = 2 * aperture.HalfExtentWorld / float(aperture.Resolution);
    float phaseScale = PI / (aperture.WavelengthWorld * receiverApertureWorld.z);
    float2 phaseStep = phaseScale * stepWorld * 2 * (aperture.HalfExtentWorld + abs(receiverApertureWorld.xy));
    return receiverApertureWorld.z > 0 && isfinite(phaseScale) == true && all(phaseStep <= PI * 0.5) == true;
}

/// Fresnel field contribution of one midpoint cell, with the common exp(ikz) omitted and 1/i retained.
float2 EvaluateWaveCell(ScalarWaveAperture aperture, float2 positionApertureWorld, float3 receiverApertureWorld)
{
    float2 stepWorld = 2 * aperture.HalfExtentWorld / float(aperture.Resolution);
    float weight = stepWorld.x * stepWorld.y / (aperture.WavelengthWorld * receiverApertureWorld.z);
    float amplitude =
        weight * exp(-0.5 * dot(positionApertureWorld * positionApertureWorld, 1 / aperture.SigmaWorldSquared));
    float2 offsetWorld = positionApertureWorld - receiverApertureWorld.xy;
    float phase = PI * dot(offsetWorld, offsetWorld) / (aperture.WavelengthWorld * receiverApertureWorld.z);
    return amplitude * float2(sin(phase), -cos(phase));
}

/// Inclusive edges; mask union removes shared-edge and overlapping-triangle double counting.
bool IsWavePointCovered(float2 positionApertureWorld,
                        float2 firstApertureWorld,
                        float2 secondApertureWorld,
                        float2 thirdApertureWorld)
{
    float2 firstEdgeWorld = secondApertureWorld - firstApertureWorld;
    float2 secondEdgeWorld = thirdApertureWorld - firstApertureWorld;
    float2 offsetWorld = positionApertureWorld - firstApertureWorld;
    float determinant = firstEdgeWorld.x * secondEdgeWorld.y - firstEdgeWorld.y * secondEdgeWorld.x;
    if (determinant == 0)
        return false;
    float u = (offsetWorld.x * secondEdgeWorld.y - offsetWorld.y * secondEdgeWorld.x) / determinant;
    float v = (firstEdgeWorld.x * offsetWorld.y - firstEdgeWorld.y * offsetWorld.x) / determinant;
    return u >= 0 && v >= 0 && u + v <= 1;
}
