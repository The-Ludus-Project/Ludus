// Raster first hits, one cosine-weighted continuation, and one next-event sample at each vertex.
// The first light connection includes all direct scattering. The second connects diffuse lobes only.
// An emitter or the environment seen along the continuation is excluded from the indirect term:
// the first light connection already accounts for that path. A camera ray that misses still shows the sky.

static const uint PRIMARY_LIGHT_SELECTION_DIMENSION = 0;
static const uint PRIMARY_LIGHT_SHAPE_FIRST_DIMENSION = 1;
static const uint BOUNCE_RADIUS_DIMENSION = 4;
static const uint BOUNCE_ANGLE_DIMENSION = 5;
static const uint BOUNCE_LIGHT_SELECTION_DIMENSION = 6;
static const uint BOUNCE_LIGHT_SHAPE_FIRST_DIMENSION = 7;
static const float BOUNCE_MAXIMUM_DISTANCE_WORLD = 3.402823466e38f;

bool HasDiffuseLobe(MaterialSample material)
{
    return material.Model == MaterialModel::Diffuse || ((material.Model == MaterialModel::MetallicRoughness ||
                                                         material.Model == MaterialModel::DiffuseConductorMix) &&
                                                        material.Metallic < 1);
}

bool TryLoadUnitMaterial(SceneContext scene,
                         SurfaceHit hit,
                         inout RayQueryContext inoutQueries,
                         out MaterialSample outMaterial)
{
    outMaterial = EvaluateMaterial(scene, hit);
    const float normalLengthSquared = dot(outMaterial.ShadingNormalWorld, outMaterial.ShadingNormalWorld);
    if (outMaterial.IsValid == false || normalLengthSquared <= 0)
    {
        FailSceneQuery(inoutQueries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return false;
    }
    outMaterial.ShadingNormalWorld *= rsqrt(normalLengthSquared);
    return true;
}

/// Diffuse BRDF times pi, using the same diffuse terms as EvaluateScattering.
/// For a cosine-weighted continuation this is also the complete f*cos/pdf weight.
float3 EvaluateDiffuseWeight(MaterialSample material, float3 viewDirectionWorld, float3 lightDirectionWorld)
{
    const float viewCosine = dot(material.ShadingNormalWorld, viewDirectionWorld);
    const float lightCosine = dot(material.ShadingNormalWorld, lightDirectionWorld);
    if (HasDiffuseLobe(material) == false || viewCosine <= 0 || lightCosine <= 0)
        return 0;
    if (material.Model == MaterialModel::Diffuse)
        return material.BaseColor;
    if (material.Model == MaterialModel::DiffuseConductorMix)
    {
        const float specularProbability = ComputeMaterialSpecularProbability(material.Model, material.Metallic);
        return material.BaseColor * (1 - specularProbability);
    }
    const float3 fresnel =
        EvaluateGgxReflectionTerms(material.BaseColor,
                                   material.Metallic,
                                   material.Model,
                                   material.Eta,
                                   material.K,
                                   ComputeMaterialAlpha(material.Model, material.Roughness, material.AlphaU),
                                   material.ShadingNormalWorld,
                                   viewDirectionWorld,
                                   lightDirectionWorld,
                                   viewCosine,
                                   lightCosine)
            .Fresnel;
    return material.BaseColor * (1 - fresnel) * (1 - material.Metallic);
}

float3 SampleDirectLight(SceneContext scene,
                         RayLaunch launch,
                         SurfaceHit receiver,
                         MaterialSample material,
                         float3 viewDirectionWorld,
                         bool isBounce,
                         inout RayQueryContext inoutQueries)
{
    const uint selectionDimension =
        isBounce == true ? BOUNCE_LIGHT_SELECTION_DIMENSION : PRIMARY_LIGHT_SELECTION_DIMENSION;
    const uint shapeDimension =
        isBounce == true ? BOUNCE_LIGHT_SHAPE_FIRST_DIMENSION : PRIMARY_LIGHT_SHAPE_FIRST_DIMENSION;
    const SceneLightSample sample = SampleLight(scene,
                                                receiver,
                                                launch,
                                                selectionDimension,
                                                shapeDimension,
                                                LightSelectionPolicy::Uniform,
                                                inoutQueries);
    const IncidentLightSample light = sample.Incident;
    if (light.HasSample == false || any(light.IncidentCoefficient > 0) == false)
        return 0;
    const float cosine = ComputeStandardBsdfCosine(material, viewDirectionWorld, light.DirectionWorld);
    if (cosine <= 0)
        return 0;

    float3 scattering;
    if (isBounce == true)
        scattering = EvaluateDiffuseWeight(material, viewDirectionWorld, light.DirectionWorld) / PI;
    else
    {
        float unusedPdf;
        EvaluateStandardBsdf(material, viewDirectionWorld, light.DirectionWorld, scattering, unusedPdf);
    }
    if (any(scattering > 0) == false || TestVisibility(scene, inoutQueries, receiver, light) != SceneQuery::Miss)
        return 0;

    return scattering * light.IncidentCoefficient *
           (cosine / (sample.Selection.SelectionPmf * light.ConditionalProbability));
}

float3 SampleOneDiffuseBounce(SceneContext scene,
                              RayLaunch launch,
                              SurfaceHit receiver,
                              MaterialSample material,
                              float3 viewDirectionWorld,
                              inout RayQueryContext inoutQueries)
{
    if (HasDiffuseLobe(material) == false || inoutQueries.HasFailed == true)
        return 0;

    const float radiusSample = SampleUniformFloat01(launch, BOUNCE_RADIUS_DIMENSION);
    const float angleSample = SampleUniformFloat01(launch, BOUNCE_ANGLE_DIMENSION);
    // Probability zero selects the cosine hemisphere branch of the shared sampler.
    const float3 directionWorld = SampleScatteringBsdf(material.ShadingNormalWorld,
                                                       viewDirectionWorld,
                                                       1,
                                                       0,
                                                       float3(0, radiusSample, angleSample));
    const float3 weight = EvaluateDiffuseWeight(material, viewDirectionWorld, directionWorld);
    if (any(weight > 0) == false)
        return 0;

    SurfaceHit bounce;
    const Ray ray = SpawnSurfaceRay(receiver, directionWorld, BOUNCE_MAXIMUM_DISTANCE_WORLD);
    if (TraceClosest(scene, inoutQueries, ray, bounce) != SceneQuery::Surface)
        return 0;

    MaterialSample bounceMaterial;
    if (TryLoadUnitMaterial(scene, bounce, inoutQueries, bounceMaterial) == false)
        return 0;
    return weight * SampleDirectLight(scene, launch, bounce, bounceMaterial, -directionWorld, true, inoutQueries);
}

float3 ShadeIndirectDiffuse(SceneContext scene, RayLaunch launch, inout RayQueryContext inoutQueries)
{
    SurfaceHit receiver;
    const SceneQuery status = LoadVisibilitySurface(scene, launch, inoutQueries, receiver);
    if (status == SceneQuery::Miss)
        return EvaluateEnvironment(scene, launch.CameraRay.DirectionWorld);
    if (status != SceneQuery::Surface)
        return 0;

    MaterialSample material;
    if (TryLoadUnitMaterial(scene, receiver, inoutQueries, material) == false)
        return 0;

    const float3 viewDirectionWorld = -launch.CameraRay.DirectionWorld;
    const float3 direct = SampleDirectLight(scene, launch, receiver, material, viewDirectionWorld, false, inoutQueries);
    const float3 indirect = SampleOneDiffuseBounce(scene, launch, receiver, material, viewDirectionWorld, inoutQueries);
    const float3 radiance = EvaluateEmission(scene, receiver, viewDirectionWorld) + direct + indirect;
    if (all(isfinite(radiance)) == false || any(radiance < 0) == true)
    {
        FailSceneQuery(inoutQueries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
        return 0;
    }
    return radiance;
}
