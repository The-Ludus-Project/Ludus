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

static const float AURORA_BASE_KILOMETRES = SHELL_HEIGHT;
static const float AURORA_TOP_KILOMETRES = 300;
static const float AURORA_HALF_LENGTH_KILOMETRES = 950;
static const float AURORA_END_FADE_KILOMETRES = 220;
static const float AURORA_WIDE_FOLD_KILOMETRES = 53;
static const float AURORA_NARROW_FOLD_KILOMETRES = 19;
static const float AURORA_WIDE_FOLD_SCALE_KILOMETRES = 147;
static const float AURORA_NARROW_FOLD_SCALE_KILOMETRES = 46;
static const uint AURORA_RIBBON_COUNT = 3;
static const float AURORA_FIRST_RIBBON_KILOMETRES = 320;
static const float AURORA_RIBBON_SEPARATION_KILOMETRES = 330;
static const float AURORA_FIRST_HALF_WIDTH_KILOMETRES = 13;
static const float AURORA_WIDTH_INCREASE_KILOMETRES = 3;
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
    return AURORA_WIDE_FOLD_KILOMETRES * sin(x / AURORA_WIDE_FOLD_SCALE_KILOMETRES + phase) +
           AURORA_NARROW_FOLD_KILOMETRES * sin(x / AURORA_NARROW_FOLD_SCALE_KILOMETRES - 2 * phase);
}

float3 EvaluateAuroraEmission(in float2 pointAurora, float altitude, float offset, float halfWidth, float phase)
{
    const float GREEN_FADE_START_KILOMETRES = 120;
    const float GREEN_FADE_END_KILOMETRES = 235;
    const float LOWER_EDGE_FADE_KILOMETRES = 11;
    const float RED_START_KILOMETRES = 165;
    const float RED_PEAK_KILOMETRES = 234;
    const float FIRST_RAY_SCALE_KILOMETRES = 5.7;
    const float SECOND_RAY_SCALE_KILOMETRES = 2.3;
    const float RAY_TILT_KILOMETRES_PER_KILOMETRE = 0.059;
    const float RAY_BASE_BRIGHTNESS = 0.16;
    const float RED_RELATIVE_EMISSION = 0.27;
    const float3 GREEN_EMISSION_RGB = float3(0.14, 1, 0.35);
    const float3 RED_EMISSION_RGB = float3(1, 0.055, 0.23);
    float crossRibbon = saturate(1 - abs(offset) / halfWidth);
    float feather = smoothstep(0, 1, crossRibbon);
    float alongRibbon = 1 - smoothstep(AURORA_HALF_LENGTH_KILOMETRES - AURORA_END_FADE_KILOMETRES,
                                       AURORA_HALF_LENGTH_KILOMETRES,
                                       abs(pointAurora.x));
    float ray = pointAurora.x + RAY_TILT_KILOMETRES_PER_KILOMETRE * (altitude - AURORA_BASE_KILOMETRES);
    float shafts = (0.5 + 0.5 * sin(ray / FIRST_RAY_SCALE_KILOMETRES - phase)) *
                   (0.5 + 0.5 * sin(ray / SECOND_RAY_SCALE_KILOMETRES + 2 * phase));
    float green = smoothstep(AURORA_BASE_KILOMETRES, AURORA_BASE_KILOMETRES + LOWER_EDGE_FADE_KILOMETRES, altitude) *
                  (1 - smoothstep(GREEN_FADE_START_KILOMETRES, GREEN_FADE_END_KILOMETRES, altitude));
    float red = smoothstep(RED_START_KILOMETRES, RED_PEAK_KILOMETRES, altitude) *
                (1 - smoothstep(RED_PEAK_KILOMETRES, AURORA_TOP_KILOMETRES, altitude));
    return (GREEN_EMISSION_RGB * green + RED_EMISSION_RGB * (RED_RELATIVE_EMISSION * red)) *
           (feather * alongRibbon * lerp(RAY_BASE_BRIGHTNESS, 1, shafts));
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

    const float MAXIMUM_RIBBON_SLOPE = AURORA_WIDE_FOLD_KILOMETRES / AURORA_WIDE_FOLD_SCALE_KILOMETRES +
                                       AURORA_NARROW_FOLD_KILOMETRES / AURORA_NARROW_FOLD_SCALE_KILOMETRES;
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
        float extent = AURORA_WIDE_FOLD_KILOMETRES + AURORA_NARROW_FOLD_KILOMETRES + halfWidth;
        float2 ribbonRange = range;
        if (ClipAuroraAxis(eyeAurora.y, directionAurora.y, center - extent, center + extent, ribbonRange) == false)
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
            emission += EvaluateAuroraEmission(pointAurora, altitude, offset, halfWidth, phase) * (next - distance);
            distance = next;
        }
    }
    return emission * (LUDUS_ATMOSPHERE_METRES_PER_KILOMETRE * aurora.AxisXWorld.w);
}
