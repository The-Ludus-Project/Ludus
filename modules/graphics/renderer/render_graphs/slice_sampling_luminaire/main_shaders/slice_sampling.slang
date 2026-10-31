#pragma once
#include "luminaire.slang"
// Direct-light proposals for the luminaire of luminaire.slang. Thanks to Shlomi Steinberg, "Slice Sampling: Importance
// sampling with an auxiliary map" (draft, University of Waterloo); section and equation numbers below are the draft's.
//
// Slice sampling (sections 2 to 4, and 6.2). The target is the guide f(z) = xi(z, g(z)) over emitted-direction slopes
// z: the auxiliary map g(z) is the panel point a = s - h z that the receiver sees along slope z, and the slice xi(z, a)
// = c_j N(z; M a + b_j, L_j L_j^T) is the emission lobe of that point's tile, which is easy to sample at a fixed a.
// Its transport is T_a(u) = M a + b_j + L_j eta with eta = Phi^-1(u) coordinatewise (section 2.1). A uniform seed u
// fixes eta; on tile j the seed equation z = T_g(z)(u) is the 2 x 2 linear system of Eq. (20),
//   (I + h M) z = M s + b_j + L_j eta,
// whose solution is a regular root only if its panel point lies on tile j. Enumerating the tiles therefore finds every
// root (section 6.2): there is no iterative solve and no missed branch. Each root has the branch weight of Eq. (9),
//   omega_j = f / J = c_j / |det(I + h M)|,
// because J = N(z) |det(I + h M)| / det(L_j) and f = c_j N(z) / det(L_j) per unit slope area. The seed weight W(u) is
// the sum of the roots' weights (Eq. 9); a root is chosen with probability omega_j / W (section 4.1), and the
// continuous output density per unit slope area is q(z) = f(z) / W(u(z)) (Eq. 13). A seed with no root is a null
// outcome that contributes zero; it is kept, not retried, so q stays an exact subdensity (section 4.1).
//
// The receiver's integrand is in solid angle at the receiver: dw = (1 + |z|^2)^(-3/2) dz for the slope parameterization
// (section 6.2's running example), and no extra inverse-square factor applies (Eq. 19). Uniform seeds leave receivers
// whose seeds are unattainable unlit, such as the far Gaussian tails of section 6.4's third shading point; the
// defensive mixture of Eq. (15) with uniform panel-area sampling restores full support.
//
// Departures from the draft: the guide here equals the radiance up to each tile's RGB tint and the global radiance
// scale, so every root of a seed has the same importance weight (Eq. 13) and the slice estimate is RS tint_j a(z) W.
// The seed u is moved off the endpoints of [0, 1) to the open interval (0, 1) of section 2.1 before Phi^-1.

/// Receivers lower than this, in half extents above the panel plane, receive no light: the slopes that reach them
/// grow without bound and the panel is seen edge on.
static const float LUMINAIRE_MINIMUM_RECEIVER_HEIGHT = 0.001;
/// Normalized Gaussian coordinates beyond this magnitude give a density below float precision; they evaluate to zero
/// without squaring a large value under Metal's fast math.
static const float LUMINAIRE_MAXIMUM_STANDARD_SCORE = 30;
/// Half of the 2^-24 spacing of a [0, 1) random float; adding it centres every draw in its interval, inside (0, 1).
static const float LUMINAIRE_SEED_HALF_SPACING = 0.5 / 16777216.0;
/// 1 / (2 pi): the peak of the two-dimensional standard normal density.
static const float LUMINAIRE_STANDARD_NORMAL_2D_PEAK = 0.5 / PI;

/// A receiver in panel coordinates (luminaire.slang).
struct LuminaireReceiver
{
    float3 PositionWorld;
    float2 PositionPanelTangent; ///< Tangential panel coordinates of the receiver, s in Eq. (20).
    float HeightPanel;           ///< Height above the panel plane in half extents, h in Eq. (20).
    bool IsLit;                  ///< False below LUMINAIRE_MINIMUM_RECEIVER_HEIGHT or behind the panel.
};

LuminaireReceiver MakeLuminaireReceiver(float3 positionWorld)
{
    LuminaireReceiver result;
    float3 offsetPanel = (positionWorld - LUMINAIRE_CENTER_WORLD) / LUMINAIRE_HALF_EXTENT_WORLD;
    result.PositionWorld = positionWorld;
    result.PositionPanelTangent =
        float2(dot(offsetPanel, LUMINAIRE_TANGENT_WORLD), dot(offsetPanel, LUMINAIRE_BITANGENT_WORLD));
    result.HeightPanel = dot(offsetPanel, LUMINAIRE_NORMAL_WORLD);
    result.IsLit = result.HeightPanel > LUMINAIRE_MINIMUM_RECEIVER_HEIGHT;
    return result;
}

