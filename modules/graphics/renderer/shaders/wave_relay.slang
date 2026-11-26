#pragma once
// GPU edge connections and specular relays; included by wave_fraunhofer_diffraction.slang. See
// EvaluateWaveEdgeChain in wave_edge_diffraction.cpp and the relay functions in wave_connection.cpp for the
// declared fields and limits, and reference_path.cpp for attribution (Steinberg and Pharr 2026, Sections 3, 4.2
// and 5.2). Leg walks, edge fields, relay bookkeeping and coherent accumulation execute here on the GPU.

/// The scalar connection operator of an optical connection; the values of WT_OPTICAL_CONNECTION and
/// ReferenceOpticalConnection.
enum WaveConnectionOperator : uint
{
    AngularSpectrum = 0,
    UtdEdgeDiffraction = 1,
    FraunhoferEdgeDiffraction = 2,
};
/// The shared edge-connection driver relayed through a chain: each leg is the virtual straight connection a paraxial
/// observer in its medium sees (apparent-depth endpoints, wavelength in the medium) over the leg's own regions, and
/// the legs combine by single diffraction. A one-leg chain is the straight edge connection. Failure preserves the
/// output.
[noinline] WaveQuery EvaluateWaveEdgeChain(SceneContext scene,
                                           WaveConnectionChain chain,
                                           float wavelengthWorld,
                                           WaveConnectionOperator connectionOperator,
                                           inout float2 inoutField)
{
    float distanceWorld;
    float endpointEscapeWorld;
    float roundoffWorld;
    WaveQuery status =
        MeasureWaveConnectionChain(chain, wavelengthWorld, distanceWorld, endpointEscapeWorld, roundoffWorld);
    if (status != WaveQuery::Ready)
        return status;
    float wavenumber = 2 * PI / wavelengthWorld;
    float sigmaWorldSquared = distanceWorld / wavenumber;
    // The same Gaussian envelope as the angular-spectrum operator bounds which edges this connection sees.
    float halfWidthWorld = LUDUS_WAVE_CONNECTION_TAIL_DEVIATIONS * sqrt(2 * sigmaWorldSquared);
    float3 axisXWorld = MakeWaveConnectionAxis(chain.DirectionWorld[0]);
    float handedness = 1;
    // Single diffraction along the chain: every leg's ballistic field, plus one leg's diffracted field with the other
    // legs ballistic.
    float ballisticProduct = 1;
    float2 diffracted = float2(0);
    float legStartWorld = 0;
    uint interaction = 0;
    for (uint leg = 0; leg < chain.LegCount; ++leg)
    {
        if (leg > 0 && TryCarryWaveConnectionFrame(chain.DirectionWorld[leg - 1],
                                                   chain.DirectionWorld[leg],
                                                   (chain.ReflectedEntryMask & (1u << leg)) != 0,
                                                   axisXWorld,
                                                   handedness) == false)
            return WaveQuery::Invalid;
        float refractiveIndex = chain.RefractiveIndex[leg];
        float legLengthWorld = chain.LengthWorld[leg];
        float3 beginWorld = chain.BeginWorld[leg];
        float3 directionWorld = chain.DirectionWorld[leg];
        float legStartReducedWorld = legStartWorld;
        legStartWorld += legLengthWorld / refractiveIndex;
        // Inside both endpoint escapes: the caller's own path traced this leg clear, and nothing diffracts.
        if (2 * endpointEscapeWorld >= legLengthWorld)
            continue;
        EllipticalCone cone = (EllipticalCone)0;
        cone.OriginWorld = beginWorld;
        cone.DirectionWorld = directionWorld;
        cone.AxisXWorld = axisXWorld;
        cone.RadiusWorld = halfWidthWorld;
        cone.MinorToMajor = 1;
        cone.NearWorld = endpointEscapeWorld;
        cone.FarWorld = legLengthWorld - endpointEscapeWorld;
        bool isBlocked;
        status = TraceWaveBallistic(scene, cone, uint2(WAVE_CONNECTION_NO_EXCLUDED_TRIANGLE), isBlocked);
        if (status != WaveQuery::Ready)
            return status;
        float legBallistic = isBlocked == true ? 0 : 1;
        // A one-leg connection adds its edge fields to its ballistic field directly.
        float2 legField = float2(chain.LegCount == 1 ? legBallistic : 0, 0);
        // Apparent depth: the leg's medium sees the source and receiver at its index times their reduced distances.
        float legOffsetWorld = refractiveIndex * legStartReducedWorld;
        float remainingWorld = refractiveIndex * max(0.0, distanceWorld - legStartWorld);
        WaveEdgeConnection connection = (WaveEdgeConnection)0;
        connection.SourceWorld = beginWorld - directionWorld * legOffsetWorld;
        connection.ReceiverWorld = chain.EndWorld[leg] + directionWorld * remainingWorld;
        connection.DirectionWorld = directionWorld;
        connection.AxisXWorld = axisXWorld;
        connection.DistanceWorld = refractiveIndex * distanceWorld;
        connection.WavelengthWorld = wavelengthWorld / refractiveIndex;
        connection.SigmaWorldSquared = sigmaWorldSquared;
        connection.EnvelopeRadiusWorld = halfWidthWorld;
        connection.EndpointEscapeWorld = endpointEscapeWorld;
        connection.RoundoffWorld = roundoffWorld;
        connection.IsBallisticBlocked = isBlocked;
        connection.LegBeginWorld = legOffsetWorld;
        connection.LegEndWorld = legOffsetWorld + legLengthWorld;
        // Edges at the endpoints' own surfaces lie inside their escapes and are not diffracted.
        float sourceEscapeWorld;
        bool isEscapeBlocked;
        bool isEntirelyBallistic;
        status = EscapeWaveOrigin(scene,
                                  cone,
                                  wavelengthWorld,
                                  false,
                                  sourceEscapeWorld,
                                  isEscapeBlocked,
                                  isEntirelyBallistic);
        if (status != WaveQuery::Ready)
            return status;
        bool hasRegions = isEntirelyBallistic == false;
        float receiverEscapeWorld = 0;
        if (hasRegions == true)
        {
            EllipticalCone reverse = cone;
            reverse.OriginWorld = chain.EndWorld[leg];
            reverse.DirectionWorld = -directionWorld;
            status = EscapeWaveOrigin(scene,
                                      reverse,
                                      wavelengthWorld,
                                      false,
                                      receiverEscapeWorld,
                                      isEscapeBlocked,
                                      isEntirelyBallistic);
            if (status != WaveQuery::Ready)
                return status;
            hasRegions = isEntirelyBallistic == false;
        }
        if (hasRegions == true)
        {
            cone.NearWorld = sourceEscapeWorld;
            cone.FarWorld = max(cone.NearWorld, legLengthWorld - receiverEscapeWorld);
            bool isLegComplete = false;
            for (; interaction < LUDUS_WAVE_CONNECTION_REGIONS; ++interaction)
            {
                if (cone.NearWorld >= cone.FarWorld)
                {
                    isLegComplete = true;
                    break;
                }
                WaveInteractionRegion region = (WaveInteractionRegion)0;
                status = FindWaveInteractionRegion(scene, cone, region);
                if (status != WaveQuery::Ready)
                    return status;
                if (region.TriangleCount == 0)
                {
                    isLegComplete = true;
                    break;
                }
                // Operators measure depths from the virtual source on the same axis.
                WaveInteractionRegion virtualRegion = region;
                virtualRegion.Frustum.OriginWorld = connection.SourceWorld;
                virtualRegion.Frustum.NearWorld = region.Frustum.NearWorld + legOffsetWorld;
                virtualRegion.Frustum.FarWorld = region.Frustum.FarWorld + legOffsetWorld;
                if (connectionOperator == WaveConnectionOperator::FraunhoferEdgeDiffraction)
                    status = AddFraunhoferRegionFields(scene, connection, virtualRegion, legField);
                else
                    status = AddUtdRegionEdgeFields(scene, connection, virtualRegion, legField);
                if (status != WaveQuery::Ready)
                    return status;
                float nextNearWorld = asfloat(asuint(region.Frustum.FarWorld) + 1);
                cone.NearWorld = min(cone.FarWorld, nextNearWorld);
                if (cone.FarWorld - cone.NearWorld > LUDUS_WAVE_CONNECTION_BALLISTIC_WAVELENGTHS * wavelengthWorld)
                {
                    // Skip geometry that continues straight past the region, as the angular-spectrum operator does.
                    EllipticalCone skip = cone;
                    skip.OriginWorld = beginWorld + directionWorld * cone.NearWorld;
                    skip.FarWorld = cone.FarWorld - cone.NearWorld;
                    skip.NearWorld = min(skip.FarWorld, endpointEscapeWorld);
                    float skippedWorld;
                    status = EscapeWaveOrigin(scene,
                                              skip,
                                              wavelengthWorld,
                                              false,
                                              skippedWorld,
                                              isEscapeBlocked,
                                              isEntirelyBallistic);
                    if (status != WaveQuery::Ready)
                        return status;
                    cone.NearWorld =
                        isEntirelyBallistic == true ? cone.FarWorld : min(cone.FarWorld, cone.NearWorld + skippedWorld);
                }
            }
            if (isLegComplete == false)
                return WaveQuery::PropagationCapacity;
            ++interaction;
        }
        if (chain.LegCount == 1)
        {
            inoutField = legField;
            return WaveQuery::Ready;
        }
        diffracted = diffracted * legBallistic + legField * ballisticProduct;
        ballisticProduct *= legBallistic;
    }
    inoutField = float2(ballisticProduct + diffracted.x, diffracted.y);
    return WaveQuery::Ready;
}
/// Ballistic field plus coherent single-edge UTD fields, relative to the unobstructed free-space field.
/// Accepts and rejects the same inputs as EvaluateWaveConnectionToReceiver; failure preserves the output.
WaveQuery EvaluateUtdConnectionToReceiver(SceneContext scene,
                                          float3 sourceWorld,
                                          float3 receiverWorld,
                                          float3 directionWorld,
                                          float distanceWorld,
                                          float wavelengthWorld,
                                          inout float2 inoutField)
{
    return EvaluateWaveEdgeChain(scene,
                                 MakeStraightWaveConnection(sourceWorld, receiverWorld, directionWorld, distanceWorld),
                                 wavelengthWorld,
                                 WaveConnectionOperator::UtdEdgeDiffraction,
                                 inoutField);
}
/// Ballistic field plus the coherent Fraunhofer boundary-diffraction waves of each region's projected obstacle,
/// relative to the unobstructed Gaussian beam. Accepts and rejects the same inputs as
/// EvaluateWaveConnectionToReceiver; failure preserves the output.
WaveQuery EvaluateFraunhoferConnectionToReceiver(SceneContext scene,
                                                 float3 sourceWorld,
                                                 float3 receiverWorld,
                                                 float3 directionWorld,
                                                 float distanceWorld,
                                                 float wavelengthWorld,
                                                 inout float2 inoutField)
{
    return EvaluateWaveEdgeChain(scene,
                                 MakeStraightWaveConnection(sourceWorld, receiverWorld, directionWorld, distanceWorld),
                                 wavelengthWorld,
                                 WaveConnectionOperator::FraunhoferEdgeDiffraction,
                                 inoutField);
}
/// The connection of `connectionOperator` through the chain's interface events. Failure preserves the output.
WaveQuery EvaluateWaveChain(SceneContext scene,
                            WaveConnectionChain chain,
                            float wavelengthWorld,
                            WaveConnectionOperator connectionOperator,
                            inout float2 inoutField)
{
    if (connectionOperator == WaveConnectionOperator::AngularSpectrum)
        return EvaluateWaveChainConnection(scene, chain, wavelengthWorld, inoutField);
    return EvaluateWaveEdgeChain(scene, chain, wavelengthWorld, connectionOperator, inoutField);
}

