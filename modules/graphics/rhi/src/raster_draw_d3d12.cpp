#include "internal/d3d12_raster.h"
#include "internal/dxil_container.h"

#include <ludus/foundation/containers/static_array.hpp>

// Thanks to Microsoft, "Using Resource Barriers to Synchronize Resource States
// in Direct3D 12" and "Creating a basic Direct3D 12 component", for explicit
// attachment transitions and input-assembler/viewport setup. Passes use the
// baseline command-list API so feature-level 11_0/WARP needs no newer runtime.
// https://learn.microsoft.com/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12
// https://learn.microsoft.com/windows/win32/direct3d12/creating-a-basic-direct3d-12-component
namespace ludus::graphics::rhi::backend
{
namespace
{
using namespace foundation;
using namespace internal;
using namespace d3d12;
constexpr usize COLOR_COUNT = MAX_COLOR_ATTACHMENTS;
constexpr usize ATTRIBUTE_COUNT = 16;
constexpr usize STREAM_COUNT = 2;
constexpr uint32 DEPTH_BYTES_PER_PIXEL = sizeof(float32);
struct Pipeline final
{
    ComOwner<ID3D12PipelineState> State;
    RasterPipelineInfo Description;
};
StorageTable<Pipeline> gPipelines;
// Surface depth survives pass boundaries and is replaced only after a queue drain.
ComOwner<ID3D12Resource> gSurfaceDepth;
ComOwner<ID3D12DescriptorHeap> gSurfaceDepthHeap;
uint64 gSurfaceDepthBytes = 0;
uint32 gSurfaceWidth = 0;
uint32 gSurfaceHeight = 0;
RasterArea gArea;
bool isPassOpen = false;

[[nodiscard]] RasterStatus GetFailure(HRESULT result) noexcept
{
    return result == E_OUTOFMEMORY                               ? RasterStatus::OutOfMemory
           : GetDeviceOrNull()->GetDeviceRemovedReason() != S_OK ? RasterStatus::DeviceLost
                                                                 : RasterStatus::Failed;
}
[[nodiscard]] DXGI_FORMAT GetVertexFormat(RasterVertexFormat format) noexcept
{
    return format == RasterVertexFormat::Float2   ? DXGI_FORMAT_R32G32_FLOAT
           : format == RasterVertexFormat::Float3 ? DXGI_FORMAT_R32G32B32_FLOAT
                                                  : DXGI_FORMAT_R32G32B32A32_FLOAT;
}
[[nodiscard]] D3D12_COMPARISON_FUNC GetDepthCompare(RasterDepthCompare compare) noexcept
{
    switch (compare)
    {
        case RasterDepthCompare::Less:
            return D3D12_COMPARISON_FUNC_LESS;
        case RasterDepthCompare::LessEqual:
            return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case RasterDepthCompare::Greater:
            return D3D12_COMPARISON_FUNC_GREATER;
        case RasterDepthCompare::GreaterEqual:
            return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case RasterDepthCompare::Always:
            return D3D12_COMPARISON_FUNC_ALWAYS;
    }
    return D3D12_COMPARISON_FUNC_ALWAYS;
}
[[nodiscard]] D3D12_DEPTH_STENCILOP_DESC GetStencilFace(const RasterStencilFace& face) noexcept
{
    // Public comparison/operation enums follow the D3D12 order, starting at zero.
    return {static_cast<D3D12_STENCIL_OP>(static_cast<uint8>(face.Fail) + 1),
            static_cast<D3D12_STENCIL_OP>(static_cast<uint8>(face.DepthFail) + 1),
            static_cast<D3D12_STENCIL_OP>(static_cast<uint8>(face.Pass) + 1),
            static_cast<D3D12_COMPARISON_FUNC>(static_cast<uint8>(face.Compare) + 1)};
}
[[nodiscard]] RasterStatus PrepareSurfaceDepth(uint32 width, uint32 height) noexcept
{
    if (gSurfaceDepth.Get() != nullptr && width == gSurfaceWidth && height == gSurfaceHeight)
    {
        return RasterStatus::Ready;
    }
    const uint64 bytes = uint64{width} * height * DEPTH_BYTES_PER_PIXEL;
    if (ReserveResourceStorage(bytes) == false)
    {
        return StorageFailure();
    }
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_D32_FLOAT;
    description.SampleDesc.Count = 1;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    const D3D12_HEAP_PROPERTIES heap{ .Type = D3D12_HEAP_TYPE_DEFAULT };
    ID3D12Resource* depthOrNull = nullptr;
    HRESULT result = GetDeviceOrNull()->CreateCommittedResource(&heap,
                                                                D3D12_HEAP_FLAG_NONE,
                                                                &description,
                                                                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                                nullptr,
                                                                GetInterfaceId(depthOrNull),
                                                                GetInterfaceOutput(depthOrNull));
    ComOwner<ID3D12Resource> depth(depthOrNull);
    ID3D12DescriptorHeap* descriptorsOrNull = nullptr;
    if (SUCCEEDED(result) == true)
    {
        const D3D12_DESCRIPTOR_HEAP_DESC descriptors{D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                                                     1,
                                                     D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
                                                     0};
        result = GetDeviceOrNull()->CreateDescriptorHeap(&descriptors,
                                                         GetInterfaceId(descriptorsOrNull),
                                                         GetInterfaceOutput(descriptorsOrNull));
    }
    ComOwner<ID3D12DescriptorHeap> descriptors(descriptorsOrNull);
    if (FAILED(result) == true)
    {
        ReleaseResourceStorage(bytes);
        return GetFailure(result);
    }
    if (gSurfaceDepth.Get() != nullptr && WaitForQueue() == false)
    {
        ReleaseResourceStorage(bytes);
        return RasterStatus::DeviceLost;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptors->GetCPUDescriptorHandleForHeapStart();
    GetDeviceOrNull()->CreateDepthStencilView(depth.Get(), nullptr, handle);
    gSurfaceDepth = Move(depth);
    gSurfaceDepthHeap = Move(descriptors);
    ReleaseResourceStorage(gSurfaceDepthBytes);
    gSurfaceDepthBytes = bytes;
    gSurfaceWidth = width;
    gSurfaceHeight = height;
    return RasterStatus::Ready;
}
} // namespace
RasterStatus RasterCreatePipeline(usize slot, const RasterPipelineInfo& info, uint32) noexcept
{
    if (info.SampleCount != 1)
    {
        return RasterStatus::Unsupported;
    }
    const D3D12_SHADER_BYTECODE vertex = GetShader(info.Vertex);
    StaticArray<D3D12_INPUT_ELEMENT_DESC, ATTRIBUTE_COUNT> attributes{};
    for (usize index = 0; index < info.AttributeCount; ++index)
    {
        const RasterVertexAttribute& attribute = info.Attributes[index];
        DxilVertexInput input;
        if (ReadDxilVertexInput({static_cast<const uint8*>(vertex.pShaderBytecode), vertex.BytecodeLength},
                                attribute.Location,
                                input) == false)
        {
            return RasterStatus::InvalidDescription;
        }
        const bool isInstance = info.Streams[attribute.Stream].PerInstance;
        attributes[index] = {input.NameOrNull,
                             input.Index,
                             GetVertexFormat(attribute.Format),
                             attribute.Stream,
                             attribute.Offset,
                             isInstance == true ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                                : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                             isInstance == true ? 1U : 0U};
    }
    // Active enum fields are assigned below before driver use; unused SDK slots stay zero.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC native{};
    native.pRootSignature = GetSignature(info.Layout);
    native.VS = vertex;
    native.PS = GetShader(info.Fragment);
    native.InputLayout = {attributes.GetData(), static_cast<UINT>(info.AttributeCount)};
    native.SampleMask = ~UINT{0};
    native.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    native.SampleDesc.Count = 1;
    native.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    native.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    // D3D's viewport flips clip Y; canonical counter-clockwise becomes clockwise on screen.
    native.RasterizerState.FrontCounterClockwise = FALSE;
    native.RasterizerState.DepthClipEnable = TRUE;
    native.DepthStencilState.DepthEnable = info.Depth == true ? TRUE : FALSE;
    native.DepthStencilState.DepthWriteMask =
        info.Depth == true && info.DepthWrite == true ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    native.DepthStencilState.DepthFunc = GetDepthCompare(info.DepthCompare);
    native.DepthStencilState.StencilEnable = info.Stencil == true ? TRUE : FALSE;
    native.DepthStencilState.StencilReadMask = static_cast<UINT8>(info.StencilReadMask);
    native.DepthStencilState.StencilWriteMask = static_cast<UINT8>(info.StencilWriteMask);
    native.DepthStencilState.FrontFace = GetStencilFace(info.StencilFront);
    native.DepthStencilState.BackFace = GetStencilFace(info.StencilBack);
    native.DSVFormat = info.Stencil == true ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
    native.NumRenderTargets = static_cast<UINT>(info.ColorCount == 0 ? 1 : info.ColorCount);
    for (usize color = 0; color < native.NumRenderTargets; ++color)
    {
        native.RTVFormats[color] = info.ColorCount != 0                     ? GetRasterFormat(info.ColorFormats[color])
                                   : info.Target == RasterTarget::Surface   ? DXGI_FORMAT_B8G8R8A8_UNORM
                                   : info.Target == RasterTarget::Rgba8Srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                   : info.Target == RasterTarget::Rgba16Float ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                                              : DXGI_FORMAT_R8G8B8A8_UNORM;
        D3D12_RENDER_TARGET_BLEND_DESC& blend = native.BlendState.RenderTarget[color];
        blend.BlendEnable = info.Blend == true ? TRUE : FALSE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = info.Blend == true ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ZERO;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = blend.DestBlend;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    native.BlendState.IndependentBlendEnable = TRUE;
    ID3D12PipelineState* stateOrNull = nullptr;
    const HRESULT result = GetDeviceOrNull()->CreateGraphicsPipelineState(&native,
                                                                          GetInterfaceId(stateOrNull),
                                                                          GetInterfaceOutput(stateOrNull));
    if (FAILED(result) == true)
    {
        return GetFailure(result);
    }
    gPipelines[slot] = {ComOwner<ID3D12PipelineState>(stateOrNull), info};
    return RasterStatus::Ready;
}
RasterStatus RasterPreparePass(const RasterPassInfo& info) noexcept
{
    if (info.SampleCount != 1)
    {
        return RasterStatus::Unsupported;
    }
    if (info.Texture == RASTER_INVALID && info.ColorCount == 0)
    {
        const FrameInfo frame = GetFrameInfo();
        return PrepareSurfaceDepth(frame.Width, frame.Height);
    }
    return RasterStatus::Ready;
}
RasterStatus RasterBeginPass(const RasterPassInfo& info) noexcept
{
    const RasterStatus prepared = RasterPreparePass(info);
    if (prepared != RasterStatus::Ready)
    {
        return prepared;
    }
    const bool isSurface = info.Texture == RASTER_INVALID && info.ColorCount == 0;
    const FrameInfo frame = GetFrameInfo();
    const uint32 width = isSurface == true ? frame.Width : info.Width;
    const uint32 height = isSurface == true ? frame.Height : info.Height;
    RasterArea area;
    if (RasterResolveArea(info.Description, width, height, area) == false)
    {
        return RasterStatus::InvalidDescription;
    }
    ID3D12GraphicsCommandList* commandsOrNull = GetCommandsOrNull();
    if (commandsOrNull == nullptr)
    {
        return RasterStatus::InvalidState;
    }
    StaticArray<D3D12_CPU_DESCRIPTOR_HANDLE, COLOR_COUNT> targets{};
    const usize count = info.ColorCount == 0 ? 1 : info.ColorCount;
    Attachment first;
    for (usize color = 0; color < count; ++color)
    {
        Attachment attachment;
        if (isSurface == true)
        {
            attachment = GetSurfaceAttachment();
            attachment.DepthOrNull = gSurfaceDepth.Get();
            attachment.Depth = gSurfaceDepthHeap->GetCPUDescriptorHandleForHeapStart();
        }
        else
        {
            const usize texture = info.ColorCount == 0 ? info.Texture : info.Colors[color].Texture;
            const uint32 mip = info.ColorCount == 0 ? 0 : info.Colors[color].Mip;
            const uint32 layer = info.ColorCount == 0 ? 0 : info.Colors[color].Layer;
            RasterTextureBarrier(texture, RasterTextureUse::ColorAttachment);
            attachment = GetAttachment(texture, mip, layer);
        }
        if (color == 0)
        {
            first = attachment;
        }
        targets[color] = attachment.Color;
        const RasterLoad load = info.ColorCount == 0 ? info.Description.ColorLoad : info.Colors[color].Load;
        const float32* clear = info.ColorCount == 0 ? info.Description.Clear : info.Colors[color].Clear;
        if (load == RasterLoad::Clear)
        {
            commandsOrNull->ClearRenderTargetView(attachment.Color, clear, 0, nullptr);
        }
    }
    D3D12_CLEAR_FLAGS flags = static_cast<D3D12_CLEAR_FLAGS>(0);
    if (info.Description.DepthLoad == RasterLoad::Clear)
    {
        flags |= D3D12_CLEAR_FLAG_DEPTH;
    }
    if (info.Stencil == true && info.Description.StencilLoad == RasterLoad::Clear)
    {
        flags |= D3D12_CLEAR_FLAG_STENCIL;
    }
    if (flags != 0)
    {
        commandsOrNull->ClearDepthStencilView(first.Depth,
                                              flags,
                                              info.Description.ClearDepth,
                                              static_cast<UINT8>(info.Description.ClearStencil),
                                              0,
                                              nullptr);
    }
    commandsOrNull->OMSetRenderTargets(static_cast<UINT>(count), targets.GetData(), FALSE, &first.Depth);
    gArea = area;
    isPassOpen = true;
    return RasterStatus::Ready;
}
void RasterEndPass() noexcept
{
    // Validity for Discard belongs to the facade; preserving native bits is legal.
    // CPU RTV/DSV handles are consumed during recording and never rewritten.
    isPassOpen = false;
}
RasterStatus RasterDraw(const RasterPacket& packet) noexcept
{
    if (packet.Indirect != RASTER_INVALID)
    {
        return RasterStatus::Unsupported;
    }
    if (isPassOpen == false)
    {
        RasterPassInfo surface;
        surface.Description.ColorLoad = RasterLoad::Load;
        const RasterStatus status = RasterBeginPass(surface);
        if (status != RasterStatus::Ready)
        {
            return status;
        }
    }
    if (gArea.Empty == true)
    {
        return RasterStatus::Ready;
    }
    const Pipeline& pipeline = gPipelines[packet.Pipeline];
    const RasterPipelineInfo& info = pipeline.Description;
    ID3D12GraphicsCommandList* commands = GetCommandsOrNull();
    commands->SetGraphicsRootSignature(GetSignature(info.Layout));
    commands->SetPipelineState(pipeline.State.Get());
    commands->OMSetStencilRef(info.StencilReference);
    BindSet(commands, packet.Set);
    StaticArray<D3D12_VERTEX_BUFFER_VIEW, STREAM_COUNT> vertices{};
    for (usize stream = 0; stream < info.StreamCount; ++stream)
    {
        vertices[stream] = {GetBuffer(packet.Vertices[stream])->GetGPUVirtualAddress() + packet.Offsets[stream],
                            static_cast<UINT>(packet.Sizes[stream]),
                            info.Streams[stream].Stride};
    }
    commands->IASetVertexBuffers(0, static_cast<UINT>(info.StreamCount), vertices.GetData());
    const uint32 indexBytes = packet.Index32 == true ? sizeof(uint32) : sizeof(uint16);
    const D3D12_INDEX_BUFFER_VIEW indices{GetBuffer(packet.Indices)->GetGPUVirtualAddress() + packet.IndexOffset,
                                          packet.IndexCount * indexBytes,
                                          packet.Index32 == true ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
    commands->IASetIndexBuffer(&indices);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const RasterRectangle& viewport = gArea.Viewport;
    const RasterRectangle& scissor = gArea.Scissor;
    const D3D12_VIEWPORT nativeViewport{static_cast<float32>(viewport.X),
                                        static_cast<float32>(viewport.Y),
                                        static_cast<float32>(viewport.Width),
                                        static_cast<float32>(viewport.Height),
                                        0,
                                        1};
    const D3D12_RECT nativeScissor{static_cast<LONG>(scissor.X),
                                   static_cast<LONG>(scissor.Y),
                                   static_cast<LONG>(scissor.X + scissor.Width),
                                   static_cast<LONG>(scissor.Y + scissor.Height)};
    commands->RSSetViewports(1, &nativeViewport);
    commands->RSSetScissorRects(1, &nativeScissor);
    commands->DrawIndexedInstanced(packet.IndexCount, packet.InstanceCount, 0, 0, 0);
    return RasterStatus::Ready;
}
namespace d3d12
{
RasterStatus GrowPipelines(usize count) noexcept
{
    return gPipelines.EnsureSlots(count) == true ? RasterStatus::Ready : StorageFailure();
}
void DestroyPipeline(usize slot) noexcept
{
    gPipelines[slot] = {};
}
void ResetRasterPasses() noexcept
{
    isPassOpen = false;
    gSurfaceDepth.Reset();
    gSurfaceDepthHeap.Reset();
    ReleaseResourceStorage(gSurfaceDepthBytes);
    gSurfaceDepthBytes = 0;
    gSurfaceWidth = 0;
    gSurfaceHeight = 0;
}
} // namespace d3d12
} // namespace ludus::graphics::rhi::backend