/// One proposal: a panel point, the direction towards it and the incident radiance divided by the proposal's density
/// per unit solid angle at the receiver. Multiply RadianceOverDensity by the BSDF, the receiver cosine and visibility.
struct LuminaireLightSample
{
    bool HasSample; ///< False for a null outcome or a receiver the panel cannot light; every other field is then 0.
    uint Tile;
    float2 SlopePanel;           ///< Slope z of the emitted direction.
    float2 PositionPanelTangent; ///< Panel point a = s - h z.
    float3 PositionWorld;        ///< The panel point, the shadow ray's endpoint.
    float3 DirectionWorld;       ///< Unit receiver-to-panel direction.
    float3 RadianceOverDensity;
};

/// |det(I + h M)|, the factor that converts the tile's slice density into the seed map's Jacobian (Eq. 7).
float ComputeSeedJacobianFactor(LuminaireTile tile, float heightPanel)
{
    float2x2 system = float2x2(1, 0, 0, 1) + heightPanel * tile.EmissionToSlope;
    return abs(determinant(system));
}

/// Branch weight omega_j = c_j / |det(I + h M)| of a root on tile j (Eq. 9).
float ComputeBranchWeight(LuminaireTile tile, float heightPanel)
{
    return tile.Strength / ComputeSeedJacobianFactor(tile, heightPanel);
}

/// Solve tile j's seed equation (Eq. 20) for the normalized seed eta. Returns true and the slope of the root when its
/// panel point lies on tile j; otherwise the seed has no root on this tile.
bool TrySolveSeedEquation(LuminaireTile tile,
                          uint tileIndex,
                          LuminaireReceiver receiver,
                          float2 standardSeed,
                          out float2 outSlopePanel)
{
    float2x2 system = float2x2(1, 0, 0, 1) + receiver.HeightPanel * tile.EmissionToSlope;
    float2 rightSide = mul(tile.EmissionToSlope, receiver.PositionPanelTangent) + tile.SlopeOffset +
                       mul(tile.WidthFactor, standardSeed);
    float systemDeterminant = determinant(system);
    // Cramer's rule; the determinant is at least one for the nonnegative spread and twist of luminaire.slang.
    outSlopePanel = float2(rightSide.x * system[1][1] - rightSide.y * system[0][1],
                           rightSide.y * system[0][0] - rightSide.x * system[1][0]) /
                    systemDeterminant;
    return FindLuminaireTile(receiver.PositionPanelTangent - receiver.HeightPanel * outSlopePanel) == tileIndex;
}

/// Seed weight W(eta), the sum of the branch weights of every regular root (Eq. 9). A tile already known to hold a
/// root is counted without solving again, so rounding at a tile edge cannot drop it; pass LUMINAIRE_INVALID_TILE when
/// no root is known.
float ComputeSeedWeight(LuminaireProfile profile, LuminaireReceiver receiver, float2 standardSeed, uint knownRootTile)
{
    float weight = 0;
    for (uint tileIndex = 0; tileIndex < LUMINAIRE_TILE_COUNT; ++tileIndex)
    {
        LuminaireTile tile = GetLuminaireTile(profile, tileIndex);
        float2 slopePanel;
        if (tileIndex == knownRootTile ||
            TrySolveSeedEquation(tile, tileIndex, receiver, standardSeed, slopePanel) == true)
            weight += ComputeBranchWeight(tile, receiver.HeightPanel);
    }
    return weight;
}

/// The normalized seed eta = L_j^-1 (z - M a - b_j) that maps panel point a on tile j to slope z (the inverse
/// transport of section 2.1, before Phi).
float2 ComputeStandardSeed(LuminaireTile tile, float2 positionPanel, float2 slopePanel)
{
    float2 offset = slopePanel - mul(tile.EmissionToSlope, positionPanel) - tile.SlopeOffset;
    float first = offset.x / tile.WidthFactor[0][0];
    return float2(first, (offset.y - tile.WidthFactor[1][0] * first) / tile.WidthFactor[1][1]);
}

