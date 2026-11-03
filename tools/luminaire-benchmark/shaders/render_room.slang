#include "math.slang"
#include "portable_flow.slang"
#include "raster_bindings.slang"
#include "slice_sampling.slang"
// Headless controlled-room driver. Sampling and physical emission come unchanged from the Ludus graph.
// Triangle vertices/normals are loaded from the generated room OBJ files; all receivers are unit Lambertian.
LUDUS_SHADER_BINDING(0) StructuredBuffer<float4> Triangles;
LUDUS_SHADER_BINDING(1) RWStructuredBuffer<float4> Accumulation;
LUDUS_SHADER_BINDING(2) cbuffer Parameters
{
    uint Width;
    uint Height;
    uint SamplesPerDispatch;
    uint Seed;
    uint RowOffset;
    uint RowCount;
    uint ColumnOffset;
    uint ColumnCount;
};
static const uint ROOM_GROUP_WIDTH = 8;
static const uint ROOM_GROUP_HEIGHT = 8;
static const uint ROOM_RECORDS_PER_TRIANGLE = 4;
static const uint ROOM_ACCUMULATION_RECORDS = 2;
static const uint ROOM_INVALID_TRIANGLE = 0xffffffffu;
static const float ROOM_RAY_EPSILON_WORLD = 0.0001;
static const float ROOM_CAMERA_DISTANCE_WORLD = 3.2;
static const float ROOM_CAMERA_TARGET_HEIGHT_WORLD = -0.1;
static const float ROOM_CAMERA_TANGENT_HALF_FOV = 0.414213562373095;
static const uint ROOM_RANDOM_KEY = 0x524f4f4du;
static const float ROOM_DEFENSIVE_FRACTION = 0.1;

bool TryIntersectRoom(float3 originWorld,
                      float3 directionWorld,
                      uint ignoredTriangle,
                      float farDistanceWorld,
                      out float outDistanceWorld,
                      out float3 outNormalWorld,
                      out uint outTriangle)
{
    uint records;
    uint stride;
    Triangles.GetDimensions(records, stride);
    outDistanceWorld = farDistanceWorld;
    outNormalWorld = float3(0);
    outTriangle = ROOM_INVALID_TRIANGLE;
    [loop] for (uint triangle = 0; triangle < records / ROOM_RECORDS_PER_TRIANGLE; ++triangle)
    {
        if (triangle == ignoredTriangle)
            continue;
        uint base = triangle * ROOM_RECORDS_PER_TRIANGLE;
        float3 firstWorld = Triangles[base].xyz;
        float3 edgeFirstWorld = Triangles[base + 1].xyz - firstWorld;
        float3 edgeSecondWorld = Triangles[base + 2].xyz - firstWorld;
        float3 crossedWorld = cross(directionWorld, edgeSecondWorld);
        float determinantValue = dot(edgeFirstWorld, crossedWorld);
        if (determinantValue == 0)
            continue;
        float inverse = 1 / determinantValue;
        float3 offsetWorld = originWorld - firstWorld;
        float u = dot(offsetWorld, crossedWorld) * inverse;
        float3 secondCrossedWorld = cross(offsetWorld, edgeFirstWorld);
        float v = dot(directionWorld, secondCrossedWorld) * inverse;
        float t = dot(edgeSecondWorld, secondCrossedWorld) * inverse;
        if (u >= 0 && v >= 0 && u + v <= 1 && t > ROOM_RAY_EPSILON_WORLD && t < outDistanceWorld)
        {
            outDistanceWorld = t;
            outNormalWorld = Triangles[base + ROOM_RECORDS_PER_TRIANGLE - 1].xyz;
            outTriangle = triangle;
        }
    }
    return outTriangle != ROOM_INVALID_TRIANGLE;
}

float DrawRoomRandom(uint2 pixel, uint sampleIndex, uint dimension)
{
    return SampleRandomFloat01(uint2(Seed, ROOM_RANDOM_KEY), MakeRandomAddress(pixel, sampleIndex, dimension));
}

