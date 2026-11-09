#include <ludus/content/scene_source/scene_source.h>

#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace
{
using ludus::content::scene_source::ApertureQuads;
using ludus::content::scene_source::ApertureShape;
using ludus::content::scene_source::BuildApertureQuads;
using ludus::content::scene_source::IsApertureShapeValid;
using ludus::content::scene_source::IsSceneSourcePrefix;
using ludus::content::scene_source::ParseSceneSource;
using ludus::content::scene_source::SceneSource;
using ludus::content::scene_source::SceneSourceError;
using ludus::content::scene_source::SceneSourceStatus;
using ludus::content::scene_source::SourceMaterialModel;
using ludus::content::scene_source::SourceObject;
using ludus::foundation::float64;
using ludus::foundation::uint32;
using ludus::foundation::usize;
namespace math = ludus::foundation::math;

constexpr std::string_view MATERIALS = R"("materials": [
    {"name": "Wall", "model": "diffuse", "base_color": [0.8, 0.5, 0.25]},
    {"name": "Light", "model": "metallic_roughness", "base_color": [0, 0, 0], "metallic": 0.25, "roughness": 0.5,
     "emission": {"radiance": [6, 5, 4], "two_sided": true}}
  ])";

/// A valid two-object document; `objects` replaces the object list.
std::string MakeDocument(std::string_view objects)
{
    std::string text = R"({
  "format": "ludus.scene",
  "version": 1,
  "name": "Test room",
  "description": "A wall and a light.",
  "camera": {"eye": [0, 0, 3.4], "target": [0, 0, -1], "up": [0, 1, 0], "vertical_fov_degrees": 45},
  )";
    text += MATERIALS;
    text += ",\n  \"objects\": ";
    text += objects;
    text += "\n}\n";
    return text;
}

constexpr std::string_view VALID_OBJECTS = R"([
    {"name": "Wall", "material": "Wall", "positions": [[-1, -1, -1], [1, -1, -1], [1, 1, -1], [-1, 1, -1]],
     "polygons": [[0, 1, 2, 3]]},
    {"name": "Light", "material": "Light",
     "positions": [[-0.5, 1, -0.5], [0.5, 1, -0.5], [0.5, 1, 0.5], [0, 1, 0.75], [-0.5, 1, 0.5]],
     "polygons": [[0, 4, 3, 2, 1]]}
  ])";

/// Parse text that must fail with `expected`, and check that the output scene is untouched.
void RequireFailure(const std::string& text, SceneSourceStatus expected, std::string_view messagePart)
{
    SceneSource scene;
    REQUIRE(scene.Name.TryAssign("unchanged") == ludus::foundation::StringStatus::Ok);
    SceneSourceError error;
    const SceneSourceStatus status = ParseSceneSource(text, scene, error);
    INFO(error.Message.View());
    CHECK(status == expected);
    CHECK(error.Status == expected);
    CHECK(error.Message.View().find(messagePart) != std::string_view::npos);
    CHECK(scene.Name.GetView() == "unchanged");
    CHECK(scene.Materials.IsEmpty() == true);
}

/// Replace the first occurrence of `from` in the valid document.
std::string ReplaceInValid(std::string_view from, std::string_view to)
{
    std::string text = MakeDocument(VALID_OBJECTS);
    const usize at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}
} // namespace

