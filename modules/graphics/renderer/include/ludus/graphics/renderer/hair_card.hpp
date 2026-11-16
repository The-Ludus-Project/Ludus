#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/vector.hpp>

#include <ludus/content/scene_format/material.h>

/// @file
/// CPU evaluation of the raster hair-card lobe that renderer_lit.slang applies to
/// content::scene_format::MaterialModel::HairCard materials. The shader and these functions implement the same
/// declared approximation, so tests can predict raster captures from them.
///
/// The highlights follow James T. Kajiya and Timothy L. Kay, "Rendering Fur with Three Dimensional Textures",
/// SIGGRAPH '89, pp. 271-280, "The specular component" and Fig. 8: light reflected by a fibre leaves on a cone around
/// its tangent, so the highlight is a power of the sine between the tangent and the half vector. Thanks to the
/// authors. Ludus evaluates that lobe around the strand direction of a flat card, with the two longitudinal shifts of
/// HairCardParameters, a Blinn-Phong-style (p + 2) / (2 pi) normalization and card-side gating by the facing normal;
/// none of these is part of the paper's fibre model. The diffuse term is Lambertian about the facing normal. See
/// docs/architecture/renderer-systems.md, "Raster hair cards".
namespace ludus::graphics::renderer
{
/// Smallest highlight exponent; perceptual roughness one gives a nearly flat sine lobe.
inline constexpr ludus::foundation::float32 MIN_HAIR_CARD_EXPONENT = 1;
/// Largest highlight exponent. It bounds the normalization at the smallest admitted roughness so a single-pixel
/// highlight stays finite in half-precision targets.
inline constexpr ludus::foundation::float32 MAX_HAIR_CARD_EXPONENT = 65536;

/// Hair-card controls in the units the raster shader consumes.
struct HairCardShading final
{
    /// Strand direction in radians from the tangent toward the bitangent.
    ludus::foundation::float32 StrandRadians = 0;
    /// Primary highlight shift in radians toward the facing normal.
    ludus::foundation::float32 PrimaryShiftRadians = 0;
    /// Secondary highlight shift in radians toward the facing normal.
    ludus::foundation::float32 SecondaryShiftRadians = 0;
    /// Untinted primary highlight weight in [0, 1].
    ludus::foundation::float32 PrimaryWeight = 0;
    /// Base-colour-tinted secondary highlight weight in [0, 1].
    ludus::foundation::float32 SecondaryWeight = 0;
};

/// Convert authored degrees to the shader's radians. Validate the input with IsHairCardParametersValid first.
[[nodiscard]] HairCardShading MakeHairCardShading(const content::scene_format::HairCardParameters& parameters) noexcept;

/// Highlight exponent for a perceptual roughness: the Blinn-Phong exponent 2 / alpha^2 - 2 of the GGX width
/// alpha = roughness^2, clamped to [MIN_HAIR_CARD_EXPONENT, MAX_HAIR_CARD_EXPONENT]. Non-finite roughness returns
/// the minimum.
[[nodiscard]] ludus::foundation::float32 ComputeHairCardExponent(ludus::foundation::float32 roughness) noexcept;

/// Tangent frame of a surface without an authored tangent or usable texture coordinates: the tangent is the
/// horizontal direction Cross(+Y, normal), or Cross(normal, +Z) when the normal is within a milliradian of +Y or
/// -Y, so a vertical card facing +Z has the tangent +X and the bitangent +Y.
/// @param normalWorld Unit surface normal.
/// @return A unit tangent perpendicular to normalWorld.
[[nodiscard]] ludus::foundation::math::Vector3
ComputeHairCardFallbackTangent(ludus::foundation::math::Vector3 normalWorld) noexcept;

/// Strand direction in the tangent plane.
/// @param normalWorld Unit surface normal before the card is turned toward the viewer, so both sides share one strand.
/// @param tangentWorld Surface tangent in XYZ, which need not be unit length or perpendicular to normalWorld, and the
/// bitangent sign in W, as in a mesh tangent: the bitangent is Cross(normalWorld, tangent) times -1 for a negative W
/// and +1 otherwise.
/// @param strandRadians Rotation from the tangent toward the bitangent.
/// @return The unit strand direction. A tangent parallel to the normal, including zero, selects
/// ComputeHairCardFallbackTangent with a positive sign.
[[nodiscard]] ludus::foundation::math::Vector3 ComputeHairCardStrand(ludus::foundation::math::Vector3 normalWorld,
                                                                     ludus::foundation::math::Vector4 tangentWorld,
                                                                     ludus::foundation::float32 strandRadians) noexcept;

/// Unit world-space directions at one hair-card shading point.
struct HairCardDirections final
{
    /// Card normal turned toward the viewer.
    ludus::foundation::math::Vector3 FacingNormalWorld{0, 0, 1};
    /// Strand from ComputeHairCardStrand.
    ludus::foundation::math::Vector3 StrandWorld{1, 0, 0};
    /// Direction toward the viewer.
    ludus::foundation::math::Vector3 ViewDirectionWorld{0, 0, 1};
    /// Direction toward the light.
    ludus::foundation::math::Vector3 LightDirectionWorld{0, 0, 1};
};

/// Reflected radiance per unit light radiance, including the cosine, for one direct light.
///
/// The result is cos * (baseColor / pi + PrimaryWeight * S(primary) + SecondaryWeight * baseColor * S(secondary)),
/// where cos is the cosine between the facing normal and the light. The result is zero when the light or the viewer
/// is behind the card.
/// S(shift) = (p + 2) / (2 pi) * sin^p(strand', half vector) for the strand tilted toward the facing normal by shift
/// and p = ComputeHairCardExponent(roughness). Directions near the tangent and a vanishing half vector return finite
/// values; the shader returns the same.
/// @param baseColor Linear reflectance in [0, 1].
/// @param roughness Perceptual roughness in [0.045, 1].
/// @param shading Strand direction, shifts and weights from MakeHairCardShading.
/// @param directions Facing normal, strand, view and light directions.
[[nodiscard]] ludus::foundation::math::Vector3
EvaluateHairCardReflectance(ludus::foundation::math::Vector3 baseColor,
                            ludus::foundation::float32 roughness,
                            const HairCardShading& shading,
                            const HairCardDirections& directions) noexcept;
} // namespace ludus::graphics::renderer
