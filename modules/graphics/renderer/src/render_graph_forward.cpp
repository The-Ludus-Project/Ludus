#include <ludus/graphics/renderer/render_graph.hpp>

#include "../shaders/renderer_environment_layout.h"
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/renderer/deferred_layout.h>
#include <ludus/graphics/renderer/lighting_layout.h>

namespace ludus::graphics::renderer
{
using namespace foundation;
namespace format = content::render_graph_format;
namespace
{
constexpr usize FORWARD_BINDING_COUNT = 10;
constexpr usize FORWARD_MEMBER_COUNT = 14;
constexpr usize OUTPUT_BINDING_COUNT = 3;
constexpr usize CLUSTERED_BINDING_COUNT = FORWARD_BINDING_COUNT + 1;
constexpr usize CLUSTERED_MEMBER_COUNT = FORWARD_MEMBER_COUNT + CLUSTERED_GRID_ROWS;
constexpr usize FLOAT4_BYTES = sizeof(float32) * 4;
constexpr usize PARAMETER_BYTES = FORWARD_SURFACE_OFFSET + FLOAT4_BYTES * (1 + LUDUS_AUTHORED_PARAMETER_WORDS);
constexpr StaticArray<rhi::RasterShaderInput, format::FORWARD_INPUT_COUNT> FORWARD_INPUTS{
    {{0, rhi::RasterVertexFormat::Float3},
     {1, rhi::RasterVertexFormat::Float4},
     {2, rhi::RasterVertexFormat::Float4},
     {3, rhi::RasterVertexFormat::Float4},
     {4, rhi::RasterVertexFormat::Float4},
     {5, rhi::RasterVertexFormat::Float4},
     {6, rhi::RasterVertexFormat::Float2},
     {7, rhi::RasterVertexFormat::Float3},
     {8, rhi::RasterVertexFormat::Float4},
     {9, rhi::RasterVertexFormat::Float4},
     {10, rhi::RasterVertexFormat::Float4},
     {11, rhi::RasterVertexFormat::Float4},
     {12, rhi::RasterVertexFormat::Float4},
     {13, rhi::RasterVertexFormat::Float2},
     {14, rhi::RasterVertexFormat::Float4},
     {15, rhi::RasterVertexFormat::Float2}},
};
constexpr StaticArray<rhi::RasterBinding, FORWARD_BINDING_COUNT> FORWARD_BINDINGS{
    {{0, rhi::RasterBindingKind::UniformBuffer, rhi::RasterVisibility::Fragment, PARAMETER_BYTES},
     {1, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
     {2, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0},
     {3, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
     {4, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
     {5, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0},
     {6, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
     {7, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
     {8, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0},
     {9, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0}},
};
constexpr StaticArray<rhi::RasterUniformMember, FORWARD_MEMBER_COUNT> FORWARD_MEMBERS{
    {{0, "Tint", 0, rhi::RasterUniformShape::Float4},
     {0, "UvTransform", FLOAT4_BYTES, rhi::RasterUniformShape::Float4},
     {0, "Options", 2 * FLOAT4_BYTES, rhi::RasterUniformShape::Float4},
     {0, "Pbr", 3 * FLOAT4_BYTES, rhi::RasterUniformShape::Float4},
     {0, "Emission", 4 * FLOAT4_BYTES, rhi::RasterUniformShape::Float4},
     {0, "EnvironmentIrradiance", 5 * FLOAT4_BYTES, rhi::RasterUniformShape::Float4},
     {0, "EnvironmentRadiance", 6 * FLOAT4_BYTES, rhi::RasterUniformShape::Float4},
     {0, "BakeScale", 7 * FLOAT4_BYTES, rhi::RasterUniformShape::Float4},
     {0, "BakeUv", 8 * FLOAT4_BYTES, rhi::RasterUniformShape::Float4},
     {0, "Lights", FORWARD_HEADER_WORDS * FLOAT4_BYTES, rhi::RasterUniformShape::Float4Array},
     {0, "ShadowData", FORWARD_SHADOW_OFFSET, rhi::RasterUniformShape::Float4Array},
     {0, "SurfaceMap", FORWARD_SURFACE_OFFSET, rhi::RasterUniformShape::Float4},
     {0, "Authored", FORWARD_SURFACE_OFFSET + FLOAT4_BYTES, rhi::RasterUniformShape::Float4},
     {0, "IrradianceSh", FORWARD_SURFACE_OFFSET + 2 * FLOAT4_BYTES, rhi::RasterUniformShape::Float4Array}},
};
constexpr StaticArray<rhi::RasterBinding, CLUSTERED_BINDING_COUNT> CLUSTERED_BINDINGS = []() noexcept {
    StaticArray<rhi::RasterBinding, CLUSTERED_BINDING_COUNT> bindings;
    for (usize i = 0; i < FORWARD_BINDING_COUNT; ++i)
    {
        bindings[i] = FORWARD_BINDINGS[i];
    }
    bindings[0].MinSize = CLUSTERED_SURFACE_OFFSET + FLOAT4_BYTES * (1 + LUDUS_AUTHORED_PARAMETER_WORDS);
    constexpr usize ENVIRONMENT_SLOT = 8;
    constexpr usize ENVIRONMENT_SAMPLER_SLOT = 9;
    constexpr usize EMISSION_SLOT = 10;
    bindings[ENVIRONMENT_SLOT].Kind = rhi::RasterBindingKind::Texture2D;
    bindings[ENVIRONMENT_SAMPLER_SLOT].Kind = rhi::RasterBindingKind::Sampler;
    bindings[EMISSION_SLOT] = {EMISSION_SLOT, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0};
    return bindings;
}();
constexpr StaticArray<rhi::RasterUniformMember, CLUSTERED_MEMBER_COUNT> CLUSTERED_MEMBERS = []() noexcept {
    StaticArray<rhi::RasterUniformMember, CLUSTERED_MEMBER_COUNT> members{};
    for (usize i = 0; i < FORWARD_MEMBER_COUNT; ++i)
    {
        members[i] = FORWARD_MEMBERS[i];
        if (members[i].Offset >= FORWARD_SURFACE_OFFSET)
        {
            members[i].Offset += CLUSTERED_SURFACE_OFFSET - FORWARD_SURFACE_OFFSET;
        }
        else if (members[i].Offset == FORWARD_SHADOW_OFFSET)
        {
            members[i].Offset = CLUSTERED_SHADOW_OFFSET;
        }
    }
    members[FORWARD_MEMBER_COUNT] = {0, "GridRows", CLUSTERED_GRID_OFFSET, rhi::RasterUniformShape::Float4Array};
    members[FORWARD_MEMBER_COUNT + 1] = {0,
                                         "GridMinimum",
                                         CLUSTERED_GRID_OFFSET + CLUSTERED_GRID_ROWS * FLOAT4_BYTES,
                                         rhi::RasterUniformShape::Float4};
    members[FORWARD_MEMBER_COUNT + 2] = {0,
                                         "GridScale",
                                         CLUSTERED_GRID_OFFSET + (CLUSTERED_GRID_ROWS + 1) * FLOAT4_BYTES,
                                         rhi::RasterUniformShape::Float4};
    return members;
}();
constexpr StaticArray<rhi::RasterBinding, OUTPUT_BINDING_COUNT> OUTPUT_BINDINGS{
    {{0, rhi::RasterBindingKind::UniformBuffer, rhi::RasterVisibility::Fragment, FLOAT4_BYTES},
     {1, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
     {2, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0}},
};
constexpr StaticArray<rhi::RasterUniformMember, 1> OUTPUT_MEMBERS{{{0, "Output", 0, rhi::RasterUniformShape::Float4}}};
constexpr StaticArray<rhi::RasterShaderInput, 1> OUTPUT_INPUTS{{{0, rhi::RasterVertexFormat::Float4}}};
constexpr usize DEFERRED_MEMBER_COUNT = CLUSTERED_MEMBER_COUNT + DEFERRED_VIEW_ROWS;
constexpr StaticArray<rhi::RasterBinding, DEFERRED_BINDING_COUNT> DEFERRED_BINDINGS = []() noexcept {
    StaticArray<rhi::RasterBinding, DEFERRED_BINDING_COUNT> bindings{};
    for (usize index = 0; index < CLUSTERED_BINDING_COUNT; ++index)
    {
        bindings[index] = CLUSTERED_BINDINGS[index];
    }
    for (uint32 slot = DEFERRED_NORMAL_BINDING; slot < DEFERRED_VIEW_BINDING; ++slot)
    {
        bindings[slot] = {slot, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0};
    }
    bindings[DEFERRED_DEPTH_BINDING].Kind = rhi::RasterBindingKind::TextureDepth2D;
    bindings[DEFERRED_VIEW_BINDING] = {DEFERRED_VIEW_BINDING,
                                       rhi::RasterBindingKind::UniformBuffer,
                                       rhi::RasterVisibility::Fragment,
                                       DEFERRED_VIEW_WORDS * FLOAT4_BYTES};
    return bindings;
}();
constexpr StaticArray<rhi::RasterUniformMember, DEFERRED_MEMBER_COUNT> DEFERRED_MEMBERS = []() noexcept {
    StaticArray<rhi::RasterUniformMember, DEFERRED_MEMBER_COUNT> members{};
    for (usize index = 0; index < CLUSTERED_MEMBER_COUNT; ++index)
    {
        members[index] = CLUSTERED_MEMBERS[index];
    }
    members[CLUSTERED_MEMBER_COUNT] = {DEFERRED_VIEW_BINDING,
                                       "ClipToWorldRows",
                                       0,
                                       rhi::RasterUniformShape::Float4Array};
    members[CLUSTERED_MEMBER_COUNT + 1] = {DEFERRED_VIEW_BINDING,
                                           "EyeWorld",
                                           DEFERRED_VIEW_ROWS * FLOAT4_BYTES,
                                           rhi::RasterUniformShape::Float4};
    members[CLUSTERED_MEMBER_COUNT + 2] = {DEFERRED_VIEW_BINDING,
                                           "TargetSizePixel",
                                           (DEFERRED_VIEW_ROWS + 1) * FLOAT4_BYTES,
                                           rhi::RasterUniformShape::Float4};
    members[DEFERRED_MEMBER_COUNT - 1] = {DEFERRED_VIEW_BINDING,
                                          "Options",
                                          (DEFERRED_VIEW_ROWS + 2) * FLOAT4_BYTES,
                                          rhi::RasterUniformShape::Float4};
    return members;
}();
constexpr StaticArray<rhi::RasterBinding, 1> DEPTH_COPY_BINDINGS{
    {{0, rhi::RasterBindingKind::TextureDepth2D, rhi::RasterVisibility::Fragment, 0}}};
} // namespace
void SetRenderGraphForwardInterface(rhi::RasterShaderDescription& inoutDescription,
                                    format::StageRole role,
                                    bool isClustered) noexcept
{
    if (static_cast<uint32>(role) >= format::STAGE_ROLE_COUNT ||
        static_cast<uint32>(role) < format::FORWARD_STAGE_ROLE_FIRST ||
        (static_cast<uint32>(role) >= format::NATIVE_RAY_STAGE_ROLE && role != format::StageRole::ForwardDepth &&
         static_cast<uint32>(role) < format::DEFERRED_STAGE_ROLE_FIRST))
    {
        return;
    }
    inoutDescription.Inputs = {};
    inoutDescription.Bindings = {};
    inoutDescription.UniformMembers = {};
    switch (role)
    {
        case format::StageRole::DeferredGBufferFragment:
            inoutDescription.Artifact.Stage = rhi::ShaderStage::Fragment;
            inoutDescription.Bindings = CLUSTERED_BINDINGS.AsSpan();
            inoutDescription.UniformMembers = CLUSTERED_MEMBERS.AsSpan();
            break;
        case format::StageRole::DeferredLightingVertex:
            inoutDescription.Artifact.Stage = rhi::ShaderStage::Vertex;
            inoutDescription.Inputs = OUTPUT_INPUTS.AsSpan();
            break;
        case format::StageRole::DeferredLightingFragment:
            inoutDescription.Artifact.Stage = rhi::ShaderStage::Fragment;
            inoutDescription.Bindings = DEFERRED_BINDINGS.AsSpan();
            inoutDescription.UniformMembers = DEFERRED_MEMBERS.AsSpan();
            break;
        case format::StageRole::DeferredDepthCopyFragment:
            inoutDescription.Artifact.Stage = rhi::ShaderStage::Fragment;
            inoutDescription.Bindings = DEPTH_COPY_BINDINGS.AsSpan();
            break;
        case format::StageRole::ForwardDepth:
            inoutDescription.Artifact.Stage = rhi::ShaderStage::Fragment;
            break;
        case format::StageRole::ForwardVertex:
            inoutDescription.Artifact.Stage = rhi::ShaderStage::Vertex;
            inoutDescription.Inputs = FORWARD_INPUTS.AsSpan();
            break;
        case format::StageRole::ForwardFragment:
        case format::StageRole::ForwardShadow:
            inoutDescription.Artifact.Stage = rhi::ShaderStage::Fragment;
            inoutDescription.Bindings = {isClustered == true ? CLUSTERED_BINDINGS.GetData()
                                                             : FORWARD_BINDINGS.GetData(),
                                         role == format::StageRole::ForwardShadow ? OUTPUT_BINDING_COUNT
                                         : isClustered == true                    ? CLUSTERED_BINDING_COUNT
                                                                                  : FORWARD_BINDING_COUNT};
            inoutDescription.UniformMembers =
                isClustered == true ? CLUSTERED_MEMBERS.AsSpan() : FORWARD_MEMBERS.AsSpan();
            break;
        case format::StageRole::LightingVertex:
            inoutDescription.Artifact.Stage = rhi::ShaderStage::Vertex;
            inoutDescription.Inputs = OUTPUT_INPUTS.AsSpan();
            break;
        case format::StageRole::LightingFragment:
            inoutDescription.Artifact.Stage = rhi::ShaderStage::Fragment;
            inoutDescription.Bindings = OUTPUT_BINDINGS.AsSpan();
            inoutDescription.UniformMembers = OUTPUT_MEMBERS.AsSpan();
            break;
        case format::StageRole::LightingShadow:
            inoutDescription.Artifact.Stage = rhi::ShaderStage::Fragment;
            break;
        default:
            break;
    }
}
} // namespace ludus::graphics::renderer
