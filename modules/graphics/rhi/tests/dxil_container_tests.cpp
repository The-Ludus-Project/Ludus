// DXIL container checks used by the facade and the Direct3D 12 backend. Synthetic
// containers follow the DXC part layout, so these cases run on every host; the
// Windows lane exercises real DXC output (d3d12_tests.cpp).

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/array.hpp>

#include "internal/dxil_container.h"

#include <span>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

using ludus::foundation::Array;
using ludus::foundation::uint32;
using ludus::foundation::uint8;
using ludus::foundation::usize;
using ludus::graphics::rhi::ShaderStage;
using ludus::graphics::rhi::internal::DXIL_CONTAINER_HEADER_BYTES;
using ludus::graphics::rhi::internal::HasDxilEntry;
using ludus::graphics::rhi::internal::IsDxilContainer;

namespace
{
constexpr uint32 PIXEL_KIND = 0;
constexpr uint32 VERTEX_KIND = 1;
/// PSV0 runtime info sizes of versions 2 and 3; version 3 adds the entry name.
constexpr uint32 RUNTIME_VERSION2_BYTES = 48;
constexpr uint32 RUNTIME_VERSION3_BYTES = 52;
constexpr usize RUNTIME_STAGE_OFFSET = 24;
constexpr usize RUNTIME_ENTRY_OFFSET = 48;
/// One resource binding record of PSV0 version 2.
constexpr uint32 RESOURCE_RECORD_BYTES = 24;
constexpr usize CONTAINER_SIZE_OFFSET = 24;

struct ContainerShape final
{
    uint32 ProgramKind = VERTEX_KIND;
    uint32 ValidationKind = VERTEX_KIND;
    std::string_view Entry = "vertexMain";
    uint32 RuntimeBytes = RUNTIME_VERSION3_BYTES;
    bool ShouldTerminateEntry = true;
    bool ShouldDuplicateValidation = false;
};

void AppendUint32(Array<uint8>& inoutBytes, uint32 value)
{
    uint8 encoded[sizeof(uint32)]{};
    REQUIRE(ludus::foundation::core::TryWriteLittleEndian(value, encoded) == true);
    inoutBytes.AddRange(encoded);
}

void AppendCode(Array<uint8>& inoutBytes, std::string_view code)
{
    for (const char character : code)
    {
        inoutBytes.Add(static_cast<uint8>(character));
    }
}

void WriteUint32(Array<uint8>& inoutBytes, usize offset, uint32 value)
{
    REQUIRE(ludus::foundation::core::TryWriteLittleEndian(
                value,
                std::span<uint8>{inoutBytes.GetData() + offset, sizeof(uint32)}) == true);
}

Array<uint8> MakeProgramPart(uint32 kind)
{
    Array<uint8> part;
    // Shader model 6.0: kind in the upper half, major and minor in the low byte.
    AppendUint32(part, (kind << 16) | 0x60U);
    AppendUint32(part, 0);
    return part;
}

Array<uint8> MakeValidationPart(const ContainerShape& shape)
{
    Array<uint8> part;
    AppendUint32(part, shape.RuntimeBytes);
    const usize runtime = part.GetSize();
    for (uint32 i = 0; i < shape.RuntimeBytes; ++i)
    {
        part.Add(0);
    }
    part[runtime + RUNTIME_STAGE_OFFSET] = static_cast<uint8>(shape.ValidationKind);
    // The string table starts with an empty string, so the entry name is at offset 1.
    if (shape.RuntimeBytes >= RUNTIME_VERSION3_BYTES)
    {
        WriteUint32(part, runtime + RUNTIME_ENTRY_OFFSET, 1);
    }
    AppendUint32(part, 1);
    AppendUint32(part, RESOURCE_RECORD_BYTES);
    for (uint32 i = 0; i < RESOURCE_RECORD_BYTES; ++i)
    {
        part.Add(0);
    }
    Array<uint8> strings;
    strings.Add(0);
    AppendCode(strings, shape.Entry);
    if (shape.ShouldTerminateEntry == true)
    {
        strings.Add(0);
    }
    AppendUint32(part, static_cast<uint32>(strings.GetSize()));
    part.AddRange(std::span<const uint8>{strings.GetData(), strings.GetSize()});
    return part;
}

Array<uint8> MakeContainer(const ContainerShape& shape)
{
    Array<uint8> program = MakeProgramPart(shape.ProgramKind);
    Array<uint8> validation = MakeValidationPart(shape);
    const uint32 partCount = shape.ShouldDuplicateValidation == true ? 3 : 2;
    Array<uint8> bytes;
    AppendCode(bytes, "DXBC");
    for (usize i = 0; i < 16; ++i)
    {
        bytes.Add(0);
    }
    AppendUint32(bytes, 1);
    AppendUint32(bytes, 0);
    AppendUint32(bytes, partCount);
    const usize offsets = bytes.GetSize();
    for (uint32 i = 0; i < partCount; ++i)
    {
        AppendUint32(bytes, 0);
    }
    const auto appendPart = [&bytes, offsets](usize index, std::string_view code, const Array<uint8>& content) {
        WriteUint32(bytes, offsets + index * sizeof(uint32), static_cast<uint32>(bytes.GetSize()));
        AppendCode(bytes, code);
        AppendUint32(bytes, static_cast<uint32>(content.GetSize()));
        bytes.AddRange(std::span<const uint8>{content.GetData(), content.GetSize()});
    };
    appendPart(0, "DXIL", program);
    appendPart(1, "PSV0", validation);
    if (shape.ShouldDuplicateValidation == true)
    {
        appendPart(2, "PSV0", validation);
    }
    WriteUint32(bytes, CONTAINER_SIZE_OFFSET, static_cast<uint32>(bytes.GetSize()));
    return bytes;
}

std::span<const uint8> View(const Array<uint8>& bytes)
{
    return {bytes.GetData(), bytes.GetSize()};
}
} // namespace

