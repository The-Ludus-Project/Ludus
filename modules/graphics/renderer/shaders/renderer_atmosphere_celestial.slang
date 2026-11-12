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

float ComputeCelestialDiskCoverage(float chord, float chordRadius, float chordWidth)
{
    return saturate(0.5 + (chordRadius - chord) / chordWidth);
}

// Thanks to Sébastien Hillaire, "Physically Based Sky, Atmosphere and Cloud
// Rendering in Frostbite", SIGGRAPH 2016 course notes, §4.1.3 and Appendix B.
// https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016-pbs-frostbite-sky-clouds-new.pdf
// The RGB exponents approximate three wavelengths from Daniel Hestroffer and
// Christian Magnan, "Wavelength dependency of the Solar limb darkening",
// A&A 333 (1998), §2 and Table 2, https://ui.adsabs.harvard.edu/abs/1998A%26A...333..338H
float3 EvaluateSolarDisk(float chord, float chordRadius, float angularFootprint)
{
    const float3 LIMB_EXPONENT = float3(0.397, 0.503, 0.652);
    float chordWidth = max(fwidth(chord), DIRECTION_EPSILON);
    float coverage = ComputeCelestialDiskCoverage(chord, chordRadius, chordWidth);
    // Use the disk-average radiance when the pixel cannot resolve the profile.
    float profileWeight = saturate(2 - 2 * angularFootprint / chordRadius);
    if (coverage == 0 || profileWeight == 0)
    {
        return float3(coverage);
    }

    // Sample the midpoint of the covered part of a partially covered pixel.
    float sampleChord = max(0, min(chord, chordRadius - 0.5 * coverage * chordWidth));
    float halfChord = 0.5 * sampleChord;
    float halfRadius = 0.5 * chordRadius;
    float radiusRatio = sampleChord / chordRadius;
    // mu^2 = 1 - (sin(view angle) / sin(angular radius))^2. Work in
    // chord distances to keep precision for the small apparent Earth Sun.
    float muSquared =
        saturate(1 - radiusRatio * radiusRatio * (1 - halfChord * halfChord) / (1 - halfRadius * halfRadius));
    // Projected disk area has measure 2*mu dmu. The mean of mu^a is 2/(2+a).
    float3 exponent = 0.5 * LIMB_EXPONENT;
    float3 profile = (1 + exponent) * pow(muSquared, exponent);
    return coverage * lerp(float3(1), profile, profileWeight);
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
