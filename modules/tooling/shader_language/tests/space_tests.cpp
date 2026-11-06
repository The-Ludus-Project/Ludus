#include "checker_test_support.hpp"
#include "preprocessor_test_support.hpp"

#include <ludus/tooling/shader_language/pipeline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace
{
namespace language = ludus::tooling::shader_language;
using language::DiagnosticCode;
using language::Status;
using language::SyntaxKind;
using language::test::CheckedSource;

/// Spaces, a camera and helpers shared by the tests.
constexpr std::string_view PRELUDE = R"(space Object;
space World;
space View;
space Clip;
struct Camera
{
    float32x4x4<World, View> WorldToView;
    float32x4x4<View, Clip> ViewToClip;
};
ConstantBuffer<Camera> camera;
struct Surface
{
    float32x3<World> NormalWorld;
    float32 Roughness;
};
float32 Shade(in float32x3<World> normalWorld, in float32x3<World> lightWorld)
{
    return dot(normalWorld, lightWorld);
}
)";

/// A function with `body` that may use the parameters below.
[[nodiscard]] std::string MakeFunction(std::string_view body)
{
    return std::string(PRELUDE) +
           "float32x4<Clip> Project(in float32x4x4<Object, World> objectToWorld, in float32x3<Object> positionObject, "
           "in float32x3<World> normalWorld, in float32x3<View> normalView, in float32x3 direction)\n{\n" +
           std::string(body) + "\n}\n";
}

void RequireValid(std::string_view body)
{
    const CheckedSource checked(MakeFunction(body));
    INFO(body << "\n" << checked.DescribeDiagnostics());
    CHECK(checked.GetStatus() == Status::Ready);
}

void RequireMismatch(std::string_view body)
{
    const CheckedSource checked(MakeFunction(body));
    INFO(body << "\n" << checked.DescribeDiagnostics());
    CHECK(checked.GetStatus() == Status::Failed);
    CHECK(checked.CountDiagnostics(DiagnosticCode::SpaceMismatch) == 1);
}

[[nodiscard]] std::string GetMessage(const CheckedSource& checked, DiagnosticCode code)
{
    const language::Diagnostic* diagnosticOrNull = checked.FindDiagnosticOrNull(code);
    REQUIRE(diagnosticOrNull != nullptr);
    return std::string(diagnosticOrNull->Message.GetView());
}

bool Contains(const std::string& text, std::string_view part)
{
    return text.find(part) != std::string::npos;
}
} // namespace

TEST_CASE("LUSL parses space declarations as a contextual keyword", "[shader-language][spaces]")
{
    const CheckedSource checked("space World;\nfloat32 Count()\n{\n    float32 space = 1.0;\n    return space;\n}\n");
    INFO(checked.DescribeDiagnostics());
    CHECK(checked.GetStatus() == Status::Ready);
    const language::SyntaxTree& tree = checked.GetTree();
    const ludus::foundation::uint32 first = tree.Nodes[0].FirstChild;
    REQUIRE(first != language::NO_NODE);
    CHECK(tree.Nodes[first].Kind == SyntaxKind::SpaceDeclaration);
    CHECK(checked.GetModel().Symbols[checked.GetModel().NodeSymbols[first]].Kind == language::SymbolKind::Space);
}

TEST_CASE("LUSL values carry the spaces their types name", "[shader-language][spaces]")
{
    // Matrices map vectors and compose; untagged values adopt the space of their context.
    RequireValid("    float32x4x4<World, Clip> worldToClip = camera.ViewToClip * camera.WorldToView;\n"
                 "    float32x4<World> positionWorld = objectToWorld * float32x4(positionObject, 1.0);\n"
                 "    float32x3<World> lightWorld = normalize(float32x3(0.0, 1.0, 0.0));\n"
                 "    float32x3<World> tangentWorld = cross(normalWorld, direction) * 2.0 + positionWorld.xyz;\n"
                 "    float32 light = Shade(normalWorld, lightWorld) + Shade(direction, tangentWorld);\n"
                 "    Surface surface = { .NormalWorld = normalWorld, .Roughness = light };\n"
                 "    float32x4x4<Clip, World> clipToWorld = transpose(worldToClip);\n"
                 "    return worldToClip * positionWorld + float32x4(surface.Roughness);");
}

TEST_CASE("LUSL rejects values of different spaces meeting", "[shader-language][spaces]")
{
    RequireMismatch("    float32x4<World> positionWorld = objectToWorld * float32x4(positionObject, 1.0);\n"
                    "    float32x4<Clip> positionClip = camera.WorldToView * positionWorld;\n    return positionClip;");
    RequireMismatch("    float32 light = Shade(normalView, normalWorld);\n    return float32x4(light);");
    RequireMismatch("    float32x3 sum = normalWorld + normalView;\n    return float32x4(sum, 1.0);");
    RequireMismatch("    float32 cosine = dot(normalWorld, normalView);\n    return float32x4(cosine);");
    RequireMismatch("    bool isSame = all(normalWorld == normalView);\n"
                    "    return float32x4(0.0);");
    RequireMismatch("    float32x3 chosen = normalWorld.x > 0.5 ? normalWorld : normalView;\n"
                    "    return float32x4(chosen, 1.0);");
    RequireMismatch(
        "    Surface surface = { .NormalWorld = normalView, .Roughness = 0.5 };\n    return float32x4(0.0);");
    RequireMismatch("    float32x3<World> target = normalWorld;\n    target = normalView;\n    return float32x4(0.0);");
    RequireMismatch("    return float32x4(normalView, 1.0);");
    // A composed matrix reports its mismatch once.
    RequireMismatch(
        "    float32x4x4<Object, Clip> chain = camera.ViewToClip * objectToWorld;\n    return float32x4(0.0);");
}