TEST_CASE("Scene source reads materials, objects and the camera", "[scene-source]")
{
    const std::string text = MakeDocument(VALID_OBJECTS);
    SceneSource scene;
    SceneSourceError error;
    REQUIRE(ParseSceneSource(text, scene, error) == SceneSourceStatus::Success);
    CHECK(scene.Name.GetView() == "Test room");
    CHECK(scene.Description.GetView() == "A wall and a light.");

    REQUIRE(scene.Materials.GetSize() == 2);
    CHECK(scene.Materials[0].Name.GetView() == "Wall");
    CHECK(scene.Materials[0].Model == SourceMaterialModel::Diffuse);
    CHECK(scene.Materials[0].BaseColor == math::Vector3d{0.8, 0.5, 0.25});
    CHECK(scene.Materials[0].Metallic == 0.0);
    CHECK(scene.Materials[0].Roughness == 1.0);
    CHECK(scene.Materials[0].EmittedRadiance == math::Vector3d{});
    CHECK(scene.Materials[1].Model == SourceMaterialModel::MetallicRoughness);
    CHECK(scene.Materials[1].Metallic == 0.25);
    CHECK(scene.Materials[1].Roughness == 0.5);
    CHECK(scene.Materials[1].EmittedRadiance == math::Vector3d{6, 5, 4});
    CHECK(scene.Materials[1].IsEmissionTwoSided == true);

    REQUIRE(scene.Objects.GetSize() == 2);
    CHECK(scene.Objects[0].Name.GetView() == "Wall");
    CHECK(scene.Objects[0].Material == 0);
    REQUIRE(scene.Objects[0].PositionsWorld.GetSize() == 4);
    CHECK(scene.Objects[0].PositionsWorld[2] == math::Vector3d{1, 1, -1});
    REQUIRE(scene.Objects[0].PolygonVertexCounts.GetSize() == 1);
    CHECK(scene.Objects[0].PolygonVertexCounts[0] == 4);
    CHECK(scene.Objects[1].Material == 1);
    REQUIRE(scene.Objects[1].PolygonIndices.GetSize() == 5);
    CHECK(scene.Objects[1].PolygonIndices[1] == uint32{4});

    CHECK(scene.Camera.IsPresent == true);
    CHECK(scene.Camera.EyeWorld == math::Vector3d{0, 0, 3.4});
    CHECK(scene.Camera.TargetWorld == math::Vector3d{0, 0, -1});
    CHECK(scene.Camera.VerticalFovRadians == math::DegreesToRadians(45.0));
}

TEST_CASE("Scene source Mix requires a weight and valid rough conductor parameters", "[scene-source][mix]")
{
    constexpr std::string_view MODEL = R"("model": "diffuse")";
    RequireFailure(ReplaceInValid(MODEL, R"("model":"mix","conductor":{"alpha":0.1,"eta":[1,1,1],"k":[2,2,2]})"),
                   SceneSourceStatus::Malformed,
                   "weight");
    RequireFailure(
        ReplaceInValid(MODEL, R"("model":"mix","weight":1.1,"conductor":{"alpha":0.1,"eta":[1,1,1],"k":[2,2,2]})"),
        SceneSourceStatus::InvalidValue,
        "weight");
    RequireFailure(
        ReplaceInValid(MODEL, R"("model":"mix","weight":0.5,"conductor":{"alpha":0.1,"eta":[0,1,1],"k":[2,2,2]})"),
        SceneSourceStatus::InvalidValue,
        "conductor.eta");
}

TEST_CASE("Scene source checks the versioned project BRDF program", "[scene-source][project-brdf]")
{
    constexpr std::string_view MATERIAL = R"("name": "Wall", "model": "diffuse")";
    RequireFailure(ReplaceInValid(MATERIAL, R"("name":"demo/wall","model":"project_brdf","roughness":0.35,
                                  "brdf":{"version":2,"weights":[0.25,0.5,0.25],"powers":[0,4,12]})"),
                   SceneSourceStatus::InvalidValue,
                   "brdf.version");
    RequireFailure(ReplaceInValid(MATERIAL, R"("name":"demo/wall","model":"project_brdf","roughness":0.35,
                                  "brdf":{"version":1,"weights":[0,0,0],"powers":[0,4,12]})"),
                   SceneSourceStatus::InvalidValue,
                   "brdf.weights");
    RequireFailure(ReplaceInValid(MATERIAL, R"("name":"demo/wall","model":"project_brdf","roughness":0.35,
                                  "brdf":{"version":1,"weights":[0.25,0.5,0.25],"powers":[0,4,33]})"),
                   SceneSourceStatus::InvalidValue,
                   "brdf.powers");
}

