// GPU twin of reference_medium.cpp: the RT1 reference kernel's bounded homogeneous absorbing medium. Every
// function repeats the CPU oracle's float32 arithmetic in the same order; see ReferenceMediumDescription.
// Thanks to Julian Fong, Magnus Wrenninge, Christopher Kulla and Ralf Habel, "Production Volume Rendering",
// SIGGRAPH 2017 course, https://doi.org/10.1145/3084873.3084907, sections 2.3.1, 3.2 and 3.4, for closed-form
// homogeneous transmittance accumulated over ordered boundary crossings, and to Sven Woop, Carsten Benthin and
// Ingo Wald, "Watertight Ray/Triangle Intersection", JCGT 2(1), 2013, section 3,
// https://jcgt.org/published/0002/01/05/paper.pdf, for the sheared edge test, here with a top-left-style tie
// rule for exactly zero edge functions.
//
// The including kernel supplies SceneValue, the Parameters Tile word and ComputeBvhEdgeFunction
// (ray_bvh_traversal.slang).

bool HasMedium()
{
    return Tile.z != 0;
}
float3 GetMediumAbsorptionPerWorld()
{
    return SceneValue(Tile.y).xyz;
}
float3 GetMediumCornerWorld(uint triangle, uint corner)
{
    return SceneValue(Tile.y + LUDUS_REFERENCE_MEDIUM_HEADER_WORDS + triangle * LUDUS_REFERENCE_MEDIUM_TRIANGLE_WORDS +
                      corner)
        .xyz;
}
// A zero edge function admits the hit for one direction class of its edge, so exactly one of the two triangles
// sharing the edge, which traverse it in opposite directions, keeps it.
bool IsZeroMediumEdgeAdmitted(float2 from, float2 to, bool isPositiveWinding)
{
    bool isAscending = from.x < to.x || (from.x == to.x && from.y < to.y);
    return isAscending == isPositiveWinding;
}
bool IntersectMediumTriangle(float3 cornersWorld[3],
                             float3 originWorld,
                             float3 directionWorld,
                             out float outDistanceWorld)
{
    outDistanceWorld = 0;
    uint z = 0;
    for (uint axis = 1; axis < 3; ++axis)
    {
        if (abs(directionWorld[axis]) > abs(directionWorld[z]))
            z = axis;
    }
    uint x = (z + 1) % 3;
    uint y = (x + 1) % 3;
    if (directionWorld[z] < 0)
    {
        uint swapped = x;
        x = y;
        y = swapped;
    }
    float shearX = directionWorld[x] / directionWorld[z];
    float shearY = directionWorld[y] / directionWorld[z];
    float shearZ = 1 / directionWorld[z];
    float2 sheared[3];
    float3 depth;
    for (uint corner = 0; corner < 3; ++corner)
    {
        float dz = cornersWorld[corner][z] - originWorld[z];
        sheared[corner] = float2((cornersWorld[corner][x] - originWorld[x]) - shearX * dz,
                                 (cornersWorld[corner][y] - originWorld[y]) - shearY * dz);
        depth[corner] = dz * shearZ;
    }
    // Edge i is opposite corner i and runs from corner i + 1 to corner i + 2 in winding order.
    float3 edge = float3(ComputeBvhEdgeFunction(sheared[2], sheared[1]),
                         ComputeBvhEdgeFunction(sheared[0], sheared[2]),
                         ComputeBvhEdgeFunction(sheared[1], sheared[0]));
    if (any(edge < 0) && any(edge > 0))
        return false;
    float determinant = edge.x + edge.y + edge.z;
    if (determinant == 0)
        return false;
    for (uint i = 0; i < 3; ++i)
    {
        if (edge[i] == 0 &&
            IsZeroMediumEdgeAdmitted(sheared[(i + 1) % 3], sheared[(i + 2) % 3], determinant > 0) == false)
            return false;
    }
    float distanceWorld = (edge.x * depth.x + edge.y * depth.y + edge.z * depth.z) / determinant;
    if (isfinite(distanceWorld) == false || distanceWorld <= 0)
        return false;
    outDistanceWorld = distanceWorld;
    return true;
}
// Attenuate the unit-direction segment [0, lengthWorld] from originWorld. inoutInside is the inside count (0 or 1)
// at the origin; shouldDeriveStart replaces it with the count implied by every crossing along the ray. Returns
// false, keeping both outputs, when a count leaves {0, 1}.
bool AttenuateMediumSegment(float3 originWorld,
                            float3 directionWorld,
                            float lengthWorld,
                            bool shouldDeriveStart,
                            inout uint inoutInside,
                            out float3 outTransmittance)
{
    outTransmittance = 1;
    if (HasMedium() == false)
        return true;
    // Inside length over [0, L] is w(L) L - sum s_i t_i for crossing signs s_i at distances t_i < L.
    int totalSign = 0;
    int segmentSign = 0;
    float signedDistance = 0;
    for (uint triangle = 0; triangle < Tile.z; ++triangle)
    {
        float3 cornersWorld[3];
        for (uint corner = 0; corner < 3; ++corner)
            cornersWorld[corner] = GetMediumCornerWorld(triangle, corner);
        float distanceWorld;
        if (IntersectMediumTriangle(cornersWorld, originWorld, directionWorld, distanceWorld) == false)
            continue;
        float facing = dot(cross(cornersWorld[1] - cornersWorld[0], cornersWorld[2] - cornersWorld[0]), directionWorld);
        if (facing == 0)
            continue;
        // Moving against the outward normal enters the medium.
        int sign = facing < 0 ? 1 : -1;
        totalSign += sign;
        if (distanceWorld < lengthWorld)
        {
            segmentSign += sign;
            signedDistance += float(sign) * distanceWorld;
        }
    }
    int start = shouldDeriveStart == true ? -totalSign : int(inoutInside);
    int end = start + segmentSign;
    if (start < 0 || start > 1 || end < 0 || end > 1)
        return false;
    float insideWorld = max(0.0, float(end) * lengthWorld - signedDistance);
    outTransmittance = exp(-(GetMediumAbsorptionPerWorld() * insideWorld));
    inoutInside = uint(end);
    return true;
}
