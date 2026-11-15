#pragma once

// Private host/shader schema: parameter words are four binary32 values.
#define LUDUS_ENVIRONMENT_SH_COEFFICIENTS 9
// HairStrand and HairHighlights follow the irradiance coefficients (renderer_lit.slang, hair_card.hpp).
#define LUDUS_HAIR_CARD_PARAMETER_WORDS 2
#define LUDUS_AUTHORED_PARAMETER_WORDS (1 + LUDUS_ENVIRONMENT_SH_COEFFICIENTS + LUDUS_HAIR_CARD_PARAMETER_WORDS)
#define LUDUS_AUTHORED_VERTEX_BYTES 80
#define LUDUS_AUTHORED_TANGENT_OFFSET 40
#define LUDUS_AUTHORED_COLOR_OFFSET 56
#define LUDUS_AUTHORED_UV1_OFFSET 72
