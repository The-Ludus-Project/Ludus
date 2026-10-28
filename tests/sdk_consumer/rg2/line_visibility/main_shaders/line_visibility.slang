// Scene compute kernel for FindVisibleLineIntervals (point-line spec PL1). The receiver is the World
// origin and the segment runs from (-1, 2, 0) to (1, 2, 0), so the shadow plane is z = 0 and an
// occluder at height y covers segment parameters (x / y + 1) / 2. Each row runs one query form and
// writes its result as exact fixed-point bytes (no Accumulate or ToneMap):
// - column 0: status, interval count and HasMaskedOccluders, each as a byte;
// - column 1: VisibleLengthWorld / 2 as 24-bit fixed point;
// - columns 2 + 2i and 3 + 2i: interval i's start and end as 24-bit fixed point.
// Rows: 0 the plain query; 1 the occluder named as the receiver's triangle; 2 the occluder named as
// the segment's carrying triangle; 3 a zero-length segment; 4 a second query beyond the budget of
// one; 5 a receiver in the segment's line. Rows 3 to 5 must fail and show the error color.
static const uint ROW_PLAIN = 0;
static const uint ROW_RECEIVER_EXCLUDED = 1;
static const uint ROW_SEGMENT_EXCLUDED = 2;
static const uint ROW_DEGENERATE_SEGMENT = 3;
static const uint ROW_BUDGET_EXHAUSTED = 4;
static const uint ROW_RECEIVER_IN_LINE = 5;
static const uint FIRST_INTERVAL_COLUMN = 2;
static const float FIXED_POINT_SCALE = 16777215.0;
static const float BYTE_SCALE = 255.0;
static const float SEGMENT_LENGTH_WORLD = 2.0;
// The occluder that rows 1 and 2 name lies between the receiver and the segment.
static const float OCCLUDER_MINIMUM_HEIGHT_WORLD = 0.5;
static const float OCCLUDER_MAXIMUM_HEIGHT_WORLD = 1.75;

float3 EncodeFixed24(float value)
{
    uint bits = min(uint(clamp(value, 0.0, 1.0) * FIXED_POINT_SCALE + 0.5), uint(FIXED_POINT_SCALE));
    return float3(float((bits >> 16) & 255), float((bits >> 8) & 255), float(bits & 255)) / BYTE_SCALE;
}

/// First packed triangle wholly between the receiver and the segment, or SCENE_INVALID_INDEX.
uint FindOccluderTriangle(SceneContext scene)
{
    for (uint index = 0; index < GetSceneInfo(scene).TriangleCount; ++index)
    {
        TriangleData triangle = GetTriangle(scene, index);
        float lowestWorld = min(triangle.Position0World.y, min(triangle.Position1World.y, triangle.Position2World.y));
        float highestWorld = max(triangle.Position0World.y, max(triangle.Position1World.y, triangle.Position2World.y));
        if (lowestWorld > OCCLUDER_MINIMUM_HEIGHT_WORLD && highestWorld < OCCLUDER_MAXIMUM_HEIGHT_WORLD)
            return index;
    }
    return SCENE_INVALID_INDEX;
}

float3 ReadLineVisibility(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    uint row = launch.PositionPixel.y;
    if (row > ROW_RECEIVER_IN_LINE)
        return float3(0);
    SurfaceHit receiver = (SurfaceHit)0;
    receiver.TriangleIndex = SCENE_INVALID_INDEX;
    receiver.IsValid = true;
    LineSegment segment;
    segment.StartWorld = float3(-1, 2, 0);
    segment.EndWorld = float3(1, 2, 0);
    segment.TriangleIndex = SCENE_INVALID_INDEX;
    if (row == ROW_RECEIVER_EXCLUDED)
        receiver.TriangleIndex = FindOccluderTriangle(scene);
    else if (row == ROW_SEGMENT_EXCLUDED)
        segment.TriangleIndex = FindOccluderTriangle(scene);
    else if (row == ROW_DEGENERATE_SEGMENT)
        segment.EndWorld = segment.StartWorld;
    else if (row == ROW_RECEIVER_IN_LINE)
        receiver.PositionWorld = float3(-2, 2, 0);
    LineVisibility visibility;
    uint status = FindVisibleLineIntervals(scene, queries, receiver, segment, visibility);
    if (row == ROW_BUDGET_EXHAUSTED)
        status = FindVisibleLineIntervals(scene, queries, receiver, segment, visibility);
    uint column = launch.PositionPixel.x;
    if (column == 0)
        return float3(float(status), float(visibility.IntervalCount), visibility.HasMaskedOccluders == true ? 1 : 0) /
               BYTE_SCALE;
    if (column == 1)
        return EncodeFixed24(visibility.VisibleLengthWorld / SEGMENT_LENGTH_WORLD);
    uint field = column - FIRST_INTERVAL_COLUMN;
    uint interval = field / 2;
    if (interval >= SCENE_LINE_INTERVAL_CAPACITY)
        return float3(0);
    return EncodeFixed24(field % 2 == 0 ? visibility.Intervals[interval].x : visibility.Intervals[interval].y);
}
