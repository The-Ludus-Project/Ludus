#pragma once

// Tile component containing the optional sky offset plus one. Zero leaves the ordinary background.
#define LUDUS_REFERENCE_SKY_TILE_COMPONENT 3
// float4 words of the frozen camera background. Shared by the oracle and compute kernel.
#define LUDUS_REFERENCE_SKY_UP_WORLD 0              // radial up and the geometric horizon cosine
#define LUDUS_REFERENCE_SKY_SUN_WORLD 1             // Sun direction and disk chord radius
#define LUDUS_REFERENCE_SKY_SUN_RADIANCE 2          // mean disk radiance and star peak luminance
#define LUDUS_REFERENCE_SKY_MOON_WORLD 3            // Moon direction and disk chord radius
#define LUDUS_REFERENCE_SKY_MOON_RADIANCE 4         // lunar peak radiance and sine of angular radius
#define LUDUS_REFERENCE_SKY_LUNAR_LIGHT_WORLD 5     // lunar lighting direction and planet count
#define LUDUS_REFERENCE_SKY_GALACTIC_NORTH_WORLD 6  // galactic north and frame presence
#define LUDUS_REFERENCE_SKY_GALACTIC_CENTER_WORLD 7 // galactic center and angular pixel footprint
#define LUDUS_REFERENCE_SKY_PLANETS 8               // pairs of directions and RGB illuminance
#define LUDUS_REFERENCE_SKY_PLANET_WORDS 2
#define LUDUS_REFERENCE_SKY_TRANSMITTANCE                                                                              \
    (LUDUS_REFERENCE_SKY_PLANETS + ATMOSPHERE_PLANET_CAPACITY * LUDUS_REFERENCE_SKY_PLANET_WORDS)
#define LUDUS_REFERENCE_SKY_TRANSMITTANCE_SAMPLES 256
#define LUDUS_REFERENCE_SKY_WORDS (LUDUS_REFERENCE_SKY_TRANSMITTANCE + LUDUS_REFERENCE_SKY_TRANSMITTANCE_SAMPLES)
