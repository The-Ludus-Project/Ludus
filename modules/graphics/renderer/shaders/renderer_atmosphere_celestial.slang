// World-fixed procedural stars. A cell owns its star and leaves a margin around
// it so changing cube faces cannot split a star across two different seeds.
float3 EvaluateCelestialStars(in float3 directionWorld, float angularFootprint, float peakRadiance)
{
    const uint FACE_CELLS = 96;
    const float OCCUPANCY = 0.26;
    const float STAR_CELL_RADIUS = 0.042;
    const float STAR_CELL_MARGIN = 0.24;
    const float MAXIMUM_FILTER_RADIUS = 0.19;
    float3 magnitude = abs(directionWorld);
    float2 facePosition;
    uint face;
    if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z)
    {
        facePosition = directionWorld.yz / magnitude.x;
        face = directionWorld.x >= 0 ? 0 : 1;
    }
    else if (magnitude.y >= magnitude.z)
    {
        facePosition = directionWorld.xz / magnitude.y;
        face = directionWorld.y >= 0 ? 2 : 3;
    }
    else
    {
        facePosition = directionWorld.xy / magnitude.z;
        face = directionWorld.z >= 0 ? 4 : 5;
    }
    float2 coordinate = saturate(facePosition * 0.5 + 0.5) * FACE_CELLS;
    uint2 cell = min(uint2(coordinate), FACE_CELLS - 1);
    uint4 samples = Philox4x32(uint4(cell, face, 0), uint2(0, 0));
    float brightness = RandomFloat01(samples.w);
    if (brightness >= OCCUPANCY)
    {
        return 0;
    }
    float2 starPosition =
        STAR_CELL_MARGIN + (1 - 2 * STAR_CELL_MARGIN) * float2(RandomFloat01(samples.x), RandomFloat01(samples.y));
    float filterRadius = clamp(angularFootprint * (0.5 * FACE_CELLS), STAR_CELL_RADIUS, MAXIMUM_FILTER_RADIUS);
    float distanceToStar = length(coordinate - float2(cell) - starPosition);
    float coverage = 1 - smoothstep(0, filterRadius, distanceToStar);
    float areaRatio = STAR_CELL_RADIUS / filterRadius;
    return float3(1) * (peakRadiance * (1 - brightness / OCCUPANCY) * coverage * areaRatio * areaRatio);
}

float ComputeCelestialDiskCoverage(float chord, float chordRadius)
{
    return saturate(0.5 + (chordRadius - chord) / max(fwidth(chord), DIRECTION_EPSILON));
}

// Thanks to NASA Science, "Moon Phases", for the Sun-lit sphere geometry.
// https://science.nasa.gov/moon/moon-phases/
// The visible surface normal comes from a unit-distance ray/sphere intersection.
// A Lambertian cosine gives a simple reflected-light model for the disk.
float EvaluateLunarPhase(in float3 directionWorld, in float3 moonWorld, in float3 sunWorld, float sineRadius)
{
    float projected = dot(directionWorld, moonWorld);
    float3 transverseWorld = directionWorld * projected - moonWorld;
    float root = sqrt(max(0, sineRadius * sineRadius - dot(transverseWorld, transverseWorld)));
    float3 normalWorld = (transverseWorld - root * directionWorld) / sineRadius;
    return max(dot(normalWorld, sunWorld), 0);
}