TEST_CASE("Scene source compiles closure programs into bounded register code", "[scene-source][program]")
{
    namespace format = ludus::content::scene_format;
    constexpr std::string_view MATERIAL = R"("name": "Wall", "model": "diffuse")";
    // A rusted metal: the diffuse rust weight is one minus the conductor weight; an unused
    // parameter is dropped, and equal inline constants share one slot.
    const std::string text = ReplaceInValid(MATERIAL, R"("name":"Wall","model":"program","metallic":0.75,
        "roughness":0.4,"program":{"version":1,
          "parameters":[{"name":"metal","input":"metallic"},
                        {"name":"rust","one_minus":"metal"},
                        {"name":"unused","add":[1,2]},
                        {"name":"tint","multiply":["rust",[0.6,0.3,0.1]]},
                        {"name":"rough","input":"roughness"},
                        {"name":"sheen_weight","lerp":[0,0.25,"rust"]}],
          "lobes":[{"operation":"diffuse","weight":"tint"},
                   {"operation":"conductor","weight":"metal","roughness":"rough","eta":[0.2,0.9,1.1],
                    "k":[3.9,2.4,2.2]},
                   {"operation":"cosine_power","weight":"sheen_weight","roughness":"rough","power":8}]})");
    SceneSource scene;
    SceneSourceError error;
    const SceneSourceStatus status = ParseSceneSource(text, scene, error);
    INFO(error.Message.View());
    REQUIRE(status == SceneSourceStatus::Success);
    const ludus::content::scene_source::SourceMaterial& material = scene.Materials[0];
    CHECK(material.Model == SourceMaterialModel::Program);
    CHECK(material.Metallic == 0.75);
    CHECK(material.Roughness == 0.4);
    const format::MaterialProgram& program = material.Program;
    REQUIRE(format::IsMaterialProgramValid(program) == true);
    // metal, rust, tint constant, tint, rough, two lerp constants and the lerp; "unused" and its
    // constants are dropped.
    CHECK(program.InstructionCount == 8);
    CHECK(program.ConstantCount == 3);
    REQUIRE(program.LobeCount == 3);
    CHECK(program.Lobes[0].Operation == format::MaterialLobeOperation::Diffuse);
    CHECK(program.Lobes[0].RoughnessRegister == format::NO_MATERIAL_PROGRAM_REGISTER);
    CHECK(program.Lobes[1].Operation == format::MaterialLobeOperation::Conductor);
    CHECK(program.Lobes[1].K[0] == 3.9F);
    CHECK(program.Lobes[2].Operation == format::MaterialLobeOperation::CosinePower);
    CHECK(program.Lobes[2].Power == 8);
    CHECK(program.Lobes[1].RoughnessRegister == program.Lobes[2].RoughnessRegister);
    CHECK(format::GetMaterialProgramCapabilities(program) ==
          (format::LUDUS_MATERIAL_CAPABILITY_CONTINUOUS_REFLECTION |
           format::LUDUS_MATERIAL_CAPABILITY_FIXED_DIMENSION_SAMPLER));
    // No register is read before it is written, and each live value keeps its own register.
    for (uint32 index = 0; index < program.InstructionCount; ++index)
    {
        CHECK(program.Instructions[index].Destination < format::MAX_MATERIAL_PROGRAM_REGISTERS);
    }
}

