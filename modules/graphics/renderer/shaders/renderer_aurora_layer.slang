#pragma once

#include "renderer_atmosphere_transport.slang"

// Thanks to Orion Sky Lawlor and Jon Genetti, Interactive Volume Rendering Aurora on the GPU,
// WSCG 2011, §§2–3. https://lawlor.cs.uaf.edu/~olawlor/papers/2010/aurora/lawlor_aurora_2010.pdf
// Use their separation of curtain footprint and vertical deposition, with analytic ribbons
// and a bound on their slope in place of a simulated flux texture and distance field.
struct AuroraLayerParameters
{
    float4 AxisXWorld; // xyz: tangent X; w: emission in linear nits per metre.
    float4 AxisZWorld; // xyz: tangent Z; w: periodic animation phase in radians.
};

static const uint AURORA_LAYER_UNIFORM_ROWS = LUDUS_AURORA_LAYER_UNIFORM_ROWS;

AuroraLayerParameters UnpackAuroraLayer(in float4 rows[AURORA_LAYER_UNIFORM_ROWS])
{
    AuroraLayerParameters aurora;
    aurora.AxisXWorld = rows[0];
    aurora.AxisZWorld = rows[1];
    return aurora;
}

static const float AURORA_BASE_KILOMETRES = SHELL_HEIGHT;
static const float AURORA_TOP_KILOMETRES = 300;
static const float AURORA_HALF_LENGTH_KILOMETRES = 950;
static const float AURORA_END_FADE_KILOMETRES = 220;
static const float AURORA_ARC_BEND_KILOMETRES = 145;
static const float AURORA_WIDE_FOLD_KILOMETRES = 96;
static const float AURORA_NARROW_FOLD_KILOMETRES = 34;
static const float AURORA_WIDE_FOLD_SCALE_KILOMETRES = 147;
static const float AURORA_NARROW_FOLD_SCALE_KILOMETRES = 57;
static const float AURORA_RIPPLE_KILOMETRES = 12;
static const float AURORA_RIPPLE_SCALE_KILOMETRES = 21;
static const float AURORA_RIPPLE_PHASE_RATE = 3;
static const uint AURORA_RIBBON_COUNT = 2;
static const float AURORA_FIRST_RIBBON_KILOMETRES = 320;
static const float AURORA_RIBBON_SEPARATION_KILOMETRES = 225;
static const float AURORA_FIRST_HALF_WIDTH_KILOMETRES = 9;
static const float AURORA_WIDTH_INCREASE_KILOMETRES = 4;
static const float AURORA_RIBBON_PHASE_OFFSET = 1.71;

// Positive sphere intersection for an eye at or below the emitting layer. Near the zenith
// the rationalized root preserves precision when the eye and the shell have similar radii.
float GetAuroraShellExit(float height, float zenithCosine, float shellHeight)
{
    float radius = GROUND_RADIUS + height;
    float projection = radius * zenithCosine;
    float squaredDifference = (shellHeight - height) * (radius + GROUND_RADIUS + shellHeight);
    float root = sqrt(max(0, projection * projection + squaredDifference));
    return projection > 0 ? squaredDifference / (root + projection) : root - projection;
}

bool ClipAuroraAxis(float eye, float direction, float minimum, float maximum, inout float2 inoutRange)
{
    if (abs(direction) < DIRECTION_EPSILON)
    {
        return eye >= minimum && eye <= maximum;
    }
    float first = (minimum - eye) / direction;
    float second = (maximum - eye) / direction;
    inoutRange.x = max(inoutRange.x, min(first, second));
    inoutRange.y = min(inoutRange.y, max(first, second));
    return inoutRange.x < inoutRange.y;
}

float GetAuroraRibbonOffset(float x, float phase)
{
    float arc = x / AURORA_HALF_LENGTH_KILOMETRES;
    float broadFold = sin(x / AURORA_WIDE_FOLD_SCALE_KILOMETRES + phase);
    float ripple = (0.5 + 0.5 * broadFold) * sin(x / AURORA_RIPPLE_SCALE_KILOMETRES + AURORA_RIPPLE_PHASE_RATE * phase);
    return AURORA_ARC_BEND_KILOMETRES * arc * arc + AURORA_WIDE_FOLD_KILOMETRES * broadFold +
           AURORA_NARROW_FOLD_KILOMETRES * sin(x / AURORA_NARROW_FOLD_SCALE_KILOMETRES - 2 * phase) +
           AURORA_RIPPLE_KILOMETRES * ripple;
}

float SampleAuroraRayBrightness(float coordinate, uint ribbon)
{
    const uint CELLS_PER_BLOCK = 4;
    uint cell = asuint(int(floor(coordinate)));
    uint lane = cell % CELLS_PER_BLOCK;
    uint nextLane = (lane + 1) % CELLS_PER_BLOCK;
    uint2 key = uint2(ribbon, 0);
    uint4 samples = Philox4x32(uint4(cell / CELLS_PER_BLOCK, 0, 0, 0), key);
    float lower = RandomFloat01(samples[lane]);
    if (nextLane == 0)
    {
        samples = Philox4x32(uint4((cell + 1) / CELLS_PER_BLOCK, 0, 0, 0), key);
    }
    return lerp(lower, RandomFloat01(samples[nextLane]), smoothstep(0, 1, frac(coordinate)));
}

