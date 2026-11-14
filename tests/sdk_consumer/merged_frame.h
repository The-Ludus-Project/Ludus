#pragma once
#include "ordered_frame.h"

namespace ludus::qa
{
// A scene followed by painter-ordered overlays, then offscreen sampling.
// The declarations deliberately include the same conservative read scope in
// each overlay. No alternate planner or native API exists in this fixture.
inline rhi::RasterStatus BuildMergedFrame(rhi::DeviceHandle device,
                                          const OrderedFrameInputs& inputs,
                                          rhi::OrderedGraph& graph,
                                          rhi::GraphReport& report,
                                          bool merge,
                                          uint32 layers = 16,
                                          bool shouldDiscardFinalDepth = false) noexcept
{
    if (layers == 0 || layers > 31)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
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
    rhi::GraphVersion versions[10]{};
    for (usize i = 0; i < 8; ++i)
    {
        status = rhi::ImportGraphBuffer(device, graph, buffers[i], versions[i]);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
    }
    const rhi::TextureHandle textures[]{inputs.Image, inputs.Offscreen};
    for (usize i = 0; i < 2; ++i)
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
    const usize indexBytes = inputs.Index32 ? 24 : 12;
    rhi::GraphUse uses[]{{versions[0], rhi::GraphAccessMode::Vertex, 0, 80},
                         {versions[1], rhi::GraphAccessMode::Vertex, 0, 80},
                         {versions[2], rhi::GraphAccessMode::Vertex, 0, 16},
                         {versions[3], rhi::GraphAccessMode::Vertex, 0, 8},
                         {versions[4], rhi::GraphAccessMode::Index, 0, indexBytes},
                         {versions[5], rhi::GraphAccessMode::Uniform, 0, 16},
                         {versions[6], rhi::GraphAccessMode::Uniform, 0, 16},
                         {versions[8], rhi::GraphAccessMode::Sampled},
                         {versions[9], rhi::GraphAccessMode::ColorWrite}};
    const rhi::RasterVertexSlice front[2]{{inputs.Vertices, 0, 80}, {inputs.Instances, 0, 16}};
    const rhi::RasterVertexSlice back[2]{{inputs.Background, 0, 80}, {inputs.Center, 0, 8}};
    const rhi::RasterDraw scene[]{{inputs.Scene, inputs.SceneSet, front, inputs.Indices, 0, 6, 4, 2},
                                  {inputs.Scene, inputs.BackgroundSet, back, inputs.Indices, 0, 6, 4, 1}};
    for (uint32 layer = 0; layer < layers; ++layer)
    {
        rhi::GraphVersion output;
        status = rhi::NextGraphVersion(device, graph, versions[9], output);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
        uses[8] = {output, layer == 0 ? rhi::GraphAccessMode::ColorWrite : rhi::GraphAccessMode::ColorReadWrite};
        const rhi::RasterDraw overlay{shouldDiscardFinalDepth == true ? inputs.Scene : inputs.Ui,
                                      shouldDiscardFinalDepth == true || layer % 2 == 0 ? inputs.SceneSet
                                                                                        : inputs.BackgroundSet,
                                      back,
                                      inputs.Indices,
                                      0,
                                      6,
                                      4,
                                      1};
        rhi::GraphPassDescription pass;
        pass.Name = "painter overlay";
        pass.Source = "merged_frame.h";
        pass.Attachment.Color = inputs.Offscreen;
        pass.Attachment.ColorLoad = pass.Attachment.DepthLoad =
            layer == 0 ? rhi::RasterLoad::Clear : rhi::RasterLoad::Load;
        pass.Attachment.DepthStore = shouldDiscardFinalDepth == true && layer + 1 == layers ? rhi::RasterStore::Discard
                                                                                            : rhi::RasterStore::Store;
        pass.Uses = uses;
        pass.UseCount = 9;
        pass.Draws = layer == 0 ? scene : &overlay;
        pass.DrawCount = layer == 0 ? 2 : 1;
        status = rhi::AddGraphPass(device, graph, pass);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
        versions[9] = output;
    }
    const rhi::GraphUse compositeUses[]{{versions[7], rhi::GraphAccessMode::Vertex, 0, 80},
                                        {versions[3], rhi::GraphAccessMode::Vertex, 0, 8},
                                        {versions[4], rhi::GraphAccessMode::Index, 0, indexBytes},
                                        {versions[5], rhi::GraphAccessMode::Uniform, 0, 16},
                                        {versions[9], rhi::GraphAccessMode::Sampled}};
    const rhi::RasterVertexSlice full[2]{{inputs.Fullscreen, 0, 80}, {inputs.Center, 0, 8}};
    const rhi::RasterDraw draw{inputs.Composite, inputs.CompositeSet, full, inputs.Indices, 0, 6, 4, 1};
    rhi::GraphPassDescription composite;
    composite.Name = "sample merged output";
    composite.Source = "merged_frame.h";
    composite.Uses = compositeUses;
    composite.UseCount = 5;
    composite.Draws = &draw;
    composite.DrawCount = 1;
    status = rhi::AddGraphPass(device, graph, composite);
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    return rhi::CompileOrderedGraph(device, graph, report, rhi::GraphCompileOptions{true, merge});
}
} // namespace ludus::qa
