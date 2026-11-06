// Windows render-graph DXIL build evidence (D6c): rg2 graphs authored for the dxil profile
// compile through the pinned DXC, cook into runtime packages and decode into Direct3D 12
// shader descriptions. Pipeline creation and drawing on Direct3D 12 are separate.

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>

#include "rg2_dxil_bound.h"
#include "rg2_dxil_surface.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <fstream>
#include <span>

using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace packages = ludus::render_graph_packages;
namespace
{
// DXBC header: code, 16-byte hash, version and total size; unsigned output has a constant hash.
bool IsSignedDxil(std::span<const uint8> bytes)
{
    constexpr usize HEADER_BYTES = 32;
    constexpr usize HASH_OFFSET = 4;
    constexpr usize HASH_BYTES = 16;
    constexpr usize SIZE_OFFSET = 24;
    if (bytes.size() < HEADER_BYTES || std::memcmp(bytes.data(), "DXBC", HASH_OFFSET) != 0)
    {
        return false;
    }
    uint32 size = 0;
    std::memcpy(&size, bytes.data() + SIZE_OFFSET, sizeof(size));
    bool isConstant = true;
    for (usize i = 1; i < HASH_BYTES; ++i)
    {
        isConstant = isConstant == true && bytes[HASH_OFFSET + i] == bytes[HASH_OFFSET];
    }
    return size == bytes.size() && isConstant == false;
}
void CheckPackage(const char* path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    REQUIRE(file.good());
    const std::streamoff size = file.tellg();
    REQUIRE(size > 0);
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(static_cast<usize>(size)));
    file.seekg(0);
    REQUIRE(file.read(reinterpret_cast<char*>(bytes.GetData()), size).good());
    renderer::RenderGraphPackage package;
    REQUIRE(package.Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
    const renderer::RenderGraphPrograms programs = package.GetPrograms({}, {});
    for (const rhi::RasterShaderDescription* stage : {&programs.ColorVertex, &programs.ColorFragment})
    {
        INFO(path);
        CHECK(IsSignedDxil(stage->Artifact.Dxil) == true);
        CHECK(stage->Artifact.DxilEntry.empty() == false);
        // A dxil-only package carries no other representation.
        CHECK(stage->Artifact.Spirv.empty() == true);
        CHECK(stage->Artifact.Wgsl.empty() == true);
    }
}
} // namespace

TEST_CASE("Render graphs built for the dxil profile cook signed DXIL stages", "[render-graph][dxil]")
{
    CheckPackage(LUDUS_RG2_DXIL_SURFACE_PACKAGE);
    CheckPackage(LUDUS_RG2_DXIL_BOUND_PACKAGE);
    // The compiled headers carry the same plans; their programs need the composite stages.
    CHECK(packages::rg2_dxil_surface::MakePlan().Passes.GetSize() != 0);
    CHECK(packages::rg2_dxil_bound::MakePlan().Passes.GetSize() != 0);
}