/// The tile's Gaussian emission density N(z; M a + b_j, L_j L_j^T) per unit slope area, from the normalized seed.
float EvaluateEmissionDensity(LuminaireTile tile, float2 standardSeed)
{
    if (any(abs(standardSeed) > LUMINAIRE_MAXIMUM_STANDARD_SCORE) == true)
        return 0;
    float widthDeterminant = tile.WidthFactor[0][0] * tile.WidthFactor[1][1];
    return LUMINAIRE_STANDARD_NORMAL_2D_PEAK * exp(-0.5 * dot(standardSeed, standardSeed)) / widthDeterminant;
}

/// Solid angle at the receiver per unit slope area, (1 + |z|^2)^(-3/2).
float ComputeSolidAnglePerSlopeArea(float2 slopePanel)
{
    float squaredSecant = 1 + dot(slopePanel, slopePanel);
    return rsqrt(squaredSecant) / squaredSecant;
}

/// Fill the geometric fields of a proposal at panel point a with slope z on the given tile.
LuminaireLightSample MakeLuminaireLightSample(LuminaireReceiver receiver, uint tile, float2 slopePanel)
{
    LuminaireLightSample result = (LuminaireLightSample)0;
    result.HasSample = true;
    result.Tile = tile;
    result.SlopePanel = slopePanel;
    result.PositionPanelTangent = receiver.PositionPanelTangent - receiver.HeightPanel * slopePanel;
    result.PositionWorld = GetLuminairePositionWorld(result.PositionPanelTangent);
    // y - s = -R h (z_1 tangent + z_2 bitangent + normal).
    result.DirectionWorld = NormalizeOrZero(
        -(slopePanel.x * LUMINAIRE_TANGENT_WORLD + slopePanel.y * LUMINAIRE_BITANGENT_WORLD + LUMINAIRE_NORMAL_WORLD));
    return result;
}

/// Uniform-seed slice sampling: draw the seed, enumerate its roots and choose one with probability omega_j / W.
/// seedSample is a [0, 1)^2 random pair, branchSample a [0, 1) random number. A seed without a root is a null outcome.
/// RadianceOverDensity is L_o a(z) / q(z) = RS tint_j a(z) W (Eq. 13); outSeedWeight is W and outGuideDensity is f(z),
/// both per unit slope area, for the defensive mixture. All three are 0 for a null outcome.
LuminaireLightSample SampleLuminaireSlice(LuminaireProfile profile,
                                          LuminaireReceiver receiver,
                                          float2 seedSample,
                                          float branchSample,
                                          out float outSeedWeight,
                                          out float outGuideDensity)
{
    outSeedWeight = 0;
    outGuideDensity = 0;
    if (receiver.IsLit == false)
        return (LuminaireLightSample)0;
    float2 standardSeed = float2(ComputeStandardNormalQuantile(seedSample.x + LUMINAIRE_SEED_HALF_SPACING),
                                 ComputeStandardNormalQuantile(seedSample.y + LUMINAIRE_SEED_HALF_SPACING));
    float seedWeight = ComputeSeedWeight(profile, receiver, standardSeed, LUMINAIRE_INVALID_TILE);
    if (seedWeight <= 0)
        return (LuminaireLightSample)0;
    // Select the root whose cumulative weight interval contains branchSample * W; the last root absorbs rounding.
    float target = branchSample * seedWeight;
    float cumulative = 0;
    uint selectedTile = LUMINAIRE_INVALID_TILE;
    float2 selectedSlope = float2(0);
    for (uint tileIndex = 0; tileIndex < LUMINAIRE_TILE_COUNT; ++tileIndex)
    {
        LuminaireTile tile = GetLuminaireTile(profile, tileIndex);
        float2 slopePanel;
        if (TrySolveSeedEquation(tile, tileIndex, receiver, standardSeed, slopePanel) == false)
            continue;
        if (selectedTile == LUMINAIRE_INVALID_TILE || cumulative <= target)
        {
            selectedTile = tileIndex;
            selectedSlope = slopePanel;
        }
        cumulative += ComputeBranchWeight(tile, receiver.HeightPanel);
    }
    // The two loops solve identically, so a positive W always has a root; stay null rather than index past the tiles.
    if (selectedTile == LUMINAIRE_INVALID_TILE)
        return (LuminaireLightSample)0;
    LuminaireTile selected = GetLuminaireTile(profile, selectedTile);
    LuminaireLightSample result = MakeLuminaireLightSample(receiver, selectedTile, selectedSlope);
    outSeedWeight = seedWeight;
    outGuideDensity = selected.Strength * EvaluateEmissionDensity(selected, standardSeed);
    result.RadianceOverDensity =
        profile.RadianceScale * selected.Tint * (ComputeSolidAnglePerSlopeArea(selectedSlope) * seedWeight);
    return result;
}

