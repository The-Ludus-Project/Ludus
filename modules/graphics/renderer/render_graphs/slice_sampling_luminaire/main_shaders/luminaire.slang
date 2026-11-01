#pragma once
// The directional luminaire that lights this template's scene: an analytic, position-dependent LED panel after
// Shlomi Steinberg, "Slice Sampling: Importance sampling with an auxiliary map" (draft, University of Waterloo),
// section 6.2, "An analytic implementation without iterative root finding", and the controlled experiment of section
// 6.4. Thanks to the author: the panel, its affine Gaussian tiles and the seed equation solved in slice_sampling.slang
// follow those sections.
//
// The panel is a square exit surface outside the scene geometry. It is not visible to camera rays and it does not
// occlude; scene geometry between a receiver and the panel does (shade.slang traces a shadow ray). Scene lights,
// mesh emitters and the environment do not light surfaces in this template. Edit the placement constants below to put
// the panel into another scene: they are part of this project-owned copy. The emission profile is set by the Graph
// settings (main.lushader.json): LUMINAIRE_RADIANCE_SCALE, LUMINAIRE_BEAM_WIDTH, LUMINAIRE_BEAM_STEERING,
// LUMINAIRE_BEAM_SPREAD and LUMINAIRE_BEAM_TWIST.
//
// Panel coordinates: a receiver or panel point is measured from LUMINAIRE_CENTER_WORLD in units of
// LUMINAIRE_HALF_EXTENT_WORLD along the right-handed frame (tangent, bitangent, normal), so the panel is the square
// [-1, 1)^2 at height 0 and it emits towards positive height. An emitted direction v is represented by its slope
// z = (v . tangent, v . bitangent) / (v . normal), the "two-dimensional slope" of the paper's section 6.2.
//
// The panel has LUMINAIRE_TILE_COUNT equal square tiles, LUMINAIRE_TILES_PER_AXIS per axis; tile i + 3 j covers
// column i along the tangent and row j along the bitangent. On tile j the radiance leaving panel point a in slope z is
//   L_o(a, z) = LUMINAIRE_RADIANCE_SCALE * strength_j * tint_j * N(z; M a + b_j, L_j L_j^T),
// a Gaussian density per unit slope area (section 6.4 sets the physical radiance to the same Gaussian), with
//   M = [[spread, -twist], [twist, spread]]       (LUMINAIRE_BEAM_SPREAD and LUMINAIRE_BEAM_TWIST, per half extent)
//   b_j = LUMINAIRE_BEAM_STEERING * steering_j     (a pinwheel of unit-scale directions)
//   L_j = LUMINAIRE_BEAM_WIDTH * width_j           (lower triangular with a positive diagonal)
// The per-tile tables below give the tiles different steering, widths between 0.8 and 1.2 times the beam width and
// strengths, in the spirit of section 6.4's nine tiles; the default widths span 0.016 to 0.024 as there. Nonnegative
// spread and twist keep det(I + h M) = (1 + h spread)^2 + (h twist)^2 >= 1 at every receiver height h > 0, so a
// tile's beams never focus into a caustic.

// Placement: the default fits the 2 x 2 x 2 Cornell box of the renderer's tests, just below its ceiling light. The
// editor reads the center, half extent and normal to warn when the panel cannot light the viewport's World; keep them
// plain literals so it can.
static const float3 LUMINAIRE_CENTER_WORLD = float3(0, 0.97, 0);
static const float LUMINAIRE_HALF_EXTENT_WORLD = 0.35;
// A right-handed orthonormal frame: LUMINAIRE_NORMAL_WORLD = cross(tangent, bitangent) points into the lit room.
static const float3 LUMINAIRE_TANGENT_WORLD = float3(1, 0, 0);
static const float3 LUMINAIRE_BITANGENT_WORLD = float3(0, 0, 1);
static const float3 LUMINAIRE_NORMAL_WORLD = float3(0, -1, 0);

static const uint LUMINAIRE_TILES_PER_AXIS = 3;
static const uint LUMINAIRE_TILE_COUNT = LUMINAIRE_TILES_PER_AXIS * LUMINAIRE_TILES_PER_AXIS;
static const uint LUMINAIRE_INVALID_TILE = LUMINAIRE_TILE_COUNT;
// The panel spans [-1, 1) along each axis in panel coordinates.
static const float LUMINAIRE_PANEL_WIDTH = 2;