// Thanks to NOAA Space Weather Prediction Center, Aurora Tutorial, "Shapes and
// Morphology of the Aurora" and "Colors of the Aurora."
// https://www.spaceweather.gov/content/aurora-tutorial
// Use smooth surges along a folded arc with clustered vertical rays and
// predominantly green emission. Give the weaker red emission a broader altitude profile.
float3
EvaluateAuroraEmission(in float2 pointAurora, float altitude, float offset, float halfWidth, float phase, uint ribbon)
{
    const float ACTIVITY_SCALE_KILOMETRES = 89;
    const float SURGE_SCALE_KILOMETRES = 37;
    const float ACTIVITY_WEIGHT = 0.52;
    const float RAY_SCALE_KILOMETRES = 4.8;
    const float RAY_DRIFT_KILOMETRES = 23;
    const float RAY_TILT_KILOMETRES_PER_KILOMETRE = 0.065;
    const float RAY_BACKGROUND = 0.18;
    const float SECONDARY_RIBBON_BRIGHTNESS = 0.32;
    const float GREEN_LOWER_EDGE_KILOMETRES = 7;
    const float GREEN_PEAK_KILOMETRES = 108;
    const float GREEN_CORE_TOP_KILOMETRES = 136;
    const float GREEN_TAIL_LOW_TOP_KILOMETRES = 166;
    const float GREEN_TAIL_HIGH_TOP_KILOMETRES = 214;
    const float GREEN_TAIL_SHARE = 0.22;
    const float DEPOSITION_LIFT_KILOMETRES = 5;
    const float RED_START_KILOMETRES = 171;
    const float RED_PEAK_KILOMETRES = 233;
    const float RED_RELATIVE_EMISSION = 0.055;
    const float3 GREEN_EMISSION_RGB = float3(0.26, 1, 0.43);
    const float3 RED_EMISSION_RGB = float3(1, 0.07, 0.19);

    float feather = smoothstep(0, 1, saturate(1 - abs(offset) / halfWidth));
    float alongRibbon = 1 - smoothstep(AURORA_HALF_LENGTH_KILOMETRES - AURORA_END_FADE_KILOMETRES,
                                       AURORA_HALF_LENGTH_KILOMETRES,
                                       abs(pointAurora.x));
    float broad = 0.5 + 0.5 * sin(pointAurora.x / ACTIVITY_SCALE_KILOMETRES + phase);
    float surge = 0.5 + 0.5 * sin(pointAurora.x / SURGE_SCALE_KILOMETRES - 2 * phase);
    float activity = lerp(broad, surge, ACTIVITY_WEIGHT);
    float activePatch = smoothstep(0, 1, activity);
    float ray = pointAurora.x + RAY_TILT_KILOMETRES_PER_KILOMETRE * (altitude - AURORA_BASE_KILOMETRES) +
                RAY_DRIFT_KILOMETRES * sin(phase);
    float shafts = SampleAuroraRayBrightness(ray / RAY_SCALE_KILOMETRES, ribbon);
    float brightness = lerp(RAY_BACKGROUND, 1, activePatch * shafts);
    float depositionAltitude = altitude - DEPOSITION_LIFT_KILOMETRES * (1 - activePatch);
    float lowerEdge =
        smoothstep(AURORA_BASE_KILOMETRES, AURORA_BASE_KILOMETRES + GREEN_LOWER_EDGE_KILOMETRES, depositionAltitude);
    float core = 1 - smoothstep(GREEN_PEAK_KILOMETRES, GREEN_CORE_TOP_KILOMETRES, depositionAltitude);
    float tail = 1 - smoothstep(GREEN_PEAK_KILOMETRES,
                                lerp(GREEN_TAIL_LOW_TOP_KILOMETRES, GREEN_TAIL_HIGH_TOP_KILOMETRES, activePatch),
                                depositionAltitude);
    float green = lowerEdge * lerp(core, tail, GREEN_TAIL_SHARE);
    float red = smoothstep(RED_START_KILOMETRES, RED_PEAK_KILOMETRES, altitude) *
                (1 - smoothstep(RED_PEAK_KILOMETRES, AURORA_TOP_KILOMETRES, altitude)) * activePatch * activePatch;
    float ribbonBrightness = ribbon == 0 ? 1 : SECONDARY_RIBBON_BRIGHTNESS * activePatch;
    return (GREEN_EMISSION_RGB * green + RED_EMISSION_RGB * (RED_RELATIVE_EMISSION * red)) *
           (feather * alongRibbon * brightness * ribbonBrightness);
}

