#pragma once

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/core.h>

#include <ludus/content/render_graph_format/runtime_package.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>

#include <span>

/// RG4 transient-pooling workload shared by the Renderer GPU tests and the installed
/// SDK consumer. No admitted cooked graph has a transient attachment that carries useful
/// work: no pass samples a graph resource, and a scene pass into an unexported target
/// repeats camera coverage that lowering never approves. This rewrites a cooked
/// single-camera offscreen `.lurgrt` generation into the numeric plan the Renderer does
/// admit for that case: every pass approves repeated coverage and `scratchCount`
/// unexported color + private-depth backings, each drawn by one more ColorAndDepth pass
/// of the camera, follow the exported pass. All resources take the given extent, so a
/// resize is a reload of a generation with another extent, as for a re-cooked graph.
namespace ludus::render_graph_scratch
{
namespace format = content::render_graph_format;
using foundation::uint32;
using foundation::uint8;
using foundation::usize;
/// Read the little-endian word at offset; false when it does not fit.
[[nodiscard]] inline bool ReadWord(foundation::BorrowedView<const uint8> bytes, usize offset, uint32& outWord) noexcept
{
    return offset <= bytes.GetSize() && bytes.GetSize() - offset >= sizeof(uint32) &&
           foundation::TryReadLittleEndian(std::span<const uint8>{bytes.GetDataOrNull() + offset, sizeof(uint32)},
                                           outWord);
}
/// Append one little-endian word.
inline void WriteWord(foundation::Array<uint8>& inoutBytes, uint32 word) noexcept
{
    for (usize i = 0; i < sizeof(uint32); ++i)
    {
        inoutBytes.Add(static_cast<uint8>((word >> (8U * i)) & 0xFFU));
    }
}
/// Scratch backings and the extent every resource of a rewritten generation takes.
struct ScratchShape final
{
    uint32 Scratch = 0; ///< Unexported color + private-depth backings to add.
    uint32 Width = 0;   ///< Physical width of every resource.
    uint32 Height = 0;  ///< Physical height of every resource.
};
/// Rewrite a cooked offscreen generation with scratch backings and the shape's extent.
/// @param offscreen Complete generation whose header and plan records are well formed.
/// @param outBytes Receives the rewritten generation; preserved when false is returned.
/// @return False for a truncated or non-generation input, or when the scratch backings
/// exceed the plan's resource or pass capacity. Stage records are copied unchanged.
[[nodiscard]] inline bool WriteScratchGeneration(foundation::BorrowedView<const uint8> offscreen,
                                                 const ScratchShape& shape,
                                                 foundation::Array<uint8>& outBytes) noexcept
{
    const uint32 scratchCount = shape.Scratch;
    const uint32 width = shape.Width;
    const uint32 height = shape.Height;
    constexpr usize RESOURCE_COUNT_FIELD = 2;
    constexpr usize PASS_COUNT_FIELD = 3;
    constexpr usize CAMERA_COUNT_FIELD = 4;
    constexpr usize REPEATED_COVERAGE_FIELD = 8;
    constexpr usize RESOURCE_FIELDS = format::RESOURCE_RECORD_BYTES / sizeof(uint32);
    constexpr usize PASS_FIELDS = format::PASS_RECORD_BYTES / sizeof(uint32);
    uint32 magic = 0;
    uint32 version = 0;
    uint32 resources = 0;
    uint32 passes = 0;
    uint32 cameras = 0;
    if (ReadWord(offscreen, 0, magic) == false || magic != format::PACKAGE_MAGIC ||
        ReadWord(offscreen, sizeof(uint32), version) == false || version != format::PACKAGE_VERSION ||
        ReadWord(offscreen, RESOURCE_COUNT_FIELD * sizeof(uint32), resources) == false ||
        ReadWord(offscreen, PASS_COUNT_FIELD * sizeof(uint32), passes) == false ||
        ReadWord(offscreen, CAMERA_COUNT_FIELD * sizeof(uint32), cameras) == false ||
        resources + usize{2} * scratchCount > format::MAX_RESOURCES ||
        passes + usize{scratchCount} > format::MAX_PASSES)
    {
        return false;
    }
    const usize camerasAt = format::HEADER_BYTES + usize{resources} * format::RESOURCE_RECORD_BYTES;
    const usize passesAt = camerasAt + usize{cameras} * format::CAMERA_RECORD_BYTES;
    const usize restAt = passesAt + usize{passes} * format::PASS_RECORD_BYTES;
    if (restAt > offscreen.GetSize())
    {
        return false;
    }
    foundation::Array<uint8> bytes;
    for (usize offset = 0; offset < camerasAt; offset += sizeof(uint32))
    {
        uint32 word = 0;
        static_cast<void>(ReadWord(offscreen, offset, word));
        const usize field = offset / sizeof(uint32);
        const bool isResource = offset >= format::HEADER_BYTES;
        const usize resourceField =
            isResource == true ? (offset - format::HEADER_BYTES) / sizeof(uint32) % RESOURCE_FIELDS : 0;
        if (field == RESOURCE_COUNT_FIELD)
        {
            word += 2 * scratchCount;
        }
        else if (field == PASS_COUNT_FIELD)
        {
            word += scratchCount;
        }
        else if (isResource == true && resourceField < 2)
        {
            word = resourceField == 0 ? width : height;
        }
        WriteWord(bytes, word);
    }
    for (uint32 i = 0; i < scratchCount; ++i)
    {
        const uint32 color = resources + 2 * i;
        for (const format::Aspect aspect : {format::Aspect::Color, format::Aspect::Depth})
        {
            for (const uint32 word : {width, height, static_cast<uint32>(aspect), color, 0U, 0U, 0U, 1U})
            {
                WriteWord(bytes, word);
            }
        }
    }
    for (usize offset = camerasAt; offset < restAt; offset += sizeof(uint32))
    {
        uint32 word = 0;
        static_cast<void>(ReadWord(offscreen, offset, word));
        const bool isApproval =
            offset >= passesAt && (offset - passesAt) / sizeof(uint32) % PASS_FIELDS == REPEATED_COVERAGE_FIELD;
        WriteWord(bytes, isApproval == true ? 1U : word);
    }
    for (uint32 i = 0; i < scratchCount; ++i)
    {
        const uint32 color = resources + 2 * i;
        for (const uint32 word : {static_cast<uint32>(format::Technique::ColorAndDepth),
                                  0U,
                                  color,
                                  color + 1,
                                  static_cast<uint32>(format::Load::Clear),
                                  static_cast<uint32>(format::Store::Store),
                                  static_cast<uint32>(format::Load::Clear),
                                  static_cast<uint32>(format::Store::Store),
                                  1U,
                                  ::ludus::content::render_graph_format::SCENE_DATA_ALL,
                                  0U,
                                  format::ABSENT,
                                  format::ABSENT,
                                  format::ABSENT,
                                  format::ABSENT})
        {
            WriteWord(bytes, word);
        }
    }
    for (usize offset = restAt; offset < offscreen.GetSize(); ++offset)
    {
        bytes.Add(offscreen[offset]);
    }
    constexpr uint32 SCRATCH_NAME_BYTES = 7;
    constexpr uint32 SCRATCH_NAME_PADDING = 0;
    for (uint32 i = 0; i < scratchCount; ++i)
    {
        WriteWord(bytes, SCRATCH_NAME_BYTES);
        for (const char character : {'S', 'c', 'r', 'a', 't', 'c', 'h'})
        {
            bytes.Add(static_cast<uint8>(character));
        }
        bytes.Add(static_cast<uint8>(SCRATCH_NAME_PADDING));
    }
    outBytes = Move(bytes);
    return true;
}
} // namespace ludus::render_graph_scratch
