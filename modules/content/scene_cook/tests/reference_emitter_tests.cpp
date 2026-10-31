#include <ludus/content/scene_cook/scene_assets.h>
#include <ludus/content/scene_cook/scene_cook.h>
#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/runtime/resources/scene.h>

#include "python_compat.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace
{
using namespace ludus::foundation;
namespace cook = ludus::content::scene_cook;
namespace format = ludus::content::scene_format;
namespace math = ludus::foundation::math;
namespace resources = ludus::resources;

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path);
    std::ostringstream result;
    result << input.rdbuf();
    return result.str();
}

struct EmitterFixture
{
    std::filesystem::path Source;
    std::string Original;

    EmitterFixture()
    {
        char directory[] = "/tmp/ludus-emitter-XXXXXX";
        REQUIRE(mkdtemp(directory) != nullptr);
        Source = directory;
        std::filesystem::copy(std::filesystem::path(LUDUS_SCENE_GOLDENS_DIR) / "source",
                              Source,
                              std::filesystem::copy_options::recursive);
        Original = ReadText(Source / "scene-v4.pbrt");
    }
    LUDUS_INLINE ~EmitterFixture()
    {
        std::error_code ignored;
        std::filesystem::remove_all(Source, ignored);
    }
    void WriteScene(const std::string& addition) const
    {
        std::string text = Original;
        const usize end = text.rfind("WorldEnd");
        REQUIRE(end != std::string::npos);
        text.insert(end, addition);
        std::ofstream(Source / "scene-v4.pbrt", std::ios::binary) << text;
    }
    void WritePly(bool hasAuthoredNormals) const
    {
        Array<uint8> bytes;
        cook::detail::ByteWriter writer(bytes);
        writer.Append("ply\nformat binary_little_endian 1.0\nelement vertex 4\n"
                      "property float x\nproperty float y\nproperty float z\n");
        if (hasAuthoredNormals == true)
        {
            writer.Append("property float nx\nproperty float ny\nproperty float nz\n");
        }
        writer.Append("element face 2\nproperty list uchar int vertex_indices\nend_header\n");
        constexpr usize VERTEX_COUNT = 4;
        constexpr StaticArray<math::Vector3, VERTEX_COUNT> POSITIONS{{{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}}};
        for (const math::Vector3 positionMesh : POSITIONS)
        {
            writer.AppendFloat32(static_cast<float64>(positionMesh.X));
            writer.AppendFloat32(static_cast<float64>(positionMesh.Y));
            writer.AppendFloat32(static_cast<float64>(positionMesh.Z));
            if (hasAuthoredNormals == true)
            {
                writer.AppendFloat32(0);
                writer.AppendFloat32(0);
                writer.AppendFloat32(-1);
            }
        }
        constexpr usize INDEX_COUNT = 6;
        constexpr StaticArray<uint32, INDEX_COUNT> INDICES{{0, 1, 2, 0, 2, 3}};
        for (usize triangle = 0; triangle < 2; ++triangle)
        {
            writer.Append(static_cast<char>(format::TRIANGLE_CORNERS));
            for (usize corner = 0; corner < format::TRIANGLE_CORNERS; ++corner)
            {
                writer.AppendUint32(INDICES[triangle * format::TRIANGLE_CORNERS + corner]);
            }
        }
        REQUIRE(writer.IsOk() == true);
        std::ofstream file(Source / "models/oriented_light.ply", std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.GetData()), static_cast<std::streamsize>(bytes.GetSize()));
        REQUIRE(file.good() == true);
    }
};
} // namespace

