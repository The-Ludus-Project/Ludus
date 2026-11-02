#pragma once
#include "pairwise_distribution.slang"
#include "pairwise_weights.slang"
#include "reservoir.slang"

[noinline] SurfaceHit GetPairwiseReceiver(SceneContext scene, Reservoir source)
{
    SurfaceHit receiver = GetSurfaceHit(scene, source.ReceiverTriangleIndex, source.ReceiverBarycentric);
    receiver.IncomingRay.DirectionWorld = -source.ViewDirectionWorld;
    return receiver;
}
