#include "checker_test_support.hpp"
#include "preprocessor_test_support.hpp"

#include <ludus/tooling/shader_language/formatter.hpp>
#include <ludus/tooling/shader_language/pipeline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace
{
namespace language = ludus::tooling::shader_language;
using language::DiagnosticCode;
using language::test::CheckedSource;
using language::test::Snapshot;

/// Interfaces, a conforming struct, an extension and templates, as the design shows them.
constexpr std::string_view MATERIALS = R"(interface Material
{
    static const uint32 LOBE_COUNT;
    float32x3 Evaluate(in float32x3 normalWorld, in float32x3 lightWorld);
};
interface ClearcoatMaterial : Material
{
    float32x3 EvaluateClearcoat(in float32x3 normalWorld);
};
struct CarPaint : ClearcoatMaterial
{
    static const uint32 LOBE_COUNT = 3;
    float32x3 Albedo;
    float32x3 Evaluate(in float32x3 normalWorld, in float32x3 lightWorld)
    {
        return Albedo * dot(normalWorld, lightWorld) * float32(LOBE_COUNT);
    }
    float32x3 EvaluateClearcoat(in float32x3 normalWorld)
    {
        return normalWorld;
    }
};
struct Plastic
{
    float32 Roughness;
};
extension Plastic : Material
{
    static const uint32 LOBE_COUNT = 1;
    float32x3 Evaluate(in float32x3 normalWorld, in float32x3 lightWorld)
    {
        return float32x3(Roughness);
    }
};
template <Material M>
float32x3 Shade(in M material, in float32x3 normalWorld)
{
    return material.Evaluate(normalWorld, lightWorld = normalWorld) * float32(M::LOBE_COUNT);
}
template <ClearcoatMaterial M>
float32x3 ShadeClearcoat(in M material, in float32x3 normalWorld)
{
    return Shade(material, normalWorld) + material.EvaluateClearcoat(normalWorld);
}
)";

std::string Lower(std::string text)
{
    Snapshot snapshot;
    snapshot.AddFile("main.lusl", std::move(text));
    (void)snapshot.Run();
    language::LoweringResult result;
    const language::Status status = language::LowerToSlang(snapshot.GetRequest(), language::LoweringOptions{}, result);
    std::string diagnostics;
    for (const language::Diagnostic& diagnostic : result.Diagnostics)
    {
        diagnostics += diagnostic.Message.GetView();
        diagnostics += '\n';
    }
    INFO(diagnostics);
    REQUIRE(status == language::Status::Ready);
    return std::string(result.Text.GetData(), result.Text.GetSize());
}

bool Contains(const std::string& text, std::string_view part)
{
    return text.find(part) != std::string::npos;
}
} // namespace

TEST_CASE("LUSL interfaces record conformances and template arguments", "[shader-language][interfaces]")
{
    const CheckedSource checked(std::string(MATERIALS) + R"(float32x3 Use(in CarPaint paint, in Plastic plastic)
{
    return Shade(material = paint, normalWorld = float32x3(1.0)) + Shade(plastic, float32x3(0.0)) +
           ShadeClearcoat(paint, float32x3(1.0));
}
)");
    INFO(checked.DescribeDiagnostics());
    REQUIRE(checked.GetStatus() == language::Status::Ready);
    const language::SemanticModel& model = checked.GetModel();
    // CarPaint : ClearcoatMaterial, ClearcoatMaterial : Material, Plastic : Material by extension,
    // and one constraint per template parameter.
    REQUIRE(model.Conformances.GetSize() == 5);
    ludus::foundation::usize templates = 0;
    for (const language::Symbol& symbol : model.Symbols)
    {
        templates += symbol.Kind == language::SymbolKind::Function && symbol.TypeParameterCount == 1 ? 1 : 0;
    }
    CHECK(templates == 2);
    // Four template calls: three in Use and the nested Shade call in ShadeClearcoat.
    CHECK(model.TypeArguments.GetSize() == 4);
}

TEST_CASE("LUSL interfaces lower to Slang interfaces, extensions and generics", "[shader-language][interfaces]")
{
    const std::string text = Lower(std::string(MATERIALS) + R"(float32x3 Use(in CarPaint paint, in Plastic plastic)
{
    return Shade(material = paint, normalWorld = float32x3(1.0)) + Shade(plastic, float32x3(0.0)) +
           ShadeClearcoat(paint, float32x3(1.0));
}
)");
    CHECK(Contains(text,
                   "interface Material\n{\n    static const uint LOBE_COUNT;\n"
                   "    float3 Evaluate(float3 normalWorld, float3 lightWorld);\n};"));
    CHECK(Contains(text, "interface ClearcoatMaterial : Material\n"));
    CHECK(Contains(
        text,
        "struct CarPaint : ClearcoatMaterial\n{\n    static const uint LOBE_COUNT = 3u;\n    float3 Albedo;\n"));
    CHECK(Contains(text, "extension Plastic : Material\n{\n    static const uint LOBE_COUNT = 1u;\n"));
    CHECK(Contains(text, "return float3(this.Roughness);"));
    CHECK(Contains(text, "float3 Shade<M : Material>(M material, float3 normalWorld)"));
    CHECK(Contains(text, "return material.Evaluate(normalWorld, normalWorld) * float(M.LOBE_COUNT);"));
    CHECK(Contains(text, "float3 ShadeClearcoat<M : ClearcoatMaterial>(M material, float3 normalWorld)"));
    CHECK(Contains(text, "return Shade<M>(material, normalWorld) + material.EvaluateClearcoat(normalWorld);"));
    CHECK(Contains(text, "Shade<CarPaint>(paint, float3(1.0f))"));
    CHECK(Contains(text, "Shade<Plastic>(plastic, float3(0.0f))"));
    CHECK(Contains(text, "ShadeClearcoat<CarPaint>(paint, float3(1.0f))"));
}