TEST_CASE("DXIL container header requires the code and its exact size", "[rhi][dxil]")
{
    Array<uint8> bytes = MakeContainer({});
    CHECK(IsDxilContainer(View(bytes)) == true);
    CHECK(IsDxilContainer(View(bytes).first(DXIL_CONTAINER_HEADER_BYTES - 1)) == false);
    CHECK(IsDxilContainer(View(bytes).first(bytes.GetSize() - 1)) == false);
    CHECK(IsDxilContainer({}) == false);
    bytes[0] = 'S';
    CHECK(IsDxilContainer(View(bytes)) == false);
}

TEST_CASE("DXIL container entry matches the program kind and PSV0 entry", "[rhi][dxil]")
{
    const Array<uint8> vertex = MakeContainer({});
    CHECK(HasDxilEntry(View(vertex), ShaderStage::Vertex, "vertexMain") == true);
    CHECK(HasDxilEntry(View(vertex), ShaderStage::Fragment, "vertexMain") == false);
    CHECK(HasDxilEntry(View(vertex), ShaderStage::Vertex, "missing") == false);
    CHECK(HasDxilEntry(View(vertex), ShaderStage::Vertex, "vertex") == false);
    const Array<uint8> fragment = MakeContainer(
    {
        .ProgramKind = PIXEL_KIND,
        .ValidationKind = PIXEL_KIND,
        .Entry = "fragmentMain",
    });
    CHECK(HasDxilEntry(View(fragment), ShaderStage::Fragment, "fragmentMain") == true);
    // The program header and PSV0 must agree on the stage.
    const Array<uint8> mixed = MakeContainer({ .ProgramKind = VERTEX_KIND, .ValidationKind = PIXEL_KIND });
    CHECK(HasDxilEntry(View(mixed), ShaderStage::Vertex, "vertexMain") == false);
    CHECK(HasDxilEntry(View(mixed), ShaderStage::Fragment, "vertexMain") == false);
}

TEST_CASE("DXIL container rejects malformed or ambiguous parts", "[rhi][dxil]")
{
    const Array<uint8> withoutEntryName = MakeContainer({ .RuntimeBytes = RUNTIME_VERSION2_BYTES });
    CHECK(HasDxilEntry(View(withoutEntryName), ShaderStage::Vertex, "vertexMain") == false);
    const Array<uint8> unterminated = MakeContainer({ .ShouldTerminateEntry = false });
    CHECK(HasDxilEntry(View(unterminated), ShaderStage::Vertex, "vertexMain") == false);
    const Array<uint8> duplicated = MakeContainer({ .ShouldDuplicateValidation = true });
    CHECK(HasDxilEntry(View(duplicated), ShaderStage::Vertex, "vertexMain") == false);
    // A part offset outside the container is rejected before any read.
    Array<uint8> outside = MakeContainer({});
    WriteUint32(outside, DXIL_CONTAINER_HEADER_BYTES, static_cast<uint32>(outside.GetSize()));
    CHECK(HasDxilEntry(View(outside), ShaderStage::Vertex, "vertexMain") == false);
    // So is a part whose recorded size runs past the container.
    Array<uint8> oversized = MakeContainer({});
    uint32 programOffset = 0;
    REQUIRE(ludus::foundation::core::TryReadLittleEndian(
                std::span<const uint8>{oversized.GetData() + DXIL_CONTAINER_HEADER_BYTES, sizeof(uint32)},
                programOffset) == true);
    WriteUint32(oversized, programOffset + sizeof(uint32), static_cast<uint32>(oversized.GetSize()));
    CHECK(HasDxilEntry(View(oversized), ShaderStage::Vertex, "vertexMain") == false);
    // And a part count larger than the offset table can hold.
    Array<uint8> counted = MakeContainer({});
    WriteUint32(counted, DXIL_CONTAINER_HEADER_BYTES - sizeof(uint32), 0xFFFFFFFFU);
    CHECK(HasDxilEntry(View(counted), ShaderStage::Vertex, "vertexMain") == false);
}
