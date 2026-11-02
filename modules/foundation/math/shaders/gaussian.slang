#pragma once
#include "math.slang"

/// Correlated displacement axes of a positive semidefinite 3D covariance (gaussian.hpp).
struct GaussianAxes
{
    float3 Axis0;
    float3 Axis1;
    float3 Axis2;
};
/// Cholesky factorization matching TryComputeGaussianAxes in gaussian.cpp; no output preservation on failure.
bool TryComputeGaussianAxes(float3 diagonal, float3 cross, out GaussianAxes outAxes)
{
    static const float COVARIANCE_RELATIVE_TOLERANCE = 0.00001;
    outAxes = (GaussianAxes)0;
    if (all(isfinite(diagonal)) == false || all(isfinite(cross)) == false)
        return false;
    float tolerance = max(max(abs(diagonal.x), abs(diagonal.y)), abs(diagonal.z)) * COVARIANCE_RELATIVE_TOLERANCE;
    if (any(diagonal < -tolerance) == true)
        return false;
    outAxes.Axis0.x = sqrt(max(diagonal.x, 0));
    if (outAxes.Axis0.x > 0)
        outAxes.Axis0.yz = cross.xy / outAxes.Axis0.x;
    else if (any(abs(cross.xy) > tolerance) == true)
        return false;
    float varianceY = diagonal.y - outAxes.Axis0.y * outAxes.Axis0.y;
    if (varianceY < -tolerance)
        return false;
    outAxes.Axis1.y = sqrt(max(varianceY, 0));
    float covarianceYz = cross.z - outAxes.Axis0.y * outAxes.Axis0.z;
    if (outAxes.Axis1.y > 0)
        outAxes.Axis1.z = covarianceYz / outAxes.Axis1.y;
    else if (abs(covarianceYz) > tolerance)
        return false;
    float varianceZ = diagonal.z - outAxes.Axis0.z * outAxes.Axis0.z - outAxes.Axis1.z * outAxes.Axis1.z;
    if (varianceZ < -tolerance)
        return false;
    outAxes.Axis2.z = sqrt(max(varianceZ, 0));
    return all(isfinite(outAxes.Axis0)) == true && all(isfinite(outAxes.Axis1)) == true &&
           all(isfinite(outAxes.Axis2)) == true;
}