void RenderRoomPixel(uint2 pixel, bool isPureLight)
{
    if (pixel.x >= Width || pixel.y >= Height)
        return;
    uint index = (pixel.y * Width + pixel.x) * ROOM_ACCUMULATION_RECORDS;
    float4 stored = Accumulation[index];
    uint sampleCount = asuint(stored.w);
    float3 sum = stored.xyz;
    float3 compensation = Accumulation[index + 1].xyz;
    uint nullCount = asuint(Accumulation[index + 1].w);
    float3 originWorld = float3(0, 0, -ROOM_CAMERA_DISTANCE_WORLD);
    float3 forwardWorld = normalize(float3(0, ROOM_CAMERA_TARGET_HEIGHT_WORLD, ROOM_CAMERA_DISTANCE_WORLD));
    float3 rightWorld = normalize(cross(float3(0, 1, 0), forwardWorld));
    float3 upWorld = cross(forwardWorld, rightWorld);
    float2 screen = (float2(pixel) + 0.5) / float2(Width, Height) * 2 - 1;
    float3 directionWorld =
        normalize(forwardWorld + ROOM_CAMERA_TANGENT_HALF_FOV * (screen.x * rightWorld - screen.y * upWorld));
    float distanceWorld;
    float3 normalWorld;
    uint triangle;
    bool hasHit = TryIntersectRoom(originWorld,
                                   directionWorld,
                                   ROOM_INVALID_TRIANGLE,
                                   ROOM_CAMERA_DISTANCE_WORLD * ROOM_CAMERA_DISTANCE_WORLD,
                                   distanceWorld,
                                   normalWorld,
                                   triangle);
    float3 positionWorld = originWorld + distanceWorld * directionWorld;
    Luminaire luminaire = MakeRoomLuminaire();
    LuminaireReceiver receiver = MakeLuminaireReceiver(luminaire, positionWorld);
    [loop] for (uint offset = 0; offset < SamplesPerDispatch; ++offset)
    {
        float3 value = float3(0);
        if (hasHit == true)
        {
            uint sampleIndex = sampleCount + offset;
            float2 seed = float2(DrawRoomRandom(pixel, sampleIndex, 0), DrawRoomRandom(pixel, sampleIndex, 1));
            LuminaireLightSample light;
            if (isPureLight == true)
                light = SamplePortablePureLight(luminaire, receiver, seed);
            else
                light =
                    SampleLuminaireDefensive(luminaire,
                                             receiver,
                                             ROOM_DEFENSIVE_FRACTION,
                                             DrawRoomRandom(pixel, sampleIndex, 2),
                                             seed,
                                             DrawRoomRandom(pixel, sampleIndex, ROOM_RECORDS_PER_TRIANGLE),
                                             float2(DrawRoomRandom(pixel, sampleIndex, ROOM_RECORDS_PER_TRIANGLE + 1),
                                                    DrawRoomRandom(pixel, sampleIndex, ROOM_RECORDS_PER_TRIANGLE + 2)));
            if (light.HasSample == true)
            {
                float cosine = max(dot(normalWorld, light.DirectionWorld), 0);
                if (cosine > 0)
                {
                    float visibilityDistanceWorld;
                    float3 visibilityNormalWorld;
                    uint visibilityTriangle;
                    float maximumWorld = length(light.PositionWorld - positionWorld) - ROOM_RAY_EPSILON_WORLD;
                    bool isOccluded = TryIntersectRoom(positionWorld + normalWorld * ROOM_RAY_EPSILON_WORLD,
                                                       light.DirectionWorld,
                                                       triangle,
                                                       maximumWorld,
                                                       visibilityDistanceWorld,
                                                       visibilityNormalWorld,
                                                       visibilityTriangle);
                    if (isOccluded == false)
                        value = light.RadianceOverDensity * (cosine / PI);
                }
            }
            else
                ++nullCount;
        }
        float3 corrected = value - compensation;
        float3 updated = sum + corrected;
        compensation = (updated - sum) - corrected;
        sum = updated;
    }
    Accumulation[index] = float4(sum, asfloat(sampleCount + SamplesPerDispatch));
    Accumulation[index + 1] = float4(compensation, asfloat(nullCount));
}

// Keep the conventional kernel layout under the pinned C++ formatter, as in renderer_reference.slang.
#define ROOM_COMPUTE_ENTRY [shader("compute")][numthreads(ROOM_GROUP_WIDTH, ROOM_GROUP_HEIGHT, 1)]

ROOM_COMPUTE_ENTRY
void RenderRoomSlice(uint3 index : SV_DispatchThreadID)
{
    if (index.y < RowCount && index.x < ColumnCount)
        RenderRoomPixel(uint2(index.x + ColumnOffset, index.y + RowOffset), false);
}

ROOM_COMPUTE_ENTRY
void RenderRoomPureLight(uint3 index : SV_DispatchThreadID)
{
    if (index.y < RowCount && index.x < ColumnCount)
        RenderRoomPixel(uint2(index.x + ColumnOffset, index.y + RowOffset), true);
}