TEST_CASE("Scene source rejects closure programs it cannot compile", "[scene-source][program]")
{
    constexpr std::string_view MATERIAL = R"("name": "Wall", "model": "diffuse")";
    const auto program = [MATERIAL](std::string_view body) {
        return ReplaceInValid(MATERIAL, std::string(R"("name":"P","model":"program","program":)") + std::string(body));
    };
    RequireFailure(program(R"({"version":2,"lobes":[{"operation":"diffuse","weight":1}]})"),
                   SceneSourceStatus::InvalidValue,
                   "program.version");
    RequireFailure(program(R"({"version":1,"lobes":[]})"), SceneSourceStatus::Malformed, "program.lobes");
    RequireFailure(program(R"({"version":1,"lobes":[{"operation":"glass","weight":1}]})"),
                   SceneSourceStatus::InvalidValue,
                   "program.lobes[0].operation");
    RequireFailure(program(R"({"version":1,"lobes":[{"operation":"conductor","weight":1,"eta":[1,1,1],
                               "k":[1,1,1]}]})"),
                   SceneSourceStatus::InvalidValue,
                   "program.lobes[0].roughness: is required");
    RequireFailure(program(R"({"version":1,"lobes":[{"operation":"diffuse","weight":1,"power":4}]})"),
                   SceneSourceStatus::InvalidValue,
                   "program.lobes[0].power: does not apply");
    RequireFailure(program(R"({"version":1,"lobes":[{"operation":"diffuse","weight":"missing"}]})"),
                   SceneSourceStatus::UnresolvedReference,
                   "program.lobes[0].weight: names no earlier parameter");
    RequireFailure(program(R"({"version":1,"parameters":[{"name":"a","add":["a",1]}],
                               "lobes":[{"operation":"diffuse","weight":"a"}]})"),
                   SceneSourceStatus::UnresolvedReference,
                   "program.parameters[0].add[0]");
    RequireFailure(program(R"({"version":1,"parameters":[{"name":"a","constant":1},{"name":"a","constant":2}],
                               "lobes":[{"operation":"diffuse","weight":"a"}]})"),
                   SceneSourceStatus::InvalidValue,
                   "program.parameters[1].name: repeats");
    RequireFailure(program(R"({"version":1,"parameters":[{"name":"a","constant":1,"input":"metallic"}],
                               "lobes":[{"operation":"diffuse","weight":"a"}]})"),
                   SceneSourceStatus::InvalidValue,
                   "define exactly one of");
    RequireFailure(program(R"({"version":1,"parameters":[{"name":"a","input":"opacity"}],
                               "lobes":[{"operation":"diffuse","weight":"a"}]})"),
                   SceneSourceStatus::InvalidValue,
                   "program.parameters[0].input");
    RequireFailure(program(R"({"version":1,"parameters":[{"name":"a","lerp":[0,1]}],
                               "lobes":[{"operation":"diffuse","weight":"a"}]})"),
                   SceneSourceStatus::Malformed,
                   "three operands");
    RequireFailure(program(R"({"version":1,"lobes":[{"operation":"diffuse","weight":20000}]})"),
                   SceneSourceStatus::InvalidValue,
                   "program.lobes[0].weight");
    RequireFailure(ReplaceInValid(MATERIAL, R"("name":"P","model":"diffuse","program":{"version":1})"),
                   SceneSourceStatus::InvalidValue,
                   "program requires the program model");
    RequireFailure(ReplaceInValid(MATERIAL, R"("name":"P","model":"program")"),
                   SceneSourceStatus::Malformed,
                   "program: missing member");

    // Nine lobes exceed the lobe limit.
    std::string lobes;
    for (usize i = 0; i < 9; ++i)
    {
        lobes += i == 0 ? "" : ",";
        lobes += R"({"operation":"diffuse","weight":1})";
    }
    RequireFailure(program(R"({"version":1,"lobes":[)" + lobes + "]}"), SceneSourceStatus::Limit, "at most 8 lobes");
    // Eight lobe weights from a chain of sums, and one roughness read at the end, keep nine
    // values live from two constants.
    std::string chain = R"({"name":"r","constant":0.5},{"name":"x0","constant":0.1})";
    lobes.clear();
    for (usize i = 1; i <= 8; ++i)
    {
        const std::string name = "x" + std::to_string(i);
        chain += R"(,{"name":")" + name + R"(","add":["x)" + std::to_string(i - 1) + R"(","x0"]})";
        lobes += i == 1 ? "" : ",";
        lobes +=
            i < 8 ? R"({"operation":"diffuse","weight":")" + name + "\"}"
                  : R"({"operation":"conductor","weight":")" + name + R"(","roughness":"r","eta":[1,1,1],"k":[1,1,1]})";
    }
    RequireFailure(program(R"({"version":1,"parameters":[)" + chain + R"(],"lobes":[)" + lobes + "]}"),
                   SceneSourceStatus::Limit,
                   "live at once");
    // A chain of sums keeps few values live but loads nine distinct constants.
    std::string parameters = R"({"name":"s0","constant":1})";
    for (usize i = 1; i < 9; ++i)
    {
        parameters += R"(,{"name":"s)" + std::to_string(i) + R"(","add":["s)" + std::to_string(i - 1) + R"(",)" +
                      std::to_string(i + 1) + "]}";
    }
    RequireFailure(program(R"({"version":1,"parameters":[)" + parameters +
                           R"(],"lobes":[{"operation":"diffuse","weight":"s8"}]})"),
                   SceneSourceStatus::Limit,
                   "distinct constants");
}

