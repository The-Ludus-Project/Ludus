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

// Quadrature samples of the environment integrals, successive azimuths a golden angle apart.
static const uint ATMOSPHERE_ENVIRONMENT_SAMPLES = 12;
static const float ATMOSPHERE_ENVIRONMENT_GOLDEN_ANGLE = 2.39996323;
float3 OrientAtmosphereSample(in float3 axisWorld, float cosine, float azimuth)
{
    const float BASIS_THRESHOLD = 0.99;
    float3 referenceWorld = abs(axisWorld.y) < BASIS_THRESHOLD ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangentWorld = normalize(cross(referenceWorld, axisWorld));
    float3 bitangentWorld = cross(axisWorld, tangentWorld);
    float sine = sqrt(saturate(1 - cosine * cosine));
    return cosine * axisWorld + sine * (cos(azimuth) * tangentWorld + sin(azimuth) * bitangentWorld);
}

// Split-sum GGX environment specular: half vectors sampled about the reflection axis, normalized by projected weight.
float3 EvaluateAtmosphereSpecular(in float3 normalWorld,
                                  in float3 viewDirectionWorld,
                                  float roughness,
                                  in float3 f0,
                                  float cosineView)
{
    float3 reflectionWorld = reflect(-viewDirectionWorld, normalWorld);
    float3 radiance = 0;
    float weightSum = 0;
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    for (uint sampleIndex = 0; sampleIndex < ATMOSPHERE_ENVIRONMENT_SAMPLES; ++sampleIndex)
    {
        float quantile = (float(sampleIndex) + 0.5) / ATMOSPHERE_ENVIRONMENT_SAMPLES;
        float azimuth = float(sampleIndex) * ATMOSPHERE_ENVIRONMENT_GOLDEN_ANGLE;
        float halfCosine = sqrt((1 - quantile) / (1 + (alphaSquared - 1) * quantile));
        float3 halfWorld = OrientAtmosphereSample(reflectionWorld, halfCosine, azimuth);
        float3 specularWorld = reflect(-reflectionWorld, halfWorld);
        float weight = max(dot(reflectionWorld, specularWorld), 0);
        radiance += weight * SampleAtmosphereEnvironment(specularWorld);
        weightSum += weight;
    }
    float2 dfg = EvaluateEnvironmentDfg(roughness, cosineView);
    return radiance * (f0 * dfg.x + dfg.y) / max(weightSum, DIRECTION_EPSILON);
}

void EvaluateAtmosphereEnvironment(in float3 normalWorld,
                                   in float3 viewDirectionWorld,
                                   float roughness,
                                   in float3 f0,
                                   float cosineView,
                                   out float3 outIrradiance,
                                   out float3 outSpecular)
{
    // Cosine quadrature for diffuse, used when the scene has no sky visibility maps.
    float3 irradiance = 0;
    for (uint sampleIndex = 0; sampleIndex < ATMOSPHERE_ENVIRONMENT_SAMPLES; ++sampleIndex)
    {
        float quantile = (float(sampleIndex) + 0.5) / ATMOSPHERE_ENVIRONMENT_SAMPLES;
        float azimuth = float(sampleIndex) * ATMOSPHERE_ENVIRONMENT_GOLDEN_ANGLE;
        irradiance += SampleAtmosphereEnvironment(OrientAtmosphereSample(normalWorld, sqrt(1 - quantile), azimuth));
    }
    outIrradiance = irradiance * (PI / ATMOSPHERE_ENVIRONMENT_SAMPLES);
    outSpecular = EvaluateAtmosphereSpecular(normalWorld, viewDirectionWorld, roughness, f0, cosineView);
}

// Sky irradiance where the sky visibility maps say the receiver sees the sky. Each fixed direction carries an equal
// solid angle, so the sum over them integrates the upper hemisphere with continuous cosine weights: unlike the
// quadrature about the normal above, no sample jumps across the horizon as normals vary, which drew stepped bands
// and lines on curved and normal-mapped surfaces. Below the horizon the sky table's ground half is integrated as one
// radiance over the projected solid angle, scaled by how open the receiver is. visibleRadiance and ground come from
// the sky visibility pass (renderer_lit.slang, EvaluateSkyVisibility).
void EvaluateVisibleAtmosphereEnvironment(in float3 normalWorld,
                                          in float3 viewDirectionWorld,
                                          float roughness,
                                          in float3 f0,
                                          float cosineView,
                                          in float3 visibleRadiance,
                                          float ground,
                                          float specularVisibility,
                                          out float3 outIrradiance,
                                          out float3 outSpecular)
{
    float3 upWorld = deferredView.AtmosphereRadialUpWorld.xyz;
    outIrradiance = visibleRadiance + SampleAtmosphereEnvironment(-upWorld) *
                                          (ground * PI * saturate(0.5 - 0.5 * dot(normalWorld, upWorld)));
    outSpecular =
        EvaluateAtmosphereSpecular(normalWorld, viewDirectionWorld, roughness, f0, cosineView) * specularVisibility;
}