/// Camera segments of a specular chain since the last non-specular vertex (or the camera); see
/// ReferenceOpticalRelay in reference_data.h.
struct WaveRelay
{
    /// Start of each camera segment: the receiver's offset ray origin, then each glass event's surface position.
    float3 OriginsWorld[LUDUS_WAVE_CONNECTION_LEGS];
    /// Real index of each segment's medium; zero until an event on that segment infers it.
    float RefractiveIndices[LUDUS_WAVE_CONNECTION_LEGS];
    /// Bit i: the event that ends segment i reflects; otherwise it transmits.
    uint ReflectionMask;
    /// Segments begun: one more than the glass events recorded.
    uint SegmentCount;
    /// Two events disagreed about a segment's medium, or the chain needs more than LUDUS_WAVE_CONNECTION_LEGS legs.
    bool HasConflict;
};
/// Start a chain whose first camera segment begins at the receiver `originWorld`.
WaveRelay BeginWaveRelay(float3 originWorld)
{
    WaveRelay relay = (WaveRelay)0;
    relay.OriginsWorld[0] = originWorld;
    relay.SegmentCount = 1;
    return relay;
}
/// Record a glass event at the end of the current camera segment (AppendWaveRelayEvent in wave_connection.cpp): a
/// solid dielectric of index eta fills the viewer's side when isViewerInside; a reflection stays on that side and a
/// transmission crosses. A thin dielectric has air on both sides.
void AppendWaveRelayEvent(inout WaveRelay inoutRelay,
                          bool isSolid,
                          float eta,
                          bool isViewerInside,
                          bool isReflected,
                          float3 nextOriginWorld)
{
    if (inoutRelay.SegmentCount == 0 || inoutRelay.SegmentCount >= LUDUS_WAVE_CONNECTION_LEGS)
    {
        inoutRelay.HasConflict = true;
        return;
    }
    float incomingIndex = isSolid == true && isViewerInside == true ? eta : 1;
    bool isOutgoingInside = isReflected == true ? isViewerInside : isViewerInside == false;
    float outgoingIndex = isSolid == true && isOutgoingInside == true ? eta : 1;
    uint segment = inoutRelay.SegmentCount - 1;
    if (inoutRelay.RefractiveIndices[segment] != 0 && inoutRelay.RefractiveIndices[segment] != incomingIndex)
        inoutRelay.HasConflict = true;
    inoutRelay.RefractiveIndices[segment] = incomingIndex;
    if (isReflected == true)
        inoutRelay.ReflectionMask |= 1u << segment;
    inoutRelay.OriginsWorld[segment + 1] = nextOriginWorld;
    inoutRelay.RefractiveIndices[segment + 1] = outgoingIndex;
    inoutRelay.SegmentCount = segment + 2;
}
/// The light-ordered chain from `sourceWorld` back through the relay's glass events to its receiver. False for a
/// conflicting relay, one with no glass event or degenerate legs.
bool TryMakeWaveRelayChain(WaveRelay relay, float3 sourceWorld, out WaveConnectionChain outChain)
{
    outChain = (WaveConnectionChain)0;
    if (relay.HasConflict == true || relay.SegmentCount < 2 || relay.SegmentCount > LUDUS_WAVE_CONNECTION_LEGS)
        return false;
    float3 beginWorld = sourceWorld;
    for (uint leg = 0; leg < relay.SegmentCount; ++leg)
    {
        uint segment = relay.SegmentCount - 1 - leg;
        float3 endWorld = relay.OriginsWorld[segment];
        if (TryAppendWaveConnectionLeg(outChain,
                                       beginWorld,
                                       endWorld,
                                       relay.RefractiveIndices[segment],
                                       leg > 0 && (relay.ReflectionMask & (1u << segment)) != 0) == false)
            return false;
        beginWorld = endWorld;
    }
    return true;
}
/// Field of the light's connection from sourceWorld, relayed through the glass events to the receiver, divided by the
/// unobstructed field of the same chain. Ready with outIsAdmitted false rejects the relay's media
/// (LUDUS_SCENE_FAILURE_WAVE_INTERFACE); other failures preserve the field.
WaveQuery EvaluateWaveRelay(SceneContext scene,
                            WaveRelay relay,
                            float3 sourceWorld,
                            float wavelengthWorld,
                            WaveConnectionOperator connectionOperator,
                            out bool outIsAdmitted,
                            inout float2 inoutField)
{
    outIsAdmitted = false;
    if (relay.HasConflict == true || relay.SegmentCount < 2 || relay.SegmentCount > LUDUS_WAVE_CONNECTION_LEGS)
        return WaveQuery::Ready;
    WaveConnectionChain chain;
    if (TryMakeWaveRelayChain(relay, sourceWorld, chain) == false)
        return WaveQuery::Invalid;
    outIsAdmitted = true;
    return EvaluateWaveChain(scene, chain, wavelengthWorld, connectionOperator, inoutField);
}