TEST_CASE("Scene source camera, description and emission are optional", "[scene-source]")
{
    const std::string text = R"({"format": "ludus.scene", "version": 1, "name": "Bare",
        "materials": [{"name": "Grey", "model": "metallic_roughness", "base_color": [0.5, 0.5, 0.5]}],
        "objects": [{"name": "Triangle", "material": "Grey", "positions": [[0, 0, 0], [1, 0, 0], [0, 1, 0]],
                     "polygons": [[0, 1, 2]]}]})";
    SceneSource scene;
    SceneSourceError error;
    REQUIRE(ParseSceneSource(text, scene, error) == SceneSourceStatus::Success);
    CHECK(scene.Camera.IsPresent == false);
    CHECK(scene.Description.IsEmpty() == true);
    CHECK(scene.Materials[0].Metallic == 0.0);
    CHECK(scene.Materials[0].Roughness == 1.0);
    CHECK(scene.Materials[0].EmittedRadiance == math::Vector3d{});
    CHECK(scene.Materials[0].IsEmissionTwoSided == false);
}

TEST_CASE("Scene source rejects malformed envelopes and members", "[scene-source]")
{
    RequireFailure("{not json", SceneSourceStatus::Malformed, "not valid JSON");
    RequireFailure("[]", SceneSourceStatus::Malformed, "must be an object");
    RequireFailure(ReplaceInValid(R"("format": "ludus.scene",)", R"("format": "ludus.world",)"),
                   SceneSourceStatus::UnsupportedVersion,
                   "first member");
    RequireFailure(ReplaceInValid(R"("version": 1)", R"("version": 2)"), SceneSourceStatus::UnsupportedVersion, "1");
    // The format member must come first, so a prefix check classifies every valid file.
    RequireFailure(ReplaceInValid(R"("format": "ludus.scene",
  "version": 1,)",
                                  R"("version": 1,
  "format": "ludus.scene",)"),
                   SceneSourceStatus::UnsupportedVersion,
                   "first member");
    RequireFailure(ReplaceInValid(R"("name": "Test room",)", R"("name": "Test room", "units": "feet",)"),
                   SceneSourceStatus::Malformed,
                   "units: unknown member");
    RequireFailure(ReplaceInValid(R"("name": "Test room",)", ""), SceneSourceStatus::Malformed, "name: missing member");
    RequireFailure(ReplaceInValid(R"("name": "Test room",)", R"("name": "Test room", "name": "Again",)"),
                   SceneSourceStatus::Malformed,
                   "repeats a member");
    RequireFailure(ReplaceInValid(R"("name": "Test room",)", R"("name": "",)"),
                   SceneSourceStatus::InvalidValue,
                   "name: must not be empty");
}

TEST_CASE("Scene source rejects out-of-range materials and cameras", "[scene-source]")
{
    RequireFailure(ReplaceInValid("[0.8, 0.5, 0.25]", "[1.5, 0.5, 0.25]"),
                   SceneSourceStatus::InvalidValue,
                   "materials[0].base_color[0]: is out of range");
    RequireFailure(ReplaceInValid("[0.8, 0.5, 0.25]", "[0.8, 0.5]"),
                   SceneSourceStatus::Malformed,
                   "materials[0].base_color: must be an array of three numbers");
    RequireFailure(ReplaceInValid(R"("model": "diffuse",)", R"("model": "diffuse", "roughness": 0.5,)"),
                   SceneSourceStatus::Malformed,
                   "materials[0].roughness: applies to metallic_roughness only");
    RequireFailure(ReplaceInValid(R"("model": "diffuse")", R"("model": "phong")"),
                   SceneSourceStatus::InvalidValue,
                   "materials[0].model");
    RequireFailure(ReplaceInValid("[6, 5, 4]", "[6, -5, 4]"),
                   SceneSourceStatus::InvalidValue,
                   "materials[1].emission.radiance[1]: is out of range");
    RequireFailure(ReplaceInValid(R"("two_sided": true)", R"("two_sided": 1)"),
                   SceneSourceStatus::Malformed,
                   "materials[1].emission.two_sided: must be a boolean");
    RequireFailure(ReplaceInValid(R"({"name": "Light", "model")", R"({"name": "Wall", "model")"),
                   SceneSourceStatus::InvalidValue,
                   "materials[1].name: repeats a material name");
    RequireFailure(ReplaceInValid(R"("vertical_fov_degrees": 45)", R"("vertical_fov_degrees": 180)"),
                   SceneSourceStatus::InvalidValue,
                   "camera.vertical_fov_degrees");
    RequireFailure(ReplaceInValid(R"("target": [0, 0, -1])", R"("target": [0, 0, 3.4])"),
                   SceneSourceStatus::InvalidValue,
                   "camera.target: must differ from eye");
    RequireFailure(ReplaceInValid(R"("up": [0, 1, 0])", R"("up": [0, 0, 2])"),
                   SceneSourceStatus::InvalidValue,
                   "camera.up");
}

