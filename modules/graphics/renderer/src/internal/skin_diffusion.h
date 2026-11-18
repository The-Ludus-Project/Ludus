#pragma once

#include <ludus/foundation/base/core.h>

#include "../../shaders/skin_diffusion_layout.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>

// Raster skin diffusion kernel (MaterialModel::Skin in the clustered deferred profile). Thanks to Jorge Jimenez,
// Károly Zsolnai, Adrian Jarabo, Christian Freude, Thomas Auzinger, Xian-Chun Wu, Javier von der Pahlen, Michael
// Wimmer and Diego Gutierrez, "Separable Subsurface Scattering", Computer Graphics Forum 34(6), 2015,
// https://iryoku.com/separable-sss/: section 4, equations 4 and 5, for the pre-integrated kernel, the 1D marginal of
// a radially symmetric profile, applied as one horizontal and one vertical convolution; section 6 for the
// screen-space footprint and for sampling all channels at taps placed by the dominant channel's energy. Ludus does
// not yet rotate the filtering axes per pixel (the paper's jittering).
//
// The profile is Brent Burley's normalized diffusion as given by Per H. Christensen, "An Approximate Reflectance
// Profile for Efficient Subsurface Scattering", SIGGRAPH 2015 Talks, section 2,
// https://doi.org/10.1145/2775280.2792555: R(r) = (exp(-r / l) + exp(-r / (3 l))) / (8 pi l r), with its radial CDF 1 -
// exp(-r / l) / 4 - 3 exp(-r / (3 l)) / 4. Ludus takes l as each channel's authored scattering distance and leaves the
// albedo weight to the shaded diffuse radiance, so the profile integrates to one.
namespace ludus::graphics::renderer::internal
{
/// Number of side taps and their offsets/bin widths, from skin_diffusion_layout.h.
inline constexpr foundation::usize SKIN_KERNEL_SIDE_TAPS = LUDUS_SKIN_KERNEL_SIDE_TAPS;
/// Entries of the strip-energy table.
inline constexpr foundation::usize SKIN_ENERGY_ENTRIES = LUDUS_SKIN_ENERGY_ENTRIES;
/// Float4 words of the kernel uniform block.
inline constexpr foundation::usize SKIN_KERNEL_WORDS = LUDUS_SKIN_KERNEL_WORDS;
/// Binary32 components of one kernel word.
inline constexpr foundation::usize SKIN_KERNEL_WORD_COMPONENTS = 4;
static_assert(SKIN_KERNEL_SIDE_TAPS + 1 <= SKIN_KERNEL_WORD_COMPONENTS,
              "the offsets and bin half-widths each fit one float4 word");
static_assert(SKIN_ENERGY_ENTRIES % SKIN_KERNEL_WORD_COMPONENTS == 0, "the energy table fills whole float4 words");

/// Fraction of the profile's energy within the strip |x| <= u d, the two-sided integral of the pre-integrated 1D
/// kernel over [-u d, u d]. Zero for u <= 0 and one in the limit; accurate to about 1e-9 by quadrature.
/// @param halfWidthPerDistance u: the strip's half-width in units of the scattering distance d.
[[nodiscard]] foundation::float64 ComputeSkinStripEnergy(foundation::float64 halfWidthPerDistance) noexcept;
/// Inverse of ComputeSkinStripEnergy by bisection: the half-width, in units of d, holding the energy.
/// @param energy In (0, 1); values outside are clamped to the table's domain.
[[nodiscard]] foundation::float64 FindSkinStripHalfWidth(foundation::float64 energy) noexcept;

/// Kernel uniform block of the diffusion passes, in the layout of skin_diffusion_layout.h: the strip-energy table,
/// the side taps' offsets and the bins' half-widths. It depends only on the profile's shape, so one block serves
/// every skin material and view.
struct SkinDiffusionKernel final
{
    foundation::StaticArray<foundation::math::Vector4, SKIN_KERNEL_WORDS> Words{};
};
/// Build the kernel: bins of equal dominant-channel energy (the centre bin and each pair of side bins), truncated at
/// LUDUS_SKIN_KERNEL_CAPTURED_ENERGY, with each side tap at its bin's energy median.
[[nodiscard]] SkinDiffusionKernel MakeSkinDiffusionKernel() noexcept;
/// The shader's table lookup: ComputeSkinStripEnergy interpolated linearly in log2 of the half-width.
[[nodiscard]] foundation::float32 LookupSkinStripEnergy(const SkinDiffusionKernel& kernel,
                                                        foundation::float32 halfWidthPerDistance) noexcept;
/// The shader's tap weights for one channel: the centre tap's weight, then each side tap's (one side), normalized so
/// the centre plus both sides sum to one.
/// @param distanceRatio The channel's scattering distance over the dominant channel's, in [0, 1]. Zero puts every
/// weight on the centre tap.
[[nodiscard]] foundation::StaticArray<foundation::float32, SKIN_KERNEL_SIDE_TAPS + 1>
ComputeSkinTapWeights(const SkinDiffusionKernel& kernel, foundation::float32 distanceRatio) noexcept;
} // namespace ludus::graphics::renderer::internal