// Unit-scale beam steering directions, a pinwheel around the panel center: tile (i, j) of the outer ring steers
// roughly towards (-j, i), and the center tile steers off axis.
static const float2 LUMINAIRE_TILE_STEERING[LUMINAIRE_TILE_COUNT] = {
    float2(0.70, -0.75),
    float2(0.95, 0.10),
    float2(0.75, 0.70),
    float2(0.05, -1.00),
    float2(0.30, -0.20),
    float2(-0.10, 0.95),
    float2(-0.70, -0.70),
    float2(-1.00, 0.05),
    float2(-0.75, 0.65),
};
// Lower-triangular width factors (L11, L21, L22), multiplied by LUMINAIRE_BEAM_WIDTH; L21 correlates the two slopes.
static const float3 LUMINAIRE_TILE_WIDTH[LUMINAIRE_TILE_COUNT] = {
    float3(1.00, 0.10, 0.90),
    float3(0.85, -0.20, 1.10),
    float3(1.15, 0.00, 0.95),
    float3(0.90, 0.25, 1.05),
    float3(1.20, -0.10, 0.80),
    float3(0.95, 0.15, 1.15),
    float3(1.10, -0.25, 0.85),
    float3(0.80, 0.05, 1.00),
    float3(1.05, 0.20, 1.20),
};
// Scalar emission strength c_j, which also guides slice sampling (slice_sampling.slang).
static const float LUMINAIRE_TILE_STRENGTH[LUMINAIRE_TILE_COUNT] = {
    0.90,
    1.20,
    0.80,
    1.10,
    1.00,
    1.30,
    0.70,
    1.05,
    0.95,
};
// Linear RGB tint of each tile; it does not guide sampling. Warm, neutral and cool tiles make each tile's footprint,
// and so each inverse branch of the seed map, visible on the receivers.
static const float3 LUMINAIRE_TILE_TINT[LUMINAIRE_TILE_COUNT] = {
    float3(1.00, 0.75, 0.50),
    float3(0.90, 0.90, 0.90),
    float3(0.55, 0.75, 1.00),
    float3(0.55, 0.75, 1.00),
    float3(1.00, 0.75, 0.50),
    float3(0.90, 0.90, 0.90),
    float3(0.90, 0.90, 0.90),
    float3(0.55, 0.75, 1.00),
    float3(1.00, 0.75, 0.50),
};

/// The emission profile's Graph settings. MakeLuminaireProfile in shade.slang fills it from main.lushader.json; test
/// kernels fill it directly.
struct LuminaireProfile
{
    float RadianceScale; ///< Multiplies every tile's radiance; it does not change the sampling densities.
    float BeamWidth;     ///< Gaussian width scale of every tile in slope units; must be positive.
    float BeamSteering;  ///< Slope offset of each tile's beams along its steering direction.
    float BeamSpread;    ///< Slope change per half extent of panel position; nonnegative.
    float BeamTwist;     ///< Rotational slope change per half extent of panel position; nonnegative.
};

/// One tile's affine Gaussian emission: mean slope EmissionToSlope * a + SlopeOffset, Cholesky factor WidthFactor.
struct LuminaireTile
{
    float2x2 EmissionToSlope; ///< M: slope change per panel position.
    float2 SlopeOffset;       ///< b_j.
    float2x2 WidthFactor;     ///< L_j, lower triangular with a positive diagonal.
    float Strength;           ///< c_j, the slice-sampling guide's scalar strength.
    float3 Tint;
    float2 MinimumPanel; ///< Inclusive lower corner of the tile in panel coordinates.
    float2 MaximumPanel; ///< Exclusive upper corner.
};

LuminaireTile GetLuminaireTile(LuminaireProfile profile, uint tile)
{
    LuminaireTile result;
    result.EmissionToSlope = float2x2(profile.BeamSpread, -profile.BeamTwist, profile.BeamTwist, profile.BeamSpread);
    result.SlopeOffset = profile.BeamSteering * LUMINAIRE_TILE_STEERING[tile];
    float3 width = profile.BeamWidth * LUMINAIRE_TILE_WIDTH[tile];
    result.WidthFactor = float2x2(width.x, 0, width.y, width.z);
    result.Strength = LUMINAIRE_TILE_STRENGTH[tile];
    result.Tint = LUMINAIRE_TILE_TINT[tile];
    float tileWidth = LUMINAIRE_PANEL_WIDTH / float(LUMINAIRE_TILES_PER_AXIS);
    float2 cell = float2(float(tile % LUMINAIRE_TILES_PER_AXIS), float(tile / LUMINAIRE_TILES_PER_AXIS));
    result.MinimumPanel = -1 + cell * tileWidth;
    result.MaximumPanel = result.MinimumPanel + tileWidth;
    return result;
}

/// The tile that owns a panel point, or LUMINAIRE_INVALID_TILE off the panel. Tiles are half open, so a point on a
/// shared edge belongs to the tile with the larger coordinate, as the paper assigns boundary values (section 2).
uint FindLuminaireTile(float2 positionPanel)
{
    float2 cell = floor((positionPanel + 1) * (float(LUMINAIRE_TILES_PER_AXIS) / LUMINAIRE_PANEL_WIDTH));
    if (any(cell < 0) == true || any(cell >= float(LUMINAIRE_TILES_PER_AXIS)) == true)
        return LUMINAIRE_INVALID_TILE;
    return uint(cell.x) + uint(cell.y) * LUMINAIRE_TILES_PER_AXIS;
}

/// World position of a panel point.
float3 GetLuminairePositionWorld(float2 positionPanel)
{
    return LUMINAIRE_CENTER_WORLD + LUMINAIRE_HALF_EXTENT_WORLD * (positionPanel.x * LUMINAIRE_TANGENT_WORLD +
                                                                   positionPanel.y * LUMINAIRE_BITANGENT_WORLD);
}

/// Area of the whole panel in square world units.
float GetLuminaireAreaWorld()
{
    float widthWorld = LUMINAIRE_PANEL_WIDTH * LUMINAIRE_HALF_EXTENT_WORLD;
    return widthWorld * widthWorld;
}
