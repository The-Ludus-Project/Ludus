static const float SECOND_FIELD_BASE = 8;
static const float SECOND_LANE_BASE = 4;

float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext inoutQueries)
{
    uint index = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    if (launch.SampleIndex == 0)
    {
        ScenePayload payload = (ScenePayload)0;
        TraceAndShade(scene, inoutQueries, launch.CameraRay, payload);
        float value = float(index + 1);
        ProbeRecord record;
        record.First = float2(value, value + SECOND_LANE_BASE);
        record.Second = record.First + SECOND_FIELD_BASE;
        Written[index] = record;
        return payload.Radiance;
    }
    ProbeRecord neighbor = Written[index ^ 1];
    ProbeRecord reference = Reference[index];
    return float3(neighbor.Second.y, neighbor.First.y, neighbor.Second.x) + reference.First.x;
}