TEST_CASE("Scene source rejects unresolved references and invalid polygons", "[scene-source]")
{
    RequireFailure(ReplaceInValid(R"("material": "Wall")", R"("material": "Floor")"),
                   SceneSourceStatus::UnresolvedReference,
                   "objects[0].material: names no declared material");
    RequireFailure(ReplaceInValid(R"({"name": "Light", "material")", R"({"name": "Wall", "material")"),
                   SceneSourceStatus::InvalidValue,
                   "objects[1].name: repeats an object name");
    RequireFailure(ReplaceInValid("[[0, 1, 2, 3]]", "[[0, 1, 4]]"),
                   SceneSourceStatus::InvalidGeometry,
                   "objects[0].polygons[0][2]: names no position");
    RequireFailure(ReplaceInValid("[[0, 1, 2, 3]]", "[[0, 1]]"),
                   SceneSourceStatus::InvalidGeometry,
                   "objects[0].polygons[0]: must have 3 to 64 corners");
    RequireFailure(ReplaceInValid("[[0, 1, 2, 3]]", "[[0, 1, -2]]"),
                   SceneSourceStatus::Malformed,
                   "objects[0].polygons[0][2]: must be a nonnegative integer");
    // A repeated corner and a self-intersecting (bow-tie) order.
    for (const std::string_view polygon : {std::string_view("[[0, 1, 1, 2]]"), std::string_view("[[0, 2, 1, 3]]")})
    {
        INFO(polygon);
        RequireFailure(ReplaceInValid("[[0, 1, 2, 3]]", polygon),
                       SceneSourceStatus::InvalidGeometry,
                       "objects[0].polygons[0]: must be planar and strictly convex");
    }
    RequireFailure(ReplaceInValid("[-1, 1, -1]]", "[-1, 1, -0.9]]"),
                   SceneSourceStatus::InvalidGeometry,
                   "objects[0].polygons[0]: must be planar and strictly convex");
    // A reflex corner: the light's pentagon with its fourth corner pushed inside.
    RequireFailure(ReplaceInValid("[0, 1, 0.75]", "[0, 1, 0.25]"),
                   SceneSourceStatus::InvalidGeometry,
                   "objects[1].polygons[0]: must be planar and strictly convex");
    // Three collinear corners.
    RequireFailure(ReplaceInValid("[0, 1, 0.75]", "[0, 1, 0.5]"),
                   SceneSourceStatus::InvalidGeometry,
                   "objects[1].polygons[0]: must be planar and strictly convex");
    RequireFailure(ReplaceInValid("[-1, -1, -1], [1, -1, -1]", "[-1, -1, -1], [1e7, -1, -1]"),
                   SceneSourceStatus::InvalidValue,
                   "objects[0].positions[1][0]: is out of range");
}

constexpr std::string_view APERTURE_OBJECTS = R"([
    {"name": "Wall", "material": "Wall", "positions": [[-1, -1, -1], [1, -1, -1], [1, 1, -1], [-1, 1, -1]],
     "polygons": [[0, 1, 2, 3]]},
    {"name": "Slit", "material": "Wall",
     "aperture": {"center": [0, 0, 0], "normal": [0, 0, 1], "up": [0, 1, 0], "screen_width": 3,
                  "screen_height": 3, "slit_length": 0.8, "opening": 0.05}}
  ])";

