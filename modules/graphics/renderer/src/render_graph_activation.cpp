#include <ludus/graphics/renderer/render_graph_activation.hpp>

#include <ludus/foundation/base/byte_order.hpp>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/ray.h>

#include "../shaders/renderer_gpu_preview_layout.h"
#include "internal/gpu_denoiser.h"

#include <cstring>
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
constexpr uint32 MAX_ASPECT = static_cast<uint32>(format::Aspect::Buffer);
constexpr uint32 MAX_TECHNIQUE = static_cast<uint32>(format::TECHNIQUE_COUNT - 1);
constexpr uint32 MAX_VISIBILITY = static_cast<uint32>(format::Visibility::Both);
constexpr uint32 MAX_TARGET = static_cast<uint32>(format::ShaderTarget::GlslEs);
constexpr uint32 MAX_LOAD = static_cast<uint32>(format::Load::Discard);
constexpr uint32 MAX_STORE = static_cast<uint32>(format::Store::Discard);
static_assert(static_cast<uint32>(format::RayTraversal::NativeQuery) == LUDUS_GRAPH_TRAVERSAL_NATIVE_QUERY &&
                  static_cast<uint32>(format::RayTraversal::TriangleLoop) == LUDUS_GRAPH_TRAVERSAL_TRIANGLE_LOOP &&
                  static_cast<uint32>(format::RayTraversal::Custom) == LUDUS_GRAPH_TRAVERSAL_CUSTOM &&
                  static_cast<uint32>(format::RayTraversal::HardwarePipeline) ==
                      LUDUS_GRAPH_TRAVERSAL_HARDWARE_PIPELINE &&
                  static_cast<uint32>(format::RayTraversal::ComputeBvh) == LUDUS_GRAPH_TRAVERSAL_COMPUTE_BVH,
              "cooked pass traversal codes select the linked kernel's strategy");
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
        uint32 InputCount = 0;
        StaticArray<uint32, format::RAY_WORKGROUP_DIMENSIONS> WorkgroupSize{};
        std::string_view Entry;
        std::string_view Text;
        Array<uint32> Words;
        /// GLSL ES uniform block names by binding number; empty for other targets.
        StaticArray<std::string_view, format::UNIFORM_SLOT_COUNT> Blocks{};
        StaticArray<std::string_view, rhi::RASTER_BINDING_COUNT> TextureNames{};
        StaticArray<uint32, rhi::RASTER_BINDING_COUNT> TextureSamplers{};
    };
    Array<uint8> Bytes;
    format::Header Header{};
    StaticArray<format::Resource, format::MAX_RESOURCES> Resources{};
    StaticArray<format::Camera, format::MAX_CAMERAS> Cameras{};
    StaticArray<format::Pass, format::MAX_PASSES> Passes{};
    StaticArray<BorrowedView<const char>, format::MAX_PASSES> PassNames{};
    bool HasPassNames = false;
    /// Decoded by DecodeRayPathCapture: the ray kernel is the diagnostic capture variant.
    bool IsRayPathCapture = false;
    StaticArray<format::StorageBuffer, format::MAX_STORAGE_BUFFERS> StorageBuffers{};
    usize StorageBufferCount = 0;
    bool HasRelativeSizes = false;
    bool HasPixelBuffers = false;
    bool HasSceneData = false;
    bool HasNativeRay = false;
    bool HasExplicitRayWorkgroup = false;
    bool HasExplicitRayTraversal = false;
    bool HasCustomRayTraversal = false;
    bool HasNativeRayPipeline = false;
    bool HasComputeBvhTraversal = false;
    bool HasNativeRayQueries = false;
    bool HasIndependentPrograms = false;
    bool HasVisibilityFields = false;
    bool HasPersistentBuffers = false;
    bool HasForwardStages = false;
    bool CanHaveClusteredForward = false;
    bool HasExplicitAccumulation = false;
    bool HasExplicitToneMapping = false;
    bool HasBilinearUpscale = false;
    bool HasMetalFxSpatialUpscale = false;
    bool HasDenoising = false;
    // DecodeUniforms validates and writes every active stage before any binding is published.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
    StaticArray<format::UniformBinding, format::MAX_UNIFORM_BINDINGS> UniformBindings{};
    StaticArray<format::UniformBlock, format::MAX_UNIFORM_BLOCKS> UniformBlocks{};
    StaticArray<format::UniformField, format::MAX_UNIFORM_FIELDS> UniformFields{};
    /// Stage binding requirements derived from the uniform records, one table per role.
    static constexpr usize STAGE_BINDING_CAPACITY = LUDUS_GRAPH_VISIBILITY_BINDING_COUNT + format::MAX_STORAGE_BUFFERS;
    // RasterBinding initializes Visibility to Both, including every nested array element.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
    StaticArray<StaticArray<rhi::RasterBinding, STAGE_BINDING_CAPACITY>, format::STAGE_ROLE_COUNT> Bindings{};
    StaticArray<usize, format::STAGE_ROLE_COUNT> BindingCounts{};
    StaticArray<uint8, format::IDENTITY_BYTES> InterfaceIdentity{};
    StaticArray<uint8, format::IDENTITY_BYTES> GenerationIdentity{};
    StaticArray<rhi::RasterShaderInput, format::VISIBILITY_INPUT_COUNT> Inputs{};
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
        if (HasNativeRay == false && role >= static_cast<uint32>(format::StageRole::RayCompute))
        {
            return RenderGraphPackageStatus::UnsupportedVersion;
        }
        const uint32 target = inoutReader.ReadWord();
        const uint32 inputCount = inoutReader.ReadWord();
        const uint32 entryBytes = inoutReader.ReadWord();
        const uint32 artifactBytes = inoutReader.ReadWord();
        const uint32 blockCount = inoutReader.ReadWord();
        if (inoutReader.IsOk() == false || role >= format::STAGE_ROLE_COUNT ||
            (HasIndependentPrograms == false && role >= format::LEGACY_STAGE_ROLE_COUNT) ||
            (role == static_cast<uint32>(format::StageRole::PersistentRayCompute) && HasPersistentBuffers == false) ||
            target >= format::SHADER_TARGET_COUNT || blockCount > format::UNIFORM_SLOT_COUNT ||
            (blockCount != 0 && target != static_cast<uint32>(format::ShaderTarget::GlslEs)))
        {
            return RenderGraphPackageStatus::Malformed;
        }
        const bool isNativeRayRole = role == format::NATIVE_RAY_STAGE_ROLE;
        if (isNativeRayRole == true &&
            (HasNativeRayPipeline == false || target != static_cast<uint32>(format::ShaderTarget::Metal)))
        {
            return RenderGraphPackageStatus::Malformed;
        }
        const bool isClusteredFragment = role == format::CLUSTERED_FORWARD_FRAGMENT_ROLE;
        const bool isForwardDepth = role == format::FORWARD_DEPTH_STAGE_ROLE;
        if ((isClusteredFragment == true || isForwardDepth == true) && CanHaveClusteredForward == false)
        {
            return RenderGraphPackageStatus::UnsupportedVersion;
        }
        const bool isForwardRole = (role >= format::FORWARD_STAGE_ROLE_FIRST && role < format::NATIVE_RAY_STAGE_ROLE) ||
                                   isClusteredFragment == true || isForwardDepth == true;
        if (isForwardRole == true && HasForwardStages == false)
        {
            return RenderGraphPackageStatus::UnsupportedVersion;
        }
        const bool isRayRole =
            (role >= static_cast<uint32>(format::StageRole::RayCompute) && role < format::LEGACY_STAGE_ROLE_COUNT) ||
            role == static_cast<uint32>(format::StageRole::PersistentRayCompute) ||
            role == format::NATIVE_RAY_STAGE_ROLE;
        const bool isVertex = isForwardRole == true ? (role == static_cast<uint32>(format::StageRole::ForwardVertex) ||
                                                       role == static_cast<uint32>(format::StageRole::LightingVertex))
                              : isRayRole == true   ? role == static_cast<uint32>(format::StageRole::RayImageVertex)
                                                    : role % format::TECHNIQUE_STAGE_COUNT == 0;
        if (HasVisibilityFields == false && isVertex == true && isRayRole == true && inputCount == 2)
        {
            return RenderGraphPackageStatus::UnsupportedVersion;
        }
        if ((isVertex == false && inputCount != 0) || (isVertex == true && isRayRole == true && inputCount != 1) ||
            (isForwardRole == true &&
             inputCount != (role == static_cast<uint32>(format::StageRole::ForwardVertex) ? format::FORWARD_INPUT_COUNT
                            : role == static_cast<uint32>(format::StageRole::LightingVertex) ? 1
                                                                                             : 0)) ||
            (isVertex == true && isRayRole == false && isForwardRole == false &&
             inputCount != format::STATIC_INPUT_COUNT &&
             (HasVisibilityFields == false || inputCount != format::VISIBILITY_INPUT_COUNT)))
        {
            return RenderGraphPackageStatus::Malformed;
        }
        const bool isCompute = role == static_cast<uint32>(format::StageRole::RayCompute) ||
                               role == static_cast<uint32>(format::StageRole::SceneRayCompute) ||
                               role == static_cast<uint32>(format::StageRole::PersistentRayCompute);
        StaticArray<uint32, format::RAY_WORKGROUP_DIMENSIONS> workgroup{};
        if (HasExplicitRayWorkgroup == true && isCompute == true)
        {
            for (uint32& dimension : workgroup)
            {
                dimension = inoutReader.ReadWord();
            }
            if (inoutReader.IsOk() == false || workgroup[0] == 0 || workgroup[1] == 0 || workgroup[2] != 1)
            {
                return RenderGraphPackageStatus::Malformed;
            }
        }
        for (uint32 input = 0; input < inputCount; ++input)
        {
            const uint32 location = inoutReader.ReadWord();
            const uint32 inputFormat = inoutReader.ReadWord();
            const format::InputFormat expected =
                isForwardRole == true
                    ? (role == static_cast<uint32>(format::StageRole::LightingVertex) ? format::InputFormat::Float4
                       : (input == 0 || input == 7)                                   ? format::InputFormat::Float3
                       : (input == 6 || input == 13 || input == 15)                   ? format::InputFormat::Float2
                                                                                      : format::InputFormat::Float4)
                : input == 0 ? (isRayRole == true ? format::InputFormat::Float2 : format::InputFormat::Float3)
                : input >= format::STATIC_INPUT_COUNT ? format::InputFormat::Float2
                                                      : format::InputFormat::Float4;
            if (location != input || inputFormat != static_cast<uint32>(expected))
            {
                return RenderGraphPackageStatus::Malformed;
            }
        }
        for (const StageView& other : Stages[role])
        {
            if (other.IsPresent == true && (other.InputCount != inputCount || other.WorkgroupSize != workgroup))
            {
                return RenderGraphPackageStatus::Malformed;
            }
        }
        StageView& view = Stages[role][target];
        view.InputCount = inputCount;
        view.WorkgroupSize = workgroup;
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
        if (isForwardRole == true)
        {
            const uint32 textureCount = inoutReader.ReadWord();
            if (textureCount > rhi::RASTER_BINDING_COUNT ||
                (textureCount != 0 && target != static_cast<uint32>(format::ShaderTarget::GlslEs)))
            {
                return RenderGraphPackageStatus::Malformed;
            }
            for (uint32 texture = 0; texture < textureCount; ++texture)
            {
                const uint32 slot = inoutReader.ReadWord();
                const uint32 sampler = inoutReader.ReadWord();
                const uint32 count = inoutReader.ReadWord();
                if (slot >= rhi::RASTER_BINDING_COUNT ||
                    (sampler >= rhi::RASTER_BINDING_COUNT && sampler != format::ABSENT) ||
                    count > format::MAX_ENTRY_BYTES || view.TextureNames[slot].empty() == false)
                {
                    return RenderGraphPackageStatus::Malformed;
                }
                const std::span<const uint8> name = inoutReader.ReadPadded(count);
                if (inoutReader.IsOk() == false || IsBlockName(name) == false)
                {
                    return RenderGraphPackageStatus::Malformed;
                }
                view.TextureNames[slot] = {reinterpret_cast<const char*>(name.data()), name.size()};
                view.TextureSamplers[slot] = sampler;
            }
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
    for (usize technique = 0; technique < format::TECHNIQUE_COUNT; ++technique)
    {
        if (technique == static_cast<usize>(format::Technique::RayTracing) ||
            format::IsImageTechnique(static_cast<format::Technique>(technique)) == true)
        {
            continue;
        }
        const usize vertexRole = format::GetRasterStageRole(static_cast<format::Technique>(technique), 0);
        for (usize target = 0; target < format::SHADER_TARGET_COUNT; ++target)
        {
            if (Stages[vertexRole][target].IsPresent != Stages[vertexRole + 1][target].IsPresent)
            {
                return RenderGraphPackageStatus::Malformed;
            }
        }
    }
    for (usize target = 0; target < format::SHADER_TARGET_COUNT; ++target)
    {
        const bool hasForward = Stages[format::FORWARD_STAGE_ROLE_FIRST][target].IsPresent;
        if (Stages[format::FORWARD_DEPTH_STAGE_ROLE][target].IsPresent !=
            Stages[format::CLUSTERED_FORWARD_FRAGMENT_ROLE][target].IsPresent)
        {
            return RenderGraphPackageStatus::Malformed;
        }
        for (usize role = format::FORWARD_STAGE_ROLE_FIRST; role < format::NATIVE_RAY_STAGE_ROLE; ++role)
        {
            const bool isFragment = role == static_cast<usize>(format::StageRole::ForwardFragment);
            const bool hasClusteredFragment = Stages[format::CLUSTERED_FORWARD_FRAGMENT_ROLE][target].IsPresent;
            if ((isFragment == true && hasClusteredFragment == true && Stages[role][target].IsPresent == true) ||
                (isFragment == true ? (Stages[role][target].IsPresent == true || hasClusteredFragment == true)
                                    : Stages[role][target].IsPresent) != hasForward)
            {
                return RenderGraphPackageStatus::Malformed;
            }
        }
    }
    bool hasClusteredForward = false;
    bool hasLegacyForward = false;
    for (usize target = 0; target < format::SHADER_TARGET_COUNT; ++target)
    {
        hasClusteredForward =
            hasClusteredForward == true || Stages[format::CLUSTERED_FORWARD_FRAGMENT_ROLE][target].IsPresent == true;
        hasLegacyForward = hasLegacyForward == true ||
                           Stages[static_cast<usize>(format::StageRole::ForwardFragment)][target].IsPresent == true;
    }
    if (hasClusteredForward == true && hasLegacyForward == true)
    {
        return RenderGraphPackageStatus::Malformed;
    }
    bool hasNativeRayInvocation = false;
    for (usize pass = 0; pass < Header.PassCount; ++pass)
    {
        hasNativeRayInvocation =
            hasNativeRayInvocation == true || Passes[pass].Traversal == format::RayTraversal::HardwarePipeline;
    }
    if (Stages[format::NATIVE_RAY_STAGE_ROLE][static_cast<usize>(format::ShaderTarget::Metal)].IsPresent !=
        hasNativeRayInvocation)
    {
        return RenderGraphPackageStatus::Malformed;
    }
    bool hasLegacyProfile = false;
    bool hasSceneProfile = false;
    for (usize target = 0; target < format::SHADER_TARGET_COUNT; ++target)
    {
        const bool hasLegacyCompute = Stages[static_cast<usize>(format::StageRole::RayCompute)][target].IsPresent;
        const bool hasPersistentCompute =
            Stages[static_cast<usize>(format::StageRole::PersistentRayCompute)][target].IsPresent;
        const bool hasSceneCompute = Stages[static_cast<usize>(format::StageRole::SceneRayCompute)][target].IsPresent ||
                                     hasPersistentCompute == true;
        if (hasPersistentCompute == true &&
            Stages[static_cast<usize>(format::StageRole::SceneRayCompute)][target].IsPresent == true)
        {
            return RenderGraphPackageStatus::Malformed;
        }
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
        if (program > MAX_TECHNIQUE || (HasIndependentPrograms == false && program >= format::LEGACY_TECHNIQUE_COUNT) ||
            program == static_cast<uint32>(format::Technique::RayTracing) ||
            format::IsImageTechnique(static_cast<format::Technique>(program)) == true || stages == 0 ||
            stages > MAX_VISIBILITY || binding.Slot >= format::UNIFORM_SLOT_COUNT)
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
            const usize role = format::GetRasterStageRole(binding.Program, stage);
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
        if (HasRelativeSizes == true)
        {
            resource.ViewNumerator = inoutReader.ReadWord();
            resource.ViewDenominator = inoutReader.ReadWord();
            if (resource.ViewNumerator > format::MAX_TARGET_EXTENT || resource.ViewDenominator == 0 ||
                resource.ViewDenominator > format::MAX_TARGET_EXTENT)
            {
                return RenderGraphPackageStatus::Malformed;
            }
        }
        if (HasPixelBuffers == true)
        {
            resource.BytesPerPixel = inoutReader.ReadWord();
        }
        if (aspect > MAX_ASPECT || (aspect == MAX_ASPECT && HasPersistentBuffers == false))
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
        pass.SceneData =
            HasSceneData == true ? inoutReader.ReadWord() : ::ludus::content::render_graph_format::SCENE_DATA_ALL;
        if (HasPersistentBuffers == true)
        {
            pass.RayProgram = inoutReader.ReadWord();
            pass.Buffer0 = inoutReader.ReadWord();
            pass.Buffer1 = inoutReader.ReadWord();
        }
        pass.Visibility = HasVisibilityFields == true ? inoutReader.ReadWord() : format::ABSENT;
        pass.VisibilityDepth = HasVisibilityFields == true ? inoutReader.ReadWord() : format::ABSENT;
        pass.AccumulationSamples =
            HasExplicitAccumulation == true
                ? inoutReader.ReadWord()
                : (program == static_cast<uint32>(format::Technique::RayTracing) ? format::MAX_ACCUMULATION_SAMPLES
                                                                                 : 0);
        if (HasExplicitToneMapping == true)
        {
            const uint32 exposureBits = inoutReader.ReadWord();
            std::memcpy(&pass.ToneMappingExposure, &exposureBits, sizeof(exposureBits));
        }
        pass.SourceColor = HasBilinearUpscale == true ? inoutReader.ReadWord() : format::ABSENT;
        pass.DenoisingQuality = HasDenoising == true ? inoutReader.ReadWord() : 0;
        const uint32 traversal = HasExplicitRayTraversal == true ? inoutReader.ReadWord() : 0;
        const uint32 maximumTraversal =
            static_cast<uint32>(HasComputeBvhTraversal == true  ? format::RayTraversal::ComputeBvh
                                : HasNativeRayPipeline == true  ? format::RayTraversal::HardwarePipeline
                                : HasCustomRayTraversal == true ? format::RayTraversal::Custom
                                                                : format::RayTraversal::TriangleLoop);
        if (traversal > maximumTraversal ||
            (program != static_cast<uint32>(format::Technique::RayTracing) && traversal != 0))
        {
            return RenderGraphPackageStatus::Malformed;
        }
        pass.Traversal = static_cast<format::RayTraversal>(traversal);
        HasNativeRayQueries =
            HasNativeRayQueries == true || (program == static_cast<uint32>(format::Technique::RayTracing) &&
                                            (pass.Traversal == format::RayTraversal::NativeQuery ||
                                             pass.Traversal == format::RayTraversal::HardwarePipeline));
        const bool isUpscale = format::IsImageTechnique(static_cast<format::Technique>(program)) == true;
        if (pass.DenoisingQuality > static_cast<uint32>(format::ProcessingQuality::High) ||
            (isUpscale == true && (HasBilinearUpscale == false || pass.SceneData != 0)) ||
            (program > static_cast<uint32>(format::Technique::BilinearUpscale) && HasMetalFxSpatialUpscale == false) ||
            (isUpscale == false && pass.SourceColor != format::ABSENT) || program > MAX_TECHNIQUE ||
            (HasIndependentPrograms == false && program >= format::LEGACY_TECHNIQUE_COUNT) || colorLoad > MAX_LOAD ||
            depthLoad > MAX_LOAD || colorStore > MAX_STORE || depthStore > MAX_STORE ||
            pass.AccumulationSamples > format::MAX_ACCUMULATION_SAMPLES ||
            (pass.ToneMappingExposure != 0 &&
             (pass.ToneMappingExposure >= format::MIN_TONE_MAPPING_EXPOSURE &&
              pass.ToneMappingExposure <= format::MAX_TONE_MAPPING_EXPOSURE) == false) ||
            (program != static_cast<uint32>(format::Technique::RayTracing) &&
             (pass.AccumulationSamples != 0 || pass.ToneMappingExposure != 0 || pass.DenoisingQuality != 0)) ||
            (pass.SceneData & ~::ludus::content::render_graph_format::SCENE_DATA_ALL) != 0 ||
            (isUpscale == false && (pass.SceneData & ::ludus::content::render_graph_format::SCENE_DATA_GEOMETRY) == 0))
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
    return decode(bytes, format::PACKAGE_MAGIC);
}
RenderGraphPackageStatus RenderGraphPackage::DecodeRayPathCapture(BorrowedView<const uint8> bytes) noexcept
{
    return decode(bytes, format::RAY_PATH_CAPTURE_PACKAGE_MAGIC);
}
RenderGraphPackageStatus RenderGraphPackage::decode(BorrowedView<const uint8> bytes, uint32 expectedMagic) noexcept
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
    if (magic != expectedMagic ||
        (version != format::CLUSTERED_FORWARD_PACKAGE_VERSION && version != format::COMPUTE_BVH_PACKAGE_VERSION &&
         version != format::RAY_WORKGROUP_PACKAGE_VERSION && version != format::NATIVE_RAY_PIPELINE_PACKAGE_VERSION &&
         version != format::CUSTOM_RAY_PACKAGE_VERSION && version != format::SOFTWARE_RAY_PACKAGE_VERSION &&
         version != format::METALFX_SPATIAL_PACKAGE_VERSION && version != format::DENOISING_PACKAGE_VERSION &&
         version != format::BILINEAR_PACKAGE_VERSION && version != format::TONE_MAPPING_PACKAGE_VERSION &&
         version != format::ACCUMULATION_PACKAGE_VERSION && version != format::FORWARD_PACKAGE_VERSION &&
         version != format::PIXEL_BUFFER_PACKAGE_VERSION && version != format::VISIBILITY_PACKAGE_VERSION &&
         version != format::ORDERED_PACKAGE_VERSION && version != format::STORAGE_PACKAGE_VERSION &&
         version != format::INDEPENDENT_PROGRAM_PACKAGE_VERSION && version != format::NATIVE_RAY_PACKAGE_VERSION &&
         version != format::SCENE_PACKAGE_VERSION && version != format::VIEW_PACKAGE_VERSION &&
         version != format::NAMED_PACKAGE_VERSION && version != format::LEGACY_PACKAGE_VERSION))
    {
        return RenderGraphPackageStatus::UnsupportedVersion;
    }
    candidate->IsRayPathCapture = expectedMagic == format::RAY_PATH_CAPTURE_PACKAGE_MAGIC;
    candidate->HasRelativeSizes = version >= format::VIEW_PACKAGE_VERSION;
    candidate->HasSceneData = version >= format::SCENE_PACKAGE_VERSION;
    candidate->HasPersistentBuffers = version >= format::ORDERED_PACKAGE_VERSION;
    candidate->HasPixelBuffers = version >= format::PIXEL_BUFFER_PACKAGE_VERSION;
    candidate->HasForwardStages = version >= format::FORWARD_PACKAGE_VERSION;
    candidate->CanHaveClusteredForward = version >= format::CLUSTERED_FORWARD_PACKAGE_VERSION;
    candidate->HasExplicitAccumulation = version >= format::ACCUMULATION_PACKAGE_VERSION;
    candidate->HasExplicitToneMapping = version >= format::TONE_MAPPING_PACKAGE_VERSION;
    candidate->HasBilinearUpscale = version >= format::BILINEAR_PACKAGE_VERSION;
    candidate->HasMetalFxSpatialUpscale = version >= format::METALFX_SPATIAL_PACKAGE_VERSION;
    candidate->HasDenoising = version >= format::DENOISING_PACKAGE_VERSION;
    candidate->HasExplicitRayWorkgroup = version >= format::RAY_WORKGROUP_PACKAGE_VERSION;
    candidate->HasExplicitRayTraversal = version >= format::SOFTWARE_RAY_PACKAGE_VERSION;
    candidate->HasCustomRayTraversal = version >= format::CUSTOM_RAY_PACKAGE_VERSION;
    candidate->HasNativeRayPipeline = version >= format::NATIVE_RAY_PIPELINE_PACKAGE_VERSION;
    candidate->HasComputeBvhTraversal = version >= format::COMPUTE_BVH_PACKAGE_VERSION;
    candidate->HasNativeRay = version >= format::NATIVE_RAY_PACKAGE_VERSION;
    candidate->HasIndependentPrograms = version >= format::INDEPENDENT_PROGRAM_PACKAGE_VERSION;
    candidate->HasVisibilityFields = version >= format::VISIBILITY_PACKAGE_VERSION;
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
    if (version >= format::STORAGE_PACKAGE_VERSION)
    {
        candidate->StorageBufferCount = reader.ReadWord();
        if (candidate->StorageBufferCount > format::MAX_STORAGE_BUFFERS)
        {
            return RenderGraphPackageStatus::CapacityExceeded;
        }
        for (usize i = 0; i < candidate->StorageBufferCount; ++i)
        {
            format::StorageBuffer& storage = candidate->StorageBuffers[i];
            storage.Slot = reader.ReadWord();
            storage.Stride = reader.ReadWord();
            storage.Count = reader.ReadWord();
            storage.IsReadOnly = reader.ReadWord();
            constexpr uint32 WORD_BYTES = sizeof(uint32);
            if (reader.IsOk() == false || storage.Slot < format::FIRST_STORAGE_SLOT ||
                storage.Slot >= format::FIRST_STORAGE_SLOT + format::MAX_STORAGE_BUFFERS || storage.Stride == 0 ||
                storage.Stride > format::MAX_STORAGE_STRIDE || storage.Stride % WORD_BYTES != 0 ||
                storage.Count > format::MAX_STORAGE_ELEMENTS || storage.IsReadOnly > 1 ||
                (i != 0 && candidate->StorageBuffers[i - 1].Slot >= storage.Slot))
            {
                return RenderGraphPackageStatus::Malformed;
            }
        }
        if (candidate->StorageBufferCount != 0)
        {
            bool hasRay = false;
            for (usize i = 0; i < candidate->Header.PassCount; ++i)
            {
                hasRay = hasRay == true || candidate->Passes[i].Program == format::Technique::RayTracing;
            }
            if (hasRay == false)
            {
                return RenderGraphPackageStatus::Malformed;
            }
            for (const format::StageRole role : {format::StageRole::RayCompute,
                                                 format::StageRole::SceneRayCompute,
                                                 format::StageRole::NativeRayStages})
            {
                const usize index = static_cast<usize>(role);
                rhi::RasterShaderDescription description;
                SetRenderGraphRayInterface(description,
                                           rhi::ShaderStage::Compute,
                                           false,
                                           false,
                                           candidate->HasNativeRayQueries);
                for (usize i = 0; i < description.Bindings.size(); ++i)
                {
                    candidate->Bindings[index][i] = description.Bindings[i];
                }
                const usize base = description.Bindings.size();
                for (usize i = 0; i < candidate->StorageBufferCount; ++i)
                {
                    const format::StorageBuffer& storage = candidate->StorageBuffers[i];
                    candidate->Bindings[index][base + i] = {storage.Slot,
                                                            storage.IsReadOnly == 1
                                                                ? rhi::RasterBindingKind::StorageRead
                                                                : rhi::RasterBindingKind::StorageReadWrite,
                                                            rhi::RasterVisibility::Compute,
                                                            storage.Stride};
                }
                usize bindingCount = base + candidate->StorageBufferCount;
                bool hasVisibilityBuffer = false;
                for (usize i = 0; i < candidate->Header.PassCount; ++i)
                {
                    hasVisibilityBuffer =
                        hasVisibilityBuffer == true || candidate->Passes[i].Visibility != format::ABSENT;
                }
                if (hasVisibilityBuffer == true)
                {
                    SetRenderGraphRayInterface(description,
                                               rhi::ShaderStage::Compute,
                                               false,
                                               true,
                                               candidate->HasNativeRayQueries);
                    for (usize i = base; i < description.Bindings.size(); ++i)
                    {
                        candidate->Bindings[index][bindingCount++] = description.Bindings[i];
                    }
                }
                candidate->BindingCounts[index] = bindingCount;
            }
        }
    }
    if (version >= format::NAMED_PACKAGE_VERSION)
    {
        for (uint32 index = 0; index < candidate->Header.PassCount; ++index)
        {
            const uint32 length = reader.ReadWord();
            if (length == 0 || length > format::MAX_PASS_NAME_BYTES)
            {
                return RenderGraphPackageStatus::Malformed;
            }
            const std::span<const uint8> name = reader.ReadPadded(length);
            rhi::GpuScopeDescription validated;
            candidate->PassNames[index] = {reinterpret_cast<const char*>(name.data()), name.size()};
            if (reader.IsOk() == false || rhi::MakeGpuScope(index + 1, candidate->PassNames[index], validated) == false)
            {
                return RenderGraphPackageStatus::Malformed;
            }
        }
        candidate->HasPassNames = true;
    }
    if (reader.IsComplete() == false)
    {
        return RenderGraphPackageStatus::Malformed;
    }
    for (uint32 i = 0; i < format::VISIBILITY_INPUT_COUNT; ++i)
    {
        candidate->Inputs[i] = {i,
                                i == 0                           ? rhi::RasterVertexFormat::Float3
                                : i < format::STATIC_INPUT_COUNT ? rhi::RasterVertexFormat::Float4
                                                                 : rhi::RasterVertexFormat::Float2};
    }
    if (version < format::NATIVE_RAY_PACKAGE_VERSION)
    {
        for (uint32 pass = 0; pass < candidate->Header.PassCount; ++pass)
        {
            if (candidate->Passes[pass].Program == format::Technique::RayTracing)
            {
                return RenderGraphPackageStatus::UnsupportedVersion;
            }
        }
    }
    if (candidate->HasExplicitAccumulation == false)
    {
        const usize persistentRole = static_cast<usize>(format::StageRole::PersistentRayCompute);
        bool hasPersistentKernel = false;
        for (usize target = 0; target < format::SHADER_TARGET_COUNT; ++target)
        {
            hasPersistentKernel = hasPersistentKernel == true || candidate->Stages[persistentRole][target].IsPresent;
        }
        if (hasPersistentKernel == true)
        {
            for (usize pass = 0; pass < candidate->Header.PassCount; ++pass)
            {
                candidate->Passes[pass].AccumulationSamples = 0;
            }
        }
    }
    if (candidate->HasExplicitToneMapping == false)
    {
        bool hasSceneKernel = false;
        for (usize target = 0; target < format::SHADER_TARGET_COUNT; ++target)
        {
            hasSceneKernel =
                hasSceneKernel == true ||
                candidate->Stages[static_cast<usize>(format::StageRole::SceneRayCompute)][target].IsPresent == true ||
                candidate->Stages[static_cast<usize>(format::StageRole::PersistentRayCompute)][target].IsPresent ==
                    true;
        }
        for (usize pass = 0; pass < candidate->Header.PassCount; ++pass)
        {
            if (candidate->Passes[pass].Program == format::Technique::RayTracing && hasSceneKernel == true)
            {
                candidate->Passes[pass].ToneMappingExposure = 1;
            }
        }
    }
    candidate->DeriveBindings();
    mState.Swap(candidate);
    return RenderGraphPackageStatus::Ready;
}
bool RenderGraphPackage::IsLoaded() const noexcept
{
    return mState.Get() != nullptr;
}
bool RenderGraphPackage::IsRayPathCapture() const noexcept
{
    return mState.Get() != nullptr && mState->IsRayPathCapture == true;
}
void RenderGraphPackage::BypassDenoising() noexcept
{
    if (mState.Get() == nullptr)
    {
        return;
    }
    for (uint32 i = 0; i < mState->Header.PassCount; ++i)
    {
        mState->Passes[i].DenoisingQuality = 0;
    }
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
            {state.UniformFields.GetData(), state.Header.UniformFieldCount},
            state.HasPassNames == true
                ? BorrowedView<const BorrowedView<const char>>{state.PassNames.GetData(), state.Header.PassCount}
                : BorrowedView<const BorrowedView<const char>>{},
            {state.StorageBuffers.GetData(), state.StorageBufferCount}};
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
    result.HasRayPathCapture = state.IsRayPathCapture;
    for (const State::StageView& view : state.Stages[format::CLUSTERED_FORWARD_FRAGMENT_ROLE])
    {
        result.HasClusteredForward = result.HasClusteredForward == true || view.IsPresent == true;
    }
    for (usize pass = 0; pass < state.Header.PassCount; ++pass)
    {
        result.HasVisibilityBuffer =
            result.HasVisibilityBuffer == true || state.Passes[pass].Visibility != format::ABSENT;
    }
    StaticArray<rhi::RasterShaderDescription*, format::LEGACY_STAGE_ROLE_COUNT> members{{&result.ColorVertex,
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
    const StaticArray<rhi::RasterShaderDescription*, format::FORWARD_STAGE_ROLE_COUNT> forwardMembers{
        {&result.ForwardVertex,
         &result.ForwardFragment,
         &result.ForwardShadow,
         &result.LightingVertex,
         &result.LightingFragment,
         &result.LightingShadow},
    };
    for (usize role = 0; role < format::STAGE_ROLE_COUNT; ++role)
    {
        rhi::RasterShaderDescription& description =
            role == format::FORWARD_DEPTH_STAGE_ROLE          ? result.ForwardDepth
            : role == format::CLUSTERED_FORWARD_FRAGMENT_ROLE ? result.ForwardFragment
            : role == format::NATIVE_RAY_STAGE_ROLE           ? result.NativeRayStages
            : role >= format::FORWARD_STAGE_ROLE_FIRST        ? *forwardMembers[role - format::FORWARD_STAGE_ROLE_FIRST]
            : role == static_cast<usize>(format::StageRole::PersistentRayCompute) ? result.RayCompute
            : role < format::LEGACY_STAGE_ROLE_COUNT
                ? *members[role]
                : result.AdditionalRasterStages[role - format::LEGACY_STAGE_ROLE_COUNT];
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
        const bool isRayRole =
            (role >= static_cast<uint32>(format::StageRole::RayCompute) && role < format::LEGACY_STAGE_ROLE_COUNT) ||
            role == static_cast<uint32>(format::StageRole::PersistentRayCompute) ||
            role == format::NATIVE_RAY_STAGE_ROLE;
        const bool isVertex = isRayRole == true ? role == static_cast<uint32>(format::StageRole::RayImageVertex)
                                                : role % format::TECHNIQUE_STAGE_COUNT == 0;
        description.Artifact.Stage = isVertex == true ? rhi::ShaderStage::Vertex : rhi::ShaderStage::Fragment;
        if (isVertex == true)
        {
            if (isRayRole == false && role < format::FORWARD_STAGE_ROLE_FIRST)
            {
                usize inputCount = 0;
                for (const State::StageView& view : views)
                {
                    if (view.IsPresent == true)
                    {
                        inputCount = view.InputCount;
                    }
                }
                description.Inputs = {state.Inputs.GetData(), inputCount};
            }
        }
        if (isRayRole == true)
        {
            const bool isPersistentCompute =
                role == static_cast<usize>(format::StageRole::PersistentRayCompute) ||
                (role == format::NATIVE_RAY_STAGE_ROLE && result.HasPersistentRayBuffers == true);
            description.Artifact.HasNativeRayStages = role == format::NATIVE_RAY_STAGE_ROLE;
            const bool isSceneCompute = role == static_cast<usize>(format::StageRole::SceneRayCompute) ||
                                        isPersistentCompute == true || role == format::NATIVE_RAY_STAGE_ROLE;
            result.HasPersistentRayBuffers = result.HasPersistentRayBuffers == true || isPersistentCompute == true;
            result.HasSceneShaderInterface = result.HasSceneShaderInterface == true || isSceneCompute == true;
            description.Artifact.Stage =
                (role == static_cast<usize>(format::StageRole::RayCompute) || isSceneCompute == true)
                    ? rhi::ShaderStage::Compute
                : isVertex == true ? rhi::ShaderStage::Vertex
                                   : rhi::ShaderStage::Fragment;
            SetRenderGraphRayInterface(description,
                                       description.Artifact.Stage,
                                       isPersistentCompute,
                                       result.HasVisibilityBuffer,
                                       state.HasNativeRayQueries);
            if (description.Artifact.Stage == rhi::ShaderStage::Compute && role != format::NATIVE_RAY_STAGE_ROLE &&
                state.HasExplicitRayWorkgroup == true)
            {
                for (const State::StageView& view : views)
                {
                    if (view.IsPresent == true)
                    {
                        for (usize axis = 0; axis < format::RAY_WORKGROUP_DIMENSIONS; ++axis)
                        {
                            description.WorkgroupSize[axis] = view.WorkgroupSize[axis];
                        }
                    }
                }
            }
        }
        if ((role >= format::FORWARD_STAGE_ROLE_FIRST && role < format::NATIVE_RAY_STAGE_ROLE) ||
            role == format::CLUSTERED_FORWARD_FRAGMENT_ROLE || role == format::FORWARD_DEPTH_STAGE_ROLE)
        {
            SetRenderGraphForwardInterface(description,
                                           role == format::CLUSTERED_FORWARD_FRAGMENT_ROLE
                                               ? format::StageRole::ForwardFragment
                                               : static_cast<format::StageRole>(role),
                                           result.HasClusteredForward);
        }
        if (state.BindingCounts[role] != 0)
        {
            description.Bindings = {state.Bindings[role].GetData(), state.BindingCounts[role]};
        }
        const State::StageView& metal = views[static_cast<usize>(format::ShaderTarget::Metal)];
        const State::StageView& spirv = views[static_cast<usize>(format::ShaderTarget::Spirv)];
        const State::StageView& wgsl = views[static_cast<usize>(format::ShaderTarget::Wgsl)];
        const State::StageView& glsl = views[static_cast<usize>(format::ShaderTarget::GlslEs)];
        for (usize slot = 0; slot < rhi::RASTER_BINDING_COUNT; ++slot)
        {
            description.TextureNames[slot] = glsl.TextureNames[slot];
            description.TextureSamplers[slot] = glsl.TextureSamplers[slot];
        }
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

bool RequiresHardwareRayTracing(const RenderGraphPlan& plan) noexcept
{
    for (usize i = 0; i < plan.Passes.GetSize(); ++i)
    {
        if (plan.Passes[i].Program == format::Technique::RayTracing &&
            (plan.Passes[i].Traversal == format::RayTraversal::NativeQuery ||
             plan.Passes[i].Traversal == format::RayTraversal::HardwarePipeline))
        {
            return true;
        }
    }
    return false;
}
bool RequiresNativeRayPipeline(const RenderGraphPlan& plan) noexcept
{
    for (usize index = 0; index < plan.Passes.GetSize(); ++index)
    {
        const format::Pass& pass = plan.Passes[index];
        if (pass.Program == format::Technique::RayTracing && pass.Traversal == format::RayTraversal::HardwarePipeline)
        {
            return true;
        }
    }
    return false;
}
bool UsesRayTracing(const RenderGraphPlan& plan) noexcept
{
    for (usize index = 0; index < plan.Passes.GetSize(); ++index)
    {
        if (plan.Passes[index].Program == format::Technique::RayTracing)
        {
            return true;
        }
    }
    return false;
}
bool RequiresDenoising(const RenderGraphPlan& plan) noexcept
{
    for (usize i = 0; i < plan.Passes.GetSize(); ++i)
    {
        if (plan.Passes[i].DenoisingQuality != 0)
        {
            return true;
        }
    }
    return false;
}
bool IsDenoisingAvailable() noexcept
{
    return internal::IsGpuDenoisingAvailable();
}
Status SelectRenderGraphRenderer(rhi::DeviceHandle device,
                                 RenderGraphPreference preference,
                                 RenderGraphRenderer& outRenderer) noexcept
{
    if (preference != RenderGraphPreference::Automatic && preference != RenderGraphPreference::ForceRaster)
    {
        return Status::InvalidDescription;
    }
    rhi::RayCapabilities rayCapabilities;
    Status status = rhi::GetRayCapabilities(device, rayCapabilities);
    if (status != Status::Ready && status != Status::Unsupported)
    {
        return status;
    }
    RenderGraphRenderer selected = RenderGraphRenderer::Raster;
    if (preference == RenderGraphPreference::Automatic && status == Status::Ready &&
        rayCapabilities.StaticTriangles == true && rayCapabilities.InlineQueries == true)
    {
        rhi::ComputeCapabilities computeCapabilities;
        status = rhi::GetComputeCapabilities(device, computeCapabilities);
        if (status != Status::Ready && status != Status::Unsupported)
        {
            return status;
        }
        if (status == Status::Ready && computeCapabilities.MaxWorkgroupSize[0] >= LUDUS_GPU_PREVIEW_GROUP_WIDTH &&
            computeCapabilities.MaxWorkgroupInvocations >= LUDUS_GPU_PREVIEW_GROUP_WIDTH &&
            computeCapabilities.MaxStorageBindings >= LUDUS_GPU_PREVIEW_STORAGE_BINDING_COUNT)
        {
            selected = RenderGraphRenderer::HardwareRayTracing;
        }
    }
    outRenderer = selected;
    return Status::Ready;
}

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
    uint32 ReferenceWidth = 0;
    uint32 ReferenceHeight = 0;
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
        Report.ActiveRenderer = Report.CandidateRenderer;
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
    // A diagnostic capture generation runs only in its own RenderGraph, never as drawing output.
    if (package.Get() == nullptr || package->IsLoaded() == false || package->IsRayPathCapture() == true)
    {
        return Status::InvalidDescription;
    }
    // A newer request supersedes any unactivated candidate, whatever its outcome.
    state.Candidate.Reset();
    state.Report.CandidateGeneration = ++state.NextSerial;
    state.Report.CandidateRenderer =
        RequiresHardwareRayTracing(package->GetPlan()) == true
            ? RenderGraphRenderer::HardwareRayTracing
            : (UsesRayTracing(package->GetPlan()) == true ? RenderGraphRenderer::ComputeRayTracing
                                                          : RenderGraphRenderer::Raster);
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
    // RenderGraphPrograms is tens of KiB: a stack temporary here, beneath the caller's frame,
    // overflowed the browser's default 64 KiB stack into the allocator's static state.
    // The new-expression constructs the returned value in place; Initialize copies what it keeps.
    RenderGraphPrograms* programsOrNull = new (std::nothrow)
        RenderGraphPrograms(generation->Package->GetPrograms(state.CompositeVertex, state.CompositeFragment, true));
    const core::UniquePtr<RenderGraphPrograms> programs{programsOrNull};
    if (programs.Get() == nullptr)
    {
        state.FailCandidate(Status::OutOfMemory);
        return Status::OutOfMemory;
    }
    const Status status = generation->Graph.Initialize(state.Device,
                                                       generation->Package->GetPlan(),
                                                       *programs,
                                                       state.IsPooled == true ? &state.Pool : nullptr,
                                                       state.ReferenceWidth,
                                                       state.ReferenceHeight);
    if (IsAccepted(status) == false)
    {
        state.FailCandidate(status);
        return status;
    }
    state.Candidate.Swap(generation);
    state.Report.Candidate = RenderGraphCandidateState::Preparing;
    return status;
}
Status RenderGraphActivation::StageSelected(core::UniquePtr<RenderGraphPackage> rasterPackage,
                                            core::UniquePtr<RenderGraphPackage> rayPackageOrNull,
                                            RenderGraphPreference preference) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    if (rasterPackage.Get() == nullptr || rasterPackage->IsLoaded() == false ||
        UsesRayTracing(rasterPackage->GetPlan()) == true ||
        (rayPackageOrNull.Get() != nullptr &&
         (rayPackageOrNull->IsLoaded() == false || UsesRayTracing(rayPackageOrNull->GetPlan()) == false)))
    {
        return Status::InvalidDescription;
    }
    RenderGraphRenderer selected;
    const Status status = SelectRenderGraphRenderer(mState->Device, preference, selected);
    if (status != Status::Ready)
    {
        return status;
    }
    if (preference == RenderGraphPreference::Automatic && rayPackageOrNull.Get() != nullptr &&
        RequiresHardwareRayTracing(rayPackageOrNull->GetPlan()) == false)
    {
        return Stage(core::Move(rayPackageOrNull));
    }
    if (selected == RenderGraphRenderer::HardwareRayTracing && rayPackageOrNull.Get() != nullptr &&
        RequiresNativeRayPipeline(rayPackageOrNull->GetPlan()) == true)
    {
        rhi::RayCapabilities capabilities;
        const Status capabilityStatus = rhi::GetRayCapabilities(mState->Device, capabilities);
        if (capabilityStatus != Status::Ready && capabilityStatus != Status::Unsupported)
        {
            return capabilityStatus;
        }
        if (capabilities.RayPipelines == false)
        {
            selected = RenderGraphRenderer::Raster;
        }
    }
    return selected == RenderGraphRenderer::HardwareRayTracing && rayPackageOrNull.Get() != nullptr
               ? Stage(core::Move(rayPackageOrNull))
               : Stage(core::Move(rasterPackage));
}
Status RenderGraphActivation::Resize(uint32 referenceWidth, uint32 referenceHeight) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    State& state = *mState;
    if (referenceWidth == 0 || referenceHeight == 0)
    {
        return Status::NotReady;
    }
    state.ReferenceWidth = referenceWidth;
    state.ReferenceHeight = referenceHeight;
    Status activeStatus = Status::Ready;
    if (state.Active.Get() != nullptr)
    {
        activeStatus = state.Active->Graph.Resize(referenceWidth, referenceHeight);
    }
    if (state.Candidate.Get() != nullptr)
    {
        const Status candidateStatus = state.Candidate->Graph.Resize(referenceWidth, referenceHeight);
        if (IsAccepted(candidateStatus) == false)
        {
            state.FailCandidate(candidateStatus);
            return candidateStatus;
        }
        if (candidateStatus == Status::Pending)
        {
            state.Candidate->SceneRevision = 0;
            state.Report.Candidate = RenderGraphCandidateState::NeedsUpdate;
            activeStatus = IsAccepted(activeStatus) == true ? Status::Pending : activeStatus;
        }
    }
    return activeStatus;
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
    if (candidate.Graph.HasPendingResize() == true ||
        (latest != 0 && candidate.Graph.GetDrawableSceneRevision() != latest))
    {
        state.Report.Candidate = latest != 0 && candidate.SceneRevision == latest
                                     ? RenderGraphCandidateState::Preparing
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
                                     const SceneShaderInputs* sceneInputsOrNull,
                                     uint64 contentRevision) noexcept
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
            state.Active->Graph
                .Update(meshes, items, cameras, sceneRevision, policy, outReport, sceneInputsOrNull, contentRevision);
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
        candidate.Graph
            .Update(meshes, items, cameras, sceneRevision, policy, candidateReport, sceneInputsOrNull, contentRevision);
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