float3 IntegrateAuroraLayer(in AuroraLayerParameters aurora,
                            in float3 upWorld,
                            in float3 directionWorld,
                            float height,
                            float maximumKilometres)
{
    float zenithCosine = clamp(dot(upWorld, directionWorld), -1, 1);
    if (aurora.AxisXWorld.w <= 0 || zenithCosine < ComputeHorizonCosine(height))
    {
        return 0;
    }
    float2 range = float2(GetAuroraShellExit(height, zenithCosine, AURORA_BASE_KILOMETRES),
                          min(GetAuroraShellExit(height, zenithCosine, AURORA_TOP_KILOMETRES), maximumKilometres));
    if (range.y <= range.x)
    {
        return 0;
    }

    float radius = GROUND_RADIUS + height;
    float2 eyeAurora = radius * float2(dot(upWorld, aurora.AxisXWorld.xyz), dot(upWorld, aurora.AxisZWorld.xyz));
    float2 directionAurora =
        float2(dot(directionWorld, aurora.AxisXWorld.xyz), dot(directionWorld, aurora.AxisZWorld.xyz));
    if (ClipAuroraAxis(eyeAurora.x,
                       directionAurora.x,
                       -AURORA_HALF_LENGTH_KILOMETRES,
                       AURORA_HALF_LENGTH_KILOMETRES,
                       range) == false)
    {
        return 0;
    }

    const float MAXIMUM_RIBBON_SLOPE =
        2 * AURORA_ARC_BEND_KILOMETRES / AURORA_HALF_LENGTH_KILOMETRES +
        AURORA_WIDE_FOLD_KILOMETRES / AURORA_WIDE_FOLD_SCALE_KILOMETRES +
        AURORA_NARROW_FOLD_KILOMETRES / AURORA_NARROW_FOLD_SCALE_KILOMETRES +
        AURORA_RIPPLE_KILOMETRES * (1 / AURORA_RIPPLE_SCALE_KILOMETRES + 0.5 / AURORA_WIDE_FOLD_SCALE_KILOMETRES);
    const float MAXIMUM_STEP_KILOMETRES = 4;
    const float MINIMUM_STEP_KILOMETRES = 1;
    const float RAY_STRUCTURE_STEP_KILOMETRES = 2;
    const float CROSS_RIBBON_SAMPLES = 8;
    const float QUADRATURE_STEP_SCALE = 3;
    float maximumOffsetSpeed = abs(directionAurora.y) + MAXIMUM_RIBBON_SLOPE * abs(directionAurora.x);
    float3 emission = 0;
    for (uint ribbon = 0; ribbon < AURORA_RIBBON_COUNT; ++ribbon)
    {
        float center = AURORA_FIRST_RIBBON_KILOMETRES + float(ribbon) * AURORA_RIBBON_SEPARATION_KILOMETRES;
        float halfWidth = AURORA_FIRST_HALF_WIDTH_KILOMETRES + float(ribbon) * AURORA_WIDTH_INCREASE_KILOMETRES;
        float extent =
            AURORA_WIDE_FOLD_KILOMETRES + AURORA_NARROW_FOLD_KILOMETRES + AURORA_RIPPLE_KILOMETRES + halfWidth;
        float2 ribbonRange = range;
        if (ClipAuroraAxis(eyeAurora.y,
                           directionAurora.y,
                           center - extent,
                           center + extent + AURORA_ARC_BEND_KILOMETRES,
                           ribbonRange) == false)
        {
            continue;
        }
        float phase = aurora.AxisZWorld.w + float(ribbon) * AURORA_RIBBON_PHASE_OFFSET;
        float stepKilometres =
            QUADRATURE_STEP_SCALE *
            clamp(min(halfWidth / max(maximumOffsetSpeed * CROSS_RIBBON_SAMPLES, DIRECTION_EPSILON),
                      RAY_STRUCTURE_STEP_KILOMETRES / max(abs(directionAurora.x), DIRECTION_EPSILON)),
                  MINIMUM_STEP_KILOMETRES,
                  MAXIMUM_STEP_KILOMETRES);
        float distance = ribbonRange.x;
        while (distance < ribbonRange.y)
        {
            float2 pointAurora = eyeAurora + directionAurora * distance;
            float offset = pointAurora.y - center - GetAuroraRibbonOffset(pointAurora.x, phase);
            // The offset cannot close faster than maximumOffsetSpeed. Skip empty space
            // only when that conservative distance exceeds the ordinary quadrature step.
            float emptyKilometres = (abs(offset) - halfWidth) / max(maximumOffsetSpeed, DIRECTION_EPSILON);
            if (emptyKilometres > stepKilometres)
            {
                distance += emptyKilometres;
                continue;
            }
            float next = min(distance + stepKilometres, ribbonRange.y);
            float midpoint = (distance + next) * 0.5;
            pointAurora = eyeAurora + directionAurora * midpoint;
            offset = pointAurora.y - center - GetAuroraRibbonOffset(pointAurora.x, phase);
            float altitude = length(radius * upWorld + directionWorld * midpoint) - GROUND_RADIUS;
            if (abs(offset) < halfWidth)
            {
                emission +=
                    EvaluateAuroraEmission(pointAurora, altitude, offset, halfWidth, phase, ribbon) * (next - distance);
            }
            distance = next;
        }
    }
    return emission * (LUDUS_ATMOSPHERE_METRES_PER_KILOMETRE * aurora.AxisXWorld.w);
}
