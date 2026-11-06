// The Slang-interface version of ../../material_permutation_lusl/main_shaders/closest.lusl. A GPU
// test compares the pixels of the two packages (docs/architecture/shading-language.md#delivery-plan).
//
// Slang interfaces have no concepts, so the permutation is spelled out: CarPaint calls a separate
// layered function, and the fog branch tests the lobe count at run time, which Slang folds per
// specialization.

static const float PLASTIC_BAND = 0.3333f;
static const float CAR_PAINT_BAND = 0.6667f;
static const float FOG_DENSITY = 0.125f;
static const float3 FOG_COLOR = float3(0.5f, 0.5f, 0.75f);
static const float CLEARCOAT_EXPONENT = 8.0f;
static const float CLEARCOAT_WEIGHT = 0.25f;

interface Material
{
    static const uint LOBE_COUNT;
    float3 Evaluate(float cosine);
};

interface ClearcoatMaterial : Material
{
    float3 EvaluateClearcoat(float cosine);
};

struct Plastic : Material
{
    static const uint LOBE_COUNT = 1u;
    float3 Albedo;

    float3 Evaluate(float cosine)
    {
        return Albedo * cosine;
    }
};

struct CarPaint : ClearcoatMaterial
{
    static const uint LOBE_COUNT = 2u;
    float3 Albedo;

    float3 Evaluate(float cosine)
    {
        return Albedo * (cosine * 0.5f);
    }

    float3 EvaluateClearcoat(float cosine)
    {
        return float3(pow(cosine, CLEARCOAT_EXPONENT) * CLEARCOAT_WEIGHT);
    }
};

struct Velvet : Material
{
    static const uint LOBE_COUNT = 2u;
    float3 Albedo;

    float3 Evaluate(float cosine)
    {
        return Albedo * (1.0f - cosine) + float3(cosine);
    }
};

float3 Shade<M : Material>(M material, float cosine)
{
    return material.Evaluate(cosine) / float(M.LOBE_COUNT);
}

float3 ShadeLayered<M : ClearcoatMaterial>(M material, float cosine)
{
    return material.Evaluate(cosine) + material.EvaluateClearcoat(cosine);
}

float3 ShadeWithFog<M : Material>(M material, float cosine, float distanceWorld)
{
    float3 color = Shade<M>(material, cosine);
    if (M.LOBE_COUNT == 1u)
    {
        color = lerp(color, FOG_COLOR, saturate(distanceWorld * FOG_DENSITY));
    }
    return color;
}

float3 ShadeClosest(RayHit hit)
{
    float cosine = abs(dot(hit.NormalWorld, hit.IncomingRay.DirectionWorld));
    float3 albedo = hit.Color.xyz;
    if (hit.Barycentric.x < PLASTIC_BAND)
    {
        Plastic plastic = {albedo};
        return ShadeWithFog<Plastic>(plastic, cosine, hit.DistanceWorld);
    }
    if (hit.Barycentric.x < CAR_PAINT_BAND)
    {
        CarPaint paint = {albedo};
        return ShadeLayered<CarPaint>(paint, cosine);
    }
    Velvet velvet = {albedo};
    return ShadeWithFog<Velvet>(velvet, cosine, hit.DistanceWorld);
}