TEST_CASE("LUSL space errors name both spaces and the repair", "[shader-language][spaces]")
{
    const CheckedSource product(
        MakeFunction("    float32x4x4<World, Clip> worldToClip = camera.ViewToClip * camera.WorldToView;\n"
                     "    return worldToClip * float32x4(positionObject, 1.0);"));
    const std::string productMessage = GetMessage(product, DiagnosticCode::SpaceMismatch);
    INFO(productMessage);
    CHECK(Contains(productMessage, "'*' applies a matrix from World to Clip to a vector in Object") == true);
    CHECK(Contains(productMessage, "use a matrix from Object, or transform the operand to World first") == true);

    const CheckedSource argument(MakeFunction("    return float32x4(Shade(normalView, normalWorld));"));
    const std::string argumentMessage = GetMessage(argument, DiagnosticCode::SpaceMismatch);
    INFO(argumentMessage);
    CHECK(Contains(argumentMessage, "parameter 'normalWorld' is in World, but this value is in View") == true);
    CHECK(Contains(argumentMessage, "transform it with a matrix from View to World") == true);
}

TEST_CASE("LUSL rejects malformed spaces", "[shader-language][spaces]")
{
    const auto requireCode = [](std::string_view text, DiagnosticCode code) {
        const CheckedSource checked(std::string(PRELUDE) + std::string(text));
        INFO(text << "\n" << checked.DescribeDiagnostics());
        CHECK(checked.HasDiagnostic(code) == true);
    };
    requireCode("float32x3<Model> Lift(in float32x3 value)\n{\n    return value;\n}\n", DiagnosticCode::InvalidSpace);
    requireCode("float32x3<Surface> Lift(in float32x3 value)\n{\n    return value;\n}\n", DiagnosticCode::InvalidSpace);
    requireCode("float32x3<World, View> Lift(in float32x3 value)\n{\n    return value;\n}\n",
                DiagnosticCode::InvalidSpace);
    requireCode("float32x4x4<World> Lift(in float32x4x4 value)\n{\n    return value;\n}\n",
                DiagnosticCode::InvalidSpace);
    requireCode("uint32x3<World> Lift(in uint32x3 value)\n{\n    return value;\n}\n",
                DiagnosticCode::UnsupportedConstruct);
    requireCode("float32 Use()\n{\n    return World;\n}\n", DiagnosticCode::TypeMismatch);
    requireCode("space world;\n", DiagnosticCode::NamingStyle);
}

TEST_CASE("LUSL checks spaces in each specialization once", "[shader-language][spaces]")
{
    const CheckedSource checked(std::string(PRELUDE) + R"(interface Material
{
    static const uint32 LOBE_COUNT;
};
struct Plastic : Material
{
    static const uint32 LOBE_COUNT = 1;
};
struct CarPaint : Material
{
    static const uint32 LOBE_COUNT = 3;
};
concept Lobed<T> = (T : Material) && (T::LOBE_COUNT > 0);
template <typename T> requires Lobed<T> == true
float32 Layer(in T material, in float32x3<World> normalWorld, in float32x3<View> normalView)
{
    return dot(normalWorld, normalView) * float32(T::LOBE_COUNT);
}
float32 Combine(in float32x3<World> normalWorld, in float32x3<View> normalView)
{
    const Plastic plastic = 0;
    const CarPaint paint = 0;
    return Layer(plastic, normalWorld, normalView) + Layer(paint, normalWorld, normalView);
}
)");
    INFO(checked.DescribeDiagnostics());
    CHECK(checked.GetModel().Specializations.GetSize() == 2);
    CHECK(checked.CountDiagnostics(DiagnosticCode::SpaceMismatch) == 1);
}

TEST_CASE("LUSL lowering erases spaces", "[shader-language][spaces]")
{
    language::test::Snapshot snapshot;
    snapshot.AddFile("main.lusl",
                     "space World;\nfloat32x3<World> Lift(in float32x3<World> positionWorld)\n{\n"
                     "    return positionWorld;\n}\n");
    (void)snapshot.Run();
    language::LoweringResult result;
    const Status status = language::LowerToSlang(snapshot.GetRequest(), language::LoweringOptions{}, result);
    REQUIRE(status == Status::Ready);
    const std::string text(result.Text.GetData(), result.Text.GetSize());
    INFO(text);
    CHECK(Contains(text, "float3 Lift(float3 positionWorld)") == true);
    CHECK(Contains(text, "World>") == false);
    CHECK(Contains(text, "space") == false);
}