/// Replace the first occurrence of `from` in the aperture document.
std::string ReplaceInAperture(std::string_view from, std::string_view to)
{
    std::string text = MakeDocument(APERTURE_OBJECTS);
    const usize at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

TEST_CASE("Scene source expands an aperture into the screen around its slit", "[scene-source][aperture]")
{
    SceneSource scene;
    SceneSourceError error;
    REQUIRE(ParseSceneSource(MakeDocument(APERTURE_OBJECTS), scene, error) == SceneSourceStatus::Success);
    CHECK(scene.Objects[0].Aperture.IsPresent == false);
    const SourceObject& slit = scene.Objects[1];
    REQUIRE(slit.Aperture.IsPresent == true);
    CHECK(slit.Aperture.CenterWorld == math::Vector3d{});
    CHECK(slit.Aperture.NormalWorld == math::Vector3d{0, 0, 1});
    CHECK(slit.Aperture.UpWorld == math::Vector3d{0, 1, 0});
    CHECK(slit.Aperture.Shape.ScreenWidth == 3.0);
    CHECK(slit.Aperture.Shape.SlitLength == 0.8);
    CHECK(slit.Aperture.Shape.Opening == 0.05);
    // Left, right, above and below the slit: the corners a hand-written screen lists, exactly.
    REQUIRE(slit.PolygonVertexCounts.GetSize() == 4);
    REQUIRE(slit.PositionsWorld.GetSize() == 16);
    const math::Vector3d expected[] = {
        {-1.5, -1.5, 0},
        {-0.025, -1.5, 0},
        {-0.025, 1.5, 0},
        {-1.5, 1.5, 0},
        {0.025, -1.5, 0},
        {1.5, -1.5, 0},
        {1.5, 1.5, 0},
        {0.025, 1.5, 0},
        {-0.025, 0.4, 0},
        {0.025, 0.4, 0},
        {0.025, 1.5, 0},
        {-0.025, 1.5, 0},
        {-0.025, -1.5, 0},
        {0.025, -1.5, 0},
        {0.025, -0.4, 0},
        {-0.025, -0.4, 0},
    };
    for (usize i = 0; i < slit.PositionsWorld.GetSize(); ++i)
    {
        INFO(i);
        CHECK(slit.PositionsWorld[slit.PolygonIndices[i]] == expected[i]);
    }
}

TEST_CASE("Scene source places an aperture in its own frame", "[scene-source][aperture]")
{
    // Facing +x with the slit along +z: across the slit is Cross(up, normal) = +y.
    const std::string text = ReplaceInAperture(R"("center": [0, 0, 0], "normal": [0, 0, 1], "up": [0, 1, 0])",
                                               R"("center": [1, 2, 3], "normal": [2, 0, 0], "up": [0, 0, 1])");
    SceneSource scene;
    SceneSourceError error;
    REQUIRE(ParseSceneSource(text, scene, error) == SceneSourceStatus::Success);
    const SourceObject& slit = scene.Objects[1];
    CHECK(slit.Aperture.NormalWorld == math::Vector3d{1, 0, 0});
    // The first corner of the left quad: x = -1.5 across, y = -1.5 along.
    CHECK(slit.PositionsWorld[0] == math::Vector3d{1, 0.5, 1.5});
}

TEST_CASE("Scene source aperture opening may close the slit", "[scene-source][aperture]")
{
    SceneSource scene;
    SceneSourceError error;
    REQUIRE(ParseSceneSource(ReplaceInAperture(R"("opening": 0.05)", R"("opening": 0)"), scene, error) ==
            SceneSourceStatus::Success);
    // Only the two halves remain; they meet at the closed slit.
    CHECK(scene.Objects[1].PolygonVertexCounts.GetSize() == 2);
    REQUIRE(ParseSceneSource(ReplaceInAperture(R"("slit_length": 0.8)", R"("slit_length": 3)"), scene, error) ==
            SceneSourceStatus::Success);
    CHECK(scene.Objects[1].PolygonVertexCounts.GetSize() == 2);
}

TEST_CASE("Scene source rejects invalid apertures", "[scene-source][aperture]")
{
    RequireFailure(ReplaceInAperture(R"("opening": 0.05)", R"("opening": 3)"),
                   SceneSourceStatus::InvalidValue,
                   "objects[1].aperture.opening: must be at least 0 and less than screen_width");
    RequireFailure(ReplaceInAperture(R"("opening": 0.05)", R"("opening": -0.01)"),
                   SceneSourceStatus::InvalidValue,
                   "aperture.opening");
    RequireFailure(ReplaceInAperture(R"("slit_length": 0.8)", R"("slit_length": 4)"),
                   SceneSourceStatus::InvalidValue,
                   "objects[1].aperture.slit_length: must be positive and at most screen_height");
    RequireFailure(ReplaceInAperture(R"("screen_width": 3)", R"("screen_width": 0)"),
                   SceneSourceStatus::InvalidValue,
                   "objects[1].aperture.screen_width: is out of range");
    // The camera has an "up" too; replace the aperture's, which follows its normal.
    RequireFailure(
        ReplaceInAperture(R"("normal": [0, 0, 1], "up": [0, 1, 0])", R"("normal": [0, 0, 1], "up": [0, 1, 0.5])"),
        SceneSourceStatus::InvalidValue,
        "objects[1].aperture.up: must be perpendicular to the normal");
    RequireFailure(ReplaceInAperture(R"("normal": [0, 0, 1])", R"("normal": [0, 0, 0])"),
                   SceneSourceStatus::InvalidValue,
                   "objects[1].aperture.normal: must not be zero");
    RequireFailure(ReplaceInAperture(R"("center": [0, 0, 0])", R"("center": [999999, 0, 0])"),
                   SceneSourceStatus::InvalidValue,
                   "beyond the coordinate limit");
    RequireFailure(ReplaceInAperture(R"("opening": 0.05)", R"("opening": 0.05, "thickness": 0.01)"),
                   SceneSourceStatus::Malformed,
                   "objects[1].aperture.thickness: unknown member");
    RequireFailure(ReplaceInAperture(R"("material": "Wall",
     "aperture")",
                                     R"("material": "Wall", "polygons": [[0, 1, 2]],
     "aperture")"),
                   SceneSourceStatus::Malformed,
                   "an aperture object has no positions or polygons");
    RequireFailure(ReplaceInValid(R"("polygons": [[0, 1, 2, 3]])", R"("unused": 0)"),
                   SceneSourceStatus::Malformed,
                   "unknown member");
    RequireFailure(ReplaceInValid(R"(,
     "polygons": [[0, 1, 2, 3]])",
                                  ""),
                   SceneSourceStatus::Malformed,
                   "objects[0].polygons: missing member");
}

