#include "internal/dxil_container.h"
#include "internal/raster.h"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/core.h>

// Thanks to Microsoft and the DirectX Shader Compiler contributors for the container
// and pipeline state validation layouts in "DxilContainer.h" and
// "DxilPipelineStateValidation.h" (include/dxc/DxilContainer/,
// https://github.com/microsoft/DirectXShaderCompiler). This reader follows their
// documented little-endian field order; no DXC code is copied, and it reads only the
// fields Ludus checks before handing the bytes to Direct3D 12.
namespace ludus::graphics::rhi::internal
{
namespace
{
using namespace ludus::foundation;

constexpr usize UINT32_BYTES = sizeof(uint32);
constexpr uint32 BITS_PER_BYTE = 8;

/// Four ASCII characters stored first-to-last in little-endian order, as DXC writes them.
[[nodiscard]] constexpr uint32 MakeFourCharacterCode(const char (&code)[UINT32_BYTES + 1]) noexcept
{
    uint32 value = 0;
    for (usize i = 0; i < UINT32_BYTES; ++i)
    {
        value |= static_cast<uint32>(static_cast<uint8>(code[i])) << (i * BITS_PER_BYTE);
    }
    return value;
}

constexpr uint32 CONTAINER_CODE = MakeFourCharacterCode("DXBC");
constexpr uint32 PROGRAM_PART_CODE = MakeFourCharacterCode("DXIL");
constexpr uint32 VALIDATION_PART_CODE = MakeFourCharacterCode("PSV0");
/// Container header: code (0), hash (4..19), version (20..23), total size, part count.
constexpr usize CONTAINER_SIZE_OFFSET = 24;
constexpr usize PART_COUNT_OFFSET = 28;
/// Each part starts with its four-character code and its content size.
constexpr usize PART_HEADER_BYTES = 8;
/// The DXIL program version keeps the shader kind in its upper 16 bits.
constexpr uint32 PROGRAM_KIND_SHIFT = 16;
/// DXIL::ShaderKind values, shared by the program version and PSV0.
constexpr uint32 PIXEL_KIND = 0;
constexpr uint32 VERTEX_KIND = 1;
constexpr uint32 COMPUTE_KIND = 5;
/// PSV0 runtime info: version 1 adds the shader stage byte, version 3 the entry name's
/// string table offset. Earlier versions carry no entry name and are rejected.
constexpr usize RUNTIME_STAGE_OFFSET = 24;
constexpr usize RUNTIME_ENTRY_OFFSET = 48;
constexpr usize RUNTIME_VERSION3_BYTES = 52;

[[nodiscard]] bool ReadUint32(std::span<const uint8> bytes, usize offset, uint32& outValue) noexcept
{
    return offset <= bytes.size() && core::TryReadLittleEndian(bytes.subspan(offset), outValue) == true;
}

[[nodiscard]] uint32 GetShaderKind(ShaderStage stage) noexcept
{
    return stage == ShaderStage::Vertex ? VERTEX_KIND : stage == ShaderStage::Fragment ? PIXEL_KIND : COMPUTE_KIND;
}

/// Finds the only part with code; duplicate or out-of-range parts fail.
[[nodiscard]] bool FindPart(std::span<const uint8> bytes, uint32 code, std::span<const uint8>& outPart) noexcept
{
    uint32 count = 0;
    if (ReadUint32(bytes, PART_COUNT_OFFSET, count) == false ||
        count > (bytes.size() - DXIL_CONTAINER_HEADER_BYTES) / UINT32_BYTES)
    {
        return false;
    }
    const usize partsBegin = DXIL_CONTAINER_HEADER_BYTES + static_cast<usize>(count) * UINT32_BYTES;
    bool isFound = false;
    for (usize index = 0; index < count; ++index)
    {
        uint32 offset = 0;
        uint32 partCode = 0;
        uint32 partBytes = 0;
        if (ReadUint32(bytes, DXIL_CONTAINER_HEADER_BYTES + index * UINT32_BYTES, offset) == false ||
            offset < partsBegin || offset > bytes.size() - PART_HEADER_BYTES ||
            ReadUint32(bytes, offset, partCode) == false ||
            ReadUint32(bytes, offset + UINT32_BYTES, partBytes) == false ||
            partBytes > bytes.size() - offset - PART_HEADER_BYTES)
        {
            return false;
        }
        if (partCode != code)
        {
            continue;
        }
        if (isFound == true)
        {
            return false;
        }
        outPart = bytes.subspan(offset + PART_HEADER_BYTES, partBytes);
        isFound = true;
    }
    return isFound;
}

/// Reads the PSV0 shader kind and entry name; the name borrows the part's string table.
[[nodiscard]] bool
ReadValidationEntry(std::span<const uint8> part, uint32& outKind, std::string_view& outEntry) noexcept
{
    uint32 runtimeBytes = 0;
    if (ReadUint32(part, 0, runtimeBytes) == false || runtimeBytes < RUNTIME_VERSION3_BYTES ||
        runtimeBytes > part.size() - UINT32_BYTES)
    {
        return false;
    }
    const std::span<const uint8> runtime = part.subspan(UINT32_BYTES, runtimeBytes);
    uint32 entryOffset = 0;
    if (ReadUint32(runtime, RUNTIME_ENTRY_OFFSET, entryOffset) == false)
    {
        return false;
    }
    usize position = UINT32_BYTES + runtimeBytes;
    uint32 resourceCount = 0;
    if (ReadUint32(part, position, resourceCount) == false)
    {
        return false;
    }
    position += UINT32_BYTES;
    if (resourceCount != 0)
    {
        uint32 resourceBytes = 0;
        if (ReadUint32(part, position, resourceBytes) == false)
        {
            return false;
        }
        position += UINT32_BYTES;
        const uint64 resourceTableBytes = uint64{resourceCount} * resourceBytes;
        if (resourceTableBytes > part.size() - position)
        {
            return false;
        }
        position += static_cast<usize>(resourceTableBytes);
    }
    uint32 stringBytes = 0;
    if (ReadUint32(part, position, stringBytes) == false)
    {
        return false;
    }
    position += UINT32_BYTES;
    if (stringBytes > part.size() - position || entryOffset >= stringBytes)
    {
        return false;
    }
    const std::span<const uint8> strings = part.subspan(position + entryOffset, stringBytes - entryOffset);
    usize length = 0;
    while (length < strings.size() && strings[length] != 0)
    {
        ++length;
    }
    if (length == strings.size())
    {
        return false;
    }
    outKind = runtime[RUNTIME_STAGE_OFFSET];
    outEntry = std::string_view(reinterpret_cast<const char*>(strings.data()), length);
    return true;
}
} // namespace

bool IsDxilContainer(std::span<const uint8> bytes) noexcept
{
    uint32 code = 0;
    uint32 recordedBytes = 0;
    return bytes.size() >= DXIL_CONTAINER_HEADER_BYTES && ReadUint32(bytes, 0, code) == true &&
           code == CONTAINER_CODE && ReadUint32(bytes, CONTAINER_SIZE_OFFSET, recordedBytes) == true &&
           recordedBytes == bytes.size();
}

bool HasDxilEntry(std::span<const uint8> bytes, ShaderStage stage, std::string_view entry) noexcept
{
    std::span<const uint8> program;
    std::span<const uint8> validation;
    uint32 programVersion = 0;
    uint32 validationKind = 0;
    std::string_view validationEntry;
    if (IsDxilContainer(bytes) == false || FindPart(bytes, PROGRAM_PART_CODE, program) == false ||
        FindPart(bytes, VALIDATION_PART_CODE, validation) == false || ReadUint32(program, 0, programVersion) == false ||
        ReadValidationEntry(validation, validationKind, validationEntry) == false)
    {
        return false;
    }
    const uint32 kind = GetShaderKind(stage);
    return programVersion >> PROGRAM_KIND_SHIFT == kind && validationKind == kind && validationEntry == entry;
}

bool HasDxilBindings(std::span<const uint8> bytes, const RasterLayout& layout) noexcept
{
    // PSVResourceBindInfo1: ResType, Space, LowerBound, UpperBound, ResKind,
    // ResFlags. See the DXC attribution above. Read fields, never host structs.
    constexpr usize TYPE_OFFSET = 0;
    constexpr usize SPACE_OFFSET = TYPE_OFFSET + sizeof(uint32);
    constexpr usize LOWER_OFFSET = SPACE_OFFSET + sizeof(uint32);
    constexpr usize UPPER_OFFSET = LOWER_OFFSET + sizeof(uint32);
    constexpr usize KIND_OFFSET = UPPER_OFFSET + sizeof(uint32);
    constexpr usize FLAGS_OFFSET = KIND_OFFSET + sizeof(uint32);
    constexpr usize RECORD_WORDS = (FLAGS_OFFSET + sizeof(uint32)) / sizeof(uint32);
    constexpr usize RECORD_BYTES = RECORD_WORDS * sizeof(uint32);
    constexpr uint32 SAMPLER_TYPE = 1;
    constexpr uint32 UNIFORM_TYPE = 2;
    constexpr uint32 TEXTURE_TYPE = 3;
    constexpr uint32 TEXTURE2D_KIND = 2;
    constexpr uint32 TEXTURE3D_KIND = 4;
    constexpr uint32 TEXTURE_CUBE_KIND = 5;
    constexpr uint32 TEXTURE_ARRAY_KIND = 7;
    constexpr uint32 UNIFORM_KIND = 13;
    constexpr uint32 SAMPLER_KIND = 14;
    std::span<const uint8> part;
    uint32 runtimeBytes = 0;
    if (IsDxilContainer(bytes) == false || FindPart(bytes, VALIDATION_PART_CODE, part) == false ||
        ReadUint32(part, 0, runtimeBytes) == false || runtimeBytes < RUNTIME_VERSION3_BYTES ||
        runtimeBytes > part.size() - UINT32_BYTES)
    {
        return false;
    }
    usize position = UINT32_BYTES + runtimeBytes;
    uint32 count = 0;
    if (ReadUint32(part, position, count) == false || count != layout.Count)
    {
        return false;
    }
    if (count == 0)
    {
        return true;
    }
    position += UINT32_BYTES;
    uint32 stride = 0;
    if (ReadUint32(part, position, stride) == false || stride < RECORD_BYTES)
    {
        return false;
    }
    position += UINT32_BYTES;
    if (uint64{count} * stride > part.size() - position)
    {
        return false;
    }
    uint32 matchedBindings = 0;
    for (uint32 resource = 0; resource < count; ++resource)
    {
        uint32 type = 0;
        uint32 space = 0;
        uint32 lower = 0;
        uint32 upper = 0;
        uint32 kind = 0;
        const usize record = position + usize{resource} * stride;
        if (ReadUint32(part, record, type) == false || ReadUint32(part, record + UINT32_BYTES, space) == false ||
            ReadUint32(part, record + 2 * UINT32_BYTES, lower) == false ||
            ReadUint32(part, record + UPPER_OFFSET, upper) == false ||
            ReadUint32(part, record + KIND_OFFSET, kind) == false || space != 0 || lower != upper ||
            lower >= RASTER_BINDING_COUNT || (matchedBindings & (uint32{1} << lower)) != 0)
        {
            return false;
        }
        bool isMatched = false;
        for (usize entry = 0; entry < layout.Count; ++entry)
        {
            const RasterBinding& binding = layout.Entries[entry];
            if (binding.Binding != lower || binding.Count != 1 || binding.IsRuntimeSized == true)
            {
                continue;
            }
            const uint32 expectedType = binding.Kind == RasterBindingKind::UniformBuffer ? UNIFORM_TYPE
                                        : binding.Kind == RasterBindingKind::Sampler     ? SAMPLER_TYPE
                                                                                         : TEXTURE_TYPE;
            const uint32 expectedKind = binding.Kind == RasterBindingKind::UniformBuffer    ? UNIFORM_KIND
                                        : binding.Kind == RasterBindingKind::Sampler        ? SAMPLER_KIND
                                        : binding.Kind == RasterBindingKind::Texture2D      ? TEXTURE2D_KIND
                                        : binding.Kind == RasterBindingKind::Texture2DArray ? TEXTURE_ARRAY_KIND
                                        : binding.Kind == RasterBindingKind::TextureCube    ? TEXTURE_CUBE_KIND
                                        : binding.Kind == RasterBindingKind::Texture3D      ? TEXTURE3D_KIND
                                                                                            : 0;
            isMatched = type == expectedType && kind == expectedKind && expectedKind != 0;
        }
        if (isMatched == false)
        {
            return false;
        }
        matchedBindings |= uint32{1} << lower;
    }
    return true;
}
bool ReadDxilVertexInput(std::span<const uint8> bytes, uint32 location, DxilVertexInput& outInput) noexcept
{
    // DXC DxilContainer.h: DxilProgramSignature header and 32-byte
    // DxilProgramSignatureElement (ISG1). Names are relative to the part payload.
    constexpr usize HEADER_BYTES = 2 * UINT32_BYTES;
    constexpr usize ELEMENT_WORDS = 8;
    constexpr usize ELEMENT_BYTES = ELEMENT_WORDS * UINT32_BYTES;
    constexpr usize SEMANTIC_OFFSET = UINT32_BYTES;
    constexpr usize INDEX_OFFSET = 2 * UINT32_BYTES;
    constexpr usize SYSTEM_OFFSET = 3 * UINT32_BYTES;
    constexpr usize REGISTER_OFFSET = 5 * UINT32_BYTES;
    std::span<const uint8> part;
    uint32 count = 0;
    uint32 offset = 0;
    if (IsDxilContainer(bytes) == false || FindPart(bytes, MakeFourCharacterCode("ISG1"), part) == false ||
        ReadUint32(part, 0, count) == false || ReadUint32(part, UINT32_BYTES, offset) == false ||
        offset < HEADER_BYTES || offset > part.size() || count > (part.size() - offset) / ELEMENT_BYTES)
    {
        return false;
    }
    DxilVertexInput candidate;
    bool isFound = false;
    for (uint32 element = 0; element < count; ++element)
    {
        const usize start = offset + usize{element} * ELEMENT_BYTES;
        uint32 nameOffset = 0;
        uint32 index = 0;
        uint32 system = 0;
        uint32 inputRegister = 0;
        if (ReadUint32(part, start + SEMANTIC_OFFSET, nameOffset) == false ||
            ReadUint32(part, start + INDEX_OFFSET, index) == false ||
            ReadUint32(part, start + SYSTEM_OFFSET, system) == false ||
            ReadUint32(part, start + REGISTER_OFFSET, inputRegister) == false ||
            nameOffset < offset + usize{count} * ELEMENT_BYTES || nameOffset >= part.size())
        {
            return false;
        }
        usize end = nameOffset;
        while (end < part.size() && part[end] != 0)
        {
            ++end;
        }
        if (end == nameOffset || end == part.size())
        {
            return false;
        }
        if (system == 0 && inputRegister == location)
        {
            if (isFound == true)
            {
                return false;
            }
            candidate = {reinterpret_cast<const char*>(part.data() + nameOffset), index};
            isFound = true;
        }
    }
    if (isFound == true)
    {
        outInput = candidate;
    }
    return isFound;
}
} // namespace ludus::graphics::rhi::internal
