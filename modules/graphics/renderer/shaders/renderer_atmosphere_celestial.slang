// Procedural stars in the supplied celestial frame. A cell owns its star and leaves a margin around
// it so changing cube faces cannot split a star across two different seeds.
float3 EvaluateCelestialStars(in float3 directionCelestial, float angularFootprint, float peakRadiance)
{
    const uint FACE_CELLS = 96;
    const float OCCUPANCY = 0.26;
    const float STAR_CELL_RADIUS = 0.042;
    const float STAR_CELL_MARGIN = 0.24;
    const float MAXIMUM_FILTER_RADIUS = 0.19;
    float3 magnitude = abs(directionCelestial);
    float2 facePosition;
    uint face;
    if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z)
    {
        facePosition = directionCelestial.yz / magnitude.x;
        face = directionCelestial.x >= 0 ? 0 : 1;
    }
    else if (magnitude.y >= magnitude.z)
    {
        facePosition = directionCelestial.xz / magnitude.y;
        face = directionCelestial.y >= 0 ? 2 : 3;
    }
    else
    {
        facePosition = directionCelestial.xy / magnitude.z;
        face = directionCelestial.z >= 0 ? 4 : 5;
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

float SampleGalacticNoise(float longitudeTurns, float latitudeSine, uint longitudeCells, float latitudeCells)
{
    float2 coordinate = float2(frac(longitudeTurns) * longitudeCells, (latitudeSine + 1) * latitudeCells);
    uint2 cell = uint2(floor(coordinate));
    uint west = cell.x % longitudeCells;
    uint east = (west + 1) % longitudeCells;
    uint2 key = uint2(longitudeCells, 0);
    float lowerWest = RandomFloat01(Philox4x32(uint4(west, cell.y, 0, 0), key).x);
    float lowerEast = RandomFloat01(Philox4x32(uint4(east, cell.y, 0, 0), key).x);
    float upperWest = RandomFloat01(Philox4x32(uint4(west, cell.y + 1, 0, 0), key).x);
    float upperEast = RandomFloat01(Philox4x32(uint4(east, cell.y + 1, 0, 0), key).x);
    float2 blend = smoothstep(float2(0), float2(1), frac(coordinate));
    return lerp(lerp(lowerWest, lowerEast, blend.x), lerp(upperWest, upperEast, blend.x), blend.y);
}

// Thanks to NASA Night Sky Network, "August’s Night Sky Notes: The Great Rift".
// https://science.nasa.gov/solar-system/skywatching/night-sky-network/augusts-night-sky-notes-the-great-rift/
// Approximate the visible band, central bulge and foreground dust with smooth envelopes and periodic noise.
float3 EvaluateMilkyWay(in float3 directionGalactic, float radiance)
{
    const float OUTER_BAND_LATITUDE_SINE = 0.38;
    float latitudeSine = directionGalactic.z;
    if (abs(latitudeSine) >= OUTER_BAND_LATITUDE_SINE)
    {
        return 0;
    }
    const float BULGE_LONGITUDE_POWER = 10;
    const float DISK_LATITUDE_SINE = 0.16;
    const float DUST_LATITUDE_SINE = 0.065;
    const float DUST_DRIFT_SINE = 0.12;
    const uint CLOUD_LONGITUDE_CELLS = 37;
    const float CLOUD_LATITUDE_CELLS = 13;
    const uint DUST_LONGITUDE_CELLS = 127;
    const float DUST_LATITUDE_CELLS = 47;
    const float3 DISK_TINT = float3(0.67, 0.74, 0.86);
    const float3 BULGE_TINT = float3(1, 0.88, 0.73);
    float longitudeTurns = atan2(directionGalactic.y, directionGalactic.x) / (2 * PI);
    float clouds = SampleGalacticNoise(longitudeTurns, latitudeSine, CLOUD_LONGITUDE_CELLS, CLOUD_LATITUDE_CELLS);
    float dust = SampleGalacticNoise(longitudeTurns, latitudeSine, DUST_LONGITUDE_CELLS, DUST_LATITUDE_CELLS);
    float bulge = pow(saturate(0.5 + 0.5 * directionGalactic.x), BULGE_LONGITUDE_POWER);
    float width = lerp(DISK_LATITUDE_SINE, OUTER_BAND_LATITUDE_SINE, bulge);
    float band = 1 - smoothstep(0, width, abs(latitudeSine));
    float dustLatitude = abs(latitudeSine - (clouds - 0.5) * DUST_DRIFT_SINE);
    float rift = (1 - smoothstep(0, DUST_LATITUDE_SINE, dustLatitude)) * (0.5 + 0.5 * dust);
    return lerp(DISK_TINT, BULGE_TINT, bulge) * (radiance * band * (0.5 + bulge) * (0.5 + clouds) * (1 - rift));
}

float ComputePlanetPointWeight(in float3 directionWorld, in float3 planetWorld, float angularFootprint)
{
    const float MINIMUM_CHORD_RADIUS = 0.00085;
    const float FILTER_WIDTH_IN_PIXELS = 1.25;
    float radius = max(MINIMUM_CHORD_RADIUS, FILTER_WIDTH_IN_PIXELS * angularFootprint);
    float3 offsetWorld = directionWorld - planetWorld;
    float coverage = saturate(1 - dot(offsetWorld, offsetWorld) / (radius * radius));
    // Integrate the squared radial kernel in solid angle using chord radius.
    const float KERNEL_NORMALIZATION = 3 / PI;
    return KERNEL_NORMALIZATION * coverage * coverage / (radius * radius);
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
float3 EvaluateSolarDisk(float chord, float chordRadius, float angularFootprint, out float outCoverage)
{
    const float3 LIMB_EXPONENT = float3(0.397, 0.503, 0.652);
    float chordWidth = max(fwidth(chord), DIRECTION_EPSILON);
    float coverage = ComputeCelestialDiskCoverage(chord, chordRadius, chordWidth);
    outCoverage = coverage;
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
