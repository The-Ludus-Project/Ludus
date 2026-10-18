// Independent std140 layout: view/settings at 0/16, then three arrays of
// 192 float4s at 32/3104/6176. Array stride 16; total occupied size 9248.
// 192 records hold every entity, collision box and scatter placement of a room
// (apps/world_demo/internal/world.h MAX_RENDER_DRAWS).
struct WorldUniforms
{
    float4 View;
    float4 Settings;
    float4 Geometry[192];
    float4 Color[192];
    float4 Rotation[192];
};
[[vk::binding(0, 0)]] ConstantBuffer<WorldUniforms> world;
[shader("vertex")] float4 VertexMain(uint id : SV_VulkanVertexID) : SV_Position
{
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * 2.0 - 1.0, 0.0, 1.0);
}
[shader("fragment")] float4 FragmentMain(float4 position : SV_Position) : SV_Target0
{
#if LUDUS_GLSL_ES
    position.y = world.View.y - position.y;
#endif
    float2 uv = position.xy / world.View.xy;
    float2 point =
        world.View.zw + (uv - 0.5) * float2(world.Settings.x * world.View.x / world.View.y, -world.Settings.x);
    float3 color = float3(0.025, 0.04, 0.08);
    for (uint index = 0; index < min(uint(world.Settings.y), 192u); ++index)
    {
        float4 geometry = world.Geometry[index];
        float2 delta = point - geometry.xy;
        float angle = world.Rotation[index].x;
        float c = cos(angle);
        float s = sin(angle);
        float2 local = float2(c * delta.x + s * delta.y, -s * delta.x + c * delta.y);
        if (all(abs(local) < geometry.zw))
        {
            color = lerp(color, world.Color[index].rgb, world.Color[index].a);
        }
    }
    return float4(color, 1.0);
}
