#pragma once
// Included only by raster.cpp; graph plans and direct draws share its registry.
// Thanks to the Ludus RHI/GDI architecture, "Pass declarations and deterministic
// compilation" (docs/architecture/rhi-gdi.md), for authored order, versioned
// contents, observable history roots and completion-gated object pooling.
namespace ludus::graphics::rhi
{
namespace internal
{
struct GraphVersionIndex final
{
    usize Resource;
    uint32 Version;
};
struct GraphAccess final
{
    static uint64 Owner(OrderedGraph h) noexcept
    {
        return h.Owner;
    }
    static uint64 Generation(OrderedGraph h) noexcept
    {
        return h.Generation;
    }
    static usize Slot(OrderedGraph h) noexcept
    {
        return h.Slot == 0 ? RASTER_INVALID : h.Slot - 1;
    }
    static void Set(OrderedGraph& h, const RasterIdentity& identity) noexcept
    {
        h.Owner = DeviceOwner();
        h.Generation = identity.Generation;
        h.Slot = static_cast<uint32>(identity.Slot + 1);
    }
    static bool Belongs(GraphVersion v, OrderedGraph h) noexcept
    {
        return v.Owner != 0 && v.Owner == h.Owner && v.Generation == h.Generation && v.Graph == h.Slot;
    }
    static usize Resource(GraphVersion v) noexcept
    {
        return v.Resource == 0 ? RASTER_INVALID : v.Resource - 1;
    }
    static uint32 Version(GraphVersion v) noexcept
    {
        return v.Version;
    }
    static void Set(GraphVersion& v, OrderedGraph h, const GraphVersionIndex& index) noexcept
    {
        v.Owner = h.Owner;
        v.Generation = h.Generation;
        v.Graph = h.Slot;
        v.Resource = static_cast<uint32>(index.Resource + 1);
        v.Version = index.Version;
    }
    static bool Null(GraphVersion v) noexcept
    {
        return v.Owner == 0;
    }
};
} // namespace internal
namespace
{
usize GraphCount() noexcept
{
    return CurrentStorageLimits().Graphs;
}
usize GraphResources() noexcept
{
    return CurrentStorageLimits().ResourcesPerGraph;
}
usize GraphPasses() noexcept
{
    return CurrentStorageLimits().PassesPerGraph;
}
usize GraphDependencies() noexcept
{
    return CurrentStorageLimits().DependenciesPerGraph;
}
usize GraphPool() noexcept
{
    return CurrentStorageLimits().TransientTextures;
}
struct GraphResourceRecord final
{
    RasterKind Kind = RasterKind::Buffer;
    usize Slot = 0;
    uint32 Versions = 0;
    uint32 RootVersion = 0;
    bool Root = false;
    bool Transient = false;
    RasterTextureState Import;
    uint64 BufferRevision = 0;
    RasterTextureUse Final = RasterTextureUse::SampledBoth;
};
struct GraphPassRecord final
{
    RasterPassInfo Info;
    StorageArray<GraphUse> Uses;
    usize UseCount = 0;
    usize FirstDraw = 0;
    usize DrawCount = 0;
    bool SideEffect = false;
    bool IsCompute = false;
    ComputePacket Dispatch;
};
struct FullGraphReport final
{
    GraphReport Summary;
    StorageArray<GraphPassDiagnostic> Passes;
    StorageArray<GraphResourceDiagnostic> Resources;
    StorageArray<GraphDependency> Dependencies;
};
struct GraphRecord final : LifetimeRecord
{
    bool Compiled = false;
    usize ResourceCount = 0;
    usize PassCount = 0;
    usize DrawCount = 0;
    StorageArray<GraphResourceRecord> Resources;
    StorageArray<GraphPassRecord> Passes;
    StorageArray<RasterPacket> Draws;
    FullGraphReport Report;
    bool ReportAvailable = false;
};
struct GraphPoolRecord final
{
    bool Occupied = false;
    usize Texture = 0;
};
StorageTable<GraphRecord> gGraphs;
StorageTable<GraphPoolRecord> gGraphPool;
GraphRecord* ResolveGraph(OrderedGraph handle) noexcept
{
    const auto slot = GraphAccess::Slot(handle);
    if (slot >= gGraphs.GetSize())
    {
        return nullptr;
    }
    auto& graph = gGraphs[slot];
    return graph.Published && graph.Owner == DeviceOwner() && graph.Owner == GraphAccess::Owner(handle) &&
                   graph.Generation == GraphAccess::Generation(handle)
               ? &graph
               : nullptr;
}
RasterStatus GraphAdmission(DeviceHandle device,
                            OrderedGraph handle,
                            GraphRecord*& outGraphOrNull,
                            bool isSetup = true,
                            bool canEncodeInFrame = false) noexcept
{
    const RasterStatus admission = Admission(device, canEncodeInFrame == false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    outGraphOrNull = ResolveGraph(handle);
    if (outGraphOrNull == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    return isSetup == true && outGraphOrNull->Compiled == true ? RasterStatus::InvalidState : RasterStatus::Ready;
}
bool ValidVersion(const GraphRecord& graph, OrderedGraph handle, GraphVersion version) noexcept
{
    const auto resource = GraphAccess::Resource(version);
    return GraphAccess::Belongs(version, handle) && resource < graph.ResourceCount &&
           GraphAccess::Version(version) <= graph.Resources[resource].Versions;
}
RasterTextureState TextureState(const Record& record) noexcept
{
    RasterTextureState result;
    result.Revision = record.TextureRevision;
    if (record.LastUse != 0)
    {
        LifetimeAccess::Set(result.Dependency, record.LastUse);
    }
    result.Use = record.TextureUse;
    result.ColorDefined = record.ColorDefined;
    result.DepthDefined = record.DepthDefined;
    result.StencilDefined = record.StencilDefined;
    return result;
}
bool SameTextureState(const RasterTextureState& a, const RasterTextureState& b) noexcept
{
    return a.Revision == b.Revision && a.Use == b.Use && a.ColorDefined == b.ColorDefined &&
           a.DepthDefined == b.DepthDefined && a.StencilDefined == b.StencilDefined &&
           LifetimeAccess::Owner(a.Dependency) == LifetimeAccess::Owner(b.Dependency) &&
           LifetimeAccess::Ordinal(a.Dependency) == LifetimeAccess::Ordinal(b.Dependency);
}
bool SameTextureDescription(const TextureDescription& a, const TextureDescription& b) noexcept
{
    return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format && a.Attachment == b.Attachment &&
           a.MipLevels == b.MipLevels && a.Dimension == b.Dimension && a.DepthOrLayers == b.DepthOrLayers &&
           a.SampleCount == b.SampleCount && a.Stencil == b.Stencil;
}
void ResetGraph(GraphRecord& graph) noexcept
{
    // Reset bounded members in place: a whole GraphRecord temporary exceeds the
    // default browser stack and can corrupt adjacent allocator bookkeeping.
    const auto generation = RasterNextGeneration(graph.Generation);
    static_cast<LifetimeRecord&>(graph) = {};
    graph.Generation = generation;
    graph.Compiled = false;
    graph.ReportAvailable = false;
    graph.ResourceCount = graph.PassCount = graph.DrawCount = 0;
    for (auto& resource : graph.Resources)
    {
        resource = {};
    }
    for (auto& pass : graph.Passes)
    {
        pass = {};
    }
    for (auto& draw : graph.Draws)
    {
        draw = {};
    }
    graph.Report = {};
}
void PassViews(const RasterPassInfo&, bool, bool = false) noexcept;
void DropGraph(GraphRecord& graph) noexcept
{
    for (usize i = 0; i < graph.DrawCount; ++i)
    {
        PacketReferences(graph.Draws[i], false);
    }
    for (usize p = 0; p < graph.PassCount; ++p)
    {
        PassViews(graph.Passes[p].Info, false);
        if (graph.Passes[p].IsCompute)
        {
            ComputeReferences(graph.Passes[p].Dispatch, false);
        }
    }
    for (usize i = 0; i < graph.ResourceCount; ++i)
    {
        const auto& resource = graph.Resources[i];
        if (resource.Transient)
        {
            auto& texture = At(RasterKind::Texture, resource.Slot);
            texture.Published = false;
            texture.Generation = RasterNextGeneration(texture.Generation);
        }
        Release(resource.Kind, resource.Slot);
    }
    ResetGraph(graph);
}
void ResetGraphRecords() noexcept
{
    // Teardown releases every registry record unconditionally after backend drain.
    for (auto& graph : gGraphs)
    {
        ResetGraph(graph);
    }
    for (auto& pool : gGraphPool)
    {
        pool = {};
    }
}
RasterTarget PassTarget(const RasterPassInfo& pass) noexcept
{
    if (pass.Texture == RASTER_INVALID)
    {
        return RasterTarget::Surface;
    }
    if (At(RasterKind::Texture, pass.Texture).Texture.Format == RasterFormat::Rgba16Float)
    {
        return RasterTarget::Rgba16Float;
    }
    return At(RasterKind::Texture, pass.Texture).Texture.Format == RasterFormat::Rgba8Unorm ? RasterTarget::Rgba8Unorm
                                                                                            : RasterTarget::Rgba8Srgb;
}

usize PassColorCount(const RasterPassInfo& pass) noexcept
{
    return pass.ColorCount == 0 ? 1 : pass.ColorCount;
}
const RasterAttachmentInfo* PassAttachment(const RasterPassInfo& pass, usize slot, bool& resolve) noexcept
{
    resolve = false;
    for (usize i = 0; i < PassColorCount(pass); ++i)
    {
        if (pass.Colors[i].Texture == slot)
        {
            return &pass.Colors[i];
        }
        if (pass.Colors[i].ResolveTexture == slot)
        {
            resolve = true;
            return &pass.Colors[i];
        }
    }
    return nullptr;
}
void PassViews(const RasterPassInfo& pass, bool retain, bool frame) noexcept
{
    for (usize i = 0; i < pass.ColorCount; ++i)
    {
        for (const auto slot : {pass.Colors[i].View, pass.Colors[i].ResolveView})
        {
            if (slot != RASTER_INVALID)
            {
                if (frame)
                {
                    FrameRetain(RasterKind::View, slot);
                }
                else if (retain)
                {
                    Retain(RasterKind::View, slot);
                }
                else
                {
                    Release(RasterKind::View, slot);
                }
            }
        }
    }
}
RasterStatus ValidatePassDescription(const RasterPassDescription& description, RasterPassInfo& pass) noexcept
{
    const bool validDepth = description.ClearDepth >= 0 && description.ClearDepth <= 1;
    if (static_cast<uint8>(description.ColorLoad) > 2 || static_cast<uint8>(description.DepthLoad) > 2 ||
        static_cast<uint8>(description.ColorStore) > 1 || static_cast<uint8>(description.DepthStore) > 1 ||
        static_cast<uint8>(description.StencilLoad) > 2 || static_cast<uint8>(description.StencilStore) > 1 ||
        (description.DepthReadOnly && description.DepthLoad != RasterLoad::Load) ||
        (description.StencilReadOnly && description.StencilLoad != RasterLoad::Load) || validDepth == false ||
        description.ClearStencil > 255 ||
        (description.UseViewport && (description.Viewport.Width == 0 || description.Viewport.Height == 0)) ||
        (description.ColorCount != 0 && RasterAccess::Owner(description.Color) != 0))
    {
        return RasterStatus::InvalidDescription;
    }
    if (description.ColorCount > 4 ||
        description.ColorCount > internal::CurrentDeviceProfile().Interface.MaxColorAttachments)
    {
        return RasterStatus::Unsupported;
    }
    if (description.ColorCount != 0 && description.Colors == nullptr)
    {
        return RasterStatus::InvalidDescription;
    }
    pass.Description = description;
    pass.Description.Colors = nullptr;
    pass.Description.ColorCount = 0; // Never retain the caller's borrowed array in a compiled plan.
    pass.ColorCount = description.ColorCount;
    for (usize i = 0; i < PassColorCount(pass); ++i)
    {
        auto& color = pass.Colors[i];
        color.Load = pass.ColorCount == 0 ? description.ColorLoad : description.Colors[i].Load;
        color.Store = pass.ColorCount == 0 ? description.ColorStore : description.Colors[i].Store;
        const auto* clear = pass.ColorCount == 0 ? description.Clear : description.Colors[i].Clear;
        for (usize c = 0; c < 4; ++c)
        {
            const bool validClear = clear[c] >= 0 && clear[c] <= 1;
            if (validClear == false)
            {
                return RasterStatus::InvalidDescription;
            }
            color.Clear[c] = clear[c];
        }
        if (static_cast<uint8>(color.Load) > 2 || static_cast<uint8>(color.Store) > 1)
        {
            return RasterStatus::InvalidDescription;
        }
        if (pass.ColorCount == 0 && RasterAccess::Owner(description.Color) == 0)
        {
            return RasterStatus::Ready;
        }
        const Record* texture = nullptr;
        if (pass.ColorCount == 0)
        {
            texture = Resolve(description.Color, RasterKind::Texture);
            color.Texture = RasterAccess::Slot(description.Color);
            if (texture && (texture->Texture.Dimension != TextureDimension::D2 || texture->Texture.MipLevels != 1 ||
                            texture->Texture.SampleCount != 1 || texture->Texture.Stencil))
            {
                return RasterStatus::InvalidDescription;
            }
        }
        else
        {
            const auto* view = Resolve(description.Colors[i].View, RasterKind::View);
            if (view == nullptr)
            {
                return RasterStatus::InvalidHandle;
            }
            if (view->View.Attachment == false)
            {
                return RasterStatus::InvalidDescription;
            }
            if (view->Status != RasterStatus::Ready)
            {
                return view->Status == RasterStatus::Pending ? RasterStatus::NotReady : view->Status;
            }
            color.View = RasterAccess::Slot(description.Colors[i].View);
            color.Texture = view->TextureSlot;
            color.Mip = view->View.BaseMip;
            color.Layer = view->View.BaseLayer;
            texture = &At(RasterKind::Texture, color.Texture);
        }
        if (texture == nullptr)
        {
            return RasterStatus::InvalidHandle;
        }
        if (texture->ExternalReads != 0)
        {
            return RasterStatus::InvalidState;
        }
        if (texture->Texture.Attachment == false)
        {
            return RasterStatus::InvalidDescription;
        }
        if (texture->Status != RasterStatus::Ready)
        {
            return texture->Status == RasterStatus::Pending ? RasterStatus::NotReady : texture->Status;
        }
        if (texture->TextureRevision == ~uint64{0})
        {
            return RasterStatus::IdentityExhausted;
        }
        const auto width = TextureMipExtent(texture->Texture.Width, color.Mip),
                   height = TextureMipExtent(texture->Texture.Height, color.Mip);
        color.Format = texture->Texture.Format;
        if (i == 0)
        {
            pass.Texture = color.Texture;
            pass.Width = width;
            pass.Height = height;
            pass.SampleCount = texture->Texture.SampleCount;
            pass.Stencil = texture->Texture.Stencil;
        }
        else if (width != pass.Width || height != pass.Height || texture->Texture.SampleCount != pass.SampleCount)
        {
            return RasterStatus::InvalidDescription;
        }
        if (pass.ColorCount != 0 && RasterAccess::Owner(description.Colors[i].Resolve) != 0)
        {
            const auto* view = Resolve(description.Colors[i].Resolve, RasterKind::View);
            if (view == nullptr)
            {
                return RasterStatus::InvalidHandle;
            }
            if (view->View.Attachment == false || color.Load == RasterLoad::Discard)
            {
                return RasterStatus::InvalidDescription;
            }
            if (view->Status != RasterStatus::Ready)
            {
                return view->Status == RasterStatus::Pending ? RasterStatus::NotReady : view->Status;
            }
            const auto& destination = At(RasterKind::Texture, view->TextureSlot);
            if (destination.ExternalReads != 0)
            {
                return RasterStatus::InvalidState;
            }
            if (pass.SampleCount != 4 || destination.Texture.SampleCount != 1 ||
                destination.Texture.Format != color.Format ||
                TextureMipExtent(destination.Texture.Width, view->View.BaseMip) != width ||
                TextureMipExtent(destination.Texture.Height, view->View.BaseMip) != height)
            {
                return RasterStatus::InvalidDescription;
            }
            if (destination.TextureRevision == ~uint64{0})
            {
                return RasterStatus::IdentityExhausted;
            }
            color.ResolveView = RasterAccess::Slot(description.Colors[i].Resolve);
            color.ResolveTexture = view->TextureSlot;
            color.ResolveMip = view->View.BaseMip;
            color.ResolveLayer = view->View.BaseLayer;
        }
        for (usize j = 0; j <= i; ++j)
        {
            const auto& other = pass.Colors[j];
            if ((j < i && (color.Texture == other.Texture || color.Texture == other.ResolveTexture)) ||
                (color.ResolveTexture != RASTER_INVALID &&
                 (color.ResolveTexture == other.Texture || (j < i && color.ResolveTexture == other.ResolveTexture))))
            {
                return RasterStatus::InvalidDescription;
            }
        }
    }
    RasterArea area;
    return RasterResolveArea(description, pass.Width, pass.Height, area) ? RasterStatus::Ready
                                                                         : RasterStatus::InvalidDescription;
}
RasterStatus ValidatePacketPass(const RasterPacket& packet, const RasterPassInfo& pass, bool contents) noexcept
{
    const auto& pipeline = At(RasterKind::Pipeline, packet.Pipeline).Pipeline;
    bool formats = pipeline.ColorCount == pass.ColorCount;
    for (usize i = 0; i < pass.ColorCount && formats; ++i)
    {
        formats = pipeline.ColorFormats[i] == pass.Colors[i].Format;
    }
    if (formats == false || (pass.ColorCount == 0 && pipeline.Target != PassTarget(pass)) ||
        pipeline.SampleCount != pass.SampleCount || pipeline.Stencil != pass.Stencil ||
        (pass.Description.StencilReadOnly && pipeline.Stencil && pipeline.StencilWriteMask != 0) ||
        (pass.Description.DepthReadOnly && pipeline.Depth && pipeline.DepthWrite))
    {
        return RasterStatus::InvalidDescription;
    }
    const auto& set = At(RasterKind::Set, packet.Set).Set;
    const RasterLayout& layout = set.Description;
    for (usize i = 0; i < layout.DescriptorCount; ++i)
    {
        if (internal::TextureBinding(layout.GetDescriptorBinding(i).Kind) == false)
        {
            continue;
        }
        const auto slot = At(RasterKind::View, set.Slots[i]).TextureSlot;
        const auto& texture = At(RasterKind::Texture, slot);
        bool resolve = false;
        if (PassAttachment(pass, slot, resolve) != nullptr)
        {
            return RasterStatus::InvalidDescription;
        }
        if (texture.Status != RasterStatus::Ready)
        {
            return texture.Status;
        }
        if (texture.ServiceOwned && texture.Published == false)
        {
            return RasterStatus::InvalidHandle;
        }
        if (texture.TextureRevision == ~uint64{0})
        {
            return RasterStatus::IdentityExhausted;
        }
        if (contents && texture.Texture.Attachment &&
            ViewDefined(texture,
                        At(RasterKind::View, set.Slots[i]).View,
                        texture.InFrame ? texture.FrameAttachmentState : texture.AttachmentState) == false)
        {
            return RasterStatus::InvalidDescription;
        }
        if (contents)
        {
            const auto use = texture.InFrame ? texture.FrameTextureUse : texture.TextureUse;
            const uint8 visibility = static_cast<uint8>(layout.GetDescriptorBinding(i).Visibility);
            const uint8 available = use == RasterTextureUse::SampledVertex     ? 1
                                    : use == RasterTextureUse::SampledFragment ? 2
                                    : use == RasterTextureUse::SampledBoth     ? 3
                                                                               : 0;
            if ((available & visibility) != visibility)
            {
                return RasterStatus::InvalidState;
            }
        }
    }
    return RasterStatus::Ready;
}
void RetainPacketTextures(const RasterPacket& packet) noexcept
{
    const auto& set = At(RasterKind::Set, packet.Set).Set;
    const RasterLayout& layout = set.Description;
    for (usize i = 0; i < layout.DescriptorCount; ++i)
    {
        if (internal::TextureBinding(layout.GetDescriptorBinding(i).Kind) == false)
        {
            continue;
        }
        const auto slot = At(RasterKind::View, set.Slots[i]).TextureSlot;
        FrameRetain(RasterKind::Texture, slot);
        auto& texture = At(RasterKind::Texture, slot);
        const uint8 visibility = static_cast<uint8>(layout.GetDescriptorBinding(i).Visibility);
        const auto use = visibility == 1   ? RasterTextureUse::SampledVertex
                         : visibility == 2 ? RasterTextureUse::SampledFragment
                                           : RasterTextureUse::SampledBoth;
        const bool alreadySampled = texture.FrameTextureUse >= RasterTextureUse::SampledVertex &&
                                    texture.FrameTextureUse <= RasterTextureUse::SampledBoth;
        texture.FrameTextureUse =
            alreadySampled && texture.FrameTextureUse != use ? RasterTextureUse::SampledBoth : use;
    }
}

uint8 ApplyAspect(uint8 value, uint8 bit, RasterLoad load, RasterStore store) noexcept
{
    const bool defined = load == RasterLoad::Clear || (load == RasterLoad::Load && (value & bit) != 0);
    return static_cast<uint8>((value & ~bit) | (store == RasterStore::Store && defined ? bit : 0));
}
bool ValidatePassLoads(const RasterPassInfo& pass) noexcept
{
    if (pass.Texture == RASTER_INVALID)
    {
        return (pass.Description.ColorLoad != RasterLoad::Load || gSurfaceColorDefined) &&
               (pass.Description.DepthLoad != RasterLoad::Load || gSurfaceDepthDefined);
    }
    for (usize i = 0; i < PassColorCount(pass); ++i)
    {
        const auto& c = pass.Colors[i];
        const auto& texture = At(RasterKind::Texture, c.Texture);
        const auto& states = texture.InFrame ? texture.FrameAttachmentState : texture.AttachmentState;
        const auto value = states[SubresourceIndex(texture.Texture, c.Mip, c.Layer)];
        if ((c.Load == RasterLoad::Load && (value & COLOR_DEFINED) == 0) ||
            (i == 0 &&
             ((pass.Description.DepthLoad == RasterLoad::Load && (value & DEPTH_DEFINED) == 0) ||
              (pass.Stencil && pass.Description.StencilLoad == RasterLoad::Load && (value & STENCIL_DEFINED) == 0))))
        {
            return false;
        }
    }
    return true;
}
void ApplyPassState(const RasterPassInfo& pass) noexcept
{
    const auto& description = pass.Description;
    if (pass.Texture == RASTER_INVALID)
    {
        gSurfaceColorDefined = description.ColorStore == RasterStore::Store &&
                               (description.ColorLoad == RasterLoad::Clear ||
                                (description.ColorLoad == RasterLoad::Load && gSurfaceColorDefined));
        gSurfaceDepthDefined = description.DepthStore == RasterStore::Store &&
                               (description.DepthLoad == RasterLoad::Clear ||
                                (description.DepthLoad == RasterLoad::Load && gSurfaceDepthDefined));
    }
    else
    {
        for (usize i = 0; i < PassColorCount(pass); ++i)
        {
            const auto& c = pass.Colors[i];
            FrameRetain(RasterKind::Texture, c.Texture);
            auto& texture = At(RasterKind::Texture, c.Texture);
            texture.FrameTextureUse = RasterTextureUse::ColorAttachment;
            auto& value = texture.FrameAttachmentState[SubresourceIndex(texture.Texture, c.Mip, c.Layer)];
            const bool defined =
                c.Load == RasterLoad::Clear || (c.Load == RasterLoad::Load && (value & COLOR_DEFINED) != 0);
            value = ApplyAspect(value, COLOR_DEFINED, c.Load, c.Store);
            if (i == 0)
            {
                value = ApplyAspect(value, DEPTH_DEFINED, description.DepthLoad, description.DepthStore);
                if (pass.Stencil)
                {
                    value = ApplyAspect(value, STENCIL_DEFINED, description.StencilLoad, description.StencilStore);
                }
            }
            texture.FrameColorDefined = AllDefined(texture.FrameAttachmentState, COLOR_DEFINED);
            texture.FrameDepthDefined = AllDefined(texture.FrameAttachmentState, DEPTH_DEFINED);
            texture.FrameStencilDefined = AllDefined(texture.FrameAttachmentState, STENCIL_DEFINED);
            if (c.ResolveTexture != RASTER_INVALID)
            {
                FrameRetain(RasterKind::Texture, c.ResolveTexture);
                auto& destination = At(RasterKind::Texture, c.ResolveTexture);
                auto& state =
                    destination
                        .FrameAttachmentState[SubresourceIndex(destination.Texture, c.ResolveMip, c.ResolveLayer)];
                state = static_cast<uint8>((state & ~COLOR_DEFINED) | (defined ? COLOR_DEFINED : 0));
                destination.FrameColorDefined = AllDefined(destination.FrameAttachmentState, COLOR_DEFINED);
                destination.FrameTextureUse = RasterTextureUse::ColorAttachment;
            }
        }
    }
    PassViews(pass, true, true);
    gRasterPass = pass;
}
RasterStatus StartPass(const RasterPassInfo& pass) noexcept
{
    backend::RasterEndPass();
    if (GetStartup().State != StartupState::Ready)
    {
        return GetStartup().State == StartupState::DeviceLost ? RasterStatus::DeviceLost : RasterStatus::Failed;
    }
    for (usize i = 0; i < PassColorCount(gRasterPass); ++i)
    {
        for (const auto slot : {gRasterPass.Colors[i].Texture, gRasterPass.Colors[i].ResolveTexture})
        {
            if (slot != RASTER_INVALID)
            {
                backend::RasterTextureBarrier(slot, RasterTextureUse::SampledBoth);
                if (GetStartup().State != StartupState::Ready)
                {
                    return RasterStatus::Failed;
                }
                At(RasterKind::Texture, slot).FrameTextureUse = RasterTextureUse::SampledBoth;
            }
        }
    }
    const auto started = backend::RasterBeginPass(pass);
    const auto current = GetStartup().State;
    if (current != StartupState::Ready)
    {
        return current == StartupState::DeviceLost ? RasterStatus::DeviceLost : RasterStatus::Failed;
    }
    if (started == RasterStatus::Ready)
    {
        ApplyPassState(pass);
    }
    return started;
}
bool CopyGraphText(const char* input, char* output, usize capacity, bool required) noexcept
{
    if (input == nullptr)
    {
        return required == false;
    }
    usize length = 0;
    while (length < capacity && input[length] != '\0')
    {
        ++length;
    }
    if (length == capacity || (required && length == 0))
    {
        return false;
    }
    std::memcpy(output, input, length);
    return true;
}
RasterStatus AddImport(GraphRecord& graph,
                       OrderedGraph handle,
                       RasterKind kind,
                       usize slot,
                       GraphVersion& output,
                       bool transient = false) noexcept
{
    if (GraphAccess::Null(output) == false)
    {
        return RasterStatus::InvalidState;
    }
    if (graph.ResourceCount == GraphResources())
    {
        return RasterStatus::CapacityExceeded;
    }
    for (usize i = 0; i < graph.ResourceCount; ++i)
    {
        if (graph.Resources[i].Kind == kind && graph.Resources[i].Slot == slot)
        {
            return RasterStatus::InvalidDescription;
        }
    }
    if (graph.Resources.TryResize(graph.ResourceCount + 1) == false ||
        graph.Report.Resources.TryResize(graph.ResourceCount + 1) == false)
    {
        return StorageFailure();
    }
    graph.ReportAvailable = false;
    const auto index = graph.ResourceCount++;
    auto& resource = graph.Resources[index];
    resource.Kind = kind;
    resource.Slot = slot;
    resource.Transient = transient;
    if (kind == RasterKind::Texture)
    {
        resource.Import = TextureState(At(kind, slot));
    }
    Retain(kind, slot);
    GraphAccess::Set(output, handle, {index, 0});
    return RasterStatus::Ready;
}
} // namespace
namespace internal
{
bool RasterFinishFrame() noexcept
{
    if (gSurfaceColorDefined == false)
    {
        internal::FaultRasterSession();
        return false;
    }
    backend::RasterEndPass();
    if (GetStartup().State != StartupState::Ready)
    {
        return false;
    }
    if (gGraphExecuting)
    {
        return true;
    }
    for (usize slot = 0; slot < gRecords[static_cast<usize>(RasterKind::Texture)].GetSize(); ++slot)
    {
        auto& texture = At(RasterKind::Texture, slot);
        if (texture.InFrame && texture.Texture.Attachment &&
            texture.FrameTextureUse == RasterTextureUse::ColorAttachment)
        {
            backend::RasterTextureBarrier(slot, RasterTextureUse::SampledBoth);
            if (GetStartup().State != StartupState::Ready)
            {
                return false;
            }
            texture.FrameTextureUse = RasterTextureUse::SampledBoth;
        }
    }
    return true;
}
} // namespace internal
RasterStatus GetTextureState(DeviceHandle device, TextureHandle texture, RasterTextureState& output) noexcept
{
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const auto* record = Resolve(texture, RasterKind::Texture);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (record->Status != RasterStatus::Ready)
    {
        return record->Status == RasterStatus::Pending ? RasterStatus::NotReady : record->Status;
    }
    output = TextureState(*record);
    return RasterStatus::Ready;
}
RasterStatus BeginRasterPass(DeviceHandle device, const RasterPassDescription& description) noexcept
{
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    if (FrameOpen() == false)
    {
        return RasterStatus::InvalidState;
    }
    RasterPassInfo pass;
    const auto valid = ValidatePassDescription(description, pass);
    if (valid != RasterStatus::Ready)
    {
        return valid;
    }
    if (pass.Texture == RASTER_INVALID)
    {
        const auto extent = GetFrameInfo();
        RasterArea area;
        if (RasterResolveArea(description, extent.Width, extent.Height, area) == false)
        {
            return RasterStatus::InvalidDescription;
        }
    }
    if (ValidatePassLoads(pass) == false)
    {
        return RasterStatus::InvalidDescription;
    }
    const auto prepared = backend::RasterPreparePass(pass);
    if (prepared != RasterStatus::Ready)
    {
        return prepared;
    }
    const auto reserved = ReserveFrameSubmission();
    if (reserved != RasterStatus::Ready)
    {
        return reserved;
    }
    if (ClaimRasterFrame() == false)
    {
        return RasterStatus::InvalidState;
    }
    const auto started = StartPass(pass);
    if (started != RasterStatus::Ready)
    {
        internal::FaultRasterSession();
    }
    return started;
}
RasterStatus CreateOrderedGraph(DeviceHandle device, OrderedGraph& output) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    if (GraphAccess::Owner(output) != 0)
    {
        return RasterStatus::InvalidState;
    }
    usize slot = 0;
    const auto reserved = ReserveLifetime(gGraphs, slot, GraphCount());
    if (reserved != RasterStatus::Ready)
    {
        return reserved;
    }
    auto& graph = gGraphs[slot];
    graph.Owner = DeviceOwner();
    graph.Published = true;
    GraphAccess::Set(output, {DeviceOwner(), slot, graph.Generation});
    return RasterStatus::Ready;
}
RasterStatus
ImportGraphBuffer(DeviceHandle device, OrderedGraph handle, BufferHandle buffer, GraphVersion& output) noexcept
{
    GraphRecord* graph = nullptr;
    const auto admission = GraphAdmission(device, handle, graph);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const auto* source = Resolve(buffer, RasterKind::Buffer);
    if (source == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (source->Status != RasterStatus::Ready)
    {
        return source->Status == RasterStatus::Pending ? RasterStatus::NotReady : source->Status;
    }
    const auto added = AddImport(*graph, handle, RasterKind::Buffer, RasterAccess::Slot(buffer), output);
    if (added == RasterStatus::Ready)
    {
        graph->Resources[GraphAccess::Resource(output)].BufferRevision = source->BufferRevision;
    }
    return added;
}
RasterStatus ImportGraphTexture(DeviceHandle device,
                                OrderedGraph handle,
                                TextureHandle texture,
                                const RasterTextureState& state,
                                RasterTextureUse finalUse,
                                GraphVersion& output) noexcept
{
    GraphRecord* graph = nullptr;
    const auto admission = GraphAdmission(device, handle, graph);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const auto* source = Resolve(texture, RasterKind::Texture);
    if (source == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (source->ServiceOwned)
    {
        return RasterStatus::InvalidState;
    }
    if (source->Status != RasterStatus::Ready)
    {
        return source->Status == RasterStatus::Pending ? RasterStatus::NotReady : source->Status;
    }
    if (finalUse < RasterTextureUse::SampledVertex || finalUse > RasterTextureUse::ColorAttachment ||
        (finalUse == RasterTextureUse::ColorAttachment && source->Texture.Attachment == false))
    {
        return RasterStatus::InvalidDescription;
    }
    if (SameTextureState(state, TextureState(*source)) == false)
    {
        return RasterStatus::InvalidState;
    }
    const auto added = AddImport(*graph, handle, RasterKind::Texture, RasterAccess::Slot(texture), output);
    if (added == RasterStatus::Ready)
    {
        graph->Resources[GraphAccess::Resource(output)].Final = finalUse;
    }
    return added;
}
RasterStatus CreateGraphTexture(DeviceHandle device,
                                OrderedGraph handle,
                                const TextureDescription& description,
                                GraphVersion& version,
                                TextureHandle& output) noexcept
{
    GraphRecord* graph = nullptr;
    const auto admission = GraphAdmission(device, handle, graph);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    if (GraphAccess::Null(version) == false || RasterAccess::Owner(output) != 0)
    {
        return RasterStatus::InvalidState;
    }
    if (graph->ResourceCount == GraphResources())
    {
        return RasterStatus::CapacityExceeded;
    }
    if (description.Attachment == false)
    {
        return RasterStatus::InvalidDescription;
    }
    if (graph->Resources.TryResize(graph->ResourceCount + 1) == false ||
        graph->Report.Resources.TryResize(graph->ResourceCount + 1) == false ||
        gGraphPool.EnsureSlots(GraphPool()) == false)
    {
        return StorageFailure();
    }
    const auto completed = backend::RasterCompleted();
    const auto current = Admission(device);
    if (current != RasterStatus::Ready)
    {
        return current;
    }
    usize freePool = GraphPool(), evictPool = GraphPool();
    for (usize i = 0; i < GraphPool(); ++i)
    {
        const auto& pool = gGraphPool[i];
        if (pool.Occupied == false)
        {
            if (freePool == GraphPool())
            {
                freePool = i;
            }
            continue;
        }
        auto& texture = At(RasterKind::Texture, pool.Texture);
        const bool idle = texture.References == 1 && texture.LastUse <= completed && texture.Callbacks == 0 &&
                          texture.Status != RasterStatus::Pending;
        if (idle && evictPool == GraphPool())
        {
            evictPool = i;
        }
        if (idle && texture.Status == RasterStatus::Ready && texture.Generation != 0 &&
            SameTextureDescription(texture.Texture, description))
        {
            texture.Published = true;
            texture.ColorDefined = texture.DepthDefined = texture.StencilDefined = false;
            for (auto& state : texture.AttachmentState)
            {
                state = 0;
            }
            RasterAccess::Set(output, {DeviceOwner(), pool.Texture, texture.Generation});
            return AddImport(*graph, handle, RasterKind::Texture, pool.Texture, version, true);
        }
    }
    if (freePool == GraphPool() && evictPool != GraphPool())
    {
        freePool = evictPool;
        Release(RasterKind::Texture, gGraphPool[freePool].Texture);
        gGraphPool[freePool] = {};
        Collect();
    }
    if (freePool == GraphPool())
    {
        return RasterStatus::CapacityExceeded;
    }
    const auto created = CreateTexture(device, description, {}, output);
    if (created != RasterStatus::Ready && created != RasterStatus::Pending)
    {
        return created;
    }
    const auto slot = RasterAccess::Slot(output);
    At(RasterKind::Texture, slot).ServiceOwned = true;
    gGraphPool[freePool] = {true, slot};
    (void)AddImport(*graph, handle, RasterKind::Texture, slot, version, true);
    return created;
}
RasterStatus
NextGraphVersion(DeviceHandle device, OrderedGraph handle, GraphVersion previous, GraphVersion& output) noexcept
{
    GraphRecord* graph = nullptr;
    const auto admission = GraphAdmission(device, handle, graph);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    if (ValidVersion(*graph, handle, previous) == false)
    {
        return RasterStatus::InvalidHandle;
    }
    if (GraphAccess::Null(output) == false)
    {
        return RasterStatus::InvalidState;
    }
    auto& resource = graph->Resources[GraphAccess::Resource(previous)];
    const auto& physical = At(resource.Kind, resource.Slot);
    const bool mutableResource =
        resource.Kind == RasterKind::Texture ? physical.Texture.Attachment : StorageRole(physical.Buffer.Role);
    if (mutableResource == false || GraphAccess::Version(previous) != resource.Versions)
    {
        return RasterStatus::InvalidDescription;
    }
    if (resource.Versions == GraphPasses())
    {
        return RasterStatus::CapacityExceeded;
    }
    graph->ReportAvailable = false;
    GraphAccess::Set(output, handle, {GraphAccess::Resource(previous), ++resource.Versions});
    return RasterStatus::Ready;
}
RasterStatus AddGraphPass(DeviceHandle device, OrderedGraph handle, const GraphPassDescription& description) noexcept
{
    GraphRecord* graph = nullptr;
    const auto admission = GraphAdmission(device, handle, graph);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    if (graph->PassCount == GraphPasses() ||
        description.DrawCount > CurrentStorageLimits().DrawsPerPlan - graph->DrawCount ||
        description.UseCount > GraphResources())
    {
        return RasterStatus::CapacityExceeded;
    }
    if ((description.DrawCount != 0 && description.Draws == nullptr) ||
        (description.UseCount != 0 && description.Uses == nullptr))
    {
        return RasterStatus::InvalidDescription;
    }
    GraphPassRecord pass;
    char name[64]{};
    char source[128]{};
    if (CopyGraphText(description.Name, name, sizeof(name), true) == false ||
        CopyGraphText(description.Source, source, sizeof(source), false) == false)
    {
        return RasterStatus::InvalidDescription;
    }
    pass.IsCompute = description.Dispatch != nullptr;
    if (pass.IsCompute)
    {
        if (description.DrawCount != 0 || RasterAccess::Owner(description.Attachment.Color) != 0 ||
            (description.Attachment.ColorCount == 0) == false)
        {
            return RasterStatus::InvalidDescription;
        }
        const auto valid = ValidateCompute(device, *description.Dispatch, pass.Dispatch);
        if (valid != RasterStatus::Ready)
        {
            return valid;
        }
    }
    else
    {
        const auto valid = ValidatePassDescription(description.Attachment, pass.Info);
        if (valid != RasterStatus::Ready)
        {
            return valid;
        }
    }
    if (pass.Uses.TryResize(description.UseCount) == false)
    {
        return StorageFailure();
    }
    for (usize i = 0; i < description.UseCount; ++i)
    {
        const auto& use = description.Uses[i];
        if (ValidVersion(*graph, handle, use.Resource) == false)
        {
            return RasterStatus::InvalidHandle;
        }
        if (static_cast<uint8>(use.Access) > 8 || static_cast<uint8>(use.Visibility) < 1 ||
            static_cast<uint8>(use.Visibility) > 4)
        {
            return RasterStatus::InvalidDescription;
        }
        pass.Uses[i] = use;
    }
    StorageArray<RasterPacket> packets;
    if (packets.TryResize(description.DrawCount) == false)
    {
        return StorageFailure();
    }
    for (usize i = 0; i < description.DrawCount; ++i)
    {
        const auto draw = ValidateDraw(device, description.Draws[i], packets[i]);
        if (draw != RasterStatus::Ready)
        {
            return draw;
        }
    }
    pass.UseCount = description.UseCount;
    pass.FirstDraw = graph->DrawCount;
    pass.DrawCount = description.DrawCount;
    pass.SideEffect = description.SideEffect;
    if (graph->Passes.TryResize(graph->PassCount + 1) == false ||
        graph->Report.Passes.TryResize(graph->PassCount + 1) == false ||
        graph->Draws.TryResize(graph->DrawCount + description.DrawCount) == false)
    {
        return StorageFailure();
    }
    graph->ReportAvailable = false;
    PassViews(pass.Info, true);
    const auto index = graph->PassCount++;
    graph->Passes[index] = Move(pass);
    if (pass.IsCompute)
    {
        ComputeReferences(pass.Dispatch, true);
    }
    std::memcpy(graph->Report.Passes[index].Name, name, sizeof(name));
    std::memcpy(graph->Report.Passes[index].Source, source, sizeof(source));
    graph->Report.Passes[index].Line = description.Line;
    for (usize i = 0; i < description.DrawCount; ++i)
    {
        PacketReferences(packets[i], true);
        graph->Draws[graph->DrawCount++] = packets[i];
    }
    return RasterStatus::Ready;
}
RasterStatus AddGraphRoot(DeviceHandle device, OrderedGraph handle, GraphVersion version, GraphRoot root) noexcept
{
    GraphRecord* graph = nullptr;
    const auto admission = GraphAdmission(device, handle, graph);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    if (ValidVersion(*graph, handle, version) == false)
    {
        return RasterStatus::InvalidHandle;
    }
    if (static_cast<uint8>(root) > 2)
    {
        return RasterStatus::InvalidDescription;
    }
    auto& resource = graph->Resources[GraphAccess::Resource(version)];
    if (resource.Transient)
    {
        return RasterStatus::InvalidDescription;
    }
    if (resource.Root && resource.RootVersion != GraphAccess::Version(version))
    {
        return RasterStatus::InvalidState;
    }
    graph->ReportAvailable = false;
    resource.Root = true;
    resource.RootVersion = GraphAccess::Version(version);
    return RasterStatus::Ready;
}
namespace
{
bool IsWrite(GraphAccessMode access) noexcept
{
    return access == GraphAccessMode::ColorWrite || access == GraphAccessMode::ColorReadWrite ||
           access == GraphAccessMode::StorageReadWrite;
}
const GraphUse* FindUse(const GraphRecord& graph,
                        const GraphPassRecord& pass,
                        RasterKind kind,
                        usize slot,
                        GraphAccessMode access,
                        usize offset = 0,
                        usize size = 0,
                        uint8 visibility = 0) noexcept
{
    for (usize i = 0; i < pass.UseCount; ++i)
    {
        const auto& use = pass.Uses[i];
        const auto& resource = graph.Resources[GraphAccess::Resource(use.Resource)];
        if (resource.Kind == kind && resource.Slot == slot && use.Access == access && offset >= use.Offset &&
            Fits(offset - use.Offset, size, use.Size) &&
            (static_cast<uint8>(use.Visibility) & visibility) == visibility)
        {
            return &use;
        }
    }
    return nullptr;
}
RasterStatus
CheckDeclaredPacket(const GraphRecord& graph, const GraphPassRecord& pass, const RasterPacket& packet) noexcept
{
    const auto valid = ValidatePacketPass(packet, pass.Info, false);
    if (valid != RasterStatus::Ready)
    {
        return valid;
    }
    const auto& pipeline = At(RasterKind::Pipeline, packet.Pipeline).Pipeline;
    for (usize i = 0; i < pipeline.StreamCount; ++i)
    {
        if (FindUse(graph,
                    pass,
                    RasterKind::Buffer,
                    packet.Vertices[i],
                    GraphAccessMode::Vertex,
                    packet.Offsets[i],
                    packet.Sizes[i]) == nullptr)
        {
            return RasterStatus::InvalidDescription;
        }
    }
    const auto indexSize = static_cast<usize>(packet.IndexCount) * (packet.Index32 ? 4 : 2);
    if (FindUse(graph,
                pass,
                RasterKind::Buffer,
                packet.Indices,
                GraphAccessMode::Index,
                packet.IndexOffset,
                indexSize) == nullptr)
    {
        return RasterStatus::InvalidDescription;
    }
    if (packet.Indirect != RASTER_INVALID && FindUse(graph,
                                                     pass,
                                                     RasterKind::Buffer,
                                                     packet.Indirect,
                                                     GraphAccessMode::Indirect,
                                                     packet.IndirectOffset,
                                                     sizeof(IndexedIndirectArguments)) == nullptr)
    {
        return RasterStatus::InvalidDescription;
    }
    const auto& set = At(RasterKind::Set, packet.Set).Set;
    const RasterLayout& layout = set.Description;
    bool hasRuntimeBinding = false;
    for (usize entry = 0; entry < layout.Count; ++entry)
    {
        hasRuntimeBinding = hasRuntimeBinding == true || layout.Entries[entry].IsRuntimeSized == true;
    }
    for (usize i = 0; i < layout.DescriptorCount; ++i)
    {
        const RasterBinding& binding = layout.GetDescriptorBinding(i);
        // Runtime length is immutable RHI-owned state, retained by the snapshot.
        // It has no public handle and cannot be a consumer-authored graph import.
        if (binding.Kind == RasterBindingKind::Sampler ||
            (binding.Binding == RUNTIME_BINDING_LENGTH_SLOT && hasRuntimeBinding == true))
        {
            continue;
        }
        const bool isBuffer = BufferBinding(binding.Kind);
        const usize slot = isBuffer == true ? set.Slots[i] : At(RasterKind::View, set.Slots[i]).TextureSlot;
        const GraphAccessMode access = binding.Kind == RasterBindingKind::UniformBuffer ? GraphAccessMode::Uniform
                                       : binding.Kind == RasterBindingKind::StorageRead ? GraphAccessMode::StorageRead
                                                                                        : GraphAccessMode::Sampled;
        if (FindUse(graph,
                    pass,
                    isBuffer == true ? RasterKind::Buffer : RasterKind::Texture,
                    slot,
                    access,
                    isBuffer == true ? set.Offsets[i] + packet.BindingOffsets.Bytes[binding.Binding] : 0,
                    isBuffer == true ? set.Sizes[i] : 0,
                    static_cast<uint8>(binding.Visibility)) == nullptr)
        {
            return RasterStatus::InvalidDescription;
        }
    }
    return RasterStatus::Ready;
}
struct GraphTrack final
{
    uint32 Version = 0;
    uint32 Writer = GRAPH_EXTERNAL_INDEX;
    StorageArray<bool> Readers;
    bool Defined = false;
    bool DepthDefined = false;
    bool InitializedViews = false;
    StorageArray<uint8> States;
    GraphAccessMode LastWrite = GraphAccessMode::ColorWrite;
    StorageArray<GraphAccessMode> ReadAccess;
};
bool AddDependency(FullGraphReport& report,
                   usize before,
                   usize after,
                   usize resource,
                   GraphHazard hazard,
                   GraphAccessMode source,
                   GraphAccessMode destination) noexcept
{
    if (report.Summary.DependencyCount == GraphDependencies())
    {
        StorageCapacityFailed();
        return false;
    }
    if (report.Dependencies.TryResize(report.Summary.DependencyCount + 1) == false)
    {
        return false;
    }
    report.Dependencies[report.Summary.DependencyCount++] = {static_cast<uint32>(before),
                                                             static_cast<uint32>(after),
                                                             static_cast<uint32>(resource),
                                                             hazard,
                                                             source,
                                                             destination};
    return true;
}
RasterStatus CompileGraph(GraphRecord& graph, FullGraphReport& report, bool reference) noexcept
{
    StorageArray<GraphTrack> tracks;
    StorageArray<bool> live;
    bool surfaceColor = true, surfaceDepth = false;
    StorageArray<bool> imported;
    if (tracks.TryResize(graph.ResourceCount) == false || live.TryResize(graph.PassCount) == false ||
        imported.TryResize(graph.ResourceCount) == false)
    {
        return StorageFailure();
    }
    for (auto& track : tracks)
    {
        if (track.Readers.TryResize(graph.PassCount) == false || track.ReadAccess.TryResize(graph.PassCount) == false)
        {
            return StorageFailure();
        }
    }
    for (usize r = 0; r < graph.ResourceCount; ++r)
    {
        const auto& resource = graph.Resources[r];
        const auto& physical = At(resource.Kind, resource.Slot);
        if (physical.Status != RasterStatus::Ready)
        {
            report.Summary.ErrorResource = static_cast<uint32>(r);
            return physical.Status;
        }
        if (resource.Kind == RasterKind::Texture && SameTextureState(resource.Import, TextureState(physical)) == false)
        {
            report.Summary.ErrorResource = static_cast<uint32>(r);
            return RasterStatus::InvalidState;
        }
        if (resource.Kind == RasterKind::Buffer && resource.BufferRevision != physical.BufferRevision)
        {
            report.Summary.ErrorResource = static_cast<uint32>(r);
            return RasterStatus::InvalidState;
        }
        tracks[r].Defined =
            resource.Kind == RasterKind::Buffer || (resource.Transient == false && resource.Import.ColorDefined);
        // Sampling is restricted to retained initialized views. Whole-allocation
        // roots/load operations must still require complete defined contents.
        tracks[r].InitializedViews = resource.Kind == RasterKind::Texture && physical.Texture.Attachment == false &&
                                     physical.InitializedMips != 0;
        tracks[r].DepthDefined = resource.Transient == false && resource.Import.DepthDefined;
        if (physical.Texture.Attachment && resource.Kind == RasterKind::Texture)
        {
            if (tracks[r].States.TryResize(physical.AttachmentState.GetSize()) == false)
            {
                return StorageFailure();
            }
            for (usize i = 0; i < physical.AttachmentState.GetSize(); ++i)
            {
                tracks[r].States[i] = resource.Transient ? 0 : physical.AttachmentState[i];
            }
            for (auto value : tracks[r].States)
            {
                tracks[r].InitializedViews |= value != 0;
            }
        }
    }
    for (usize p = 0; p < graph.PassCount; ++p)
    {
        report.Summary.ErrorPass = static_cast<uint32>(p);
        const auto& pass = graph.Passes[p];
        live[p] = reference || pass.SideEffect || (pass.IsCompute == false && pass.Info.Texture == RASTER_INVALID);
        bool attachment = pass.IsCompute == false && pass.Info.Texture == RASTER_INVALID;
        if (attachment)
        {
            const auto& state = pass.Info.Description;
            if ((state.ColorLoad == RasterLoad::Load && surfaceColor == false) ||
                (state.DepthLoad == RasterLoad::Load && surfaceDepth == false))
            {
                return RasterStatus::InvalidDescription;
            }
            surfaceColor =
                state.ColorStore == RasterStore::Store &&
                (state.ColorLoad == RasterLoad::Clear || (state.ColorLoad == RasterLoad::Load && surfaceColor));
            surfaceDepth =
                state.DepthStore == RasterStore::Store &&
                (state.DepthLoad == RasterLoad::Clear || (state.DepthLoad == RasterLoad::Load && surfaceDepth));
        }
        for (usize i = 0; i < pass.UseCount; ++i)
        {
            const auto& use = pass.Uses[i];
            const auto r = GraphAccess::Resource(use.Resource);
            report.Summary.ErrorResource = static_cast<uint32>(r);
            const auto& resource = graph.Resources[r];
            auto& track = tracks[r];
            for (usize j = 0; j < i; ++j)
            {
                if (r == GraphAccess::Resource(pass.Uses[j].Resource))
                {
                    return RasterStatus::InvalidDescription;
                }
            }
            const bool write = IsWrite(use.Access);
            if (imported[r] == false && resource.Kind == RasterKind::Texture &&
                resource.Import.Use != RasterTextureUse::Undefined)
            {
                const bool priorWrite = resource.Import.Use == RasterTextureUse::ColorAttachment;
                if ((priorWrite || write) &&
                    AddDependency(report,
                                  GRAPH_EXTERNAL_INDEX,
                                  p,
                                  r,
                                  priorWrite ? (write ? GraphHazard::WriteAfterWrite : GraphHazard::ReadAfterWrite)
                                             : GraphHazard::WriteAfterRead,
                                  priorWrite ? GraphAccessMode::ColorWrite : GraphAccessMode::Sampled,
                                  use.Access) == false)
                {
                    return StorageFailure();
                }
            }
            imported[r] = true;
            if (resource.Kind == RasterKind::Buffer)
            {
                const auto& buffer = At(resource.Kind, resource.Slot).Buffer;
                if (use.Size == 0 || Fits(use.Offset, use.Size, buffer.Size) == false ||
                    (use.Access == GraphAccessMode::Vertex && buffer.Role != BufferRole::Vertex &&
                     buffer.Role != BufferRole::StorageVertex) ||
                    (use.Access == GraphAccessMode::Index && buffer.Role != BufferRole::Index16 &&
                     buffer.Role != BufferRole::Index32) ||
                    (use.Access == GraphAccessMode::Uniform && buffer.Role != BufferRole::Uniform) ||
                    (use.Access == GraphAccessMode::Indirect && buffer.Role != BufferRole::StorageIndirect) ||
                    ((use.Access == GraphAccessMode::StorageRead || use.Access == GraphAccessMode::StorageReadWrite) &&
                     (StorageRole(buffer.Role) == false ||
                      (pass.IsCompute == true ? use.Visibility != RasterVisibility::Compute
                                              : use.Access != GraphAccessMode::StorageRead ||
                                                    use.Visibility != RasterVisibility::Fragment))) ||
                    (pass.IsCompute && use.Access != GraphAccessMode::Uniform &&
                     use.Access != GraphAccessMode::StorageRead && use.Access != GraphAccessMode::StorageReadWrite) ||
                    (use.Access == GraphAccessMode::Sampled || use.Access == GraphAccessMode::ColorWrite ||
                     use.Access == GraphAccessMode::ColorReadWrite))
                {
                    return RasterStatus::InvalidDescription;
                }
            }
            else if (pass.IsCompute || use.Offset != 0 || use.Size != 0 ||
                     use.Visibility == RasterVisibility::Compute ||
                     (use.Access != GraphAccessMode::Sampled && use.Access != GraphAccessMode::ColorWrite &&
                      use.Access != GraphAccessMode::ColorReadWrite))
            {
                return RasterStatus::InvalidDescription;
            }
            const auto version = GraphAccess::Version(use.Resource);
            if (write == false)
            {
                const bool initialized =
                    track.Defined || (use.Access == GraphAccessMode::Sampled && track.InitializedViews);
                if (version != track.Version || initialized == false)
                {
                    return RasterStatus::InvalidDescription;
                }
                if (track.Writer != GRAPH_EXTERNAL_INDEX && AddDependency(report,
                                                                          track.Writer,
                                                                          p,
                                                                          r,
                                                                          GraphHazard::ReadAfterWrite,
                                                                          track.LastWrite,
                                                                          use.Access) == false)
                {
                    return StorageFailure();
                }
                track.Readers[p] = true;
                track.ReadAccess[p] = use.Access;
                continue;
            }
            bool resolve = false;
            if (version != track.Version + 1 || version == 0 ||
                (resource.Kind == RasterKind::Texture && PassAttachment(pass.Info, resource.Slot, resolve) == nullptr))
            {
                return RasterStatus::InvalidDescription;
            }
            auto description = pass.Info.Description;
            const auto* selected = PassAttachment(pass.Info, resource.Slot, resolve);
            if (selected)
            {
                description.ColorLoad = resolve ? RasterLoad::Discard : selected->Load;
                description.ColorStore = resolve ? RasterStore::Store : selected->Store;
            }
            const bool first = resource.Slot == pass.Info.Texture;
            if (first == false)
            {
                description.DepthLoad = RasterLoad::Discard;
                description.DepthStore = RasterStore::Discard;
            }
            const bool bufferWrite = resource.Kind == RasterKind::Buffer;
            const bool load = bufferWrite || description.ColorLoad == RasterLoad::Load;
            if (bufferWrite)
            {
                if (track.Defined == false)
                {
                    return RasterStatus::InvalidDescription;
                }
            }
            else
            {
                attachment = true;
                const auto& texture = At(RasterKind::Texture, resource.Slot);
                const auto index = SubresourceIndex(texture.Texture,
                                                    resolve ? selected->ResolveMip : selected->Mip,
                                                    resolve ? selected->ResolveLayer : selected->Layer);
                const auto value = track.States[index];
                if (load != (use.Access == GraphAccessMode::ColorReadWrite) || (load && (value & COLOR_DEFINED) == 0) ||
                    (description.DepthLoad == RasterLoad::Load && (value & DEPTH_DEFINED) == 0) ||
                    (first && pass.Info.Stencil && description.StencilLoad == RasterLoad::Load &&
                     (value & STENCIL_DEFINED) == 0))
                {
                    return RasterStatus::InvalidDescription;
                }
            }
            if ((load || (bufferWrite == false && description.DepthLoad == RasterLoad::Load)) &&
                track.Writer != GRAPH_EXTERNAL_INDEX &&
                AddDependency(report, track.Writer, p, r, GraphHazard::ReadAfterWrite, track.LastWrite, use.Access) ==
                    false)
            {
                return StorageFailure();
            }
            if (track.Writer != GRAPH_EXTERNAL_INDEX &&
                AddDependency(report, track.Writer, p, r, GraphHazard::WriteAfterWrite, track.LastWrite, use.Access) ==
                    false)
            {
                return StorageFailure();
            }
            for (usize reader = 0; reader < p; ++reader)
            {
                if (track.Readers[reader] && AddDependency(report,
                                                           reader,
                                                           p,
                                                           r,
                                                           GraphHazard::WriteAfterRead,
                                                           track.ReadAccess[reader],
                                                           use.Access) == false)
                {
                    return StorageFailure();
                }
            }
            for (auto& reader : track.Readers)
            {
                reader = false;
            }
            track.Writer = static_cast<uint32>(p);
            track.LastWrite = use.Access;
            track.Version = version;
            if (bufferWrite == false)
            {
                const auto& texture = At(RasterKind::Texture, resource.Slot);
                auto& value = track.States[SubresourceIndex(texture.Texture,
                                                            resolve ? selected->ResolveMip : selected->Mip,
                                                            resolve ? selected->ResolveLayer : selected->Layer)];
                if (resolve)
                {
                    // Resolve initialization follows the source's pre-store contents.
                    const auto* sourceUse = FindUse(graph,
                                                    pass,
                                                    RasterKind::Texture,
                                                    selected->Texture,
                                                    selected->Load == RasterLoad::Load ? GraphAccessMode::ColorReadWrite
                                                                                       : GraphAccessMode::ColorWrite);
                    if (sourceUse == nullptr || selected->Load == RasterLoad::Discard)
                    {
                        return RasterStatus::InvalidDescription;
                    }
                    value = static_cast<uint8>(value | COLOR_DEFINED);
                }
                else
                {
                    value = ApplyAspect(value, COLOR_DEFINED, selected->Load, selected->Store);
                    if (first)
                    {
                        value = ApplyAspect(value, DEPTH_DEFINED, description.DepthLoad, description.DepthStore);
                        if (pass.Info.Stencil)
                        {
                            value =
                                ApplyAspect(value, STENCIL_DEFINED, description.StencilLoad, description.StencilStore);
                        }
                    }
                }
                track.Defined = AllDefined(track.States, COLOR_DEFINED);
                track.DepthDefined = AllDefined(track.States, DEPTH_DEFINED);
                track.InitializedViews = false;
                for (auto state : track.States)
                {
                    track.InitializedViews |= state != 0;
                }
            }
            else
            {
                track.Defined =
                    bufferWrite || (description.ColorStore == RasterStore::Store &&
                                    (description.ColorLoad == RasterLoad::Clear || (load && track.Defined)));
            }
            if (bufferWrite)
            {
                track.DepthDefined = description.DepthStore == RasterStore::Store &&
                                     (description.DepthLoad == RasterLoad::Clear ||
                                      (description.DepthLoad == RasterLoad::Load && track.DepthDefined));
            }
        }
        if (pass.IsCompute == false && pass.Info.Texture != RASTER_INVALID)
        {
            for (usize i = 0; i < PassColorCount(pass.Info); ++i)
            {
                const auto& c = pass.Info.Colors[i];
                if (FindUse(graph,
                            pass,
                            RasterKind::Texture,
                            c.Texture,
                            c.Load == RasterLoad::Load ? GraphAccessMode::ColorReadWrite
                                                       : GraphAccessMode::ColorWrite) == nullptr ||
                    (c.ResolveTexture != RASTER_INVALID &&
                     FindUse(graph, pass, RasterKind::Texture, c.ResolveTexture, GraphAccessMode::ColorWrite) ==
                         nullptr))
                {
                    return RasterStatus::InvalidDescription;
                }
            }
        }
        for (usize d = 0; d < pass.DrawCount; ++d)
        {
            const auto& set = At(RasterKind::Set, graph.Draws[pass.FirstDraw + d].Set).Set;
            const RasterLayout& layout = set.Description;
            for (usize b = 0; b < layout.DescriptorCount; ++b)
            {
                if (TextureBinding(layout.GetDescriptorBinding(b).Kind))
                {
                    const auto& view = At(RasterKind::View, set.Slots[b]);
                    const auto& texture = At(RasterKind::Texture, view.TextureSlot);
                    if (texture.Texture.Attachment)
                    {
                        for (usize r = 0; r < graph.ResourceCount; ++r)
                        {
                            if (graph.Resources[r].Kind == RasterKind::Texture &&
                                graph.Resources[r].Slot == view.TextureSlot &&
                                ViewDefined(texture, view.View, tracks[r].States) == false)
                            {
                                return RasterStatus::InvalidDescription;
                            }
                        }
                    }
                }
            }
        }
        if (attachment == false && pass.IsCompute == false)
        {
            return RasterStatus::InvalidDescription;
        }
        report.Summary.ErrorResource = GRAPH_EXTERNAL_INDEX;
        if (pass.IsCompute)
        {
            const auto& set = At(RasterKind::Set, pass.Dispatch.Set).Set;
            const RasterLayout& layout = set.Description;
            for (usize i = 0; i < layout.DescriptorCount; ++i)
            {
                const RasterBinding& binding = layout.GetDescriptorBinding(i);
                // Immutable scenes enter bindings only after their build completes.
                // The retained snapshot owns their TLAS/BLAS through submission.
                if (binding.Kind == RasterBindingKind::RayScene)
                {
                    continue;
                }
                const auto access = binding.Kind == RasterBindingKind::UniformBuffer ? GraphAccessMode::Uniform
                                    : binding.Kind == RasterBindingKind::StorageRead
                                        ? GraphAccessMode::StorageRead
                                        : GraphAccessMode::StorageReadWrite;
                if (FindUse(graph,
                            pass,
                            RasterKind::Buffer,
                            set.Slots[i],
                            access,
                            set.Offsets[i] + pass.Dispatch.BindingOffsets.Bytes[binding.Binding],
                            set.Sizes[i],
                            4) == nullptr)
                {
                    return RasterStatus::InvalidDescription;
                }
            }
        }
        for (usize i = 0; i < pass.DrawCount; ++i)
        {
            const auto valid = CheckDeclaredPacket(graph, pass, graph.Draws[pass.FirstDraw + i]);
            if (valid != RasterStatus::Ready)
            {
                return valid;
            }
        }
    }
    report.Summary.ErrorPass = GRAPH_EXTERNAL_INDEX;
    if (surfaceColor == false)
    {
        return RasterStatus::InvalidDescription;
    }
    for (usize r = 0; r < graph.ResourceCount; ++r)
    {
        report.Summary.ErrorResource = static_cast<uint32>(r);
        const auto& resource = graph.Resources[r];
        const auto& track = tracks[r];
        if (resource.Root)
        {
            if (track.Defined == false || resource.RootVersion != track.Version)
            {
                return RasterStatus::InvalidDescription;
            }
            if (track.Writer != GRAPH_EXTERNAL_INDEX)
            {
                live[track.Writer] = true;
            }
        }
    }
    // Only content dependencies retain producers. Pure overwritten writes and
    // anti-dependencies cannot make unobservable work a root.
    for (usize p = graph.PassCount; p-- > 0;)
    {
        if (live[p] == false)
        {
            continue;
        }
        for (usize i = 0; i < report.Summary.DependencyCount; ++i)
        {
            const auto& edge = report.Dependencies[i];
            if (edge.After == p && edge.Before < p && edge.Hazard == GraphHazard::ReadAfterWrite)
            {
                live[edge.Before] = true;
            }
        }
    }
    for (usize p = 0; p < graph.PassCount; ++p)
    {
        report.Passes[p].Culled = live[p] == false;
        if (live[p] == false)
        {
            continue;
        }
        ++report.Summary.LivePasses;
        const auto& pass = graph.Passes[p];
        for (usize i = 0; i < pass.UseCount; ++i)
        {
            const auto r = GraphAccess::Resource(pass.Uses[i].Resource);
            if (report.Resources[r].FirstUse == GRAPH_EXTERNAL_INDEX)
            {
                report.Resources[r].FirstUse = static_cast<uint32>(p);
            }
            report.Resources[r].LastUse = static_cast<uint32>(p);
        }
    }
    for (usize r = 0; r < graph.ResourceCount; ++r)
    {
        const auto& resource = graph.Resources[r];
        if (resource.Kind != RasterKind::Texture || report.Resources[r].LastUse == GRAPH_EXTERNAL_INDEX)
        {
            continue;
        }
        const auto p = report.Resources[r].LastUse;
        const auto& pass = graph.Passes[p];
        for (usize i = 0; i < pass.UseCount; ++i)
        {
            const auto& use = pass.Uses[i];
            if (GraphAccess::Resource(use.Resource) != r)
            {
                continue;
            }
            const bool write = IsWrite(use.Access), finalWrite = resource.Final == RasterTextureUse::ColorAttachment;
            if ((write || finalWrite) &&
                AddDependency(report,
                              p,
                              GRAPH_EXTERNAL_INDEX,
                              r,
                              write ? (finalWrite ? GraphHazard::WriteAfterWrite : GraphHazard::ReadAfterWrite)
                                    : GraphHazard::WriteAfterRead,
                              use.Access,
                              finalWrite ? GraphAccessMode::ColorWrite : GraphAccessMode::Sampled) == false)
            {
                report.Summary.ErrorResource = static_cast<uint32>(r);
                return StorageFailure();
            }
        }
    }
    report.Summary.ErrorResource = GRAPH_EXTERNAL_INDEX;
    return RasterStatus::Ready;
}
} // namespace
namespace
{
bool SameGraphRectangle(const RasterRectangle& a, const RasterRectangle& b) noexcept
{
    return a.X == b.X && a.Y == b.Y && a.Width == b.Width && a.Height == b.Height;
}
// Thanks to Khronos, Vulkan Specification, "Fragment Operations", rasterization
// order (https://docs.vulkan.org/spec/latest/chapters/fragops.html), and W3C/GPUWeb,
// WebGPU, "Resource Usages" (https://www.w3.org/TR/webgpu/#resource-usages).
// Only ordinary attachment operations combine; identical read scopes avoid new
// cross-pass resource hazards. No barrier is issued inside the merged scope.
// See docs/architecture/rhi-gdi.md, "Backend lowering and optional optimization".
bool CanMergeRaster(const GraphPassRecord& before, const GraphPassRecord& after) noexcept
{
    if (before.Info.ColorCount != 0 || after.Info.ColorCount != 0)
    {
        return false;
    }

    const auto& a = before.Info.Description;
    const auto& b = after.Info.Description;
    if (before.IsCompute || after.IsCompute || before.SideEffect || after.SideEffect ||
        before.Info.Texture != after.Info.Texture || b.ColorLoad != RasterLoad::Load ||
        b.DepthLoad != RasterLoad::Load || a.ColorStore != RasterStore::Store || b.ColorStore != RasterStore::Store ||
        a.DepthStore != RasterStore::Store || b.DepthStore != RasterStore::Store ||
        a.DepthReadOnly != b.DepthReadOnly || a.UseViewport != b.UseViewport || a.UseScissor != b.UseScissor ||
        (a.UseViewport && SameGraphRectangle(a.Viewport, b.Viewport) == false) ||
        (a.UseScissor && SameGraphRectangle(a.Scissor, b.Scissor) == false) || before.UseCount != after.UseCount)
    {
        return false;
    }
    for (usize i = 0; i < before.UseCount; ++i)
    {
        const auto& x = before.Uses[i];
        const auto& y = after.Uses[i];
        if (GraphAccess::Resource(x.Resource) != GraphAccess::Resource(y.Resource))
        {
            return false;
        }
        if (IsWrite(x.Access) && y.Access == GraphAccessMode::ColorReadWrite &&
            (x.Access == GraphAccessMode::ColorWrite || x.Access == GraphAccessMode::ColorReadWrite))
        {
            continue; // Compiler already proved the consecutive attachment version chain.
        }
        if (x.Access != y.Access || GraphAccess::Version(x.Resource) != GraphAccess::Version(y.Resource) ||
            x.Offset != y.Offset || x.Size != y.Size || x.Visibility != y.Visibility || IsWrite(x.Access))
        {
            return false;
        }
    }
    return true;
}
void CopyGraphSummary(const GraphRecord& graph, GraphReport& output) noexcept
{
    output = graph.Report.Summary;
    output.ResourceCount = static_cast<uint32>(graph.ResourceCount);
    output.ReportedPasses = static_cast<uint32>(graph.PassCount < 32 ? graph.PassCount : 32);
    output.ReportedResources = static_cast<uint32>(graph.ResourceCount < 16 ? graph.ResourceCount : 16);
    output.ReportedDependencies = output.DependencyCount < 512 ? output.DependencyCount : 512;
    output.DiagnosticsTruncated = graph.PassCount > 32 || graph.ResourceCount > 16 || output.DependencyCount > 512;
    for (usize i = 0; i < output.ReportedPasses; ++i)
    {
        const auto& pass = graph.Report.Passes[i];
        std::memcpy(output.Names[i], pass.Name, sizeof(pass.Name));
        std::memcpy(output.Sources[i], pass.Source, sizeof(pass.Source));
        output.Lines[i] = pass.Line;
        output.Culled[i] = pass.Culled;
        output.RasterGroup[i] = pass.RasterGroup;
    }
    for (usize i = 0; i < output.ReportedResources; ++i)
    {
        output.FirstUse[i] = graph.Report.Resources[i].FirstUse;
        output.LastUse[i] = graph.Report.Resources[i].LastUse;
    }
    for (usize i = 0; i < output.ReportedDependencies; ++i)
    {
        output.Dependencies[i] = graph.Report.Dependencies[i];
    }
}
void PlanRasterGroups(const GraphRecord& graph, FullGraphReport& report, bool merge) noexcept
{
    report.Summary.RasterPasses = report.Summary.ComputePasses = report.Summary.MergedPasses =
        report.Summary.DrawCount = 0;
    report.Summary.PlanStorageBytes =
        sizeof(GraphRecord) + graph.Resources.GetCapacity() * sizeof(GraphResourceRecord) +
        graph.Passes.GetCapacity() * sizeof(GraphPassRecord) + graph.Draws.GetCapacity() * sizeof(RasterPacket) +
        report.Passes.GetCapacity() * sizeof(GraphPassDiagnostic) +
        report.Resources.GetCapacity() * sizeof(GraphResourceDiagnostic) +
        report.Dependencies.GetCapacity() * sizeof(GraphDependency);
    for (const auto& pass : graph.Passes)
    {
        report.Summary.PlanStorageBytes += pass.Uses.GetCapacity() * sizeof(GraphUse);
    }
    report.Summary.BufferBytes = report.Summary.TextureBytes = 0;
    for (usize r = 0; r < graph.ResourceCount; ++r)
    {
        const auto& resource = graph.Resources[r];
        const auto& physical = At(resource.Kind, resource.Slot);
        if (resource.Kind == RasterKind::Buffer)
        {
            report.Summary.BufferBytes += physical.Buffer.Size;
        }
        else
        {
            report.Summary.TextureBytes += physical.StorageBytes;
        }
    }
    for (usize p = 0; p < graph.PassCount; ++p)
    {
        report.Passes[p].RasterGroup = GRAPH_EXTERNAL_INDEX;
        if (report.Passes[p].Culled)
        {
            continue;
        }
        const auto& pass = graph.Passes[p];
        report.Summary.DrawCount += static_cast<uint32>(pass.DrawCount);
        if (pass.IsCompute)
        {
            ++report.Summary.ComputePasses;
        }
        else if (merge && p != 0 && report.Passes[p - 1].RasterGroup != GRAPH_EXTERNAL_INDEX &&
                 CanMergeRaster(graph.Passes[p - 1], pass))
        {
            report.Passes[p].RasterGroup = report.Passes[p - 1].RasterGroup;
            ++report.Summary.MergedPasses;
        }
        else
        {
            report.Passes[p].RasterGroup = static_cast<uint32>(p);
            ++report.Summary.RasterPasses;
        }
    }
}
} // namespace
RasterStatus CompileOrderedGraph(DeviceHandle device, OrderedGraph handle, GraphReport& output, bool reference) noexcept
{
    return CompileOrderedGraph(device, handle, output, GraphCompileOptions{reference == false, false});
}
RasterStatus CompileOrderedGraph(DeviceHandle device,
                                 OrderedGraph handle,
                                 GraphReport& output,
                                 const GraphCompileOptions& options) noexcept
{
    GraphRecord* graph = nullptr;
    const auto admission = GraphAdmission(device, handle, graph);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    // Compile in service-owned storage rather than duplicating the report on
    // the browser stack alongside the caller's output report.
    auto& report = graph->Report;
    report.Summary.PassCount = static_cast<uint32>(graph->PassCount);
    report.Summary.LivePasses = report.Summary.DependencyCount = 0;
    report.Summary.ErrorPass = GRAPH_EXTERNAL_INDEX;
    report.Summary.ErrorResource = GRAPH_EXTERNAL_INDEX;
    for (auto& pass : report.Passes)
    {
        pass.Culled = false;
    }
    for (usize i = 0; i < graph->ResourceCount; ++i)
    {
        report.Resources[i].FirstUse = report.Resources[i].LastUse = GRAPH_EXTERNAL_INDEX;
    }
    const RasterStatus compiled = CompileGraph(*graph, report, options.CullPasses == false);
    // A rejected plan never publishes encoder groups or cost estimates.
    report.Summary.RasterPasses = report.Summary.ComputePasses = report.Summary.MergedPasses =
        report.Summary.DrawCount = 0;
    report.Summary.PlanStorageBytes = 0;
    report.Summary.BufferBytes = report.Summary.TextureBytes = 0;
    for (auto& pass : report.Passes)
    {
        pass.RasterGroup = GRAPH_EXTERNAL_INDEX;
    }
    if (compiled == RasterStatus::Ready)
    {
        PlanRasterGroups(*graph, report, options.MergeRasterPasses);
    }
    CopyGraphSummary(*graph, output);
    graph->ReportAvailable = true;
    if (compiled == RasterStatus::Ready)
    {
        graph->Compiled = true;
    }
    return compiled;
}
RasterStatus
GetGraphPassDiagnostic(DeviceHandle device, OrderedGraph handle, uint32 index, GraphPassDiagnostic& output) noexcept
{
    GraphRecord* graph = nullptr;
    const auto status = GraphAdmission(device, handle, graph, false);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    if (graph->ReportAvailable == false)
    {
        return RasterStatus::NotReady;
    }
    if (index >= graph->PassCount)
    {
        return RasterStatus::InvalidDescription;
    }
    output = graph->Report.Passes[index];
    return RasterStatus::Ready;
}
RasterStatus GetGraphResourceDiagnostic(DeviceHandle device,
                                        OrderedGraph handle,
                                        uint32 index,
                                        GraphResourceDiagnostic& output) noexcept
{
    GraphRecord* graph = nullptr;
    const auto status = GraphAdmission(device, handle, graph, false);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    if (graph->ReportAvailable == false)
    {
        return RasterStatus::NotReady;
    }
    if (index >= graph->ResourceCount)
    {
        return RasterStatus::InvalidDescription;
    }
    output = graph->Report.Resources[index];
    return RasterStatus::Ready;
}
RasterStatus
GetGraphDependency(DeviceHandle device, OrderedGraph handle, uint32 index, GraphDependency& output) noexcept
{
    GraphRecord* graph = nullptr;
    const auto status = GraphAdmission(device, handle, graph, false);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    if (graph->ReportAvailable == false)
    {
        return RasterStatus::NotReady;
    }
    if (index >= graph->Report.Summary.DependencyCount)
    {
        return RasterStatus::InvalidDescription;
    }
    output = graph->Report.Dependencies[index];
    return RasterStatus::Ready;
}
RasterStatus DiscardOrderedGraph(DeviceHandle device, OrderedGraph& handle) noexcept
{
    GraphRecord* graph = nullptr;
    const auto admission = GraphAdmission(device, handle, graph, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    DropGraph(*graph);
    handle = {};
    Collect();
    return RasterStatus::Ready;
}
namespace
{
class GraphGpuScope final
{
public:
    ~GraphGpuScope() noexcept
    {
        End();
    }
    void End() noexcept
    {
        if (mIsActive == true)
        {
            static_cast<void>(backend::EndScope());
            mIsActive = false;
        }
    }
    void Begin(const GpuScopeDescription& scope) noexcept
    {
        End();
        mIsActive = backend::BeginScope(scope) == RasterStatus::Ready;
    }

private:
    bool mIsActive = false;
};
RasterStatus ExecuteGraphCommands(DeviceHandle device,
                                  SurfaceHandle surface,
                                  OrderedGraph& inoutHandle,
                                  SubmissionToken& outCompletion,
                                  bool shouldSubmit) noexcept
{
    GraphRecord* graph;
    const RasterStatus admission = GraphAdmission(device, inoutHandle, graph, false, shouldSubmit == false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    if (shouldSubmit == false)
    {
        FrameInfo information;
        if (GetFrameInfo(device, surface, information) != DeviceStatus::Ready)
        {
            return RasterStatus::InvalidState;
        }
    }
    if (FrameOpen() != (shouldSubmit == false) || graph->Compiled == false || LifetimeAccess::Owner(outCompletion) != 0)
    {
        return RasterStatus::InvalidState;
    }
    if (graph->Report.Summary.DrawCount >
        CurrentDeviceProfile().Raster.DrawsPerFrame - (shouldSubmit == true ? 0 : gDrawCount))
    {
        return RasterStatus::CapacityExceeded;
    }
    for (usize i = 0; i < graph->ResourceCount; ++i)
    {
        const auto& resource = graph->Resources[i];
        const auto& physical = At(resource.Kind, resource.Slot);
        if (physical.Status != RasterStatus::Ready)
        {
            return physical.Status;
        }
        if ((resource.Kind == RasterKind::Buffer &&
             (resource.BufferRevision != physical.BufferRevision || physical.BufferRevision == ~uint64{0})) ||
            (resource.Kind == RasterKind::Texture &&
             (SameTextureState(resource.Import, TextureState(physical)) == false ||
              physical.TextureRevision == ~uint64{0})))
        {
            return RasterStatus::InvalidState;
        }
    }
    GraphGpuScope gpuScope;
    for (usize p = 0; p < graph->PassCount; ++p)
    {
        if (graph->Report.Passes[p].Culled)
        {
            continue;
        }
        const auto& pass = graph->Passes[p];
        for (usize i = 0; i < pass.DrawCount; ++i)
        {
            const auto status = PacketStatus(graph->Draws[pass.FirstDraw + i]);
            if (status != RasterStatus::Ready)
            {
                return status;
            }
        }
        const auto prepared =
            pass.IsCompute ? ComputePacketStatus(pass.Dispatch) : backend::RasterPreparePass(pass.Info);
        const auto currentPrepare = Admission(device, false);
        if (currentPrepare != RasterStatus::Ready)
        {
            inoutHandle = {};
            return currentPrepare;
        }
        if (prepared != RasterStatus::Ready)
        {
            return prepared;
        }
    }
    if (gSubmissions.Next == 0)
    {
        return RasterStatus::IdentityExhausted;
    }
    if (shouldSubmit == true)
    {
        const RasterStatus preparedStorage = backend::RasterPrepareStorage();
        if (preparedStorage != RasterStatus::Ready)
        {
            return preparedStorage;
        }
        const RasterStatus reserve = backend::RasterReserveSubmission();
        if (reserve != RasterStatus::Ready)
        {
            return reserve;
        }
        const DeviceStatus begun = BeginFrame(device, surface);
        if (begun != DeviceStatus::Ready)
        {
            backend::RasterDiscardSubmission();
            return begun == DeviceStatus::Skipped         ? RasterStatus::NotReady
                   : begun == DeviceStatus::InvalidHandle ? RasterStatus::InvalidHandle
                   : begun == DeviceStatus::DeviceLost    ? RasterStatus::DeviceLost
                                                          : RasterStatus::Failed;
        }
    }
    if (Admission(device, false) != RasterStatus::Ready)
    {
        inoutHandle = {};
        return RasterStatus::Failed;
    }
    // Surface rectangles can only be checked against the actual acquired extent.
    // Acquisition may already encode its clear; report that accepted work separately.
    const auto extent = GetFrameInfo();
    for (usize p = 0; p < graph->PassCount; ++p)
    {
        const auto& pass = graph->Passes[p];
        RasterArea area;
        if (graph->Report.Passes[p].Culled == false && pass.IsCompute == false && pass.Info.Texture == RASTER_INVALID &&
            RasterResolveArea(pass.Info.Description, extent.Width, extent.Height, area) == false)
        {
            if (shouldSubmit == false)
            {
                return RasterStatus::InvalidDescription;
            }
            const RasterStatus ended = EndFrame(device, outCompletion);
            ResetGraph(*graph);
            inoutHandle = {};
            return ended == RasterStatus::Ready ? RasterStatus::InvalidDescription : ended;
        }
    }
    gFrameSubmission = gSubmissions.Next;
    gGraphExecuting = true;
    if (ClaimRasterFrame() == false)
    {
        internal::FaultRasterSession();
        inoutHandle = {};
        return RasterStatus::Failed;
    }
    for (usize p = 0; p < graph->PassCount; ++p)
    {
        if (graph->Report.Passes[p].Culled)
        {
            continue;
        }
        const auto& pass = graph->Passes[p];
        const bool continuation = pass.IsCompute == false && graph->Report.Passes[p].RasterGroup != p;
        if (continuation == false)
        {
            backend::RasterEndPass();
            gpuScope.End();
            const auto currentEnd = Admission(device, false);
            if (currentEnd != RasterStatus::Ready)
            {
                inoutHandle = {};
                return currentEnd;
            }
            for (usize i = 0; i < pass.UseCount; ++i)
            {
                const auto& use = pass.Uses[i];
                const auto& resource = graph->Resources[GraphAccess::Resource(use.Resource)];
                if (resource.Kind == RasterKind::Buffer)
                {
                    backend::ComputeBufferBarrier(resource.Slot, use.Access);
                    const auto currentBuffer = Admission(device, false);
                    if (currentBuffer != RasterStatus::Ready)
                    {
                        inoutHandle = {};
                        return currentBuffer;
                    }
                    FrameRetain(RasterKind::Buffer, resource.Slot);
                }
                if (resource.Kind != RasterKind::Texture || use.Access != GraphAccessMode::Sampled)
                {
                    continue;
                }
                const auto visibility = static_cast<uint8>(use.Visibility);
                const auto sampled = visibility == 1   ? RasterTextureUse::SampledVertex
                                     : visibility == 2 ? RasterTextureUse::SampledFragment
                                                       : RasterTextureUse::SampledBoth;
                backend::RasterTextureBarrier(resource.Slot, sampled);
                const auto currentBarrier = Admission(device, false);
                if (currentBarrier != RasterStatus::Ready)
                {
                    inoutHandle = {};
                    return currentBarrier;
                }
                FrameRetain(RasterKind::Texture, resource.Slot);
                At(RasterKind::Texture, resource.Slot).FrameTextureUse = sampled;
            }
            const GraphPassDiagnostic& diagnostic = graph->Report.Passes[p];
            usize nameLength = 0;
            while (nameLength < sizeof(diagnostic.Name) && diagnostic.Name[nameLength] != '\0')
            {
                ++nameLength;
            }
            uint32 logicalPassCount = 1;
            for (usize member = p + 1; member < graph->PassCount; ++member)
            {
                if (graph->Report.Passes[member].Culled == false && graph->Report.Passes[member].RasterGroup == p)
                {
                    ++logicalPassCount;
                }
            }
            GpuScopeDescription description;
            if (MakeGpuScope(p + 1, {diagnostic.Name, nameLength}, description, logicalPassCount) == true)
            {
                gpuScope.Begin(description);
            }
            const auto started = pass.IsCompute ? EncodeCompute(pass.Dispatch) : backend::RasterBeginPass(pass.Info);
            if (started != RasterStatus::Ready)
            {
                internal::FaultRasterSession();
                inoutHandle = {};
                return started;
            }
            const auto current = Admission(device, false);
            if (current != RasterStatus::Ready)
            {
                inoutHandle = {};
                return current;
            }
        }
        if (pass.IsCompute == false)
        {
            ApplyPassState(pass.Info);
        }
        for (usize i = 0; i < pass.DrawCount; ++i)
        {
            const auto encoded = EncodePacket(graph->Draws[pass.FirstDraw + i]);
            const auto currentDraw = Admission(device, false);
            if (currentDraw != RasterStatus::Ready)
            {
                inoutHandle = {};
                return currentDraw;
            }
            if (encoded != RasterStatus::Ready)
            {
                internal::FaultRasterSession();
                inoutHandle = {};
                return encoded;
            }
        }
    }
    backend::RasterEndPass();
    gpuScope.End();
    const auto currentEnd = Admission(device, false);
    if (currentEnd != RasterStatus::Ready)
    {
        inoutHandle = {};
        return currentEnd;
    }
    for (usize r = 0; r < graph->ResourceCount; ++r)
    {
        const auto& resource = graph->Resources[r];
        if (resource.Kind == RasterKind::Buffer && graph->Report.Resources[r].FirstUse != GRAPH_EXTERNAL_INDEX)
        {
            const auto role = At(RasterKind::Buffer, resource.Slot).Buffer.Role;
            if (StorageRole(role))
            {
                backend::ComputeBufferBarrier(resource.Slot,
                                              role == BufferRole::StorageVertex ? GraphAccessMode::Vertex
                                              : role == BufferRole::StorageIndirect
                                                  ? GraphAccessMode::Indirect
                                                  : GraphAccessMode::StorageReadWrite);
                const auto currentBuffer = Admission(device, false);
                if (currentBuffer != RasterStatus::Ready)
                {
                    inoutHandle = {};
                    return currentBuffer;
                }
            }
        }
        if (resource.Kind != RasterKind::Texture ||
            (resource.Transient && graph->Report.Resources[r].FirstUse == GRAPH_EXTERNAL_INDEX))
        {
            continue;
        }
        backend::RasterTextureBarrier(resource.Slot, resource.Final);
        const auto currentBarrier = Admission(device, false);
        if (currentBarrier != RasterStatus::Ready)
        {
            inoutHandle = {};
            return currentBarrier;
        }
        FrameRetain(RasterKind::Texture, resource.Slot);
        At(RasterKind::Texture, resource.Slot).FrameTextureUse = resource.Final;
    }
    const RasterStatus ended = shouldSubmit == true ? EndFrame(device, outCompletion) : RasterStatus::Ready;
    if (shouldSubmit == false)
    {
        gGraphExecuting = false;
    }
    if (graph->Owner != 0)
    {
        DropGraph(*graph);
    }
    inoutHandle = {};
    Collect();
    if (ended != RasterStatus::Ready && GetStartup().State == StartupState::Ready)
    {
        internal::FaultRasterSession();
    }
    return ended;
}
} // namespace
RasterStatus ExecuteOrderedGraph(DeviceHandle device,
                                 SurfaceHandle surface,
                                 OrderedGraph& inoutHandle,
                                 SubmissionToken& outCompletion) noexcept
{
    return ExecuteGraphCommands(device, surface, inoutHandle, outCompletion, true);
}
RasterStatus EncodeOrderedGraph(DeviceHandle device, SurfaceHandle surface, OrderedGraph& inoutHandle) noexcept
{
    SubmissionToken completion;
    return ExecuteGraphCommands(device, surface, inoutHandle, completion, false);
}
} // namespace ludus::graphics::rhi
