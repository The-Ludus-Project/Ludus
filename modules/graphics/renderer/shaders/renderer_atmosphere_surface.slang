// Included only by the renderer-owned atmospheric deferred resolve. The three
// sampled slots hold the atmosphere tables after G-buffer material evaluation.

struct AtmosphereLocalFrame
{
    float3 UpWorld;
    float Height;
};

AtmosphereLocalFrame GetAtmosphereLocalFrame(in float3 positionWorld)
{
    float3 offsetWorld = (positionWorld - deferredView.AtmosphereCameraWorld.xyz) * ATMOSPHERE_KILOMETRES_PER_METRE;
    float height = deferredView.AtmosphereRadialUpWorld.w;
    float radius = GROUND_RADIUS + height;
    float3 radiusWorld = deferredView.AtmosphereRadialUpWorld.xyz * radius + offsetWorld;
    float localRadius = length(radiusWorld);
    AtmosphereLocalFrame frame;
    frame.UpWorld = radiusWorld / max(localRadius, DIRECTION_EPSILON);
    frame.Height = height + dot(offsetWorld, 2 * radius * deferredView.AtmosphereRadialUpWorld.xyz + offsetWorld) /
                                (localRadius + radius);
    return frame;
}

float3 EvaluateAtmosphereSun(in float3 positionWorld)
{
    AtmosphereLocalFrame frame = GetAtmosphereLocalFrame(positionWorld);
    float3 sunWorld = deferredView.AtmosphereSunWorld.xyz;
    float sunCosine = clamp(dot(frame.UpWorld, sunWorld), -1, 1);
    if (frame.Height < 0 || sunCosine < ComputeHorizonCosine(frame.Height))
    {
        return 0;
    }
    if (frame.Height > SHELL_HEIGHT)
    {
        float radius = GROUND_RADIUS + frame.Height;
        float impactSquared = radius * radius * saturate(1 - sunCosine * sunCosine);
        if (sunCosine >= 0 || impactSquared >= TOP_RADIUS * TOP_RADIUS)
        {
            return deferredView.AtmosphereIlluminance.rgb;
        }
        sunCosine = -sqrt(saturate(1 - impactSquared / (TOP_RADIUS * TOP_RADIUS)));
        frame.Height = SHELL_HEIGHT;
    }
    return deferredView.AtmosphereIlluminance.rgb *
           SampleTransmittance(atmosphereTransmittanceTexture, frame.Height, sunCosine);
}

float3 SampleAtmosphereEnvironment(in float3 directionWorld)
{
    return deferredView.AtmosphereIlluminance.rgb * SampleAtmosphereSky(atmosphereSkyTexture,
                                                                        directionWorld,
                                                                        deferredView.AtmosphereRadialUpWorld.xyz,
                                                                        deferredView.AtmosphereSunWorld.xyz,
                                                                        deferredView.AtmosphereRadialUpWorld.w);
}

float3 OrientAtmosphereSample(in float3 axisWorld, float cosine, float azimuth)
{
    const float BASIS_THRESHOLD = 0.99;
    float3 referenceWorld = abs(axisWorld.y) < BASIS_THRESHOLD ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangentWorld = normalize(cross(referenceWorld, axisWorld));
    float3 bitangentWorld = cross(axisWorld, tangentWorld);
    float sine = sqrt(saturate(1 - cosine * cosine));
    return cosine * axisWorld + sine * (cos(azimuth) * tangentWorld + sin(azimuth) * bitangentWorld);
}

void EvaluateAtmosphereEnvironment(in float3 normalWorld,
                                   in float3 viewDirectionWorld,
                                   float roughness,
                                   in float3 f0,
                                   float cosineView,
                                   out float3 outIrradiance,
                                   out float3 outSpecular)
{
    // Cosine quadrature for diffuse. The split-sum GGX filter samples half
    // vectors about the reflection axis and normalizes by projected weight.
    const uint ENVIRONMENT_SAMPLES = 12;
    const float GOLDEN_ANGLE = 2.39996323;
    float3 reflectionWorld = reflect(-viewDirectionWorld, normalWorld);
    float3 irradiance = 0;
    float3 radiance = 0;
    float weightSum = 0;
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    for (uint sampleIndex = 0; sampleIndex < ENVIRONMENT_SAMPLES; ++sampleIndex)
    {
        float quantile = (float(sampleIndex) + 0.5) / ENVIRONMENT_SAMPLES;
        float azimuth = float(sampleIndex) * GOLDEN_ANGLE;
        float3 incidentWorld = OrientAtmosphereSample(normalWorld, sqrt(1 - quantile), azimuth);
        irradiance += SampleAtmosphereEnvironment(incidentWorld);
        float halfCosine = sqrt((1 - quantile) / (1 + (alphaSquared - 1) * quantile));
        float3 halfWorld = OrientAtmosphereSample(reflectionWorld, halfCosine, azimuth);
        float3 specularWorld = reflect(-reflectionWorld, halfWorld);
        float weight = max(dot(reflectionWorld, specularWorld), 0);
        radiance += weight * SampleAtmosphereEnvironment(specularWorld);
        weightSum += weight;
    }
    float2 dfg = EvaluateEnvironmentDfg(roughness, cosineView);
    outIrradiance = irradiance * (PI / ENVIRONMENT_SAMPLES);
    outSpecular = radiance * (f0 * dfg.x + dfg.y) / max(weightSum, DIRECTION_EPSILON);
}

float3 ComposeAtmosphereSegment(in float3 surfaceRadiance, in float3 positionWorld)
{
    const uint AERIAL_STEPS = 16;
    float3 originWorld = deferredView.AtmosphereCameraWorld.xyz;
    float3 offsetWorld = positionWorld - originWorld;
    float distanceMetres = length(offsetWorld);
    float3 rayWorld = offsetWorld / max(distanceMetres, DIRECTION_EPSILON);
    if (deferredView.AtmosphereCameraWorld.w > 0)
    {
        rayWorld = -deferredView.EyeWorld.xyz;
        distanceMetres = max(dot(offsetWorld, rayWorld), 0);
        originWorld = positionWorld - distanceMetres * rayWorld;
    }
    AtmosphereLocalFrame frame = GetAtmosphereLocalFrame(originWorld);
    if (distanceMetres <= 0 || frame.Height < 0 || frame.Height > SHELL_HEIGHT)
    {
        return surfaceRadiance;
    }
    float zenithCosine = clamp(dot(rayWorld, frame.UpWorld), -1, 1);
    float3 sunWorld = deferredView.AtmosphereSunWorld.xyz;
    float solarZenithCosine = clamp(dot(sunWorld, frame.UpWorld), -1, 1);
    float scatteringCosine = clamp(dot(rayWorld, sunWorld), -1, 1);
    bool hitsGround = zenithCosine < ComputeHorizonCosine(frame.Height);
    float exitDistance =
        hitsGround ? GetDistanceToGround(frame.Height, zenithCosine) : GetDistanceToTop(frame.Height, zenithCosine);
    float distance = min(distanceMetres * ATMOSPHERE_KILOMETRES_PER_METRE, exitDistance);
    AtmosphereSegment segment = IntegrateAtmosphere(atmosphereTransmittanceTexture,
                                                    atmosphereMultipleTexture,
                                                    frame.Height,
                                                    zenithCosine,
                                                    solarZenithCosine,
                                                    scatteringCosine,
                                                    distance,
                                                    AERIAL_STEPS,
                                                    true);
    return surfaceRadiance * segment.Transmittance + deferredView.AtmosphereIlluminance.rgb * segment.Radiance;
}