// The clear-air view segment from the camera to positionWorld: its transmittance and the radiance it scatters toward
// the camera. Outside the shell the segment is empty (transmittance one, nothing scattered).
void IntegrateClearAtmosphereViewSegment(in float3 positionWorld,
                                         out float3 outTransmittance,
                                         out float3 outInscattered)
{
    outTransmittance = float3(1);
    outInscattered = float3(0);
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
        return;
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
    outTransmittance = segment.Transmittance;
    outInscattered = deferredView.AtmosphereIlluminance.rgb * segment.Radiance;
}
// The view segment from the camera to positionWorld, through the clear air and the optional cloud layer. A surface's
// radiance L reaches the camera as L * outTransmittance + outInscattered. The cloud sits between the air in front of
// its entry and the rest of the segment (ComposeCloudLayer), which keeps the composition affine in L.
void IntegrateAtmosphereViewSegment(in float3 positionWorld, out float3 outTransmittance, out float3 outInscattered)
{
    IntegrateClearAtmosphereViewSegment(positionWorld, outTransmittance, outInscattered);
    CloudLayerParameters cloud = UnpackCloudLayer(deferredView.AtmosphereCloudLayer);
    AuroraLayerParameters aurora = deferredView.AtmosphereAuroraLayer;
    if (cloud.AxisXWorld.w <= 0 && aurora.AxisXWorld.w <= 0)
    {
        return;
    }
    float3 originWorld = deferredView.AtmosphereCameraWorld.xyz;
    float3 offsetWorld = positionWorld - originWorld;
    float distanceMetres = length(offsetWorld);
    float3 rayWorld = offsetWorld / max(distanceMetres, DIRECTION_EPSILON);
    float3 upWorld = deferredView.AtmosphereRadialUpWorld.xyz;
    float3 sunWorld = deferredView.AtmosphereSunWorld.xyz;
    if (aurora.AxisXWorld.w > 0 && distanceMetres > 0)
    {
        float height = deferredView.AtmosphereRadialUpWorld.w;
        float3 emission =
            IntegrateAuroraLayer(aurora, upWorld, rayWorld, height, distanceMetres * ATMOSPHERE_KILOMETRES_PER_METRE);
        if (any(emission > 0))
        {
            outInscattered +=
                emission * SampleTransmittance(atmosphereTransmittanceTexture, height, dot(upWorld, rayWorld));
        }
    }
    if (cloud.AxisXWorld.w <= 0)
    {
        return;
    }
    CloudLayerSegment segment = IntegrateCloudLayer(atmosphereTransmittanceTexture,
                                                    cloud,
                                                    rayWorld,
                                                    sunWorld,
                                                    deferredView.AtmosphereIlluminance.rgb,
                                                    deferredView.AtmosphereRadialUpWorld.w,
                                                    clamp(dot(rayWorld, upWorld), -1, 1),
                                                    clamp(dot(sunWorld, upWorld), -1, 1),
                                                    distanceMetres);
    if (segment.ExitMetres <= segment.EntryMetres)
    {
        return;
    }
    float3 airTransmittance;
    float3 airInscattered;
    IntegrateClearAtmosphereViewSegment(originWorld + rayWorld * segment.EntryMetres, airTransmittance, airInscattered);
    outInscattered = airInscattered + airTransmittance * segment.Radiance +
                     segment.Transmittance * (outInscattered - airInscattered);
    outTransmittance *= segment.Transmittance;
}

float3 ComposeAtmosphereSegment(in float3 surfaceRadiance, in float3 positionWorld)
{
    float3 transmittance;
    float3 inscattered;
    IntegrateAtmosphereViewSegment(positionWorld, transmittance, inscattered);
    return surfaceRadiance * transmittance + inscattered;
}
