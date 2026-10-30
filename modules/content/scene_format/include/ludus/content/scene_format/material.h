#pragma once

#ifdef __cplusplus
#    include <ludus/foundation/base/types.h>
#    include <ludus/foundation/containers/static_array.hpp>
namespace ludus::content::scene_format
{
/// Integer representation of material-model wire tags, also consumed by Slang.
using MaterialModelTag = foundation::uint32;
#else
typedef uint MaterialModelTag;
#endif
/// Legacy metallic/roughness scattering wire tag, shared with shader code.
static const MaterialModelTag LUDUS_MATERIAL_MODEL_METALLIC_ROUGHNESS = 0;
/// Lambertian scattering wire tag, shared with shader code.
static const MaterialModelTag LUDUS_MATERIAL_MODEL_DIFFUSE = 1;
/// Complex-IOR conductor scattering wire tag, shared with shader code.
static const MaterialModelTag LUDUS_MATERIAL_MODEL_CONDUCTOR = 2;
/// Preserved coated-diffuse scattering wire tag, shared with shader code.
static const MaterialModelTag LUDUS_MATERIAL_MODEL_COATED_DIFFUSE = 3;

#ifdef __cplusplus
/// Scattering identity retained by scene assets; coating evaluation remains approximate.
enum class MaterialModel : foundation::uint32
{
    MetallicRoughness =
        LUDUS_MATERIAL_MODEL_METALLIC_ROUGHNESS,         ///< Legacy RGB GGX/Schlick and Fresnel-weighted diffuse.
    Diffuse = LUDUS_MATERIAL_MODEL_DIFFUSE,              ///< Ideal Lambertian reflection without a specular lobe.
    Conductor = LUDUS_MATERIAL_MODEL_CONDUCTOR,          ///< Isotropic GGX with complex-IOR Fresnel; no diffuse lobe.
    CoatedDiffuse = LUDUS_MATERIAL_MODEL_COATED_DIFFUSE, ///< Preserved PBRT coating parameters; legacy approximation in
                                                         ///< this milestone.
};
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
/// Source material parameters, shared by tools and runtime without renderer dependencies.
/// AlphaU/V are the resolved GGX distribution widths, not perceptual roughness.
/// The initial reference profile admits isotropic conductors only. Coated diffuse
/// retains the source/default coating parameters without claiming layered transport.
struct MaterialScattering final
{
    MaterialModel Model = MaterialModel::MetallicRoughness; ///< Evaluation model.
    foundation::StaticArray<foundation::float32, MATERIAL_RGB_CHANNELS> Eta{
        {1, 1, 1}}; ///< Positive complex-IOR real part, RGB proxy.
    foundation::StaticArray<foundation::float32, MATERIAL_RGB_CHANNELS>
        K{};                                              ///< Nonnegative complex-IOR imaginary part, RGB proxy.
    foundation::float32 AlphaU = 1;                       ///< GGX alpha along the tangent; [MIN_MATERIAL_ALPHA,1].
    foundation::float32 AlphaV = 1;                       ///< GGX alpha along the bitangent; same domain.
    foundation::float32 CoatingEta = DEFAULT_COATING_ETA; ///< Dielectric coating index of refraction.
    foundation::float32 CoatingThickness = DEFAULT_COATING_THICKNESS; ///< PBRT layer optical thickness.
};
/// Number of scalar float32 fields in the explicit MaterialScattering wire schema.
inline constexpr foundation::usize MATERIAL_SCATTERING_FLOATS = MATERIAL_RGB_CHANNELS + MATERIAL_RGB_CHANNELS + 2 + 2;
/// Validate finite bounded optical parameters. Unsupported model tags reject.
/// This validates storage; an evaluator separately admits the supported anisotropy.
[[nodiscard]] constexpr bool IsMaterialScatteringValid(const MaterialScattering& scattering) noexcept
{
    if (scattering.Model > MaterialModel::CoatedDiffuse ||
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
} // namespace ludus::content::scene_format
#endif // __cplusplus
