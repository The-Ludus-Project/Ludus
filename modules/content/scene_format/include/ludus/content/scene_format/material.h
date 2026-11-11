#pragma once

#ifdef __cplusplus
#    include <ludus/foundation/base/types.h>
#    include <ludus/foundation/containers/static_array.hpp>
namespace ludus::content::scene_format
{
/// Integer representation of material-model wire tags, also consumed by Slang.
using MaterialModelTag = foundation::uint32;
/// Binary32 scalar representation shared with Slang material policy constants.
using MaterialScalar = foundation::float32;
/// Bit mask of material capabilities, the LUDUS_MATERIAL_CAPABILITY_* values; also consumed by Slang.
using MaterialCapabilityBits = foundation::uint32;
#else
typedef uint MaterialModelTag;
typedef float MaterialScalar;
typedef uint MaterialCapabilityBits;
#endif
/// Legacy metallic/roughness scattering wire tag, shared with shader code.
static const MaterialModelTag LUDUS_MATERIAL_MODEL_METALLIC_ROUGHNESS = 0;
/// Lambertian scattering wire tag, shared with shader code.
static const MaterialModelTag LUDUS_MATERIAL_MODEL_DIFFUSE = 1;
/// Complex-IOR conductor scattering wire tag, shared with shader code.
static const MaterialModelTag LUDUS_MATERIAL_MODEL_CONDUCTOR = 2;
/// Preserved coated-diffuse scattering wire tag, shared with shader code.
static const MaterialModelTag LUDUS_MATERIAL_MODEL_COATED_DIFFUSE = 3;
/// Smooth dielectric interface (reflection and refraction) wire tag, shared with shader code.
static const MaterialModelTag LUDUS_MATERIAL_MODEL_DIELECTRIC = 4;
/// Thin dielectric slab (reflection and straight transmission) wire tag, shared with shader code.
static const MaterialModelTag LUDUS_MATERIAL_MODEL_THIN_DIELECTRIC = 5;
/// Weighted Lambertian and rough conductor wire tag.
static const MaterialModelTag LUDUS_MATERIAL_MODEL_DIFFUSE_CONDUCTOR_MIX = 6;
/// Content-only tag for a named source material without a scattering implementation.
static const MaterialModelTag LUDUS_MATERIAL_MODEL_UNRESOLVED = 7;
/// Project-authored cosine-power BRDF program, version 1.
static const MaterialModelTag LUDUS_MATERIAL_MODEL_PROJECT_BRDF = 8;
/// Bounded closure program (material_program.h): a weighted mixture of leaf lobes.
static const MaterialModelTag LUDUS_MATERIAL_MODEL_PROGRAM = 9;
/// Maximum authored cosine exponent before applying perceptual roughness.
static const MaterialScalar LUDUS_PROJECT_BRDF_MAX_POWER = 32;
/// Minimum total lobe weight, to keep normalization bounded in binary32.
static const MaterialScalar LUDUS_PROJECT_BRDF_MIN_WEIGHT = 0.001F;
/// PBRT default bounded internal layer events; independent of scene path depth.
static const MaterialModelTag LUDUS_COATING_MAX_DEPTH = 10;
/// PBRT default number of independent layered evaluation/PDF estimates.
static const MaterialModelTag LUDUS_COATING_SAMPLES = 1;
/// GGX alpha below which a dielectric interface is perfectly specular: pbrt-v4's
/// TrowbridgeReitzDistribution::EffectivelySmooth threshold.
static const MaterialScalar LUDUS_SMOOTH_INTERFACE_ALPHA = 0.001F;
/// Smallest rough coating alpha; smaller values require a delta dielectric interface.
static const MaterialScalar LUDUS_COATING_MIN_ALPHA = LUDUS_SMOOTH_INTERFACE_ALPHA;
/// Capability: evaluates reflection with a continuous solid-angle density.
static const MaterialCapabilityBits LUDUS_MATERIAL_CAPABILITY_CONTINUOUS_REFLECTION = 1U;
/// Capability: every scattering proposal consumes exactly three uniform values from the caller's path event.
static const MaterialCapabilityBits LUDUS_MATERIAL_CAPABILITY_FIXED_DIMENSION_SAMPLER = 2U;
/// Capability: may transmit through the surface into or out of its medium.
static const MaterialCapabilityBits LUDUS_MATERIAL_CAPABILITY_TRANSMISSION = 4U;
/// Capability: runs an internal random walk on its own random stream; its densities are estimated.
static const MaterialCapabilityBits LUDUS_MATERIAL_CAPABILITY_INTERNAL_WALK = 8U;
/// Capability: may produce delta events with a probability mass instead of a directional density.
static const MaterialCapabilityBits LUDUS_MATERIAL_CAPABILITY_DELTA = 16U;

#ifdef __cplusplus
/// Scattering identity retained by scene assets; evaluators declare their supported profile.
// NOLINTNEXTLINE(performance-enum-size): material wire tags retain their explicit 32-bit width.
enum class MaterialModel : foundation::uint32
{
    MetallicRoughness =
        LUDUS_MATERIAL_MODEL_METALLIC_ROUGHNESS,         ///< Legacy RGB GGX/Schlick and Fresnel-weighted diffuse.
    Diffuse = LUDUS_MATERIAL_MODEL_DIFFUSE,              ///< Ideal Lambertian reflection without a specular lobe.
    Conductor = LUDUS_MATERIAL_MODEL_CONDUCTOR,          ///< Isotropic GGX with complex-IOR Fresnel; no diffuse lobe.
    CoatedDiffuse = LUDUS_MATERIAL_MODEL_COATED_DIFFUSE, ///< Rough dielectric coating over a diffuse base;
                                                         ///< non-scattering reference profile.
    /// Interface of a nonabsorbing dielectric with real index Eta: Fresnel-weighted
    /// specular reflection and refraction. A closed mesh bounds the medium; its
    /// oriented front side faces the outside (index one).
    Dielectric = LUDUS_MATERIAL_MODEL_DIELECTRIC,
    /// Infinitesimally thin nonabsorbing dielectric slab with real index Eta:
    /// Fresnel reflection including internal interreflection, and undeviated transmission.
    ThinDielectric = LUDUS_MATERIAL_MODEL_THIN_DIELECTRIC,
    /// Weighted Lambertian and isotropic rough conductor. The material metallic field is the conductor weight.
    DiffuseConductorMix = LUDUS_MATERIAL_MODEL_DIFFUSE_CONDUCTOR_MIX,
    /// Stored import diagnostic for a named material without a scattering implementation.
    /// Only content storage accepts this tag. Renderers require a resolved material.
    Unresolved = LUDUS_MATERIAL_MODEL_UNRESOLVED,
    /// Project-authored continuous reflection program with up to three cosine-power lobes.
    ProjectBrdf = LUDUS_MATERIAL_MODEL_PROJECT_BRDF,
    /// Closure program stored beside the material (material_program.h). Its capabilities come from
    /// the program, and its MaterialScattering fields keep their defaults.
    Program = LUDUS_MATERIAL_MODEL_PROGRAM,
};
/// Report whether the model uses the complex-IOR GGX lobe and its authored alpha.
[[nodiscard]] constexpr bool HasRoughConductorLobe(MaterialModel model) noexcept
{
    return model == MaterialModel::Conductor || model == MaterialModel::DiffuseConductorMix;
}
/// Report the scattering capabilities a built-in model guarantees for every parameter value.
/// A project BRDF's fixed-dimension sampler is its own declaration; the MaterialScattering
/// overload adds it. A closure program declares its own capabilities (GetMaterialProgramCapabilities),
/// and the unresolved import marker offers nothing.
/// @return A mask of LUDUS_MATERIAL_CAPABILITY_* bits.
[[nodiscard]] constexpr MaterialCapabilityBits GetMaterialCapabilities(MaterialModel model) noexcept
{
    switch (model)
    {
        case MaterialModel::MetallicRoughness:
        case MaterialModel::Diffuse:
        case MaterialModel::Conductor:
        case MaterialModel::DiffuseConductorMix:
            return LUDUS_MATERIAL_CAPABILITY_CONTINUOUS_REFLECTION | LUDUS_MATERIAL_CAPABILITY_FIXED_DIMENSION_SAMPLER;
        case MaterialModel::CoatedDiffuse:
            return LUDUS_MATERIAL_CAPABILITY_CONTINUOUS_REFLECTION | LUDUS_MATERIAL_CAPABILITY_INTERNAL_WALK;
        case MaterialModel::Dielectric:
        case MaterialModel::ThinDielectric:
            return LUDUS_MATERIAL_CAPABILITY_TRANSMISSION | LUDUS_MATERIAL_CAPABILITY_DELTA;
        case MaterialModel::ProjectBrdf:
            return LUDUS_MATERIAL_CAPABILITY_CONTINUOUS_REFLECTION;
        case MaterialModel::Unresolved:
        case MaterialModel::Program:
            break;
    }
    return 0;
}
/// Material capabilities a scene pass consumes: every assigned material must offer each required
/// capability and none of the unsupported ones. Passes declare requirements, not model names.
struct MaterialRequirement final
{
    MaterialCapabilityBits RequiredCapabilities = 0;    ///< Bits every assigned material must set.
    MaterialCapabilityBits UnsupportedCapabilities = 0; ///< Bits no assigned material may set.
};
/// Continuous reflection with a fixed three-value sampler and no transmission, delta or internal-walk
/// events. Restricted estimators such as Rao-Blackwellized Metropolis consume exactly this contract.
inline constexpr MaterialRequirement FIXED_DIMENSION_REFLECTION_REQUIREMENT =
{
    .RequiredCapabilities =
        LUDUS_MATERIAL_CAPABILITY_CONTINUOUS_REFLECTION | LUDUS_MATERIAL_CAPABILITY_FIXED_DIMENSION_SAMPLER,
    .UnsupportedCapabilities = LUDUS_MATERIAL_CAPABILITY_TRANSMISSION | LUDUS_MATERIAL_CAPABILITY_INTERNAL_WALK |
                               LUDUS_MATERIAL_CAPABILITY_DELTA,
};
/// Test a capability mask against a pass requirement.
/// @param capabilities LUDUS_MATERIAL_CAPABILITY_* bits of one assigned material.
/// @param requirement The capabilities the consuming pass requires and those it cannot handle.
/// @return True when every required bit is set and no unsupported bit is set.
[[nodiscard]] constexpr bool SatisfiesMaterialRequirement(MaterialCapabilityBits capabilities,
                                                          MaterialRequirement requirement) noexcept
{
    return (capabilities & requirement.RequiredCapabilities) == requirement.RequiredCapabilities &&
           (capabilities & requirement.UnsupportedCapabilities) == 0;
}
/// Report whether the model supplies continuous reflection and a fixed three-value sampler.
[[nodiscard]] constexpr bool SupportsFixedDimensionReflection(MaterialModel model) noexcept
{
    return SatisfiesMaterialRequirement(GetMaterialCapabilities(model), FIXED_DIMENSION_REFLECTION_REQUIREMENT);
}
/// RGB channels in serialized optical constants.
inline constexpr foundation::usize MATERIAL_RGB_CHANNELS = 3;
/// Maximum bytes of a source material name; names are counted UTF-8, never truncated.
inline constexpr foundation::usize MAX_MATERIAL_NAME_BYTES = 128;
/// Smallest admitted rough-conductor GGX alpha; perfect-specular transport is separate.
inline constexpr foundation::float32 MIN_MATERIAL_ALPHA = 0.0001F;
/// Optical constants are finite and bounded to avoid overflow in Fresnel evaluation.
inline constexpr foundation::float32 MAX_MATERIAL_OPTICAL_CONSTANT = 10000;
/// Default PBRT coated-diffuse dielectric index of refraction.
inline constexpr foundation::float32 DEFAULT_COATING_ETA = 1.5F;
/// Default PBRT coated-diffuse optical thickness.
inline constexpr foundation::float32 DEFAULT_COATING_THICKNESS = 0.01F;
/// Default PBRT dielectric and thin-dielectric index of refraction.
inline constexpr foundation::float32 DEFAULT_DIELECTRIC_ETA = 1.5F;
/// Legacy base colour of an imported dielectric for evaluators without transmission: the
/// normal-incidence reflectance ((eta - 1) / (eta + 1))^2 of the default index.
inline constexpr foundation::float32 DIELECTRIC_FALLBACK_REFLECTANCE = 0.04F;
/// Number of weighted cosine-power terms in the version-1 project BRDF program.
inline constexpr foundation::usize PROJECT_BRDF_LOBE_COUNT = MATERIAL_RGB_CHANNELS;
/// Source material parameters, shared by tools and runtime without renderer dependencies.
/// AlphaU/V are the resolved GGX distribution widths, not perceptual roughness.
/// Reference conductors and coatings currently admit isotropic alpha only. The
/// coating reference profile models absorption without medium scattering; omitted
/// source coating controls resolve to the shared PBRT default policy constants.
/// Dielectrics store their real index in every Eta channel with zero K; reference
/// evaluators admit equal channels and smooth interfaces (alpha below
/// LUDUS_SMOOTH_INTERFACE_ALPHA). Evaluators without transmission shade a dielectric
/// through the record's legacy metallic/roughness fields instead.
struct MaterialScattering final
{
    MaterialModel Model = MaterialModel::MetallicRoughness; ///< Evaluation model.
    foundation::StaticArray<foundation::float32, MATERIAL_RGB_CHANNELS> Eta{
        {1, 1, 1}}; ///< Positive complex-IOR real part, or three project BRDF lobe weights.
    foundation::StaticArray<foundation::float32, MATERIAL_RGB_CHANNELS>
        K{};                                              ///< Complex-IOR imaginary part, or three project BRDF powers.
    foundation::float32 AlphaU = 1;                       ///< GGX alpha along the tangent; [MIN_MATERIAL_ALPHA,1].
    foundation::float32 AlphaV = 1;                       ///< GGX alpha along the bitangent; same domain.
    foundation::float32 CoatingEta = DEFAULT_COATING_ETA; ///< Dielectric coating index of refraction.
    foundation::float32 CoatingThickness =
        DEFAULT_COATING_THICKNESS; ///< Optical thickness, or project BRDF guarantee (0 or 1).
};
/// Report the capabilities of one material's scattering, including a project BRDF's declared
/// fixed-dimension sampler (stored in CoatingThickness as 0 or 1).
/// @return A mask of LUDUS_MATERIAL_CAPABILITY_* bits.
[[nodiscard]] constexpr MaterialCapabilityBits GetMaterialCapabilities(const MaterialScattering& scattering) noexcept
{
    MaterialCapabilityBits capabilities = GetMaterialCapabilities(scattering.Model);
    if (scattering.Model == MaterialModel::ProjectBrdf && scattering.CoatingThickness == 1)
    {
        capabilities |= LUDUS_MATERIAL_CAPABILITY_FIXED_DIMENSION_SAMPLER;
    }
    return capabilities;
}
/// Test the fixed-dimension reflection guarantee, including a project program's declaration.
[[nodiscard]] constexpr bool SupportsFixedDimensionReflection(const MaterialScattering& scattering) noexcept
{
    return SatisfiesMaterialRequirement(GetMaterialCapabilities(scattering), FIXED_DIMENSION_REFLECTION_REQUIREMENT);
}
/// Number of scalar float32 fields in the explicit MaterialScattering wire schema.
inline constexpr foundation::usize MATERIAL_SCATTERING_FLOATS = MATERIAL_RGB_CHANNELS + MATERIAL_RGB_CHANNELS + 2 + 2;
/// Validate finite bounded optical parameters. Unsupported model tags, including the unresolved
/// marker, reject. A closure program keeps every field at its default, so equal programs cook to
/// equal bytes; its program is validated by IsMaterialProgramValid.
/// An evaluator separately admits the supported anisotropy.
[[nodiscard]] constexpr bool IsMaterialScatteringValid(const MaterialScattering& scattering) noexcept
{
    if (scattering.Model == MaterialModel::Program)
    {
        const MaterialScattering defaults;
        for (foundation::usize channel = 0; channel < MATERIAL_RGB_CHANNELS; ++channel)
        {
            if (scattering.Eta[channel] != defaults.Eta[channel] || scattering.K[channel] != defaults.K[channel])
            {
                return false;
            }
        }
        return scattering.AlphaU == defaults.AlphaU && scattering.AlphaV == defaults.AlphaV &&
               scattering.CoatingEta == defaults.CoatingEta && scattering.CoatingThickness == defaults.CoatingThickness;
    }
    if (scattering.Model == MaterialModel::ProjectBrdf)
    {
        foundation::float32 total = 0;
        for (foundation::usize lobe = 0; lobe < PROJECT_BRDF_LOBE_COUNT; ++lobe)
        {
            if ((scattering.Eta[lobe] >= 0 && scattering.Eta[lobe] <= 1) == false ||
                (scattering.K[lobe] >= 0 && scattering.K[lobe] <= LUDUS_PROJECT_BRDF_MAX_POWER) == false)
            {
                return false;
            }
            total += scattering.Eta[lobe];
        }
        return total >= LUDUS_PROJECT_BRDF_MIN_WEIGHT && scattering.AlphaU == 1 && scattering.AlphaV == 1 &&
               scattering.CoatingEta == DEFAULT_COATING_ETA &&
               (scattering.CoatingThickness == 0 || scattering.CoatingThickness == 1);
    }
    if (scattering.Model > MaterialModel::DiffuseConductorMix ||
        (scattering.Model == MaterialModel::DiffuseConductorMix && scattering.AlphaU != scattering.AlphaV) ||
        (scattering.AlphaU >= MIN_MATERIAL_ALPHA && scattering.AlphaU <= 1) == false ||
        (scattering.AlphaV >= MIN_MATERIAL_ALPHA && scattering.AlphaV <= 1) == false ||
        (scattering.CoatingEta > 0 && scattering.CoatingEta <= MAX_MATERIAL_OPTICAL_CONSTANT) == false ||
        (scattering.CoatingThickness >= 0 && scattering.CoatingThickness <= MAX_MATERIAL_OPTICAL_CONSTANT) == false)
    {
        return false;
    }
    for (foundation::usize channel = 0; channel < MATERIAL_RGB_CHANNELS; ++channel)
    {
        if ((scattering.Eta[channel] > 0 && scattering.Eta[channel] <= MAX_MATERIAL_OPTICAL_CONSTANT) == false ||
            (scattering.K[channel] >= 0 && scattering.K[channel] <= MAX_MATERIAL_OPTICAL_CONSTANT) == false)
        {
            return false;
        }
    }
    return true;
}
/// Validate cooked optical storage, including the import-only unresolved marker.
[[nodiscard]] constexpr bool IsStoredMaterialScatteringValid(MaterialScattering scattering) noexcept
{
    if (scattering.Model == MaterialModel::Unresolved)
    {
        scattering.Model = MaterialModel::MetallicRoughness;
    }
    return IsMaterialScatteringValid(scattering);
}
} // namespace ludus::content::scene_format
#else
/// Slang spelling of MaterialModel, the type of MaterialSample.Model and of the scattering helpers' model
/// parameters. A raw tag converts only explicitly: MaterialModel(tag).
enum MaterialModel : MaterialModelTag
{
    MetallicRoughness =
        LUDUS_MATERIAL_MODEL_METALLIC_ROUGHNESS,           ///< Legacy RGB GGX/Schlick and Fresnel-weighted diffuse.
    Diffuse = LUDUS_MATERIAL_MODEL_DIFFUSE,                ///< Ideal Lambertian reflection.
    Conductor = LUDUS_MATERIAL_MODEL_CONDUCTOR,            ///< Isotropic GGX with complex-IOR Fresnel.
    CoatedDiffuse = LUDUS_MATERIAL_MODEL_COATED_DIFFUSE,   ///< Rough dielectric coating over a diffuse base.
    Dielectric = LUDUS_MATERIAL_MODEL_DIELECTRIC,          ///< Smooth nonabsorbing dielectric interface.
    ThinDielectric = LUDUS_MATERIAL_MODEL_THIN_DIELECTRIC, ///< Thin dielectric slab.
    DiffuseConductorMix = LUDUS_MATERIAL_MODEL_DIFFUSE_CONDUCTOR_MIX, ///< Weighted Lambertian and conductor.
    ProjectBrdf = LUDUS_MATERIAL_MODEL_PROJECT_BRDF,                  ///< Bounded project-authored reflection program.
    Program = LUDUS_MATERIAL_MODEL_PROGRAM, ///< Closure program at MaterialSample.ProgramOffset.
};
#endif // __cplusplus
