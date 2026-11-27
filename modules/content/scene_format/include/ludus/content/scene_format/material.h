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
/// Invisible boundary of a bounded homogeneous absorbing medium; content storage only.
static const MaterialModelTag LUDUS_MATERIAL_MODEL_MEDIUM_INTERFACE = 10;
/// Two-sided alpha-masked hair card with an authored strand direction and directional highlights.
static const MaterialModelTag LUDUS_MATERIAL_MODEL_HAIR_CARD = 11;
/// Skin: a dielectric surface whose diffuse lighting spreads over per-channel scattering distances.
static const MaterialModelTag LUDUS_MATERIAL_MODEL_SKIN = 12;
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
/// Vacuum wavelength of the Fraunhofer d (helium) line in nanometres. A dielectric's stored index n_d is its index
/// here, as in optical glass catalogues.
static const MaterialScalar LUDUS_DIELECTRIC_D_LINE_NANOMETERS = 587.5618F;
/// Vacuum wavelength of the Fraunhofer F (hydrogen) line in nanometres; the Abbe number divides by n_F - n_C.
static const MaterialScalar LUDUS_DIELECTRIC_F_LINE_NANOMETERS = 486.1327F;
/// Vacuum wavelength of the Fraunhofer C (hydrogen) line in nanometres.
static const MaterialScalar LUDUS_DIELECTRIC_C_LINE_NANOMETERS = 656.2725F;
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
    /// oriented front side faces the outside (index one). K[0] may declare dispersion
    /// (DielectricDispersion).
    Dielectric = LUDUS_MATERIAL_MODEL_DIELECTRIC,
    /// Infinitesimally thin nonabsorbing dielectric slab with real index Eta:
    /// Fresnel reflection including internal interreflection, and undeviated transmission.
    /// K[0] may declare dispersion (DielectricDispersion).
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
    /// Invisible, index-matched boundary of one bounded homogeneous absorbing medium (pbrt-v4's
    /// `interface` material with an interior medium). It neither scatters nor occludes. K holds the
    /// enclosed medium's linear RGB absorption coefficient per world metre; every other scattering
    /// field keeps its default (IsMediumInterfaceValid). The closed boundary mesh's oriented front
    /// side faces the outside. Only content storage accepts this tag: surface evaluators reject it,
    /// and RT1 reference captures receive the boundary as a renderer ReferenceMediumDescription.
    MediumInterface = LUDUS_MATERIAL_MODEL_MEDIUM_INTERFACE,
    /// Hair card: a two-sided card whose raster shading follows an authored strand direction, with two
    /// longitudinally shifted highlights. Eta holds the strand direction and the two highlight shifts in
    /// degrees and K the two highlight weights (HairCardParameters, IsHairCardValid). The record's
    /// metallic is zero; base colour, roughness and the alpha mask keep their usual meaning. Ray and
    /// reference evaluators shade the card with the metallic/roughness dielectric lobes
    /// (GetSurfaceReflectionScattering); the strand highlight is a raster technique.
    HairCard = LUDUS_MATERIAL_MODEL_HAIR_CARD,
    /// Skin: the record's base colour and roughness with zero metallic, and K holding the linear RGB
    /// scattering distance in world metres, each channel in [0, MAX_SKIN_SCATTERING_DISTANCE]. Every
    /// other scattering field keeps its default (IsMaterialScatteringValid). The surface reflection is the
    /// metallic/roughness model's dielectric lobes (GetSurfaceReflectionScattering), so a zero distance
    /// reproduces that model exactly. Renderers currently shade the surface reflection only: the raster
    /// screen-space diffusion is issue #1372 and reference subsurface transport is issue #1373.
    Skin = LUDUS_MATERIAL_MODEL_SKIN,
};
/// Highest stored MaterialModel tag; readers reject larger tags.
inline constexpr MaterialModel LAST_MATERIAL_MODEL = MaterialModel::Skin;
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
        case MaterialModel::HairCard:
        case MaterialModel::Skin:
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
        case MaterialModel::MediumInterface:
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
/// Largest admitted medium absorption coefficient per world metre: transmittance through one
/// millimetre is then exp(-10), and larger values add no reference evidence before underflow.
inline constexpr foundation::float32 MAX_MEDIUM_ABSORPTION_PER_WORLD = 10000;
/// Largest admitted skin scattering distance, in world metres. Skin spreads light over millimetres;
/// a screen-space diffusion of ten centimetres no longer represents a surface's own scattering.
inline constexpr foundation::float32 MAX_SKIN_SCATTERING_DISTANCE = 0.1F;
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
/// Largest admitted hair-card strand direction magnitude, in degrees from the surface tangent.
inline constexpr foundation::float32 MAX_HAIR_CARD_STRAND_DEGREES = 180;
/// Largest admitted hair-card highlight shift magnitude, in degrees toward the card normal. Measured
/// human hair shifts its primary highlight by five to ten degrees; larger shifts no longer describe
/// a cuticle tilt.
inline constexpr foundation::float32 MAX_HAIR_CARD_SHIFT_DEGREES = 30;
/// Default primary (surface reflection) highlight shift of a hair card, in degrees.
inline constexpr foundation::float32 DEFAULT_HAIR_CARD_PRIMARY_SHIFT_DEGREES = -5;
/// Default secondary (internal reflection) highlight shift of a hair card, in degrees: one and a half
/// times the primary shift with the opposite sign.
inline constexpr foundation::float32 DEFAULT_HAIR_CARD_SECONDARY_SHIFT_DEGREES = 7.5F;
/// Default weight of a hair card's untinted primary highlight.
inline constexpr foundation::float32 DEFAULT_HAIR_CARD_PRIMARY_WEIGHT = 0.1F;
/// Default weight of a hair card's base-colour-tinted secondary highlight.
inline constexpr foundation::float32 DEFAULT_HAIR_CARD_SECONDARY_WEIGHT = 0.05F;
/// Source material parameters, shared by tools and runtime without renderer dependencies.
/// AlphaU/V are the resolved GGX distribution widths, not perceptual roughness.
/// Reference conductors and coatings currently admit isotropic alpha only. The
/// coating reference profile models absorption without medium scattering; omitted
/// source coating controls resolve to the shared PBRT default policy constants.
/// Dielectrics store their real index n_d in every Eta channel, their Abbe number in K[0]
/// (zero for a nondispersive index) and zero K[1..2] (DielectricDispersion); reference
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
/// Authored hair-card controls, stored in a MaterialModel::HairCard MaterialScattering.
///
/// The strand direction lies in the card's tangent plane: zero degrees follows the surface tangent
/// (increasing texture U) and ninety degrees the bitangent. Each highlight tilts the strand toward the
/// card normal by its shift before the Kajiya-Kay highlight is evaluated around it. Thanks to Stephen
/// R. Marschner, Henrik Wann Jensen, Mike Cammarano, Steve Worley and Pat Hanrahan, "Light Scattering
/// from Human Hair Fibers", ACM Transactions on Graphics 22(3), 2003, https://doi.org/10.1145/882262.882345,
/// for the opposite longitudinal shifts of the surface and internal reflections, whose ratio sets the
/// defaults here.
struct HairCardParameters final
{
    /// Strand direction in degrees from the tangent toward the bitangent, in [-180, 180].
    foundation::float32 StrandDirectionDegrees = 0;
    /// Primary highlight shift in degrees, in [-MAX_HAIR_CARD_SHIFT_DEGREES, MAX_HAIR_CARD_SHIFT_DEGREES].
    foundation::float32 PrimaryShiftDegrees = DEFAULT_HAIR_CARD_PRIMARY_SHIFT_DEGREES;
    /// Secondary highlight shift in degrees, in the same range.
    foundation::float32 SecondaryShiftDegrees = DEFAULT_HAIR_CARD_SECONDARY_SHIFT_DEGREES;
    /// Untinted primary highlight weight in [0, 1].
    foundation::float32 PrimaryWeight = DEFAULT_HAIR_CARD_PRIMARY_WEIGHT;
    /// Base-colour-tinted secondary highlight weight in [0, 1].
    foundation::float32 SecondaryWeight = DEFAULT_HAIR_CARD_SECONDARY_WEIGHT;
};
/// Report whether hair-card controls are finite and inside their documented ranges.
[[nodiscard]] constexpr bool IsHairCardParametersValid(const HairCardParameters& parameters) noexcept
{
    return (parameters.StrandDirectionDegrees >= -MAX_HAIR_CARD_STRAND_DEGREES &&
            parameters.StrandDirectionDegrees <= MAX_HAIR_CARD_STRAND_DEGREES) &&
           (parameters.PrimaryShiftDegrees >= -MAX_HAIR_CARD_SHIFT_DEGREES &&
            parameters.PrimaryShiftDegrees <= MAX_HAIR_CARD_SHIFT_DEGREES) &&
           (parameters.SecondaryShiftDegrees >= -MAX_HAIR_CARD_SHIFT_DEGREES &&
            parameters.SecondaryShiftDegrees <= MAX_HAIR_CARD_SHIFT_DEGREES) &&
           (parameters.PrimaryWeight >= 0 && parameters.PrimaryWeight <= 1) &&
           (parameters.SecondaryWeight >= 0 && parameters.SecondaryWeight <= 1);
}
/// Store hair-card controls in their scattering slots: Eta holds the strand direction and the two
/// shifts, K[0..1] the two weights, and every other field keeps its default.
/// @return A MaterialModel::HairCard scattering; validate the input with IsHairCardParametersValid.
[[nodiscard]] constexpr MaterialScattering MakeHairCardScattering(const HairCardParameters& parameters) noexcept
{
    MaterialScattering scattering;
    scattering.Model = MaterialModel::HairCard;
    scattering.Eta = {
        {parameters.StrandDirectionDegrees, parameters.PrimaryShiftDegrees, parameters.SecondaryShiftDegrees}};
    scattering.K = {{parameters.PrimaryWeight, parameters.SecondaryWeight, 0}};
    return scattering;
}
/// Read the hair-card controls of a MaterialModel::HairCard scattering; other models return defaults.
[[nodiscard]] constexpr HairCardParameters GetHairCardParameters(const MaterialScattering& scattering) noexcept
{
    if (scattering.Model != MaterialModel::HairCard)
    {
        return {};
    }
    return
    {
        .StrandDirectionDegrees = scattering.Eta[0],
        .PrimaryShiftDegrees = scattering.Eta[1],
        .SecondaryShiftDegrees = scattering.Eta[2],
        .PrimaryWeight = scattering.K[0],
        .SecondaryWeight = scattering.K[1],
    };
}
/// Validate a hair card: model HairCard, valid controls, zero K[2] and every other field at its default.
[[nodiscard]] constexpr bool IsHairCardValid(const MaterialScattering& scattering) noexcept
{
    const MaterialScattering defaults;
    return scattering.Model == MaterialModel::HairCard &&
           IsHairCardParametersValid(GetHairCardParameters(scattering)) == true && scattering.K[2] == 0 &&
           scattering.AlphaU == defaults.AlphaU && scattering.AlphaV == defaults.AlphaV &&
           scattering.CoatingEta == defaults.CoatingEta && scattering.CoatingThickness == defaults.CoatingThickness;
}
/// Smallest admitted dispersive Abbe number. Optical glasses span about 17 to 95 and strongly dispersive
/// crystals such as rutile reach about 10; smaller values describe no transparent dielectric.
inline constexpr foundation::float32 MIN_DIELECTRIC_ABBE_NUMBER = 5;
/// Largest admitted dispersive Abbe number. Its index varies by less than 0.3% of n_d - 1 over the visible
/// spectrum; store zero to declare a nondispersive index instead.
inline constexpr foundation::float32 MAX_DIELECTRIC_ABBE_NUMBER = 1000;
/// Declared index of refraction of a Dielectric or ThinDielectric, as optical glass catalogues give it: the index
/// n_d at the Fraunhofer d line (LUDUS_DIELECTRIC_D_LINE_NANOMETERS) and the Abbe number V_d = (n_d - 1) /
/// (n_F - n_C). The index at a vacuum wavelength is the two-term Cauchy equation n = A + B / lambda^2 through n_d
/// with that n_F - n_C (ComputeDielectricIndex). A MaterialScattering stores n_d in every Eta channel and V_d in
/// K[0], with zero K[1..2].
///
/// Spectral evaluators (one sampled wavelength per path) refract at the index of their wavelength. RGB evaluators,
/// which carry no wavelength, explicitly use n_d for every channel: a zero wavelength selects it.
///
/// Thanks to Augustin-Louis Cauchy, "Memoire sur la dispersion de la lumiere" (1836), for the equation. Ludus
/// declares the catalogue pair instead of pbrt-v4's tabulated index spectra; its pbrt importers convert such a
/// spectrum, named or tabulated, through its F, d and C lines.
struct DielectricDispersion final
{
    /// Index of refraction n_d at the d line, in (0, MAX_MATERIAL_OPTICAL_CONSTANT]; above one when dispersive.
    foundation::float32 IndexD = DEFAULT_DIELECTRIC_ETA;
    /// Abbe number V_d in [MIN_DIELECTRIC_ABBE_NUMBER, MAX_DIELECTRIC_ABBE_NUMBER], or zero for a
    /// nondispersive index.
    foundation::float32 AbbeNumber = 0;
};
/// Report whether a declared dielectric index is finite and inside its documented ranges.
[[nodiscard]] constexpr bool IsDielectricDispersionValid(const DielectricDispersion& dispersion) noexcept
{
    const bool isIndexValid = dispersion.IndexD > 0 && dispersion.IndexD <= MAX_MATERIAL_OPTICAL_CONSTANT;
    const bool isAbbeValid =
        dispersion.AbbeNumber == 0 || (dispersion.IndexD > 1 && dispersion.AbbeNumber >= MIN_DIELECTRIC_ABBE_NUMBER &&
                                       dispersion.AbbeNumber <= MAX_DIELECTRIC_ABBE_NUMBER);
    return isIndexValid == true && isAbbeValid == true;
}
/// Report whether the model is a dielectric interface, solid or thin, whose K[0] holds its Abbe number.
[[nodiscard]] constexpr bool IsDielectricModel(MaterialModel model) noexcept
{
    return model == MaterialModel::Dielectric || model == MaterialModel::ThinDielectric;
}
/// Read the declared index of a dielectric scattering: n_d from Eta[0] and V_d from K[0]. Other models return
/// the default nondispersive index.
[[nodiscard]] constexpr DielectricDispersion GetDielectricDispersion(const MaterialScattering& scattering) noexcept
{
    if (IsDielectricModel(scattering.Model) == false)
    {
        return {};
    }
    return
    {
        .IndexD = scattering.Eta[0],
        .AbbeNumber = scattering.K[0],
    };
}
/// Store a smooth dielectric: n_d in every Eta channel, V_d in K[0], zero K[1..2] and the smallest alpha.
/// @param model Dielectric or ThinDielectric; validate the result with IsMaterialScatteringValid.
/// @param dispersion The declared index n_d and Abbe number (IsDielectricDispersionValid).
[[nodiscard]] constexpr MaterialScattering MakeDielectricScattering(MaterialModel model,
                                                                    const DielectricDispersion& dispersion) noexcept
{
    MaterialScattering scattering;
    scattering.Model = model;
    scattering.Eta = {{dispersion.IndexD, dispersion.IndexD, dispersion.IndexD}};
    scattering.K = {{dispersion.AbbeNumber, 0, 0}};
    scattering.AlphaU = MIN_MATERIAL_ALPHA;
    scattering.AlphaV = MIN_MATERIAL_ALPHA;
    return scattering;
}
/// Index of refraction at a vacuum wavelength: n_d + (n_d - 1) / V_d * ((lambda_d / lambda)^2 - 1) /
/// ((lambda_d / lambda_F)^2 - (lambda_d / lambda_C)^2), the two-term Cauchy equation through n_d with
/// n_F - n_C = (n_d - 1) / V_d. The GPU twin is ComputeDielectricIndex in the renderer's dielectric.slang.
/// @param dispersion The declared index n_d and Abbe number, valid by IsDielectricDispersionValid.
/// @param wavelengthNanometers Vacuum wavelength of a spectral path in nanometres; zero (RGB transport) selects n_d.
/// @return n_d for a nondispersive index or a zero wavelength; otherwise the index at the wavelength.
[[nodiscard]] constexpr foundation::float32 ComputeDielectricIndex(const DielectricDispersion& dispersion,
                                                                   foundation::float32 wavelengthNanometers) noexcept
{
    if (dispersion.AbbeNumber == 0 || wavelengthNanometers <= 0)
    {
        return dispersion.IndexD;
    }
    const foundation::float32 relativeF = LUDUS_DIELECTRIC_D_LINE_NANOMETERS / LUDUS_DIELECTRIC_F_LINE_NANOMETERS;
    const foundation::float32 relativeC = LUDUS_DIELECTRIC_D_LINE_NANOMETERS / LUDUS_DIELECTRIC_C_LINE_NANOMETERS;
    const foundation::float32 relative = LUDUS_DIELECTRIC_D_LINE_NANOMETERS / wavelengthNanometers;
    const foundation::float32 spread = relativeF * relativeF - relativeC * relativeC;
    return dispersion.IndexD + (dispersion.IndexD - 1) / dispersion.AbbeNumber * ((relative * relative - 1) / spread);
}
/// Report whether the model is a dielectric whose material record must store zero metallic: hair cards
/// and skin. Decoders and renderers reject such a record with a nonzero metallic fraction.
[[nodiscard]] constexpr bool RequiresZeroMetallic(MaterialModel model) noexcept
{
    return model == MaterialModel::HairCard || model == MaterialModel::Skin;
}
/// The scattering a ray or reference evaluator applies to a surface. Hair cards and skin reflect through
/// the metallic/roughness dielectric lobes there, since the strand highlight and the skin diffusion are
/// raster techniques; every other model is returned unchanged.
[[nodiscard]] constexpr MaterialScattering GetSurfaceReflectionScattering(const MaterialScattering& scattering) noexcept
{
    return scattering.Model == MaterialModel::HairCard || scattering.Model == MaterialModel::Skin ? MaterialScattering{}
                                                                                                  : scattering;
}
/// Number of scalar float32 fields in the explicit MaterialScattering wire schema.
inline constexpr foundation::usize MATERIAL_SCATTERING_FLOATS = MATERIAL_RGB_CHANNELS + MATERIAL_RGB_CHANNELS + 2 + 2;
/// Validate finite bounded optical parameters. Unsupported model tags, including the unresolved
/// marker, reject. A closure program keeps every field at its default, so equal programs cook to
/// equal bytes; its program is validated by IsMaterialProgramValid. A hair card is validated by
/// IsHairCardValid. Skin admits only its scattering distances in K, each in
/// [0, MAX_SKIN_SCATTERING_DISTANCE], with every other field at its default. A dielectric's
/// K holds its declared dispersion (IsDielectricDispersionValid) and zero K[1..2].
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
    if (scattering.Model == MaterialModel::HairCard)
    {
        return IsHairCardValid(scattering);
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
    if (scattering.Model == MaterialModel::Skin)
    {
        const MaterialScattering defaults;
        for (foundation::usize channel = 0; channel < MATERIAL_RGB_CHANNELS; ++channel)
        {
            if (scattering.Eta[channel] != defaults.Eta[channel] ||
                (scattering.K[channel] >= 0 && scattering.K[channel] <= MAX_SKIN_SCATTERING_DISTANCE) == false)
            {
                return false;
            }
        }
        return scattering.AlphaU == defaults.AlphaU && scattering.AlphaV == defaults.AlphaV &&
               scattering.CoatingEta == defaults.CoatingEta && scattering.CoatingThickness == defaults.CoatingThickness;
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
    if (IsDielectricModel(scattering.Model) == true &&
        (scattering.K[1] != 0 || scattering.K[2] != 0 ||
         IsDielectricDispersionValid(GetDielectricDispersion(scattering)) == false))
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
/// Validate a medium interface: model MediumInterface, unit Eta, finite K in
/// [0, MAX_MEDIUM_ABSORPTION_PER_WORLD] and every other field at its default.
[[nodiscard]] constexpr bool IsMediumInterfaceValid(const MaterialScattering& scattering) noexcept
{
    const MaterialScattering defaults;
    if (scattering.Model != MaterialModel::MediumInterface || scattering.AlphaU != defaults.AlphaU ||
        scattering.AlphaV != defaults.AlphaV || scattering.CoatingEta != defaults.CoatingEta ||
        scattering.CoatingThickness != defaults.CoatingThickness)
    {
        return false;
    }
    for (foundation::usize channel = 0; channel < MATERIAL_RGB_CHANNELS; ++channel)
    {
        if (scattering.Eta[channel] != defaults.Eta[channel] ||
            (scattering.K[channel] >= 0 && scattering.K[channel] <= MAX_MEDIUM_ABSORPTION_PER_WORLD) == false)
        {
            return false;
        }
    }
    return true;
}
/// Validate cooked optical storage, including the import-only unresolved marker and medium interfaces,
/// which IsMaterialScatteringValid rejects because neither is a surface.
[[nodiscard]] constexpr bool IsStoredMaterialScatteringValid(MaterialScattering scattering) noexcept
{
    if (scattering.Model == MaterialModel::MediumInterface)
    {
        return IsMediumInterfaceValid(scattering);
    }
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
    Program = LUDUS_MATERIAL_MODEL_PROGRAM,    ///< Closure program at MaterialSample.ProgramOffset.
    HairCard = LUDUS_MATERIAL_MODEL_HAIR_CARD, ///< Raster hair card; ray evaluators receive MetallicRoughness.
    Skin = LUDUS_MATERIAL_MODEL_SKIN,          ///< Ray evaluators receive MetallicRoughness; K is scattering distance.
};
#endif // __cplusplus
