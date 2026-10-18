#pragma once
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/graph.h>
// Shared acceptance packet authoring for the installed SDK and native pixel test.
namespace ludus::qa
{
namespace rhi = graphics::rhi;
using namespace foundation;
inline constexpr float32 CULL_CANDIDATES[8]{-.45F, 0, .45F, 0, -2, 0, 2, 0};
inline uint32 CullCpuInstances(float32 (&output)[4]) noexcept
{
    uint32 count = 0;
    for (usize i = 0; i < 4; ++i)
    {
        const auto x = CULL_CANDIDATES[i * 2];
        if (x >= -.75F && x <= .75F && count < 2)
        {
            output[usize{count} * 2] = x;
            output[usize{count} * 2 + 1] = CULL_CANDIDATES[i * 2 + 1];
            ++count;
        }
    }
    return count;
}
struct OrderedFrameInputs final
{
    rhi::BufferHandle Vertices, Background, Instances, Center, Indices, Tint, Dim, Fullscreen;
    rhi::TextureHandle Image, Offscreen, Unused;
    rhi::RasterPipelineHandle Scene, Ui, Composite;
    rhi::BindingSetHandle SceneSet, BackgroundSet, CompositeSet;
    bool Index32 = true;
    const rhi::ComputeDispatch* Compute = nullptr;
    rhi::BufferHandle Candidates{}, Arguments{};
};
inline rhi::RasterStatus BuildOrderedFrame(rhi::DeviceHandle device,
                                           const OrderedFrameInputs& inputs,
                                           rhi::OrderedGraph& graph,
                                           rhi::GraphReport& report,
                                           bool reference) noexcept
{
    auto status = rhi::CreateOrderedGraph(device, graph);
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    const rhi::BufferHandle buffers[]{inputs.Vertices,
                                      inputs.Background,
                                      inputs.Instances,
                                      inputs.Center,
                                      inputs.Indices,
                                      inputs.Tint,
                                      inputs.Dim,
                                      inputs.Fullscreen};
    rhi::GraphVersion versions[13]{};
    for (usize i = 0; i < 8; ++i)
    {
        status = rhi::ImportGraphBuffer(device, graph, buffers[i], versions[i]);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
    }
    const rhi::TextureHandle textures[]{inputs.Image, inputs.Offscreen, inputs.Unused};
    for (usize i = 0; i < 3; ++i)
    {
        rhi::RasterTextureState state;
        status = rhi::GetTextureState(device, textures[i], state);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
        status = rhi::ImportGraphTexture(device,
                                         graph,
                                         textures[i],
                                         state,
                                         rhi::RasterTextureUse::SampledFragment,
                                         versions[8 + i]);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
    }
    if (inputs.Compute != nullptr)
    {
        status = rhi::ImportGraphBuffer(device, graph, inputs.Candidates, versions[11]);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
        status = rhi::ImportGraphBuffer(device, graph, inputs.Arguments, versions[12]);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
        rhi::GraphVersion outputInstances, outputArguments;
        status = rhi::NextGraphVersion(device, graph, versions[2], outputInstances);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
        status = rhi::NextGraphVersion(device, graph, versions[12], outputArguments);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
        const rhi::GraphUse uses[]{
            {versions[11], rhi::GraphAccessMode::StorageRead, 0, 32, rhi::RasterVisibility::Compute},
            {outputInstances, rhi::GraphAccessMode::StorageReadWrite, 0, 16, rhi::RasterVisibility::Compute},
            {outputArguments, rhi::GraphAccessMode::StorageReadWrite, 0, 20, rhi::RasterVisibility::Compute}};
        rhi::GraphPassDescription compute;
        compute.Name = "compute instance culling";
        compute.Source = "ordered_frame.h";
        compute.Dispatch = inputs.Compute;
        compute.Uses = uses;
        compute.UseCount = 3;
        status = rhi::AddGraphPass(device, graph, compute);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
        versions[2] = outputInstances;
        versions[12] = outputArguments;
    }
    rhi::GraphVersion unused, scene, ui;
    status = rhi::NextGraphVersion(device, graph, versions[10], unused);
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    status = rhi::NextGraphVersion(device, graph, versions[9], scene);
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    status = rhi::NextGraphVersion(device, graph, scene, ui);
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    const rhi::GraphUse unusedUse{unused, rhi::GraphAccessMode::ColorWrite};
    rhi::GraphPassDescription pass;
    pass.Name = "unobserved color";
    pass.Source = "ordered_frame.h";
    pass.Attachment.Color = inputs.Unused;
    pass.Uses = &unusedUse;
    pass.UseCount = 1;
    status = rhi::AddGraphPass(device, graph, pass);
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    const usize indexBytes = inputs.Index32 ? 24 : 12;
    rhi::GraphUse sceneUses[]{{versions[0], rhi::GraphAccessMode::Vertex, 0, 80},
                              {versions[1], rhi::GraphAccessMode::Vertex, 0, 80},
                              {versions[2], rhi::GraphAccessMode::Vertex, 0, 16},
                              {versions[3], rhi::GraphAccessMode::Vertex, 0, 8},
                              {versions[4], rhi::GraphAccessMode::Index, 0, indexBytes},
                              {versions[5], rhi::GraphAccessMode::Uniform, 0, 16},
                              {versions[6], rhi::GraphAccessMode::Uniform, 0, 16},
                              {versions[8], rhi::GraphAccessMode::Sampled},
                              {scene, rhi::GraphAccessMode::ColorWrite},
                              {versions[12], rhi::GraphAccessMode::Indirect, 0, 20}};
    const rhi::RasterVertexSlice frontSlices[2]{{inputs.Vertices, 0, 80}, {inputs.Instances, 0, 16}};
    const rhi::RasterVertexSlice backSlices[2]{{inputs.Background, 0, 80}, {inputs.Center, 0, 8}};
    rhi::RasterDraw draws[]{{inputs.Scene, inputs.SceneSet, frontSlices, inputs.Indices, 0, 6, 4, 2},
                            {inputs.Scene, inputs.BackgroundSet, backSlices, inputs.Indices, 0, 6, 4, 1}};
    pass = {};
    pass.Name = "scene";
    pass.Source = "ordered_frame.h";
    pass.Attachment.Color = inputs.Offscreen;
    pass.Attachment.DepthStore = rhi::RasterStore::Store;
    pass.Uses = sceneUses;
    pass.UseCount = inputs.Compute == nullptr ? 9 : 10;
    if (inputs.Compute != nullptr)
    {
        draws[0].Indirect = inputs.Arguments;
    }
    pass.Draws = draws;
    pass.DrawCount = 2;
    status = rhi::AddGraphPass(device, graph, pass);
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    const rhi::GraphUse uiUses[]{{versions[1], rhi::GraphAccessMode::Vertex, 0, 80},
                                 {versions[3], rhi::GraphAccessMode::Vertex, 0, 8},
                                 {versions[4], rhi::GraphAccessMode::Index, 0, indexBytes},
                                 {versions[6], rhi::GraphAccessMode::Uniform, 0, 16},
                                 {versions[8], rhi::GraphAccessMode::Sampled},
                                 {ui, rhi::GraphAccessMode::ColorReadWrite}};
    // A depth-tested overlay behind the scene must preserve its foreground and
    // previously written background. This exercises Load and read-only depth.
    const rhi::RasterDraw uiDraw{inputs.Ui, inputs.BackgroundSet, backSlices, inputs.Indices, 0, 6, 4, 1};
    pass = {};
    pass.Name = "UI with read-only depth";
    pass.Source = "ordered_frame.h";
    pass.Attachment.Color = inputs.Offscreen;
    pass.Attachment.ColorLoad = rhi::RasterLoad::Load;
    pass.Attachment.DepthLoad = rhi::RasterLoad::Load;
    pass.Attachment.DepthReadOnly = true;
    pass.Uses = uiUses;
    pass.UseCount = 6;
    pass.Draws = &uiDraw;
    pass.DrawCount = 1;
    status = rhi::AddGraphPass(device, graph, pass);
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    const rhi::GraphUse compositeUses[]{{versions[7], rhi::GraphAccessMode::Vertex, 0, 80},
                                        {versions[3], rhi::GraphAccessMode::Vertex, 0, 8},
                                        {versions[4], rhi::GraphAccessMode::Index, 0, indexBytes},
                                        {versions[5], rhi::GraphAccessMode::Uniform, 0, 16},
                                        {ui, rhi::GraphAccessMode::Sampled}};
    const rhi::RasterVertexSlice fullSlices[2]{{inputs.Fullscreen, 0, 80}, {inputs.Center, 0, 8}};
    const rhi::RasterDraw composite{inputs.Composite, inputs.CompositeSet, fullSlices, inputs.Indices, 0, 6, 4, 1};
    pass = {};
    pass.Name = "composite to presentation";
    pass.Source = "ordered_frame.h";
    pass.Uses = compositeUses;
    pass.UseCount = 5;
    pass.Draws = &composite;
    pass.DrawCount = 1;
    status = rhi::AddGraphPass(device, graph, pass);
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    status = rhi::CompileOrderedGraph(device, graph, report, reference);
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    const uint32 extra = inputs.Compute == nullptr ? 0 : 1;
    return report.LivePasses == (reference ? 4U + extra : 3U + extra) && report.Culled[extra] == (reference == false)
               ? rhi::RasterStatus::Ready
               : rhi::RasterStatus::Failed;
}
} // namespace ludus::qa
