#include <ludus/graphics/renderer/render_graph_activation.hpp>

#include <ludus/foundation/base/byte_order.hpp>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <new>
#include <span>
#include <string_view>

// RG3 staging/activation follows the reload contract in
// docs/architecture/render-graph-authoring.md ("Editor interaction, diagnostics
// and hot reload"). Thanks to Christopher Maughan, "The Design of FX Composer",
// GPU Gems, chapter 30, sections 30.6.2-30.6.4, for separating device-independent
// compilation from device instantiation; Ludus prewarms a complete candidate and
// publishes it between frames instead of applying effects lazily to the device.
namespace ludus::graphics::renderer
{
using namespace foundation;
namespace format = content::render_graph_format;
using Status = rhi::RasterStatus;
namespace
{
constexpr uint32 MAX_ASPECT = static_cast<uint32>(format::Aspect::Depth);
constexpr uint32 MAX_TECHNIQUE = static_cast<uint32>(format::Technique::RayTracing);
constexpr uint32 MAX_VISIBILITY = static_cast<uint32>(format::Visibility::Both);
constexpr uint32 MAX_TARGET = static_cast<uint32>(format::ShaderTarget::GlslEs);
constexpr uint32 MAX_LOAD = static_cast<uint32>(format::Load::Discard);
constexpr uint32 MAX_STORE = static_cast<uint32>(format::Store::Discard);
constexpr uint32 FIRST_ENTRY_CHARACTER = 0x21;
constexpr uint32 LAST_ENTRY_CHARACTER = 0x7E;
bool IsAccepted(Status status) noexcept
{
    return status == Status::Ready || status == Status::Pending;
}
/// Bounded little-endian cursor; any short read latches failure.
class ByteReader final
{
public:
    explicit ByteReader(std::span<const uint8> bytes) noexcept : mBytes(bytes) {}
    [[nodiscard]] uint32 ReadWord() noexcept
    {
        uint32 value = 0;
        if (mIsOk == false || mBytes.size() - mOffset < sizeof(uint32) ||
            TryReadLittleEndian(mBytes.subspan(mOffset, sizeof(uint32)), value) == false)
        {
            mIsOk = false;
            return 0;
        }
        mOffset += sizeof(uint32);
        return value;
    }
    /// Read count bytes followed by zero padding to a four-byte boundary.
    [[nodiscard]] std::span<const uint8> ReadPadded(usize count) noexcept
    {
        const usize padding = (sizeof(uint32) - count % sizeof(uint32)) % sizeof(uint32);
        if (mIsOk == false || mBytes.size() - mOffset < count || mBytes.size() - mOffset - count < padding)
        {
            mIsOk = false;
            return {};
        }
        const std::span<const uint8> result = mBytes.subspan(mOffset, count);
        for (usize i = 0; i < padding; ++i)
        {
            mIsOk = mIsOk == true && mBytes[mOffset + count + i] == 0;
        }
        mOffset += count + padding;
        return result;
    }
    [[nodiscard]] std::span<const uint8> ReadIdentity() noexcept
    {
        if (mIsOk == false || mBytes.size() - mOffset < format::IDENTITY_BYTES)
        {
            mIsOk = false;
            return {};
        }
        const std::span<const uint8> result = mBytes.subspan(mOffset, format::IDENTITY_BYTES);
        mOffset += format::IDENTITY_BYTES;
        return result;
    }
    [[nodiscard]] bool IsOk() const noexcept
    {
        return mIsOk;
    }
    [[nodiscard]] bool IsComplete() const noexcept
    {
        return mIsOk == true && mOffset == mBytes.size();
    }

private:
    std::span<const uint8> mBytes;
    usize mOffset = 0;
    bool mIsOk = true;
};
/// GLSL ES identifier: a letter or underscore, then letters, digits or underscores.
bool IsBlockName(std::span<const uint8> name) noexcept
{
    bool isValid = name.empty() == false && name.size() <= format::MAX_ENTRY_BYTES &&
                   ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_');
    for (const uint8 character : name)
    {
        isValid =
            isValid == true && ((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                                (character >= '0' && character <= '9') || character == '_');
    }
    return isValid;
}
bool IsEntryName(std::span<const uint8> entry) noexcept
{
    bool isValid = entry.empty() == false && entry.size() <= format::MAX_ENTRY_BYTES;
    for (const uint8 character : entry)
    {
        isValid = isValid == true && character >= FIRST_ENTRY_CHARACTER && character <= LAST_ENTRY_CHARACTER;
    }
    return isValid;
}
} // namespace

struct RenderGraphPackage::State final
{
    /// One decoded artifact; text views borrow Bytes, SPIR-V words are copied aligned.
    struct StageView final
    {
        bool IsPresent = false;
        std::string_view Entry;
        std::string_view Text;
        Array<uint32> Words;
        /// GLSL ES uniform block names by binding number; empty for other targets.
        StaticArray<std::string_view, format::UNIFORM_SLOT_COUNT> Blocks{};
    };
    Array<uint8> Bytes;
    format::Header Header{};
    StaticArray<format::Resource, format::MAX_RESOURCES> Resources{};
    StaticArray<format::Camera, format::MAX_CAMERAS> Cameras{};
    StaticArray<format::Pass, format::MAX_PASSES> Passes{};
    // DecodeUniforms validates and writes every active stage before any binding is published.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
    StaticArray<format::UniformBinding, format::MAX_UNIFORM_BINDINGS> UniformBindings{};
    StaticArray<format::UniformBlock, format::MAX_UNIFORM_BLOCKS> UniformBlocks{};
    StaticArray<format::UniformField, format::MAX_UNIFORM_FIELDS> UniformFields{};
    /// Stage binding requirements derived from the uniform records, one table per role.
    // RasterBinding initializes Visibility to Both, including every nested array element.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
    StaticArray<StaticArray<rhi::RasterBinding, format::UNIFORM_SLOT_COUNT>, format::STAGE_ROLE_COUNT> Bindings{};
    StaticArray<usize, format::STAGE_ROLE_COUNT> BindingCounts{};
    StaticArray<uint8, format::IDENTITY_BYTES> InterfaceIdentity{};
    StaticArray<uint8, format::IDENTITY_BYTES> GenerationIdentity{};
    StaticArray<rhi::RasterShaderInput, format::STATIC_INPUT_COUNT> Inputs{};
    StaticArray<StaticArray<StageView, format::SHADER_TARGET_COUNT>, format::STAGE_ROLE_COUNT> Stages{};
    RenderGraphPackageStatus DecodeRecords(ByteReader& inoutReader) noexcept;
    RenderGraphPackageStatus DecodeUniforms(ByteReader& inoutReader) noexcept;
    void DeriveBindings() noexcept;
    RenderGraphPackageStatus DecodeStages(ByteReader& inoutReader, uint32 stageCount) noexcept;
};
RenderGraphPackageStatus RenderGraphPackage::State::DecodeStages(ByteReader& inoutReader, uint32 stageCount) noexcept
{
    for (uint32 stage = 0; stage < stageCount; ++stage)
    {
        const uint32 role = inoutReader.ReadWord();
        const uint32 target = inoutReader.ReadWord();
        const uint32 inputCount = inoutReader.ReadWord();
        const uint32 entryBytes = inoutReader.ReadWord();
        const uint32 artifactBytes = inoutReader.ReadWord();
        const uint32 blockCount = inoutReader.ReadWord();
        if (inoutReader.IsOk() == false || role >= format::STAGE_ROLE_COUNT || target >= format::SHADER_TARGET_COUNT ||
            blockCount > format::UNIFORM_SLOT_COUNT ||
            (blockCount != 0 && target != static_cast<uint32>(format::ShaderTarget::GlslEs)))
        {
            return RenderGraphPackageStatus::Malformed;
        }
        const bool isRayRole = role >= static_cast<uint32>(format::StageRole::RayCompute);
        const bool isVertex = isRayRole == true ? role == static_cast<uint32>(format::StageRole::RayImageVertex)
                                                : role % format::TECHNIQUE_STAGE_COUNT == 0;
        if (inputCount != (isVertex == true ? (isRayRole == true ? 2 : format::STATIC_INPUT_COUNT) : 0))
        {
            return RenderGraphPackageStatus::Malformed;
        }
        for (uint32 input = 0; input < inputCount; ++input)
        {
            const uint32 location = inoutReader.ReadWord();
            const uint32 inputFormat = inoutReader.ReadWord();
            const format::InputFormat expected =
                input == 0 ? (isRayRole == true ? format::InputFormat::Float2 : format::InputFormat::Float3)
                           : format::InputFormat::Float4;
            if (location != input || inputFormat != static_cast<uint32>(expected))
            {
                return RenderGraphPackageStatus::Malformed;
            }
        }
        StageView& view = Stages[role][target];
        for (uint32 block = 0; block < blockCount; ++block)
        {
            const uint32 slot = inoutReader.ReadWord();
            const uint32 nameBytes = inoutReader.ReadWord();
            if (inoutReader.IsOk() == false || slot >= format::UNIFORM_SLOT_COUNT ||
                nameBytes > format::MAX_ENTRY_BYTES || view.Blocks[slot].empty() == false)
            {
                return RenderGraphPackageStatus::Malformed;
            }
            const std::span<const uint8> name = inoutReader.ReadPadded(nameBytes);
            if (inoutReader.IsOk() == false || IsBlockName(name) == false)
            {
                return RenderGraphPackageStatus::Malformed;
            }
            view.Blocks[slot] = {reinterpret_cast<const char*>(name.data()), name.size()};
        }
        if (artifactBytes > format::MAX_ARTIFACT_BYTES)
        {
            return RenderGraphPackageStatus::CapacityExceeded;
        }
        const std::span<const uint8> entry = inoutReader.ReadPadded(entryBytes);
        const std::span<const uint8> artifact = inoutReader.ReadPadded(artifactBytes);
        if (inoutReader.IsOk() == false || IsEntryName(entry) == false || artifact.empty() == true ||
            view.IsPresent == true)
        {
            return RenderGraphPackageStatus::Malformed;
        }
        view.IsPresent = true;
        view.Entry = {reinterpret_cast<const char*>(entry.data()), entry.size()};
        if (target == static_cast<uint32>(format::ShaderTarget::Spirv))
        {
            if (artifact.size() % sizeof(uint32) != 0)
            {
                return RenderGraphPackageStatus::Malformed;
            }
            if (view.Words.TryResize(artifact.size() / sizeof(uint32)) == false)
            {
                return RenderGraphPackageStatus::OutOfMemory;
            }
            for (usize word = 0; word < view.Words.GetSize(); ++word)
            {
                static_cast<void>(TryReadLittleEndian(artifact.subspan(word * sizeof(uint32)), view.Words[word]));
            }
        }
        else
        {
            view.Text = {reinterpret_cast<const char*>(artifact.data()), artifact.size()};
        }
    }
    // Each technique needs its vertex and fragment stage for exactly the same targets.
    for (usize technique = 0; technique < format::RASTER_TECHNIQUE_COUNT; ++technique)
    {
        for (usize target = 0; target < format::SHADER_TARGET_COUNT; ++target)
        {
            if (Stages[technique * format::TECHNIQUE_STAGE_COUNT][target].IsPresent !=
                Stages[technique * format::TECHNIQUE_STAGE_COUNT + 1][target].IsPresent)
            {
                return RenderGraphPackageStatus::Malformed;
            }
        }
    }
    bool hasLegacyProfile = false;
    bool hasSceneProfile = false;
    for (usize target = 0; target < format::SHADER_TARGET_COUNT; ++target)
    {
        const bool hasLegacyCompute = Stages[static_cast<usize>(format::StageRole::RayCompute)][target].IsPresent;
        const bool hasSceneCompute = Stages[static_cast<usize>(format::StageRole::SceneRayCompute)][target].IsPresent;
        if (hasLegacyCompute == true && hasSceneCompute == true)
        {
            return RenderGraphPackageStatus::Malformed;
        }
        hasLegacyProfile = hasLegacyProfile == true || hasLegacyCompute == true;
        hasSceneProfile = hasSceneProfile == true || hasSceneCompute == true;
        const bool hasCompute = hasLegacyCompute == true || hasSceneCompute == true;
        if (Stages[static_cast<usize>(format::StageRole::RayImageVertex)][target].IsPresent != hasCompute ||
            Stages[static_cast<usize>(format::StageRole::RayImageFragment)][target].IsPresent != hasCompute)
        {
            return RenderGraphPackageStatus::Malformed;
        }
    }
    return hasLegacyProfile == true && hasSceneProfile == true ? RenderGraphPackageStatus::Malformed
                                                               : RenderGraphPackageStatus::Ready;
}
RenderGraphPackageStatus RenderGraphPackage::State::DecodeUniforms(ByteReader& inoutReader) noexcept
{
    for (uint32 i = 0; i < Header.UniformBindingCount; ++i)
    {
        format::UniformBinding& binding = UniformBindings[i];
        const uint32 program = inoutReader.ReadWord();
        binding.Slot = inoutReader.ReadWord();
        const uint32 stages = inoutReader.ReadWord();
        binding.ValueOffset = inoutReader.ReadWord();
        binding.ValueCount = inoutReader.ReadWord();
        if (program >= static_cast<uint32>(format::Technique::RayTracing) || stages == 0 || stages > MAX_VISIBILITY ||
            binding.Slot >= format::UNIFORM_SLOT_COUNT)
        {
            return RenderGraphPackageStatus::Malformed;
        }
        binding.Program = static_cast<format::Technique>(program);
        binding.Stages = static_cast<format::Visibility>(stages);
    }
    for (uint32 i = 0; i < Header.UniformBlockCount; ++i)
    {
        format::UniformBlock& block = UniformBlocks[i];
        block.Binding = inoutReader.ReadWord();
        const uint32 target = inoutReader.ReadWord();
        block.Size = inoutReader.ReadWord();
        if (target > MAX_TARGET || block.Binding >= Header.UniformBindingCount ||
            block.Size > format::MAX_UNIFORM_BLOCK_BYTES)
        {
            return RenderGraphPackageStatus::Malformed;
        }
        block.Target = static_cast<format::ShaderTarget>(target);
    }
    for (uint32 i = 0; i < Header.UniformFieldCount; ++i)
    {
        format::UniformField& field = UniformFields[i];
        field.Binding = inoutReader.ReadWord();
        const uint32 target = inoutReader.ReadWord();
        field.ValueOffset = inoutReader.ReadWord();
        field.Components = inoutReader.ReadWord();
        field.ByteOffset = inoutReader.ReadWord();
        if (target > MAX_TARGET || field.Binding >= Header.UniformBindingCount)
        {
            return RenderGraphPackageStatus::Malformed;
        }
        field.Target = static_cast<format::ShaderTarget>(target);
    }
    return inoutReader.IsOk() == true ? RenderGraphPackageStatus::Ready : RenderGraphPackageStatus::Malformed;
}
void RenderGraphPackage::State::DeriveBindings() noexcept
{
    // The same rule as the compiled header: each stage declares its technique's slots,
    // with the largest carried block as the uniform range. RenderGraph rechecks the result.
    for (uint32 i = 0; i < Header.UniformBindingCount; ++i)
    {
        const format::UniformBinding& binding = UniformBindings[i];
        usize bytes = 0;
        for (uint32 block = 0; block < Header.UniformBlockCount; ++block)
        {
            if (UniformBlocks[block].Binding == i && UniformBlocks[block].Size > bytes)
            {
                bytes = UniformBlocks[block].Size;
            }
        }
        for (usize stage = 0; stage < format::TECHNIQUE_STAGE_COUNT; ++stage)
        {
            const bool isVertex = stage == 0;
            const bool isRead = binding.Stages == format::Visibility::Both ||
                                (isVertex == true && binding.Stages == format::Visibility::Vertex) ||
                                (isVertex == false && binding.Stages == format::Visibility::Fragment);
            const usize role = static_cast<usize>(binding.Program) * format::TECHNIQUE_STAGE_COUNT + stage;
            if (isRead == false || BindingCounts[role] == format::UNIFORM_SLOT_COUNT)
            {
                continue;
            }
            rhi::RasterBinding& entry = Bindings[role][BindingCounts[role]++];
            entry.Binding = binding.Slot;
            entry.Kind = rhi::RasterBindingKind::UniformBuffer;
            entry.Visibility = isVertex == true ? rhi::RasterVisibility::Vertex : rhi::RasterVisibility::Fragment;
            entry.MinSize = bytes;
        }
    }
}
RenderGraphPackageStatus RenderGraphPackage::State::DecodeRecords(ByteReader& inoutReader) noexcept
{
    for (uint32 i = 0; i < Header.ResourceCount; ++i)
    {
        format::Resource& resource = Resources[i];
        resource.Width = inoutReader.ReadWord();
        resource.Height = inoutReader.ReadWord();
        const uint32 aspect = inoutReader.ReadWord();
        resource.Backing = inoutReader.ReadWord();
        resource.IsSurface = inoutReader.ReadWord();
        resource.IsExported = inoutReader.ReadWord();
        if (aspect > MAX_ASPECT)
        {
            return RenderGraphPackageStatus::Malformed;
        }
        resource.Kind = static_cast<format::Aspect>(aspect);
    }
    for (uint32 i = 0; i < Header.CameraCount; ++i)
    {
        Cameras[i].Target = inoutReader.ReadWord();
    }
    for (uint32 i = 0; i < Header.PassCount; ++i)
    {
        format::Pass& pass = Passes[i];
        const uint32 program = inoutReader.ReadWord();
        pass.Camera = inoutReader.ReadWord();
        pass.Color = inoutReader.ReadWord();
        pass.Depth = inoutReader.ReadWord();
        const uint32 colorLoad = inoutReader.ReadWord();
        const uint32 colorStore = inoutReader.ReadWord();
        const uint32 depthLoad = inoutReader.ReadWord();
        const uint32 depthStore = inoutReader.ReadWord();
        pass.AllowsRepeatedCoverage = inoutReader.ReadWord();
        if (program > MAX_TECHNIQUE || colorLoad > MAX_LOAD || depthLoad > MAX_LOAD || colorStore > MAX_STORE ||
            depthStore > MAX_STORE)
        {
            return RenderGraphPackageStatus::Malformed;
        }
        pass.Program = static_cast<format::Technique>(program);
        pass.ColorLoad = static_cast<format::Load>(colorLoad);
        pass.ColorStore = static_cast<format::Store>(colorStore);
        pass.DepthLoad = static_cast<format::Load>(depthLoad);
        pass.DepthStore = static_cast<format::Store>(depthStore);
    }
    return inoutReader.IsOk() == true ? RenderGraphPackageStatus::Ready : RenderGraphPackageStatus::Malformed;
}

RenderGraphPackage::RenderGraphPackage() noexcept = default;
RenderGraphPackage::~RenderGraphPackage() noexcept = default;
RenderGraphPackageStatus RenderGraphPackage::Decode(BorrowedView<const uint8> bytes) noexcept
{
    if (mState.Get() != nullptr)
    {
        return RenderGraphPackageStatus::InvalidState;
    }
    if (bytes.IsValid() == false || bytes.GetSize() < format::HEADER_BYTES)
    {
        return RenderGraphPackageStatus::Malformed;
    }
    if (bytes.GetSize() > format::MAX_PACKAGE_BYTES)
    {
        return RenderGraphPackageStatus::CapacityExceeded;
    }
    State* allocationOrNull = new (std::nothrow) State;
    core::UniquePtr<State> candidate{allocationOrNull};
    if (candidate.Get() == nullptr || candidate->Bytes.TryResize(bytes.GetSize()) == false)
    {
        return RenderGraphPackageStatus::OutOfMemory;
    }
    // Copy first: every later view borrows owned storage, never the caller's buffer.
    const uint8* source = bytes.GetDataOrNull();
    for (usize i = 0; i < bytes.GetSize(); ++i)
    {
        candidate->Bytes[i] = source[i];
    }
    ByteReader reader{candidate->Bytes.AsSpan()};
    const uint32 magic = reader.ReadWord();
    const uint32 version = reader.ReadWord();
    if (magic != format::PACKAGE_MAGIC || version != format::PACKAGE_VERSION)
    {
        return RenderGraphPackageStatus::UnsupportedVersion;
    }
    candidate->Header.Version = format::VERSION;
    candidate->Header.ResourceCount = reader.ReadWord();
    candidate->Header.PassCount = reader.ReadWord();
    candidate->Header.CameraCount = reader.ReadWord();
    candidate->Header.UniformValueCount = reader.ReadWord();
    candidate->Header.UniformBindingCount = reader.ReadWord();
    candidate->Header.UniformBlockCount = reader.ReadWord();
    candidate->Header.UniformFieldCount = reader.ReadWord();
    const uint32 stageCount = reader.ReadWord();
    if (candidate->Header.ResourceCount > format::MAX_RESOURCES || candidate->Header.PassCount > format::MAX_PASSES ||
        candidate->Header.CameraCount > format::MAX_CAMERAS || stageCount > format::MAX_STAGES ||
        candidate->Header.UniformValueCount > format::MAX_UNIFORM_VALUES ||
        candidate->Header.UniformBindingCount > format::MAX_UNIFORM_BINDINGS ||
        candidate->Header.UniformBlockCount > format::MAX_UNIFORM_BLOCKS ||
        candidate->Header.UniformFieldCount > format::MAX_UNIFORM_FIELDS)
    {
        return RenderGraphPackageStatus::CapacityExceeded;
    }
    const std::span<const uint8> interfaceIdentity = reader.ReadIdentity();
    const std::span<const uint8> generationIdentity = reader.ReadIdentity();
    if (reader.IsOk() == false)
    {
        return RenderGraphPackageStatus::Malformed;
    }
    for (usize i = 0; i < format::IDENTITY_BYTES; ++i)
    {
        candidate->InterfaceIdentity[i] = interfaceIdentity[i];
        candidate->GenerationIdentity[i] = generationIdentity[i];
    }
    RenderGraphPackageStatus status = candidate->DecodeRecords(reader);
    if (status != RenderGraphPackageStatus::Ready)
    {
        return status;
    }
    status = candidate->DecodeUniforms(reader);
    if (status != RenderGraphPackageStatus::Ready)
    {
        return status;
    }
    status = candidate->DecodeStages(reader, stageCount);
    if (status != RenderGraphPackageStatus::Ready)
    {
        return status;
    }
    if (reader.IsComplete() == false)
    {
        return RenderGraphPackageStatus::Malformed;
    }
    for (uint32 i = 0; i < format::STATIC_INPUT_COUNT; ++i)
    {
        candidate->Inputs[i] = {i, i == 0 ? rhi::RasterVertexFormat::Float3 : rhi::RasterVertexFormat::Float4};
    }
    candidate->DeriveBindings();
    mState.Swap(candidate);
    return RenderGraphPackageStatus::Ready;
}
bool RenderGraphPackage::IsLoaded() const noexcept
{
    return mState.Get() != nullptr;
}
RenderGraphPlan RenderGraphPackage::GetPlan() const noexcept
{
    if (mState.Get() == nullptr)
    {
        return {};
    }
    const State& state = *mState;
    return {state.Header,
            {state.Resources.GetData(), state.Header.ResourceCount},
            {state.Cameras.GetData(), state.Header.CameraCount},
            {state.Passes.GetData(), state.Header.PassCount},
            {state.UniformBindings.GetData(), state.Header.UniformBindingCount},
            {state.UniformBlocks.GetData(), state.Header.UniformBlockCount},
            {state.UniformFields.GetData(), state.Header.UniformFieldCount}};
}
RenderGraphPrograms RenderGraphPackage::GetPrograms(const rhi::RasterShaderDescription& compositeVertex,
                                                    const rhi::RasterShaderDescription& compositeFragment,
                                                    bool shouldPrepareAsynchronously) const noexcept
{
    RenderGraphPrograms result;
    result.CompositeVertex = compositeVertex;
    result.CompositeFragment = compositeFragment;
    result.CompositeVertex.ShouldPrepareAsynchronously = shouldPrepareAsynchronously;
    result.CompositeFragment.ShouldPrepareAsynchronously = shouldPrepareAsynchronously;
    if (mState.Get() == nullptr)
    {
        return result;
    }
    const State& state = *mState;
    StaticArray<rhi::RasterShaderDescription*, format::STAGE_ROLE_COUNT> members{{&result.ColorVertex,
                                                                                  &result.ColorFragment,
                                                                                  &result.DepthVertex,
                                                                                  &result.DepthFragment,
                                                                                  &result.MaskedColorVertex,
                                                                                  &result.MaskedColorFragment,
                                                                                  &result.MaskedDepthVertex,
                                                                                  &result.MaskedDepthFragment,
                                                                                  &result.RayCompute,
                                                                                  &result.RayImageVertex,
                                                                                  &result.RayImageFragment,
                                                                                  &result.RayCompute}};
    for (usize role = 0; role < format::STAGE_ROLE_COUNT; ++role)
    {
        rhi::RasterShaderDescription& description = *members[role];
        description.ShouldPrepareAsynchronously = shouldPrepareAsynchronously;
        const StaticArray<State::StageView, format::SHADER_TARGET_COUNT>& views = state.Stages[role];
        bool hasArtifact = false;
        for (const State::StageView& view : views)
        {
            hasArtifact = hasArtifact == true || view.IsPresent == true;
        }
        // Like a compiled header, a role the generation does not carry stays an empty description.
        if (hasArtifact == false)
        {
            continue;
        }
        const bool isRayRole = role >= static_cast<uint32>(format::StageRole::RayCompute);
        const bool isVertex = isRayRole == true ? role == static_cast<uint32>(format::StageRole::RayImageVertex)
                                                : role % format::TECHNIQUE_STAGE_COUNT == 0;
        description.Artifact.Stage = isVertex == true ? rhi::ShaderStage::Vertex : rhi::ShaderStage::Fragment;
        if (isVertex == true)
        {
            if (isRayRole == false)
            {
                description.Inputs = state.Inputs.AsSpan();
            }
        }
        if (isRayRole == true)
        {
            const bool isSceneCompute = role == static_cast<usize>(format::StageRole::SceneRayCompute);
            result.HasSceneShaderInterface = result.HasSceneShaderInterface == true || isSceneCompute == true;
            description.Artifact.Stage =
                (role == static_cast<usize>(format::StageRole::RayCompute) || isSceneCompute == true)
                    ? rhi::ShaderStage::Compute
                : isVertex == true ? rhi::ShaderStage::Vertex
                                   : rhi::ShaderStage::Fragment;
            SetRenderGraphRayInterface(description, description.Artifact.Stage);
        }
        if (state.BindingCounts[role] != 0)
        {
            description.Bindings = {state.Bindings[role].GetData(), state.BindingCounts[role]};
        }
        const State::StageView& metal = views[static_cast<usize>(format::ShaderTarget::Metal)];
        const State::StageView& spirv = views[static_cast<usize>(format::ShaderTarget::Spirv)];
        const State::StageView& wgsl = views[static_cast<usize>(format::ShaderTarget::Wgsl)];
        const State::StageView& glsl = views[static_cast<usize>(format::ShaderTarget::GlslEs)];
        if (metal.IsPresent == true)
        {
            description.Artifact.Msl = metal.Text;
            description.Artifact.MslEntry = metal.Entry;
        }
        if (spirv.IsPresent == true)
        {
            description.Artifact.Spirv = spirv.Words.AsSpan();
            description.Artifact.SpirvEntry = spirv.Entry;
        }
        if (wgsl.IsPresent == true)
        {
            description.Artifact.Wgsl = wgsl.Text;
            description.Artifact.WgslEntry = wgsl.Entry;
        }
        if (glsl.IsPresent == true)
        {
            description.Artifact.GlslEs = glsl.Text;
            description.Artifact.GlslEsEntry = glsl.Entry;
            for (usize slot = 0; slot < format::UNIFORM_SLOT_COUNT; ++slot)
            {
                description.UniformBlocks[slot] = glsl.Blocks[slot];
            }
        }
    }
    return result;
}
BorrowedView<const uint8> RenderGraphPackage::GetInterfaceIdentity() const noexcept
{
    return mState.Get() == nullptr
               ? BorrowedView<const uint8>{}
               : BorrowedView<const uint8>{mState->InterfaceIdentity.GetData(), format::IDENTITY_BYTES};
}
BorrowedView<const uint8> RenderGraphPackage::GetGenerationIdentity() const noexcept
{
    return mState.Get() == nullptr
               ? BorrowedView<const uint8>{}
               : BorrowedView<const uint8>{mState->GenerationIdentity.GetData(), format::IDENTITY_BYTES};
}
void RenderGraphPackage::Reset() noexcept
{
    mState.Reset();
}

namespace
{
/// One staged or active generation. Graph is declared after Package, so it is
/// destroyed first and never outlives the artifacts it was initialized from.
struct Generation final
{
    core::UniquePtr<RenderGraphPackage> Package;
    RenderGraph Graph;
    uint64 Serial = 0;
    uint64 SceneRevision = 0;
    bool IsSetupReady = false;
};
bool AreIdentitiesEqual(BorrowedView<const uint8> left,
                        const StaticArray<uint8, format::IDENTITY_BYTES>& right) noexcept
{
    bool isEqual = left.GetSize() == format::IDENTITY_BYTES;
    for (usize i = 0; isEqual == true && i < format::IDENTITY_BYTES; ++i)
    {
        isEqual = left[i] == right[i];
    }
    return isEqual;
}
} // namespace

struct RenderGraphActivation::State final
{
    rhi::DeviceHandle Device{};
    StaticArray<uint8, format::IDENTITY_BYTES> Interface{};
    rhi::RasterShaderDescription CompositeVertex{};
    rhi::RasterShaderDescription CompositeFragment{};
    /// Shared by every generation under ExactPool. Declared before the generations,
    /// so their graphs return leased attachments before the pool is destroyed.
    RenderGraphAttachmentPool Pool;
    bool IsPooled = false;
    core::UniquePtr<Generation> Active;
    core::UniquePtr<Generation> Candidate;
    uint64 NextSerial = 0;
    RenderGraphActivationReport Report{};
    void FailCandidate(Status status) noexcept
    {
        Candidate.Reset();
        Report.Candidate = RenderGraphCandidateState::Failed;
        Report.LastFailure = status;
    }
    /// Publish the complete candidate; the replaced graph retires through RHI completion.
    void Activate() noexcept
    {
        core::UniquePtr<Generation> retired;
        retired.Swap(Active);
        Active.Swap(Candidate);
        Report.ActiveGeneration = Active->Serial;
        Report.Candidate = RenderGraphCandidateState::None;
        retired.Reset();
    }
};
RenderGraphActivation::RenderGraphActivation() noexcept = default;
RenderGraphActivation::~RenderGraphActivation() noexcept
{
    Reset();
}
Status RenderGraphActivation::Initialize(rhi::DeviceHandle device,
                                         BorrowedView<const uint8> interfaceIdentity,
                                         const rhi::RasterShaderDescription& compositeVertex,
                                         const rhi::RasterShaderDescription& compositeFragment,
                                         RenderGraphAllocation allocation) noexcept
{
    if (mState.Get() != nullptr)
    {
        return Status::InvalidState;
    }
    if (interfaceIdentity.IsValid() == false || interfaceIdentity.GetSize() != format::IDENTITY_BYTES ||
        (allocation != RenderGraphAllocation::ExactPool && allocation != RenderGraphAllocation::SerialControl))
    {
        return Status::InvalidDescription;
    }
    State* allocationOrNull = new (std::nothrow) State;
    core::UniquePtr<State> candidate{allocationOrNull};
    if (candidate.Get() == nullptr)
    {
        return Status::OutOfMemory;
    }
    candidate->Device = device;
    for (usize i = 0; i < format::IDENTITY_BYTES; ++i)
    {
        candidate->Interface[i] = interfaceIdentity[i];
    }
    candidate->CompositeVertex = compositeVertex;
    candidate->CompositeFragment = compositeFragment;
    candidate->IsPooled = allocation == RenderGraphAllocation::ExactPool;
    if (candidate->IsPooled == true)
    {
        const Status bound = candidate->Pool.Initialize(device);
        if (bound != Status::Ready)
        {
            return bound;
        }
    }
    mState.Swap(candidate);
    return Status::Ready;
}
Status RenderGraphActivation::Stage(core::UniquePtr<RenderGraphPackage> package) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    State& state = *mState;
    if (package.Get() == nullptr || package->IsLoaded() == false)
    {
        return Status::InvalidDescription;
    }
    // A newer request supersedes any unactivated candidate, whatever its outcome.
    state.Candidate.Reset();
    state.Report.CandidateGeneration = ++state.NextSerial;
    if (AreIdentitiesEqual(package->GetInterfaceIdentity(), state.Interface) == false)
    {
        state.Report.Candidate = RenderGraphCandidateState::RestartRequired;
        state.Report.LastFailure = Status::Unsupported;
        return Status::Unsupported;
    }
    Generation* allocationOrNull = new (std::nothrow) Generation;
    core::UniquePtr<Generation> generation{allocationOrNull};
    if (generation.Get() == nullptr)
    {
        state.FailCandidate(Status::OutOfMemory);
        return Status::OutOfMemory;
    }
    generation->Package.Swap(package);
    generation->Serial = state.Report.CandidateGeneration;
    const Status status = generation->Graph.Initialize(
        state.Device,
        generation->Package->GetPlan(),
        generation->Package->GetPrograms(state.CompositeVertex, state.CompositeFragment, true),
        state.IsPooled == true ? &state.Pool : nullptr);
    if (IsAccepted(status) == false)
    {
        state.FailCandidate(status);
        return status;
    }
    state.Candidate.Swap(generation);
    state.Report.Candidate = RenderGraphCandidateState::Preparing;
    return status;
}
Status RenderGraphActivation::Poll() noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    State& state = *mState;
    Status activeStatus = Status::NotReady;
    if (state.Active.Get() != nullptr)
    {
        activeStatus = state.Active->Graph.Poll();
    }
    if (state.Candidate.Get() == nullptr)
    {
        return activeStatus;
    }
    if (state.Active.Get() != nullptr && state.Active->Graph.HasUnconfirmedEncoding() == true)
    {
        return activeStatus;
    }
    Generation& candidate = *state.Candidate;
    const Status candidateStatus = candidate.Graph.Poll();
    if (IsAccepted(candidateStatus) == false)
    {
        state.FailCandidate(candidateStatus);
        return state.Active.Get() != nullptr ? activeStatus : candidateStatus;
    }
    if (candidateStatus == Status::Pending)
    {
        state.Report.Candidate = RenderGraphCandidateState::Preparing;
        return state.Active.Get() != nullptr ? activeStatus : candidateStatus;
    }
    candidate.IsSetupReady = true;
    const uint64 latest = state.Report.LatestSceneRevision;
    if (latest != 0 && candidate.Graph.GetDrawableSceneRevision() != latest)
    {
        state.Report.Candidate = candidate.SceneRevision == latest ? RenderGraphCandidateState::Preparing
                                                                   : RenderGraphCandidateState::NeedsUpdate;
        return state.Active.Get() != nullptr ? activeStatus : Status::Pending;
    }
    state.Activate();
    return Status::Ready;
}
Status RenderGraphActivation::Update(BorrowedView<const RenderGraphMesh> meshes,
                                     BorrowedView<const RenderGraphItem> items,
                                     BorrowedView<const ViewDescription> cameras,
                                     uint64 sceneRevision,
                                     RenderGraphCoveragePolicy policy,
                                     RenderGraphReport& outReport,
                                     const SceneShaderInputs* sceneInputsOrNull) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    State& state = *mState;
    Status activeStatus = Status::NotReady;
    bool isActiveAccepting = false;
    if (state.Active.Get() != nullptr)
    {
        activeStatus =
            state.Active->Graph.Update(meshes, items, cameras, sceneRevision, policy, outReport, sceneInputsOrNull);
        isActiveAccepting = IsAccepted(activeStatus);
        if (isActiveAccepting == true)
        {
            state.Report.LatestSceneRevision = sceneRevision;
        }
    }
    if (state.Candidate.Get() == nullptr || state.Candidate->IsSetupReady == false)
    {
        return activeStatus;
    }
    Generation& candidate = *state.Candidate;
    RenderGraphReport candidateReport;
    const Status candidateStatus =
        candidate.Graph.Update(meshes, items, cameras, sceneRevision, policy, candidateReport, sceneInputsOrNull);
    if (IsAccepted(candidateStatus) == true)
    {
        candidate.SceneRevision = sceneRevision;
        state.Report.Candidate = RenderGraphCandidateState::Preparing;
    }
    else if (isActiveAccepting == true)
    {
        // The new generation cannot cover what the drawing generation accepts.
        state.FailCandidate(candidateStatus);
    }
    if (state.Active.Get() != nullptr)
    {
        return activeStatus;
    }
    outReport = candidateReport;
    if (IsAccepted(candidateStatus) == true)
    {
        state.Report.LatestSceneRevision = sceneRevision;
    }
    return candidateStatus;
}
RenderGraph* RenderGraphActivation::GetActiveOrNull() noexcept
{
    return mState.Get() != nullptr && mState->Active.Get() != nullptr ? &mState->Active->Graph : nullptr;
}
RenderGraphActivationReport RenderGraphActivation::GetReport() const noexcept
{
    return mState.Get() != nullptr ? mState->Report : RenderGraphActivationReport{};
}
RenderGraphPoolReport RenderGraphActivation::GetPoolReport() const noexcept
{
    return mState.Get() != nullptr ? mState->Pool.GetReport() : RenderGraphPoolReport{};
}
void RenderGraphActivation::TrimPool() noexcept
{
    if (mState.Get() != nullptr)
    {
        mState->Pool.Trim();
    }
}
void RenderGraphActivation::Reset() noexcept
{
    if (mState.Get() != nullptr)
    {
        mState->Candidate.Reset();
        mState->Active.Reset();
        mState->Pool.Reset();
    }
    mState.Reset();
}
} // namespace ludus::graphics::renderer