/// Uniform panel-area sampling, the baseline of section 6.4, evaluated in the area measure independently of the
/// slope parameterization: RadianceOverDensity = L_o cos_panel A / distance^2. areaSample is a [0, 1)^2 random pair.
LuminaireLightSample SampleLuminaireArea(LuminaireProfile profile, LuminaireReceiver receiver, float2 areaSample)
{
    if (receiver.IsLit == false)
        return (LuminaireLightSample)0;
    float2 positionPanel = areaSample * LUMINAIRE_PANEL_WIDTH - 1;
    uint tileIndex = FindLuminaireTile(positionPanel);
    if (tileIndex == LUMINAIRE_INVALID_TILE)
        return (LuminaireLightSample)0;
    LuminaireLightSample result = (LuminaireLightSample)0;
    result.HasSample = true;
    result.Tile = tileIndex;
    result.PositionPanelTangent = positionPanel;
    result.PositionWorld = GetLuminairePositionWorld(positionPanel);
    float3 offsetWorld = result.PositionWorld - receiver.PositionWorld;
    float distanceSquaredWorld = dot(offsetWorld, offsetWorld);
    result.DirectionWorld = offsetWorld * rsqrt(distanceSquaredWorld);
    // The emitted direction leaves the panel towards the receiver.
    float3 emittedWorld = -result.DirectionWorld;
    float cosinePanel = dot(emittedWorld, LUMINAIRE_NORMAL_WORLD);
    result.SlopePanel =
        float2(dot(emittedWorld, LUMINAIRE_TANGENT_WORLD), dot(emittedWorld, LUMINAIRE_BITANGENT_WORLD)) / cosinePanel;
    LuminaireTile tile = GetLuminaireTile(profile, tileIndex);
    float density = EvaluateEmissionDensity(tile, ComputeStandardSeed(tile, positionPanel, result.SlopePanel));
    result.RadianceOverDensity = profile.RadianceScale * tile.Strength * tile.Tint *
                                 (density * cosinePanel * GetLuminaireAreaWorld() / distanceSquaredWorld);
    return result;
}

/// Defensive mixture of Eq. (15): with probability defensiveFraction draw uniform panel area, otherwise slice
/// sampling, and divide by the mixture density q_mix = (1 - e) q + e b per unit slope area. b = h^2 / 4 is uniform
/// area density in slope coordinates. A null slice outcome stays null (section 4.2). componentSample chooses the
/// component; the other samples are those of SampleLuminaireSlice and SampleLuminaireArea.
LuminaireLightSample SampleLuminaireDefensive(LuminaireProfile profile,
                                              LuminaireReceiver receiver,
                                              float defensiveFraction,
                                              float componentSample,
                                              float2 seedSample,
                                              float branchSample,
                                              float2 areaSample)
{
    if (receiver.IsLit == false)
        return (LuminaireLightSample)0;
    LuminaireLightSample result;
    float seedWeight;
    float guideDensity;
    if (componentSample < defensiveFraction)
    {
        result = SampleLuminaireArea(profile, receiver, areaSample);
        if (result.HasSample == false)
            return result;
        LuminaireTile tile = GetLuminaireTile(profile, result.Tile);
        float2 standardSeed = ComputeStandardSeed(tile, result.PositionPanelTangent, result.SlopePanel);
        guideDensity = tile.Strength * EvaluateEmissionDensity(tile, standardSeed);
        // The sampled tile is a root of its own seed by construction.
        seedWeight = guideDensity > 0 ? ComputeSeedWeight(profile, receiver, standardSeed, result.Tile) : 1;
    }
    else
    {
        result = SampleLuminaireSlice(profile, receiver, seedSample, branchSample, seedWeight, guideDensity);
        if (result.HasSample == false)
            return result;
    }
    LuminaireTile tile = GetLuminaireTile(profile, result.Tile);
    float uniformAreaDensity = 0.25 * receiver.HeightPanel * receiver.HeightPanel;
    float mixtureDensity = (1 - defensiveFraction) * guideDensity / seedWeight + defensiveFraction * uniformAreaDensity;
    float3 radiance = profile.RadianceScale * tile.Tint * guideDensity;
    result.RadianceOverDensity =
        mixtureDensity > 0 ? radiance * (ComputeSolidAnglePerSlopeArea(result.SlopePanel) / mixtureDensity) : float3(0);
    return result;
}
