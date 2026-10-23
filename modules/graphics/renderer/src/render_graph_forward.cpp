#include <ludus/graphics/renderer/render_graph.hpp>

#include "../shaders/renderer_environment_layout.h"
#include <ludus/foundation/containers/static_array.hpp>
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
constexpr usize FLOAT4_BYTES = sizeof(float32) * 4;
constexpr usize PARAMETER_BYTES = FORWARD_SURFACE_OFFSET + FLOAT4_BYTES * (1 + LUDUS_AUTHORED_PARAMETER_WORDS);
const StaticArray<rhi::RasterShaderInput, format::FORWARD_INPUT_COUNT> FORWARD_INPUTS{
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
const StaticArray<rhi::RasterBinding, FORWARD_BINDING_COUNT> FORWARD_BINDINGS{
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
const StaticArray<rhi::RasterUniformMember, FORWARD_MEMBER_COUNT> FORWARD_MEMBERS{
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
const StaticArray<rhi::RasterBinding, OUTPUT_BINDING_COUNT> OUTPUT_BINDINGS{
    {{0, rhi::RasterBindingKind::UniformBuffer, rhi::RasterVisibility::Fragment, FLOAT4_BYTES},
     {1, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
     {2, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0}},
};
const StaticArray<rhi::RasterUniformMember, 1> OUTPUT_MEMBERS{{{0, "Output", 0, rhi::RasterUniformShape::Float4}}};
const StaticArray<rhi::RasterShaderInput, 1> OUTPUT_INPUTS{{{0, rhi::RasterVertexFormat::Float4}}};
} // namespace
void SetRenderGraphForwardInterface(rhi::RasterShaderDescription& inoutDescription, format::StageRole role) noexcept
{
    if (static_cast<uint32>(role) < format::FORWARD_STAGE_ROLE_FIRST ||
        static_cast<uint32>(role) >= format::STAGE_ROLE_COUNT)
    {
        return;
    }
    inoutDescription.Inputs = {};
    inoutDescription.Bindings = {};
    inoutDescription.UniformMembers = {};
    switch (role)
    {
        case format::StageRole::ForwardVertex:
            inoutDescription.Artifact.Stage = rhi::ShaderStage::Vertex;
            inoutDescription.Inputs = FORWARD_INPUTS.AsSpan();
            break;
        case format::StageRole::ForwardFragment:
        case format::StageRole::ForwardShadow:
            inoutDescription.Artifact.Stage = rhi::ShaderStage::Fragment;
            inoutDescription.Bindings = {FORWARD_BINDINGS.GetData(),
                                         role == format::StageRole::ForwardShadow ? OUTPUT_BINDING_COUNT
                                                                                  : FORWARD_BINDING_COUNT};
            inoutDescription.UniformMembers = FORWARD_MEMBERS.AsSpan();
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
