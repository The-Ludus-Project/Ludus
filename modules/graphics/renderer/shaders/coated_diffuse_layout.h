#pragma once

// One owner for the bounded non-scattering coating profile and its random layout.
#ifdef __cplusplus
#    include <ludus/foundation/base/types.h>
namespace ludus::graphics::renderer::internal
{
using CoatingWord = foundation::uint32;
using CoatingScalar = foundation::float32;
#else
typedef uint CoatingWord;
typedef float CoatingScalar;
#endif
static const CoatingWord COATING_DIMENSIONS_PER_EVENT = 256;
static const CoatingWord COATING_MAX_EVENT = 255;
static const CoatingWord COATING_KEY_WORD_SHIFT = 32;
static const CoatingWord COATING_REFLECTION = 1;
static const CoatingWord COATING_TRANSMISSION = 2;
static const CoatingWord COATING_ALL = COATING_REFLECTION | COATING_TRANSMISSION;
static const CoatingWord COATING_EVALUATION_DOMAIN = 0x434f4146;
static const CoatingWord COATING_PDF_DOMAIN = 0x434f4150;
static const CoatingWord COATING_SAMPLE_DOMAIN = 0x434f4153;
static const CoatingWord COATING_ROULETTE_START = 3;
static const CoatingScalar COATING_ROULETTE_THRESHOLD = 0.25;
static const CoatingScalar COATING_BASIS_SWITCH_COSINE = 0.99999;
static const CoatingScalar COATING_MIN_NORMAL_Z = 0.000001;
static const CoatingScalar COATING_MIN_NORMAL_COSINE_FOURTH = 1e-16;
static const CoatingScalar COATING_REFLECTION_JACOBIAN = 4;
static const CoatingScalar COATING_PDF_ESTIMATE_WEIGHT = 0.9;
#ifdef __cplusplus
} // namespace ludus::graphics::renderer::internal
#endif