TEST_CASE("Aperture quads skip the parts a slit leaves without area", "[scene-source][aperture]")
{
    ApertureQuads quads;
    CHECK(BuildApertureQuads(
              {
                  .ScreenWidth = 1,
                  .ScreenHeight = 1,
                  .SlitLength = 0.5,
                  .Opening = 0.1,
              },
              quads) == true);
    CHECK(quads.Count == 4);
    CHECK(quads.Quads[2][0] == math::Vector3d{-0.05, 0.25, 0});
    CHECK(BuildApertureQuads({ .ScreenWidth = 1, .ScreenHeight = 1, .SlitLength = 1, .Opening = 0.1 }, quads) == true);
    CHECK(quads.Count == 2);
    const ApertureQuads before = quads;
    const ApertureShape invalid{ .ScreenWidth = 1, .ScreenHeight = 1, .SlitLength = 0.5, .Opening = 1 };
    CHECK(IsApertureShapeValid(invalid) == false);
    CHECK(BuildApertureQuads(invalid, quads) == false);
    CHECK(quads.Count == before.Count);
}

TEST_CASE("Scene source prefix detection needs the leading format member", "[scene-source]")
{
    CHECK(IsSceneSourcePrefix(MakeDocument(VALID_OBJECTS).substr(0, 40)) == true);
    CHECK(IsSceneSourcePrefix("{\"format\":\"ludus.scene\"") == true);
    CHECK(IsSceneSourcePrefix(" \n{ \"format\" :\t\"ludus.scene\", \"version\": 1") == true);
    CHECK(IsSceneSourcePrefix("{\"format\": \"ludus.scenery\"}") == false);
    CHECK(IsSceneSourcePrefix("{\"format\": \"ludus.scene") == false);
    CHECK(IsSceneSourcePrefix("{\"version\": 1, \"format\": \"ludus.scene\"}") == false);
    CHECK(IsSceneSourcePrefix("{\"asset\": {\"version\": \"2.0\"}}") == false);
    CHECK(IsSceneSourcePrefix("") == false);
}