TEST_CASE("PBRT one-sided emitters preserve authored front faces", "[scene_assets][emitters]")
{
    EmitterFixture fixture;
    for (const bool isPly : {false, true})
    {
        for (const bool shouldReflect : {false, true})
        {
            // PLY rectangles use the frozen identity-transform profile. Triangle
            // quads also exercise a reflected, nonuniform/sheared source transform.
            if (isPly == true && shouldReflect == true)
            {
                continue;
            }
            for (const bool hasAuthoredNormals : {false, true})
            {
                CAPTURE(isPly, shouldReflect, hasAuthoredNormals);
                fixture.WritePly(hasAuthoredNormals);
                const std::string transform = shouldReflect == true
                                                  ? "Transform [ -2 0 0 0 0.5 1 0 0 0 0 3 0 4 0 0 1 ]\n"
                                                  : "Transform [ 1 0 0 0 0 1 0 0 0 0 1 0 4 0 0 1 ]\n";
                const std::string shape =
                    isPly == true
                        ? "Shape \"plymesh\" \"string filename\" [ \"models/oriented_light.ply\" ]\n"
                        : std::string("Shape \"trianglemesh\" \"point3 P\" [ -1 -1 0 1 -1 0 1 1 0 -1 1 0 ]\n") +
                              (hasAuthoredNormals == true ? "\"normal N\" [ 0 0 -1 0 0 -1 0 0 -1 0 0 -1 ]\n"
                                                          : "\"normal N\" []\n") +
                              "\"point2 uv\" [ 0 0 1 0 1 1 0 1 ] \"integer indices\" [ 0 1 2 0 2 3 ]\n";
                fixture.WriteScene("AttributeBegin\nAreaLightSource \"diffuse\" \"rgb L\" [ 7 6 5 ]\n"
                                   "Material \"diffuse\" \"rgb reflectance\" [ 0 0 0 ]\n" +
                                   (isPly == true ? std::string{} : transform) + shape + "AttributeEnd\n");
                cook::CookResult result;
                cook::CookError error;
                const bool isCooked = cook::CookMaterials(fixture.Source.string(), result, error);
                INFO(error.Message.View());
                REQUIRE(isCooked == true);
                resources::SceneDefinition scene;
                REQUIRE(resources::SceneDefinition::Decode(result.Bundle.GetData(), result.Bundle.GetSize(), scene) ==
                        resources::Status::Ok);
                if (isPly == false && shouldReflect == false && hasAuthoredNormals == true)
                {
                    // Exercise the Editor's project import/publication path as well
                    // as the direct cooker. Assembly must retain the corrected side.
                    const std::filesystem::path project = fixture.Source / "project";
                    std::filesystem::create_directory(project);
                    cook::AssetOptions options;
                    options.ShouldPreserveReferenceMaterials = true;
                    uint64 uuidCounter = 0;
                    cook::UuidSource uuids
                    {
                        .Generate =
                            [](void* contextOrNull,
                               uint8(&outBytes)[ludus::foundation::identity::UUID_BYTE_COUNT]) noexcept {
                                uint64* counterOrNull = static_cast<uint64*>(contextOrNull);
                                if (counterOrNull == nullptr)
                                {
                                    return;
                                }
                                const uint64 value = ++*counterOrNull;
                                for (usize byte = 0; byte < ludus::foundation::identity::UUID_BYTE_COUNT; ++byte)
                                {
                                    outBytes[byte] =
                                        byte < sizeof(value) ? 0 : static_cast<uint8>(value >> ((15 - byte) * 8));
                                }
                            },
                        .ContextOrNull = &uuidCounter,
                    };
                    options.UuidsOrNull = &uuids;
                    cook::AssetError assetError;
                    Array<uint8> empty;
                    Array<uint8> imported;
                    Array<uint8> assembled;
                    REQUIRE(cook::CreateLevel("Emitters", options, empty, assetError) == true);
                    const bool isImported = cook::ImportScene(project.string(),
                                                              fixture.Source.string(),
                                                              std::span<const uint8>(empty.GetData(), empty.GetSize()),
                                                              options,
                                                              imported,
                                                              assetError);
                    INFO(assetError.Message.View());
                    REQUIRE(isImported == true);
                    REQUIRE(cook::AssembleLevel(project.string(),
                                                std::span<const uint8>(imported.GetData(), imported.GetSize()),
                                                {},
                                                assembled,
                                                assetError) == true);
                    REQUIRE(resources::SceneDefinition::Decode(assembled.GetData(), assembled.GetSize(), scene) ==
                            resources::Status::Ok);
                }
                usize emitterCount = 0;
                for (const resources::SceneObject& object : scene.Objects)
                {
                    const resources::SceneMaterial& material = scene.Materials[object.Material];
                    if (material.Emission.X != 7)
                    {
                        continue;
                    }
                    ++emitterCount;
                    CHECK(material.IsEmissionTwoSided == false);
                    CHECK(material.Emission.Y == 6);
                    CHECK(material.Emission.Z == 5);
                    const resources::SceneMesh& mesh = scene.Meshes[object.Mesh];
                    const math::Vector3 firstWorld =
                        math::TransformPoint(object.Transform, mesh.Positions[mesh.Indices[0]]);
                    const math::Vector3 secondWorld =
                        math::TransformPoint(object.Transform, mesh.Positions[mesh.Indices[1]]);
                    const math::Vector3 thirdWorld =
                        math::TransformPoint(object.Transform, mesh.Positions[mesh.Indices[2]]);
                    const math::Vector3 areaWorld = math::Cross(secondWorld - firstWorld, thirdWorld - firstWorld);
                    CHECK(areaWorld.Z * (hasAuthoredNormals == true ? -1 : 1) > 0);
                    CHECK(math::Abs(areaWorld.Z) == (shouldReflect == true ? 8 : 4));
                }
                CHECK(emitterCount == 1);
            }
        }
    }
}

TEST_CASE("Ambiguous emitter normals reject without replacing cooked outputs", "[scene_assets][emitters]")
{
    EmitterFixture fixture;
    cook::CookResult result;
    cook::CookError error;
    REQUIRE(cook::CookMaterials(fixture.Source.string(), result, error) == true);
    const std::string preserved(reinterpret_cast<const char*>(result.Bundle.GetData()), result.Bundle.GetSize());
    for (const std::string_view normals : {"0 0 -1 0 0 -1 0 0 1 0 0 1", "1 0 0 1 0 0 1 0 0 1 0 0"})
    {
        fixture.WriteScene("AttributeBegin\nAreaLightSource \"diffuse\" \"rgb L\" [ 7 6 5 ]\n"
                           "Material \"diffuse\" \"rgb reflectance\" [ 0 0 0 ]\n"
                           "Shape \"trianglemesh\" \"point3 P\" [ -1 -1 0 1 -1 0 1 1 0 -1 1 0 ]\n"
                           "\"normal N\" [ " +
                           std::string(normals) +
                           " ]\n"
                           "\"point2 uv\" [ 0 0 1 0 1 1 0 1 ] \"integer indices\" [ 0 1 2 0 2 3 ]\n"
                           "AttributeEnd\n");
        CHECK(cook::CookMaterials(fixture.Source.string(), result, error) == false);
        CHECK(error.Result == ludus::content::Status::Invalid);
        CHECK(error.Message.View() == "emitter normals do not define a consistent emitting side");
        CHECK(std::string(reinterpret_cast<const char*>(result.Bundle.GetData()), result.Bundle.GetSize()) ==
              preserved);
    }
}