TEST_CASE("LUSL checks interface conformance", "[shader-language][interfaces]")
{
    const CheckedSource missing(R"(interface Material
{
    static const uint32 LOBE_COUNT;
    float32x3 Evaluate(in float32x3 normalWorld);
};
struct Paint : Material
{
    float32x3 Evaluate(in float32x3 normalWorld, in float32 scale)
    {
        return normalWorld * scale;
    }
};
)");
    // The method takes other parameters and the constant is absent.
    CHECK(missing.CountDiagnostics(DiagnosticCode::MissingRequirement) == 2);

    const CheckedSource body(R"(interface Material
{
    float32 Evaluate()
    {
        return 1.0;
    }
};
)");
    CHECK(body.FindDiagnosticOrNull(DiagnosticCode::InvalidInitializer) != nullptr);

    const CheckedSource notInterface(
        "struct Base\n{\n    float32 X;\n};\nstruct Derived : Base\n{\n    float32 Y;\n};\n");
    CHECK(notInterface.FindDiagnosticOrNull(DiagnosticCode::TypeMismatch) != nullptr);
}

TEST_CASE("LUSL template rules", "[shader-language][interfaces]")
{
    const std::string material = "interface Material\n{\n    float32 Evaluate();\n};\n"
                                 "struct Stone\n{\n    float32 Grain;\n};\n";
    const CheckedSource notConforming(material + R"(template <Material M>
float32 Shade(in M material)
{
    return material.Evaluate();
}
float32 Use(in Stone stone)
{
    return Shade(stone);
}
)");
    CHECK(notConforming.FindDiagnosticOrNull(DiagnosticCode::TemplateDeduction) != nullptr);

    const CheckedSource undeducible(material + R"(template <Material M>
float32 Count()
{
    return 1.0;
}
)");
    CHECK(undeducible.FindDiagnosticOrNull(DiagnosticCode::TemplateDeduction) != nullptr);

    const CheckedSource interfaceValue(material + "float32 Use(in Material material)\n{\n    return 1.0;\n}\n");
    CHECK(interfaceValue.FindDiagnosticOrNull(DiagnosticCode::TypeMismatch) != nullptr);

    const CheckedSource zero(material + R"(template <Material M>
float32 Reset(in M material)
{
    M copy = 0;
    return copy.Evaluate();
}
)");
    CHECK(zero.FindDiagnosticOrNull(DiagnosticCode::InvalidZeroInitializer) != nullptr);

    const CheckedSource field(material + R"(template <Material M>
float32 Read(in M material)
{
    return material.Grain;
}
)");
    CHECK(field.FindDiagnosticOrNull(DiagnosticCode::UnknownMember) != nullptr);

    const CheckedSource constant(material + R"(template <Material M>
float32 Read(in M material)
{
    return float32(M::LOBE_COUNT);
}
)");
    CHECK(constant.FindDiagnosticOrNull(DiagnosticCode::UnknownMember) != nullptr);

    const CheckedSource entry(material + R"(template <Material M>
[stage(compute), threads(1, 1, 1)]
void Main(in uint32x3 id : DispatchThreadId, in M material)
{
}
)");
    CHECK(entry.FindDiagnosticOrNull(DiagnosticCode::InvalidAttribute) != nullptr);

    const CheckedSource extensionOfScalar(material + "extension float32 : Material\n{\n};\n");
    CHECK(extensionOfScalar.FindDiagnosticOrNull(DiagnosticCode::TypeMismatch) != nullptr);
}

TEST_CASE("LUSL prefers a function to a template at equal cost", "[shader-language][interfaces]")
{
    const std::string text = Lower(R"(interface Material
{
    float32 Evaluate();
};
struct Stone : Material
{
    float32 Grain;
    float32 Evaluate()
    {
        return Grain;
    }
};
template <Material M>
float32 Shade(in M material)
{
    return material.Evaluate();
}
float32 Shade(in Stone stone)
{
    return stone.Grain * 2.0;
}
float32 Use(in Stone stone)
{
    return Shade(stone);
}
)");
    CHECK(Contains(text, "return Shade(stone);"));
}

TEST_CASE("LUSL formats template headers as their own line", "[shader-language][interfaces]")
{
    language::FormatResult result;
    REQUIRE(language::FormatSource("template<Material M>\nfloat32 Shade(in M material)\n{\n    return 1.0;\n}\n",
                                   result) == language::Status::Ready);
    CHECK(std::string(result.Text.GetData(), result.Text.GetSize()) ==
          "template <Material M>\nfloat32 Shade(in M material)\n{\n    return 1.0;\n}\n");
}

TEST_CASE("LUSL extern slang variables are read-only uniforms", "[shader-language][interfaces]")
{
    const CheckedSource checked(R"(extern slang
{
    struct Material
    {
        float32x3 Tint;
        float32 Gain;
    };
    Material material;
};
void Scale(inout float32 value)
{
    value = value * 2.0;
}
float32 Read()
{
    Material copy = material;
    copy.Gain = 2.0;
    return copy.Gain * material.Gain;
}
void Write()
{
    material.Gain = 2.0;
    material = 0;
    Scale(inout material.Gain);
}
)");
    // Reads and copies are fine; assignments and inout arguments are not.
    CHECK(checked.CountDiagnostics(DiagnosticCode::NotAssignable) == 3);
    const language::Diagnostic* writeOrNull = checked.FindDiagnosticOrNull(DiagnosticCode::NotAssignable);
    REQUIRE(writeOrNull != nullptr);
    CHECK(std::string_view(writeOrNull->Message.GetView()).find("copy it into a local variable") !=
          std::string_view::npos);
}
