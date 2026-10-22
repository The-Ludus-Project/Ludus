#include "internal/lifecycle.h"
#include "reference_raster.h"
#include "render_graph_scratch.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/rhi/device.h>

#include <catch2/catch_test_macros.hpp>

#include <new>
#include <string_view>

using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace format = ludus::content::render_graph_format;
namespace ludus::graphics::rhi::backend
{
extern uint32 PendingToken;
extern StartupError ImmediateError;
extern FrameStatus NextFrame;
extern Backend ActiveKind;
extern bool SelectionSupported;
} // namespace ludus::graphics::rhi::backend
namespace
{
constexpr usize COMPOSITE_INPUT_COUNT = 1;
constexpr usize COMPOSITE_BINDING_COUNT = 2;
constexpr usize VERTEX_COUNT = 3;
constexpr usize INDEX_COUNT = 3;
constexpr uint32 WIDTH = 96;
constexpr uint32 HEIGHT = 64;
constexpr uint8 HOST_INTERFACE = 0xA5;
constexpr uint8 OTHER_INTERFACE = 0x5A;
constexpr std::string_view VERTEX_TEXT = "flat vertex fixture";
constexpr std::string_view FRAGMENT_TEXT = "flat fragment fixture";
constexpr std::string_view METAL_TEXT = "metal fixture";
constexpr std::string_view GLSL_TEXT = "glsl es fixture";
constexpr usize SPIRV_WORD_COUNT = 5;
/// SPIR-V magic and version 1.3 lead; the rest set high bits and differ in every byte.
constexpr StaticArray<uint32, SPIRV_WORD_COUNT> SPIRV_WORDS{
    {0x07230203U, 0x00010300U, 0x80FF0001U, 0xFFFFFFFFU, 0x01020304U}};
/// Bytes of a stage artifact whose length is not a whole number of SPIR-V words.
constexpr std::string_view PARTIAL_WORD = "\x03\x02\x23\x07\x01\x03";

/// Independent little-endian writer for the runtime_package.h layout.
struct PackageWriter final
{
    Array<uint8> Bytes;
    void WriteWord(uint32 value)
    {
        for (usize i = 0; i < sizeof(uint32); ++i)
        {
            Bytes.Add(static_cast<uint8>((value >> (8U * i)) & 0xFFU));
        }
    }
    void WritePadded(std::string_view text)
    {
        for (const char character : text)
        {
            Bytes.Add(static_cast<uint8>(character));
        }
        while (Bytes.GetSize() % sizeof(uint32) != 0)
        {
            Bytes.Add(0);
        }
    }
    void WriteIdentity(uint8 value)
    {
        for (usize i = 0; i < format::IDENTITY_BYTES; ++i)
        {
            Bytes.Add(value);
        }
    }
};
struct PackageOptions final
{
    uint8 Interface = HOST_INTERFACE;
    uint8 Generation = 1;
    uint32 Version = format::PACKAGE_VERSION;
    uint32 ViewNumerator = 0;
    uint32 ViewDenominator = 1;
    uint32 Backing = 0;
    uint32 SceneData = ::ludus::content::render_graph_format::SCENE_DATA_ALL;
    format::Technique Program = format::Technique::ColorAndDepth;
    std::string_view PassName = "Scene/Opaque";
    bool HasDepthStages = true;
    /// Bind one Material {Float3 tint; Float32 gain} block to the color fragment at slot 0.
    bool HasUniforms = false;
};
constexpr uint32 MATERIAL_VALUE_COUNT = 4;
constexpr uint32 WGSL_MATERIAL_BYTES = 16;
constexpr uint32 METAL_MATERIAL_BYTES = 32;
constexpr std::string_view BLOCK_NAME = "material_block";
/// Write one stage record; artifact holds the raw bytes for any target, including SPIR-V words.
void WriteStage(PackageWriter& inoutWriter,
                format::StageRole role,
                format::ShaderTarget target,
                std::string_view artifact,
                std::string_view entry,
                std::string_view block = {})
{
    const bool isVertex = static_cast<uint32>(role) % format::TECHNIQUE_STAGE_COUNT == 0;
    inoutWriter.WriteWord(static_cast<uint32>(role));
    inoutWriter.WriteWord(static_cast<uint32>(target));
    inoutWriter.WriteWord(isVertex == true ? static_cast<uint32>(format::STATIC_INPUT_COUNT) : 0);
    inoutWriter.WriteWord(static_cast<uint32>(entry.size()));
    inoutWriter.WriteWord(static_cast<uint32>(artifact.size()));
    inoutWriter.WriteWord(block.empty() == true ? 0 : 1);
    for (uint32 i = 0; isVertex == true && i < format::STATIC_INPUT_COUNT; ++i)
    {
        inoutWriter.WriteWord(i);
        inoutWriter.WriteWord(static_cast<uint32>(i == 0 ? format::InputFormat::Float3 : format::InputFormat::Float4));
    }
    if (block.empty() == false)
    {
        inoutWriter.WriteWord(0);
        inoutWriter.WriteWord(static_cast<uint32>(block.size()));
        inoutWriter.WritePadded(block);
    }
    inoutWriter.WritePadded(entry);
    inoutWriter.WritePadded(artifact);
}
void WriteStage(PackageWriter& inoutWriter, format::StageRole role, std::string_view text, std::string_view entry)
{
    WriteStage(inoutWriter, role, format::ShaderTarget::Wgsl, text, entry);
}
/// One exported RGBA8 color plus its private depth, one camera and one ColorAndDepth pass.
void WritePlan(PackageWriter& inoutWriter, const PackageOptions& options, uint32 stageCount)
{
    inoutWriter.WriteWord(format::PACKAGE_MAGIC);
    inoutWriter.WriteWord(options.Version);
    inoutWriter.WriteWord(2);
    inoutWriter.WriteWord(1);
    inoutWriter.WriteWord(1);
    inoutWriter.WriteWord(options.HasUniforms == true ? MATERIAL_VALUE_COUNT : 0);
    inoutWriter.WriteWord(options.HasUniforms == true ? 1 : 0);
    inoutWriter.WriteWord(options.HasUniforms == true ? 2 : 0);
    inoutWriter.WriteWord(options.HasUniforms == true ? 4 : 0);
    inoutWriter.WriteWord(stageCount);
    inoutWriter.WriteIdentity(options.Interface);
    inoutWriter.WriteIdentity(options.Generation);
    for (const uint32 aspect : {static_cast<uint32>(format::Aspect::Color), static_cast<uint32>(format::Aspect::Depth)})
    {
        inoutWriter.WriteWord(WIDTH);
        inoutWriter.WriteWord(HEIGHT);
        inoutWriter.WriteWord(aspect);
        inoutWriter.WriteWord(options.Backing);
        inoutWriter.WriteWord(0);
        inoutWriter.WriteWord(aspect == 0 ? 1 : 0);
        if (options.Version >= format::VIEW_PACKAGE_VERSION)
        {
            inoutWriter.WriteWord(options.ViewNumerator);
            inoutWriter.WriteWord(options.ViewDenominator);
        }
    }
    inoutWriter.WriteWord(0);
    for (const uint32 value : {static_cast<uint32>(options.Program),
                               0U,
                               0U,
                               1U,
                               static_cast<uint32>(format::Load::Clear),
                               static_cast<uint32>(format::Store::Store),
                               static_cast<uint32>(format::Load::Clear),
                               static_cast<uint32>(format::Store::Store),
                               0U})
    {
        inoutWriter.WriteWord(value);
    }
    if (options.Version >= format::SCENE_PACKAGE_VERSION)
    {
        inoutWriter.WriteWord(options.SceneData);
    }
    if (options.HasUniforms == false)
    {
        return;
    }
    // Binding: ColorAndDepth fragment slot 0 reads values [0, 4).
    for (const uint32 value : {static_cast<uint32>(format::Technique::ColorAndDepth),
                               0U,
                               static_cast<uint32>(format::Visibility::Fragment),
                               0U,
                               MATERIAL_VALUE_COUNT})
    {
        inoutWriter.WriteWord(value);
    }
    // WGSL packs gain at 12 in 16 bytes; Metal pads tint to 16 bytes, so gain is at 16 of 32.
    for (const uint32 value : {0U,
                               static_cast<uint32>(format::ShaderTarget::Wgsl),
                               WGSL_MATERIAL_BYTES,
                               0U,
                               static_cast<uint32>(format::ShaderTarget::Metal),
                               METAL_MATERIAL_BYTES})
    {
        inoutWriter.WriteWord(value);
    }
    for (const format::ShaderTarget target : {format::ShaderTarget::Wgsl, format::ShaderTarget::Metal})
    {
        for (const uint32 value : {0U, static_cast<uint32>(target), 0U, 3U, 0U})
        {
            inoutWriter.WriteWord(value);
        }
        for (const uint32 value :
             {0U, static_cast<uint32>(target), 3U, 1U, target == format::ShaderTarget::Wgsl ? 12U : 16U})
        {
            inoutWriter.WriteWord(value);
        }
    }
}
Array<uint8> MakePackageBytes(const PackageOptions& options)
{
    PackageWriter writer;
    WritePlan(writer, options, options.HasDepthStages == true ? 4 : 2);
    WriteStage(writer, format::StageRole::ColorVertex, VERTEX_TEXT, "VertexMain");
    WriteStage(writer, format::StageRole::ColorFragment, FRAGMENT_TEXT, "FragmentMain");
    if (options.HasDepthStages == true)
    {
        WriteStage(writer, format::StageRole::DepthVertex, VERTEX_TEXT, "VertexMain");
        WriteStage(writer, format::StageRole::DepthFragment, FRAGMENT_TEXT, "FragmentMain");
    }
    if (options.Version >= format::NAMED_PACKAGE_VERSION)
    {
        writer.WriteWord(static_cast<uint32>(options.PassName.size()));
        writer.WritePadded(options.PassName);
    }
    return Move(writer.Bytes);
}
/// Encode SPIRV_WORDS little-endian, independently of the decoder under test.
Array<uint8> MakeSpirvBytes()
{
    PackageWriter writer;
    for (const uint32 word : SPIRV_WORDS)
    {
        writer.WriteWord(word);
    }
    return Move(writer.Bytes);
}
std::string_view AsText(const Array<uint8>& bytes)
{
    return {reinterpret_cast<const char*>(bytes.GetData()), bytes.GetSize()};
}
/// Color-only generation carrying every artifact target for both stages, as a cook
/// for Metal, Vulkan and the browser together would write it.
Array<uint8> MakeMultiTargetPackageBytes(std::string_view vertexSpirv, bool hasFragmentSpirv)
{
    const Array<uint8> fragmentSpirv = MakeSpirvBytes();
    PackageWriter writer;
    WritePlan(writer, {}, hasFragmentSpirv == true ? 8 : 7);
    WriteStage(writer, format::StageRole::ColorVertex, format::ShaderTarget::Metal, METAL_TEXT, "vertexMain");
    WriteStage(writer, format::StageRole::ColorVertex, format::ShaderTarget::Spirv, vertexSpirv, "VertexMain");
    WriteStage(writer, format::StageRole::ColorVertex, format::ShaderTarget::Wgsl, VERTEX_TEXT, "VertexMain");
    WriteStage(writer, format::StageRole::ColorVertex, format::ShaderTarget::GlslEs, GLSL_TEXT, "main");
    WriteStage(writer, format::StageRole::ColorFragment, format::ShaderTarget::Metal, METAL_TEXT, "fragmentMain");
    if (hasFragmentSpirv == true)
    {
        WriteStage(writer,
                   format::StageRole::ColorFragment,
                   format::ShaderTarget::Spirv,
                   AsText(fragmentSpirv),
                   "FragmentMain");
    }
    WriteStage(writer, format::StageRole::ColorFragment, format::ShaderTarget::Wgsl, FRAGMENT_TEXT, "FragmentMain");
    WriteStage(writer, format::StageRole::ColorFragment, format::ShaderTarget::GlslEs, GLSL_TEXT, "main");
    writer.WriteWord(static_cast<uint32>(PackageOptions{}.PassName.size()));
    writer.WritePadded(PackageOptions{}.PassName);
    return Move(writer.Bytes);
}
core::UniquePtr<renderer::RenderGraphPackage> MakePackage(const PackageOptions& options)
{
    const Array<uint8> bytes = MakePackageBytes(options);
    renderer::RenderGraphPackage* allocationOrNull = new (std::nothrow) renderer::RenderGraphPackage;
    core::UniquePtr<renderer::RenderGraphPackage> package{allocationOrNull};
    REQUIRE(package.Get() != nullptr);
    REQUIRE(package->Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
    return package;
}
/// RG4 workload: the single-export generation with two unexported scratch backings.
constexpr uint32 SCRATCH_COUNT = 2;
constexpr usize TEXTURE = static_cast<usize>(rhi::internal::RasterKind::Texture);
core::UniquePtr<renderer::RenderGraphPackage>
MakeScratchPackage(uint8 generation,
                   const ludus::render_graph_scratch::ScratchShape& shape = {SCRATCH_COUNT, WIDTH, HEIGHT})
{
    PackageOptions options;
    options.Generation = generation;
    const Array<uint8> offscreen = MakePackageBytes(options);
    Array<uint8> bytes;
    REQUIRE(ludus::render_graph_scratch::WriteScratchGeneration({offscreen.GetData(), offscreen.GetSize()},
                                                                shape,
                                                                bytes) == true);
    renderer::RenderGraphPackage* allocationOrNull = new (std::nothrow) renderer::RenderGraphPackage;
    core::UniquePtr<renderer::RenderGraphPackage> package{allocationOrNull};
    REQUIRE(package.Get() != nullptr);
    REQUIRE(package->Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
    return package;
}
renderer::RenderGraphPackageStatus DecodeBytes(const Array<uint8>& bytes)
{
    renderer::RenderGraphPackage package;
    const renderer::RenderGraphPackageStatus status = package.Decode({bytes.GetData(), bytes.GetSize()});
    CHECK(package.IsLoaded() == (status == renderer::RenderGraphPackageStatus::Ready));
    return status;
}
struct ActivationFixture final
{
    rhi::DeviceHandle Device{};
    rhi::SurfaceHandle Surface{};
    renderer::RenderGraphActivation Activation;
    StaticArray<uint8, format::IDENTITY_BYTES> Interface{};
    StaticArray<rhi::RasterShaderInput, COMPOSITE_INPUT_COUNT> CompositeInputs{{{0, rhi::RasterVertexFormat::Float4}}};
    StaticArray<rhi::RasterBinding, COMPOSITE_BINDING_COUNT> CompositeBindings{
        {{0, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
         {1, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0}}};
    StaticArray<math::Vector3, VERTEX_COUNT> Positions{{{-.1F, -.1F, .5F}, {.1F, -.1F, .5F}, {0, .1F, .5F}}};
    StaticArray<uint32, INDEX_COUNT> Indices{{0, 1, 2}};
    StaticArray<renderer::RenderGraphMesh, 1> Meshes{};
    StaticArray<renderer::RenderGraphItem, 1> Items{};
    StaticArray<renderer::ViewDescription, 1> Cameras{};
    explicit ActivationFixture(renderer::RenderGraphAllocation allocation = renderer::RenderGraphAllocation::ExactPool)
    {
        rhi::Shutdown();
        rhi::reference::Reset();
        rhi::reference::ProfileAvailable = true;
        rhi::backend::ImmediateError = rhi::StartupError::None;
        rhi::backend::NextFrame = rhi::FrameStatus::Ready;
        rhi::backend::ActiveKind = rhi::Backend::WebGPU;
        rhi::backend::SelectionSupported = true;
        REQUIRE(rhi::CreateDevice({}, {}, {}, Device, Surface) == rhi::DeviceStatus::Pending);
        rhi::internal::Complete(rhi::backend::PendingToken, rhi::StartupError::None, {4096, 16384});
        for (usize i = 0; i < format::IDENTITY_BYTES; ++i)
        {
            Interface[i] = HOST_INTERFACE;
        }
        Meshes[0].Geometry.Positions = Positions.GetData();
        Meshes[0].Geometry.VertexCount = Positions.GetSize();
        Meshes[0].Geometry.Indices = Indices.GetData();
        Meshes[0].Geometry.IndexCount = Indices.GetSize();
        Items[0].SourceId = 42;
        Items[0].Material.Color = {1, 1, 1, 1};
        rhi::RasterShaderDescription compositeVertex;
        compositeVertex.Artifact.Stage = rhi::ShaderStage::Vertex;
        compositeVertex.Artifact.Wgsl = "composite vertex fixture";
        compositeVertex.Artifact.WgslEntry = "VertexMain";
        compositeVertex.Inputs = CompositeInputs.AsSpan();
        rhi::RasterShaderDescription compositeFragment;
        compositeFragment.Artifact.Stage = rhi::ShaderStage::Fragment;
        compositeFragment.Artifact.Wgsl = "composite fragment fixture";
        compositeFragment.Artifact.WgslEntry = "FragmentMain";
        compositeFragment.Bindings = CompositeBindings.AsSpan();
        REQUIRE(Activation.Initialize(Device,
                                      {Interface.GetData(), Interface.GetSize()},
                                      compositeVertex,
                                      compositeFragment,
                                      allocation) == rhi::RasterStatus::Ready);
    }
    ~ActivationFixture()
    {
        Activation.Reset();
        rhi::Shutdown();
    }
    rhi::RasterStatus Update(renderer::RenderGraphReport& outReport, uint64 revision = 1)
    {
        return Activation.Update({Meshes.GetData(), Meshes.GetSize()},
                                 {Items.GetData(), Items.GetSize()},
                                 {Cameras.GetData(), Cameras.GetSize()},
                                 revision,
                                 renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                 outReport);
    }
    /// Activate a first generation, draw revision 1 and complete it.
    void ActivateFirst()
    {
        REQUIRE(Activation.Stage(MakePackage({})) == rhi::RasterStatus::Ready);
        REQUIRE(Activation.Poll() == rhi::RasterStatus::Ready);
        REQUIRE(Activation.GetReport().ActiveGeneration == 1);
        renderer::RenderGraphReport report;
        REQUIRE(Update(report) == rhi::RasterStatus::Ready);
        Render();
        rhi::reference::Completed = rhi::reference::Submitted;
        REQUIRE(Activation.GetActiveOrNull()->GetOutputSceneRevision() == 1);
    }
    void Render()
    {
        renderer::RenderGraph* graphOrNull = Activation.GetActiveOrNull();
        REQUIRE(graphOrNull != nullptr);
        rhi::SubmissionToken completion;
        REQUIRE(graphOrNull->Render(Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
    }
    /// Stage a generation, prepare it with the next extraction and activate it at a frame boundary.
    void Reload(core::UniquePtr<renderer::RenderGraphPackage> package, uint64 revision)
    {
        const uint64 serial = Activation.GetReport().CandidateGeneration + 1;
        REQUIRE(Activation.Stage(Move(package)) == rhi::RasterStatus::Ready);
        static_cast<void>(Activation.Poll());
        renderer::RenderGraphReport report;
        REQUIRE(Update(report, revision) == rhi::RasterStatus::Ready);
        static_cast<void>(Activation.Poll());
        REQUIRE(Activation.GetReport().ActiveGeneration == serial);
    }
    static void CompletePending(rhi::RasterStatus status)
    {
        for (usize i = 0; i < rhi::reference::PendingCount; ++i)
        {
            rhi::internal::RasterComplete(rhi::reference::PendingRequests[i], status);
        }
        rhi::reference::Next = rhi::RasterStatus::Ready;
    }
};
} // namespace

TEST_CASE("RG3 runtime packages decode plan records and copy caller bytes", "[render-graph][activation]")
{
    Array<uint8> bytes = MakePackageBytes({});
    renderer::RenderGraphPackage package;
    REQUIRE(package.Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
    // An unloadable module may release or overwrite its buffer immediately.
    for (usize i = 0; i < bytes.GetSize(); ++i)
    {
        bytes[i] = 0;
    }
    const renderer::RenderGraphPlan plan = package.GetPlan();
    REQUIRE(plan.Resources.GetSize() == 2);
    CHECK(plan.Resources[0].Width == WIDTH);
    CHECK(plan.Resources[1].Kind == format::Aspect::Depth);
    CHECK(plan.Passes[0].Program == format::Technique::ColorAndDepth);
    CHECK(plan.Cameras[0].Target == 0);
    const renderer::RenderGraphPrograms programs = package.GetPrograms({}, {});
    CHECK(programs.ColorVertex.Artifact.Wgsl == VERTEX_TEXT);
    CHECK(programs.ColorVertex.Artifact.WgslEntry == "VertexMain");
    CHECK(programs.ColorVertex.Inputs.size() == format::STATIC_INPUT_COUNT);
    CHECK(programs.ColorFragment.Inputs.empty() == true);
    CHECK(programs.DepthFragment.Artifact.Wgsl == FRAGMENT_TEXT);
    CHECK(package.GetInterfaceIdentity()[0] == HOST_INTERFACE);
    CHECK(package.GetGenerationIdentity()[31] == 1);
    CHECK(package.Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::InvalidState);
}

TEST_CASE("RG3 malformed runtime packages reject without a partial generation", "[render-graph][activation]")
{
    const Array<uint8> valid = MakePackageBytes({});
    PackageOptions options;
    options.Version = format::PACKAGE_VERSION + 1;
    CHECK(DecodeBytes(MakePackageBytes(options)) == renderer::RenderGraphPackageStatus::UnsupportedVersion);
    Array<uint8> truncated = Array<uint8>::FromRange(valid.AsSpan());
    truncated.RemoveLast();
    CHECK(DecodeBytes(truncated) == renderer::RenderGraphPackageStatus::Malformed);
    Array<uint8> trailing = Array<uint8>::FromRange(valid.AsSpan());
    for (usize i = 0; i < sizeof(uint32); ++i)
    {
        trailing.Add(0);
    }
    CHECK(DecodeBytes(trailing) == renderer::RenderGraphPackageStatus::Malformed);
    Array<uint8> badMagic = Array<uint8>::FromRange(valid.AsSpan());
    badMagic[0] = 0;
    CHECK(DecodeBytes(badMagic) == renderer::RenderGraphPackageStatus::UnsupportedVersion);
    Array<uint8> manyResources = Array<uint8>::FromRange(valid.AsSpan());
    manyResources[8] = static_cast<uint8>(format::MAX_RESOURCES + 1);
    CHECK(DecodeBytes(manyResources) == renderer::RenderGraphPackageStatus::CapacityExceeded);
    // Resource 0's aspect field lies after the header and two record words.
    Array<uint8> badAspect = Array<uint8>::FromRange(valid.AsSpan());
    badAspect[format::HEADER_BYTES + 2 * sizeof(uint32)] = 7;
    CHECK(DecodeBytes(badAspect) == renderer::RenderGraphPackageStatus::Malformed);
    // The fragment entry "FragmentMain" is 12 bytes; corrupt the vertex entry's zero padding instead.
    const usize firstStage = format::HEADER_BYTES + 2 * format::RESOURCE_RECORD_BYTES + format::CAMERA_RECORD_BYTES +
                             format::PASS_RECORD_BYTES;
    const usize vertexEntryPadding = firstStage + format::STAGE_FIELD_COUNT * sizeof(uint32) +
                                     format::STATIC_INPUT_COUNT * format::INPUT_RECORD_BYTES + 10;
    Array<uint8> badPadding = Array<uint8>::FromRange(valid.AsSpan());
    badPadding[vertexEntryPadding] = 1;
    CHECK(DecodeBytes(badPadding) == renderer::RenderGraphPackageStatus::Malformed);
    Array<uint8> badInput = Array<uint8>::FromRange(valid.AsSpan());
    badInput[firstStage + format::STAGE_FIELD_COUNT * sizeof(uint32) + sizeof(uint32)] = 0;
    CHECK(DecodeBytes(badInput) == renderer::RenderGraphPackageStatus::Malformed);
    // A color-only package omits both DepthOnly roles consistently.
    PackageOptions colorOnly;
    colorOnly.HasDepthStages = false;
    CHECK(DecodeBytes(MakePackageBytes(colorOnly)) == renderer::RenderGraphPackageStatus::Ready);
    Array<uint8> duplicate = Array<uint8>::FromRange(valid.AsSpan());
    // Relabel the DepthVertex stage as a second ColorVertex record.
    const usize stageBytes = format::STAGE_FIELD_COUNT * sizeof(uint32) +
                             format::STATIC_INPUT_COUNT * format::INPUT_RECORD_BYTES + 12 + 20 +
                             format::STAGE_FIELD_COUNT * sizeof(uint32) + 12 + 24;
    duplicate[firstStage + stageBytes] = 0;
    CHECK(DecodeBytes(duplicate) == renderer::RenderGraphPackageStatus::Malformed);
}

TEST_CASE("RG3 runtime packages decode SPIR-V words little-endian beside every text target",
          "[render-graph][activation]")
{
    const Array<uint8> spirv = MakeSpirvBytes();
    const Array<uint8> package = MakeMultiTargetPackageBytes(AsText(spirv), true);
    // Decode from an odd address: words are read field by field, never by host-aligned loads.
    Array<uint8> shifted;
    shifted.Add(0);
    for (const uint8 value : package)
    {
        shifted.Add(value);
    }
    renderer::RenderGraphPackage decoded;
    REQUIRE(decoded.Decode({shifted.GetData() + 1, package.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
    for (usize i = 0; i < shifted.GetSize(); ++i)
    {
        shifted[i] = 0;
    }
    const renderer::RenderGraphPrograms programs = decoded.GetPrograms({}, {});
    for (const rhi::RasterShaderDescription* description : {&programs.ColorVertex, &programs.ColorFragment})
    {
        const rhi::ShaderDescription& artifact = description->Artifact;
        REQUIRE(artifact.Spirv.size() == SPIRV_WORD_COUNT);
        for (usize word = 0; word < SPIRV_WORD_COUNT; ++word)
        {
            CHECK(artifact.Spirv[word] == SPIRV_WORDS[word]);
        }
        CHECK(artifact.Msl == METAL_TEXT);
        CHECK(artifact.GlslEs == GLSL_TEXT);
        CHECK(artifact.GlslEsEntry == "main");
    }
    CHECK(programs.ColorVertex.Artifact.SpirvEntry == "VertexMain");
    CHECK(programs.ColorFragment.Artifact.SpirvEntry == "FragmentMain");
    CHECK(programs.ColorVertex.Artifact.MslEntry == "vertexMain");
    CHECK(programs.ColorVertex.Artifact.Wgsl == VERTEX_TEXT);
    CHECK(programs.ColorFragment.Artifact.Wgsl == FRAGMENT_TEXT);
    CHECK(programs.ColorVertex.Artifact.Stage == rhi::ShaderStage::Vertex);
    CHECK(programs.ColorFragment.Artifact.Stage == rhi::ShaderStage::Fragment);
    // A color-only generation leaves both DepthOnly roles empty, like its compiled header.
    CHECK(programs.DepthVertex.Artifact.Spirv.empty() == true);
    CHECK(programs.DepthVertex.Inputs.empty() == true);
    CHECK(programs.DepthFragment.Artifact.Stage == rhi::ShaderStage::Vertex);
}

TEST_CASE("RG3 SPIR-V artifacts reject partial words and unpaired stages", "[render-graph][activation]")
{
    const Array<uint8> spirv = MakeSpirvBytes();
    CHECK(DecodeBytes(MakeMultiTargetPackageBytes(PARTIAL_WORD, true)) ==
          renderer::RenderGraphPackageStatus::Malformed);
    // Vulkan would find a vertex stage without its fragment stage.
    CHECK(DecodeBytes(MakeMultiTargetPackageBytes(AsText(spirv), false)) ==
          renderer::RenderGraphPackageStatus::Malformed);
}

TEST_CASE("RG3 first generation activates once its pipelines are ready", "[render-graph][activation]")
{
    ActivationFixture fixture;
    CHECK(fixture.Activation.GetActiveOrNull() == nullptr);
    rhi::reference::Next = rhi::RasterStatus::Pending;
    REQUIRE(fixture.Activation.Stage(MakePackage({})) == rhi::RasterStatus::Pending);
    CHECK(fixture.Activation.GetReport().Candidate == renderer::RenderGraphCandidateState::Preparing);
    CHECK(fixture.Activation.Poll() == rhi::RasterStatus::Pending);
    CHECK(fixture.Activation.GetActiveOrNull() == nullptr);
    renderer::RenderGraphReport report;
    CHECK(fixture.Update(report) == rhi::RasterStatus::NotReady);
    ActivationFixture::CompletePending(rhi::RasterStatus::Ready);
    REQUIRE(fixture.Activation.Poll() == rhi::RasterStatus::Ready);
    CHECK(fixture.Activation.GetReport().ActiveGeneration == 1);
    CHECK(fixture.Activation.GetReport().Candidate == renderer::RenderGraphCandidateState::None);
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    fixture.Render();
    CHECK(rhi::reference::Draws == 1);
}

TEST_CASE("RG3 pending replacement keeps drawing the active generation", "[render-graph][activation]")
{
    ActivationFixture fixture;
    fixture.ActivateFirst();
    renderer::RenderGraph* firstOrNull = fixture.Activation.GetActiveOrNull();
    const usize shaderCreates = rhi::reference::Creates[static_cast<usize>(rhi::internal::RasterKind::Shader)];
    rhi::reference::Next = rhi::RasterStatus::Pending;
    PackageOptions second;
    second.Generation = 2;
    REQUIRE(fixture.Activation.Stage(MakePackage(second)) == rhi::RasterStatus::Pending);
    CHECK(rhi::reference::Creates[static_cast<usize>(rhi::internal::RasterKind::Shader)] > shaderCreates);
    CHECK(fixture.Activation.Poll() == rhi::RasterStatus::Ready);
    CHECK(fixture.Activation.GetReport().Candidate == renderer::RenderGraphCandidateState::Preparing);
    CHECK(fixture.Activation.GetActiveOrNull() == firstOrNull);
    // Drawing never creates the candidate's pending pipelines.
    const usize pipelineCreates = rhi::reference::Creates[static_cast<usize>(rhi::internal::RasterKind::Pipeline)];
    fixture.Render();
    CHECK(rhi::reference::Draws == 2);
    CHECK(rhi::reference::Creates[static_cast<usize>(rhi::internal::RasterKind::Pipeline)] == pipelineCreates);
    ActivationFixture::CompletePending(rhi::RasterStatus::Ready);
    REQUIRE(fixture.Activation.Poll() == rhi::RasterStatus::Ready);
    // Pipelines are ready, but the candidate has not prepared the current extraction.
    CHECK(fixture.Activation.GetReport().Candidate == renderer::RenderGraphCandidateState::NeedsUpdate);
    CHECK(fixture.Activation.GetActiveOrNull() == firstOrNull);
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    rhi::reference::Completed = rhi::reference::Submitted;
    REQUIRE(fixture.Activation.Poll() == rhi::RasterStatus::Ready);
    CHECK(fixture.Activation.GetReport().ActiveGeneration == 2);
    renderer::RenderGraph* secondOrNull = fixture.Activation.GetActiveOrNull();
    REQUIRE(secondOrNull != nullptr);
    // The new generation never relabels the previous generation's pixels.
    CHECK(secondOrNull->GetOutputSceneRevision() == 0);
    CHECK(secondOrNull->GetDrawableSceneRevision() == 1);
    fixture.Render();
    rhi::reference::Completed = rhi::reference::Submitted;
    CHECK(secondOrNull->GetOutputSceneRevision() == 1);
}

TEST_CASE("RG3 failed reload preserves the active generation and its pixels", "[render-graph][activation]")
{
    ActivationFixture fixture;
    fixture.ActivateFirst();
    renderer::RenderGraph* firstOrNull = fixture.Activation.GetActiveOrNull();
    rhi::reference::Next = rhi::RasterStatus::Pending;
    PackageOptions second;
    second.Generation = 2;
    REQUIRE(fixture.Activation.Stage(MakePackage(second)) == rhi::RasterStatus::Pending);
    ActivationFixture::CompletePending(rhi::RasterStatus::Failed);
    CHECK(fixture.Activation.Poll() == rhi::RasterStatus::Ready);
    const renderer::RenderGraphActivationReport failed = fixture.Activation.GetReport();
    CHECK(failed.Candidate == renderer::RenderGraphCandidateState::Failed);
    CHECK(failed.LastFailure == rhi::RasterStatus::Failed);
    CHECK(failed.ActiveGeneration == 1);
    CHECK(fixture.Activation.GetActiveOrNull() == firstOrNull);
    CHECK(firstOrNull->GetOutputSceneRevision() == 1);
    fixture.Render();
    CHECK(rhi::reference::Draws == 2);
    // A plan rejected before GPU setup fails immediately with the same guarantee.
    PackageOptions malformed;
    malformed.Generation = 3;
    malformed.Backing = 31;
    CHECK(fixture.Activation.Stage(MakePackage(malformed)) == rhi::RasterStatus::InvalidDescription);
    CHECK(fixture.Activation.GetReport().Candidate == renderer::RenderGraphCandidateState::Failed);
    CHECK(fixture.Activation.GetReport().CandidateGeneration == 3);
    CHECK(fixture.Activation.GetActiveOrNull() == firstOrNull);
}

TEST_CASE("RG3 CPU interface mismatch requires a restart and stages nothing", "[render-graph][activation]")
{
    ActivationFixture fixture;
    fixture.ActivateFirst();
    const usize shaderCreates = rhi::reference::Creates[static_cast<usize>(rhi::internal::RasterKind::Shader)];
    PackageOptions changed;
    changed.Interface = OTHER_INTERFACE;
    CHECK(fixture.Activation.Stage(MakePackage(changed)) == rhi::RasterStatus::Unsupported);
    CHECK(fixture.Activation.GetReport().Candidate == renderer::RenderGraphCandidateState::RestartRequired);
    CHECK(rhi::reference::Creates[static_cast<usize>(rhi::internal::RasterKind::Shader)] == shaderCreates);
    CHECK(fixture.Activation.GetReport().ActiveGeneration == 1);
    fixture.Render();
}

TEST_CASE("RG3 activation waits for confirmed submission and retires the old generation", "[render-graph][activation]")
{
    ActivationFixture fixture;
    fixture.ActivateFirst();
    renderer::RenderGraph& first = *fixture.Activation.GetActiveOrNull();
    PackageOptions second;
    second.Generation = 2;
    REQUIRE(fixture.Activation.Stage(MakePackage(second)) == rhi::RasterStatus::Ready);
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Activation.Poll() == rhi::RasterStatus::Ready);
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(first.Encode(fixture.Surface) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(fixture.Device, completion) == rhi::RasterStatus::Ready);
    // The active generation's encoding still needs its actual submission token.
    CHECK(fixture.Activation.Poll() == rhi::RasterStatus::InvalidState);
    CHECK(fixture.Activation.GetReport().ActiveGeneration == 1);
    REQUIRE(first.ConfirmSubmission(completion) == rhi::RasterStatus::Pending);
    const usize textureDestroys = rhi::reference::Destroys[static_cast<usize>(rhi::internal::RasterKind::Texture)];
    REQUIRE(fixture.Activation.Poll() == rhi::RasterStatus::Ready);
    CHECK(fixture.Activation.GetReport().ActiveGeneration == 2);
    // The replaced allocation retires through RHI completion, not at activation.
    CHECK(rhi::reference::Destroys[static_cast<usize>(rhi::internal::RasterKind::Texture)] == textureDestroys);
    rhi::reference::Completed = rhi::reference::Submitted;
    fixture.Render();
    CHECK(rhi::reference::Destroys[static_cast<usize>(rhi::internal::RasterKind::Texture)] > textureDestroys);
}

TEST_CASE("RG3 a newer candidate supersedes an unactivated one", "[render-graph][activation]")
{
    ActivationFixture fixture;
    fixture.ActivateFirst();
    rhi::reference::Next = rhi::RasterStatus::Pending;
    PackageOptions second;
    second.Generation = 2;
    REQUIRE(fixture.Activation.Stage(MakePackage(second)) == rhi::RasterStatus::Pending);
    ActivationFixture::CompletePending(rhi::RasterStatus::Ready);
    PackageOptions third;
    third.Generation = 3;
    REQUIRE(fixture.Activation.Stage(MakePackage(third)) == rhi::RasterStatus::Ready);
    CHECK(fixture.Activation.GetReport().CandidateGeneration == 3);
    REQUIRE(fixture.Activation.Poll() == rhi::RasterStatus::Ready);
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    REQUIRE(fixture.Activation.Poll() == rhi::RasterStatus::Ready);
    CHECK(fixture.Activation.GetReport().ActiveGeneration == 3);
}

TEST_CASE("RG3 runtime packages decode uniform tables and derive stage bindings", "[render-graph][activation]")
{
    PackageOptions options;
    options.HasUniforms = true;
    const Array<uint8> bytes = MakePackageBytes(options);
    renderer::RenderGraphPackage package;
    REQUIRE(package.Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
    const renderer::RenderGraphPlan plan = package.GetPlan();
    CHECK(plan.Description.UniformValueCount == MATERIAL_VALUE_COUNT);
    REQUIRE(plan.UniformBindings.GetSize() == 1);
    CHECK(plan.UniformBindings[0].Program == format::Technique::ColorAndDepth);
    CHECK(plan.UniformBindings[0].Stages == format::Visibility::Fragment);
    REQUIRE(plan.UniformBlocks.GetSize() == 2);
    CHECK(plan.UniformBlocks[1].Target == format::ShaderTarget::Metal);
    REQUIRE(plan.UniformFields.GetSize() == 4);
    CHECK(plan.UniformFields[1].ByteOffset == 12);
    CHECK(plan.UniformFields[3].ByteOffset == 16);
    const renderer::RenderGraphPrograms programs = package.GetPrograms({}, {});
    CHECK(programs.ColorVertex.Bindings.empty() == true);
    CHECK(programs.DepthFragment.Bindings.empty() == true);
    REQUIRE(programs.ColorFragment.Bindings.size() == 1);
    const rhi::RasterBinding& binding = programs.ColorFragment.Bindings[0];
    CHECK(binding.Binding == 0);
    CHECK(binding.Kind == rhi::RasterBindingKind::UniformBuffer);
    CHECK(binding.Visibility == rhi::RasterVisibility::Fragment);
    // The stage admits the largest carried block, so one layout serves every target.
    CHECK(binding.MinSize == METAL_MATERIAL_BYTES);
}

TEST_CASE("RG3 runtime packages carry GLSL ES block names and reject malformed uniform records",
          "[render-graph][activation]")
{
    PackageOptions options;
    options.HasUniforms = true;
    options.HasDepthStages = false;
    const auto makeBytes = [&options](format::ShaderTarget blockTarget, std::string_view block) {
        PackageWriter writer;
        WritePlan(writer, options, 4);
        WriteStage(writer, format::StageRole::ColorVertex, format::ShaderTarget::GlslEs, GLSL_TEXT, "main");
        WriteStage(writer,
                   format::StageRole::ColorFragment,
                   format::ShaderTarget::GlslEs,
                   GLSL_TEXT,
                   "main",
                   blockTarget == format::ShaderTarget::GlslEs ? block : std::string_view{});
        WriteStage(writer, format::StageRole::ColorVertex, format::ShaderTarget::Wgsl, VERTEX_TEXT, "VertexMain");
        WriteStage(writer,
                   format::StageRole::ColorFragment,
                   format::ShaderTarget::Wgsl,
                   FRAGMENT_TEXT,
                   "FragmentMain",
                   blockTarget == format::ShaderTarget::Wgsl ? block : std::string_view{});
        writer.WriteWord(static_cast<uint32>(options.PassName.size()));
        writer.WritePadded(options.PassName);
        return Move(writer.Bytes);
    };
    const Array<uint8> valid = makeBytes(format::ShaderTarget::GlslEs, BLOCK_NAME);
    renderer::RenderGraphPackage package;
    REQUIRE(package.Decode({valid.GetData(), valid.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
    const renderer::RenderGraphPrograms programs = package.GetPrograms({}, {});
    CHECK(programs.ColorFragment.UniformBlocks[0] == BLOCK_NAME);
    CHECK(programs.ColorVertex.UniformBlocks[0].empty() == true);
    // Block names belong to GLSL ES stages only, and must be identifiers.
    CHECK(DecodeBytes(makeBytes(format::ShaderTarget::Wgsl, BLOCK_NAME)) ==
          renderer::RenderGraphPackageStatus::Malformed);
    CHECK(DecodeBytes(makeBytes(format::ShaderTarget::GlslEs, "9block")) ==
          renderer::RenderGraphPackageStatus::Malformed);
    // The binding record follows the pass record; its third word is the stage visibility.
    const usize binding = format::HEADER_BYTES + 2 * format::RESOURCE_RECORD_BYTES + format::CAMERA_RECORD_BYTES +
                          format::PASS_RECORD_BYTES;
    Array<uint8> badVisibility = Array<uint8>::FromRange(valid.AsSpan());
    badVisibility[binding + 2 * sizeof(uint32)] = 4;
    CHECK(DecodeBytes(badVisibility) == renderer::RenderGraphPackageStatus::Malformed);
    Array<uint8> manyValues = Array<uint8>::FromRange(valid.AsSpan());
    manyValues[5 * sizeof(uint32)] = static_cast<uint8>(format::MAX_UNIFORM_VALUES + 1);
    CHECK(DecodeBytes(manyValues) == renderer::RenderGraphPackageStatus::CapacityExceeded);
}

TEST_CASE("RG3 bound generations activate and draw per-draw uniform values", "[render-graph][activation]")
{
    ActivationFixture fixture;
    PackageOptions options;
    options.HasUniforms = true;
    REQUIRE(fixture.Activation.Stage(MakePackage(options)) == rhi::RasterStatus::Ready);
    REQUIRE(fixture.Activation.Poll() == rhi::RasterStatus::Ready);
    REQUIRE(fixture.Activation.GetReport().ActiveGeneration == 1);
    renderer::RenderGraphReport report;
    // A bound generation needs the complete logical value table for every drawn item.
    CHECK(fixture.Update(report) == rhi::RasterStatus::InvalidDescription);
    StaticArray<float32, MATERIAL_VALUE_COUNT> values{{1, .5F, .25F, 2}};
    fixture.Items[0].Uniforms = {values.GetData(), values.GetSize()};
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    fixture.Render();
    CHECK(rhi::reference::Draws == 1);
}

TEST_CASE("RG3 a bound generation compiled against another uniform interface requires a restart",
          "[render-graph][activation]")
{
    ActivationFixture fixture;
    fixture.ActivateFirst();
    PackageOptions bound;
    bound.HasUniforms = true;
    bound.Interface = OTHER_INTERFACE;
    bound.Generation = 2;
    CHECK(fixture.Activation.Stage(MakePackage(bound)) == rhi::RasterStatus::Unsupported);
    CHECK(fixture.Activation.GetReport().Candidate == renderer::RenderGraphCandidateState::RestartRequired);
    CHECK(fixture.Activation.GetReport().ActiveGeneration == 1);
}

TEST_CASE("RG4 the scratch generation rewrites extents and adds approved unexported backings", "[render-graph][pool]")
{
    const core::UniquePtr<renderer::RenderGraphPackage> package = MakeScratchPackage(1, {SCRATCH_COUNT, HEIGHT, WIDTH});
    const renderer::RenderGraphPlan plan = package->GetPlan();
    REQUIRE(plan.Resources.GetSize() == 2 + 2 * SCRATCH_COUNT);
    REQUIRE(plan.Passes.GetSize() == 1 + SCRATCH_COUNT);
    for (usize i = 0; i < plan.Resources.GetSize(); ++i)
    {
        CHECK(plan.Resources[i].Width == HEIGHT);
        CHECK(plan.Resources[i].Height == WIDTH);
        CHECK(plan.Resources[i].IsExported == (i == 0 ? 1U : 0U));
    }
    for (usize i = 0; i < plan.Passes.GetSize(); ++i)
    {
        CHECK(plan.Passes[i].AllowsRepeatedCoverage == 1);
    }
    CHECK(plan.Resources[3].Backing == 2);
}

TEST_CASE("RG4 generation reloads reuse the retired generation's transients after completion", "[render-graph][pool]")
{
    ActivationFixture fixture;
    constexpr usize RELOADS = 5;
    for (usize generation = 1; generation <= RELOADS; ++generation)
    {
        const usize creates = rhi::reference::Creates[TEXTURE];
        fixture.Reload(MakeScratchPackage(static_cast<uint8>(generation)), generation);
        // The first two generations overlap the active one; later ones reuse the retired set.
        const usize transientCreates = generation <= 2 ? SCRATCH_COUNT : 0;
        CHECK(rhi::reference::Creates[TEXTURE] == creates + 1 + transientCreates);
        CHECK(fixture.Activation.GetActiveOrNull()->GetAllocationReport().ReusedAllocations ==
              SCRATCH_COUNT - transientCreates);
        fixture.Render();
        rhi::reference::Completed = rhi::reference::Submitted;
        CHECK(fixture.Activation.GetActiveOrNull()->GetOutputSceneRevision() == generation);
    }
    const renderer::RenderGraphPoolReport report = fixture.Activation.GetPoolReport();
    CHECK(report.CreatedAllocations == uint64{2} * SCRATCH_COUNT);
    CHECK(report.ReusedAllocations == (RELOADS - 2) * SCRATCH_COUNT);
    CHECK(report.LeasedAllocations == SCRATCH_COUNT);
    CHECK(report.IdleAllocations == SCRATCH_COUNT);
    fixture.Activation.TrimPool();
    CHECK(fixture.Activation.GetPoolReport().IdleAllocations == 0);
}

TEST_CASE("RG4 reload defers reuse while the retired generation's frame is in flight", "[render-graph][pool]")
{
    ActivationFixture fixture;
    fixture.Reload(MakeScratchPackage(1), 1);
    // Submitted and still in flight when the generation is retired.
    fixture.Render();
    fixture.Reload(MakeScratchPackage(2), 2);
    REQUIRE(fixture.Activation.GetPoolReport().IdleAllocations == SCRATCH_COUNT);
    const usize creates = rhi::reference::Creates[TEXTURE];
    REQUIRE(fixture.Activation.Stage(MakeScratchPackage(3)) == rhi::RasterStatus::Ready);
    CHECK(rhi::reference::Creates[TEXTURE] == creates + 1 + SCRATCH_COUNT);
    CHECK(fixture.Activation.GetPoolReport().DeferredAcquisitions == SCRATCH_COUNT);
    CHECK(fixture.Activation.GetPoolReport().ReusedAllocations == 0);
    rhi::reference::Completed = rhi::reference::Submitted;
    // Superseding the unactivated candidate returns its never-submitted transients too.
    fixture.Reload(MakeScratchPackage(4), 3);
    CHECK(fixture.Activation.GetActiveOrNull()->GetAllocationReport().ReusedAllocations == SCRATCH_COUNT);
    CHECK(fixture.Activation.GetPoolReport().IdleAllocations == 2 * SCRATCH_COUNT);
    fixture.Render();
    rhi::reference::Completed = rhi::reference::Submitted;
    CHECK(fixture.Activation.GetActiveOrNull()->GetOutputSceneRevision() == 3);
}

TEST_CASE("RG4 alternating-extent reloads reuse each extent's own transients", "[render-graph][pool]")
{
    ActivationFixture fixture;
    constexpr usize RELOADS = 6;
    for (usize generation = 1; generation <= RELOADS; ++generation)
    {
        const bool isWide = generation % 2 == 1;
        fixture.Reload(
            MakeScratchPackage(static_cast<uint8>(generation),
                               {SCRATCH_COUNT, isWide == true ? WIDTH : HEIGHT, isWide == true ? HEIGHT : WIDTH}),
            generation);
        fixture.Render();
        rhi::reference::Completed = rhi::reference::Submitted;
    }
    const renderer::RenderGraphPoolReport report = fixture.Activation.GetPoolReport();
    // Generation n reuses generation n - 2's set, which has the same extent.
    CHECK(report.CreatedAllocations == uint64{2} * SCRATCH_COUNT);
    CHECK(report.ReusedAllocations == (RELOADS - 2) * SCRATCH_COUNT);
    CHECK(report.DeferredAcquisitions == 0);
}

TEST_CASE("RG4 serial-control activation creates every generation's attachments", "[render-graph][pool]")
{
    ActivationFixture fixture{renderer::RenderGraphAllocation::SerialControl};
    constexpr usize RELOADS = 4;
    for (usize generation = 1; generation <= RELOADS; ++generation)
    {
        const usize creates = rhi::reference::Creates[TEXTURE];
        fixture.Reload(MakeScratchPackage(static_cast<uint8>(generation)), generation);
        CHECK(rhi::reference::Creates[TEXTURE] == creates + 1 + SCRATCH_COUNT);
        CHECK(fixture.Activation.GetActiveOrNull()->GetAllocationReport().IsPooled == false);
        fixture.Render();
        rhi::reference::Completed = rhi::reference::Submitted;
    }
    const renderer::RenderGraphPoolReport report = fixture.Activation.GetPoolReport();
    CHECK(report.CreatedAllocations == 0);
    CHECK(report.IdleAllocations == 0);
}

TEST_CASE("runtime generation owns validated authored GPU pass names", "[renderer][render-graph][package]")
{
    PackageOptions options;
    options.Version = format::PACKAGE_VERSION;
    options.PassName = "Scene/\"Opaque\"";
    const core::UniquePtr<renderer::RenderGraphPackage> package = MakePackage(options);
    const renderer::RenderGraphPlan plan = package->GetPlan();
    REQUIRE(plan.PassNames.GetSize() == 1);
    CHECK(std::string_view(plan.PassNames[0].GetDataOrNull(), plan.PassNames[0].GetSize()) == options.PassName);
    for (const std::string_view invalid :
         {std::string_view{}, std::string_view{"a\0b", 3}, std::string_view{"\xC0\xAF", 2}})
    {
        options.PassName = invalid;
        const Array<uint8> bytes = MakePackageBytes(options);
        renderer::RenderGraphPackage rejected;
        CHECK(rejected.Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::Malformed);
        CHECK(rejected.IsLoaded() == false);
    }
    options.PassName = "Scene";
    Array<uint8> truncated = MakePackageBytes(options);
    renderer::RenderGraphPackage rejected;
    CHECK(rejected.Decode({truncated.GetData(), truncated.GetSize() - 1}) ==
          renderer::RenderGraphPackageStatus::Malformed);
}

TEST_CASE("runtime packages preserve relative policies and decode fixed legacy versions",
          "[render-graph][activation][size]")
{
    for (const uint32 version : {format::LEGACY_PACKAGE_VERSION,
                                 format::NAMED_PACKAGE_VERSION,
                                 format::VIEW_PACKAGE_VERSION,
                                 format::SCENE_PACKAGE_VERSION,
                                 format::NATIVE_RAY_PACKAGE_VERSION,
                                 format::PACKAGE_VERSION})
    {
        PackageOptions options;
        options.Version = version;
        options.ViewNumerator = 1;
        options.ViewDenominator = 2;
        const core::UniquePtr<renderer::RenderGraphPackage> package = MakePackage(options);
        const format::Resource& resource = package->GetPlan().Resources[0];
        CHECK(resource.ViewNumerator == (version >= format::VIEW_PACKAGE_VERSION ? 1 : 0));
        CHECK(resource.ViewDenominator == (version >= format::VIEW_PACKAGE_VERSION ? 2 : 1));
        CHECK(package->GetPlan().PassNames.IsEmpty() == (version == format::LEGACY_PACKAGE_VERSION));
    }
    PackageOptions invalid;
    invalid.ViewNumerator = 1;
    invalid.ViewDenominator = 0;
    CHECK(DecodeBytes(MakePackageBytes(invalid)) == renderer::RenderGraphPackageStatus::Malformed);
}

TEST_CASE("activation resizes a relative generation without staging another shader generation",
          "[render-graph][activation][size]")
{
    ActivationFixture fixture;
    REQUIRE(fixture.Activation.Resize(96, 64) == rhi::RasterStatus::Ready);
    PackageOptions options;
    options.ViewNumerator = 1;
    REQUIRE(fixture.Activation.Stage(MakePackage(options)) == rhi::RasterStatus::Ready);
    REQUIRE(fixture.Activation.Poll() == rhi::RasterStatus::Ready);
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    fixture.Render();
    rhi::reference::Completed = rhi::reference::Submitted;
    const uint64 serial = fixture.Activation.GetReport().ActiveGeneration;
    REQUIRE(fixture.Activation.Resize(193, 129) == rhi::RasterStatus::Pending);
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    CHECK(fixture.Activation.Poll() == rhi::RasterStatus::Ready);
    CHECK(fixture.Activation.GetReport().ActiveGeneration == serial);
    format::Resource resource{};
    REQUIRE(fixture.Activation.GetActiveOrNull()->GetResourceDescription(0, resource) == true);
    CHECK(resource.Width == 193);
    CHECK(resource.Height == 129);
    CHECK(fixture.Activation.Resize(0, 0) == rhi::RasterStatus::NotReady);
}

TEST_CASE("resizing a staged generation requests its first extraction before activation",
          "[render-graph][activation][size]")
{
    ActivationFixture fixture;
    REQUIRE(fixture.Activation.Resize(96, 64) == rhi::RasterStatus::Ready);
    PackageOptions options;
    options.ViewNumerator = 1;
    REQUIRE(fixture.Activation.Stage(MakePackage(options)) == rhi::RasterStatus::Ready);
    REQUIRE(fixture.Activation.Resize(193, 129) == rhi::RasterStatus::Pending);
    CHECK(fixture.Activation.Poll() == rhi::RasterStatus::Pending);
    CHECK(fixture.Activation.GetReport().ActiveGeneration == 0);
    CHECK(fixture.Activation.GetReport().Candidate == renderer::RenderGraphCandidateState::NeedsUpdate);
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    CHECK(fixture.Activation.Poll() == rhi::RasterStatus::Ready);
    REQUIRE(fixture.Activation.GetActiveOrNull() != nullptr);
    format::Resource resource{};
    REQUIRE(fixture.Activation.GetActiveOrNull()->GetResourceDescription(0, resource) == true);
    CHECK(resource.Width == 193);
}

TEST_CASE("runtime packages preserve scene selection and default legacy packages to full Scene",
          "[render-graph][activation][bundle]")
{
    PackageOptions options;
    options.SceneData = ::ludus::content::render_graph_format::SCENE_DATA_GEOMETRY;
    CHECK(MakePackage(options)->GetPlan().Passes[0].SceneData ==
          ::ludus::content::render_graph_format::SCENE_DATA_GEOMETRY);
    options.Version = format::VIEW_PACKAGE_VERSION;
    CHECK(MakePackage(options)->GetPlan().Passes[0].SceneData == ::ludus::content::render_graph_format::SCENE_DATA_ALL);
    options.Version = format::PACKAGE_VERSION;
    options.SceneData = ::ludus::content::render_graph_format::SCENE_DATA_MATERIALS;
    CHECK(DecodeBytes(MakePackageBytes(options)) == renderer::RenderGraphPackageStatus::Malformed);
    options.SceneData = ~::ludus::content::render_graph_format::SCENE_DATA_ALL;
    CHECK(DecodeBytes(MakePackageBytes(options)) == renderer::RenderGraphPackageStatus::Malformed);
}

TEST_CASE("Renderer selection chooses raster on a device without native rays", "[render-graph][selection]")
{
    ActivationFixture fixture;
    renderer::RenderGraphRenderer selected = renderer::RenderGraphRenderer::HardwareRayTracing;
    REQUIRE(renderer::SelectRenderGraphRenderer(fixture.Device, renderer::RenderGraphPreference::Automatic, selected) ==
            rhi::RasterStatus::Ready);
    CHECK(selected == renderer::RenderGraphRenderer::Raster);
    REQUIRE(renderer::SelectRenderGraphRenderer(fixture.Device,
                                                renderer::RenderGraphPreference::ForceRaster,
                                                selected) == rhi::RasterStatus::Ready);
    CHECK(selected == renderer::RenderGraphRenderer::Raster);
    selected = renderer::RenderGraphRenderer::HardwareRayTracing;
    CHECK(renderer::SelectRenderGraphRenderer({}, renderer::RenderGraphPreference::Automatic, selected) ==
          rhi::RasterStatus::InvalidHandle);
    CHECK(selected == renderer::RenderGraphRenderer::HardwareRayTracing);
    CHECK(renderer::SelectRenderGraphRenderer(fixture.Device,
                                              static_cast<renderer::RenderGraphPreference>(255),
                                              selected) == rhi::RasterStatus::InvalidDescription);
    CHECK(selected == renderer::RenderGraphRenderer::HardwareRayTracing);
}
TEST_CASE("Selected raster activation preserves the drawable on an invalid selection", "[render-graph][selection]")
{
    ActivationFixture fixture;
    REQUIRE(fixture.Activation.StageSelected(MakePackage({}), {}) == rhi::RasterStatus::Ready);
    REQUIRE(fixture.Activation.Poll() == rhi::RasterStatus::Ready);
    CHECK(fixture.Activation.GetReport().ActiveRenderer == renderer::RenderGraphRenderer::Raster);
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    fixture.Render();
    rhi::reference::Completed = rhi::reference::Submitted;
    const uint64 active = fixture.Activation.GetReport().ActiveGeneration;
    CHECK(fixture.Activation.StageSelected({}, {}) == rhi::RasterStatus::InvalidDescription);
    CHECK(fixture.Activation.StageSelected(MakePackage({}), MakePackage({})) == rhi::RasterStatus::InvalidDescription);
    CHECK(fixture.Activation.GetReport().ActiveGeneration == active);
    CHECK(fixture.Activation.GetActiveOrNull()->GetOutputSceneRevision() == 1);
    fixture.Render();
}

TEST_CASE("Unsupported native rays select raster without preparing the ray package", "[render-graph][selection]")
{
    ActivationFixture fixture;
    PackageOptions rayOptions;
    rayOptions.Program = format::Technique::RayTracing;
    core::UniquePtr<renderer::RenderGraphPackage> ray = MakePackage(rayOptions);
    REQUIRE(renderer::RequiresHardwareRayTracing(ray->GetPlan()) == true);
    // This decoded candidate deliberately has no native ray shader. Selection must discard it
    // before GPU preparation, rather than reporting a missing ray artifact or tracing in software.
    REQUIRE(fixture.Activation.StageSelected(MakePackage({}), Move(ray)) == rhi::RasterStatus::Ready);
    REQUIRE(fixture.Activation.Poll() == rhi::RasterStatus::Ready);
    CHECK(fixture.Activation.GetReport().ActiveRenderer == renderer::RenderGraphRenderer::Raster);
    renderer::RenderGraphReport report;
    REQUIRE(fixture.Update(report) == rhi::RasterStatus::Ready);
    fixture.Render();
    rhi::reference::Completed = rhi::reference::Submitted;
    CHECK(fixture.Activation.GetActiveOrNull()->GetOutputSceneRevision() == 1);
}

TEST_CASE("runtime independent shader pairs decode with their own stage and binding tables",
          "[render-graph][activation]")
{
    constexpr format::Technique GBUFFER_PROGRAM = static_cast<format::Technique>(format::LEGACY_TECHNIQUE_COUNT);
    PackageOptions options;
    options.Program = GBUFFER_PROGRAM;
    options.HasDepthStages = false;
    options.HasUniforms = true;
    PackageWriter writer;
    WritePlan(writer, options, 4);
    // WritePlan's uniform fixture starts with the legacy color program. Retarget
    // that serialized binding explicitly; the rest of its layout is unchanged.
    const usize bindingOffset = format::HEADER_BYTES + 2 * format::RESOURCE_RECORD_BYTES + format::CAMERA_RECORD_BYTES +
                                format::PASS_RECORD_BYTES;
    writer.Bytes[bindingOffset] = static_cast<uint8>(format::LEGACY_TECHNIQUE_COUNT);
    const format::StageRole vertexRole = static_cast<format::StageRole>(format::GetRasterStageRole(GBUFFER_PROGRAM, 0));
    const format::StageRole fragmentRole =
        static_cast<format::StageRole>(format::GetRasterStageRole(GBUFFER_PROGRAM, 1));
    for (const format::ShaderTarget target : {format::ShaderTarget::Wgsl, format::ShaderTarget::Metal})
    {
        WriteStage(writer, vertexRole, target, VERTEX_TEXT, "GBufferVertex");
        WriteStage(writer, fragmentRole, target, FRAGMENT_TEXT, "GBufferFragment");
    }
    writer.WriteWord(static_cast<uint32>(options.PassName.size()));
    writer.WritePadded(options.PassName);
    renderer::RenderGraphPackage package;
    REQUIRE(package.Decode({writer.Bytes.GetData(), writer.Bytes.GetSize()}) ==
            renderer::RenderGraphPackageStatus::Ready);
    CHECK(package.GetPlan().Passes[0].Program == GBUFFER_PROGRAM);
    const renderer::RenderGraphPrograms programs = package.GetPrograms({}, {});
    CHECK(programs.ColorFragment.Artifact.Wgsl.empty() == true);
    CHECK(programs.AdditionalRasterStages[0].Artifact.WgslEntry == "GBufferVertex");
    CHECK(programs.AdditionalRasterStages[1].Artifact.WgslEntry == "GBufferFragment");
    REQUIRE(programs.AdditionalRasterStages[1].Bindings.size() == 1);
    CHECK(programs.AdditionalRasterStages[1].Bindings[0].MinSize == METAL_MATERIAL_BYTES);
    // Old formats must never reinterpret an extended role as a legacy program.
    writer.Bytes[sizeof(uint32)] = static_cast<uint8>(format::NATIVE_RAY_PACKAGE_VERSION);
    renderer::RenderGraphPackage legacy;
    CHECK(legacy.Decode({writer.Bytes.GetData(), writer.Bytes.GetSize()}) ==
          renderer::RenderGraphPackageStatus::Malformed);
    CHECK(legacy.IsLoaded() == false);
}
