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
using language::test::CheckedSource;
using language::test::Snapshot;

/// The design's materials: an interface hierarchy, two conforming structs and two concepts.
constexpr std::string_view MATERIALS = R"(interface Material
{
    static const uint32 LOBE_COUNT;
    float32x3 Evaluate(in float32x3 normalWorld);
};
interface ClearcoatMaterial : Material
{
    float32x3 EvaluateClearcoat(in float32x3 normalWorld);
};
struct CarPaint : ClearcoatMaterial
{
    static const uint32 LOBE_COUNT = 3;
    float32x3 Albedo;
    float32x3 Evaluate(in float32x3 normalWorld)
    {
        return Albedo;
    }
    float32x3 EvaluateClearcoat(in float32x3 normalWorld)
    {
        return normalWorld;
    }
};
struct Plastic : Material
{
    static const uint32 LOBE_COUNT = 1;
    float32 Roughness;
    float32x3 Evaluate(in float32x3 normalWorld)
    {
        return float32x3(Roughness);
    }
};
struct Stone
{
    float32 Grain;
};
concept Opaque<T> = (T : Material) && (T::LOBE_COUNT == 1);
concept Layered<T> = (T : ClearcoatMaterial) && (T::LOBE_COUNT > 1);
)";

std::string Lower(std::string_view tail)
{
    Snapshot snapshot;
    snapshot.AddFile("main.lusl", std::string(MATERIALS) + std::string(tail));
    (void)snapshot.Run();
    language::LoweringResult result;
    const language::Status status = language::LowerToSlang(snapshot.GetRequest(), language::LoweringOptions{}, result);
    std::string diagnostics;
    for (const language::Diagnostic& diagnostic : result.Diagnostics)
    {
        diagnostics += language::GetDiagnosticCodeName(diagnostic.Code);
        diagnostics += ": ";
        diagnostics += diagnostic.Message.GetView();
        diagnostics += '\n';
    }
    INFO(diagnostics);
    REQUIRE(status == language::Status::Ready);
    return std::string(result.Text.GetData(), result.Text.GetSize());
}

CheckedSource Check(std::string_view tail)
{
    return CheckedSource(std::string(MATERIALS) + std::string(tail));
}

bool Contains(const std::string& text, std::string_view part)
{
    return text.find(part) != std::string::npos;
}
} // namespace

TEST_CASE("LUSL concepts evaluate for concrete types", "[shader-language][concepts]")
{
    const CheckedSource passing = Check(R"(static_assert(Layered<CarPaint> == true, "CarPaint is layered");
static_assert(Opaque<CarPaint> == false, "CarPaint is not opaque");
static_assert(Opaque<Plastic> == true, "Plastic is opaque");
// A missing member makes its part false rather than an error.
static_assert(Opaque<Stone> == false, "Stone is no material");
static_assert((CarPaint : Material) == true, "conformance through a base interface");
)");
    INFO(passing.DescribeDiagnostics());
    CHECK(passing.GetStatus() == language::Status::Ready);
    const CheckedSource failing = Check("static_assert(Opaque<CarPaint> == true, \"wrong\");\n");
    CHECK(failing.FindDiagnosticOrNull(DiagnosticCode::StaticAssertFailed) != nullptr);
}

TEST_CASE("LUSL picks the most constrained template", "[shader-language][concepts]")
{
    const std::string text = Lower(R"(template <Material M>
float32x3 Shade(in M material)
{
    return material.Evaluate(float32x3(1.0));
}
template <Layered M>
float32x3 Shade(in M material)
{
    return material.Evaluate(float32x3(1.0)) + material.EvaluateClearcoat(float32x3(1.0));
}
float32x3 Use(in CarPaint paint, in Plastic plastic)
{
    return Shade(paint) + Shade(plastic);
}
)");
    // Layered subsumes Material through ClearcoatMaterial : Material, so CarPaint takes it.
    CHECK(Contains(text, "return _luslSpecialization0_Shade(paint) + _luslSpecialization1_Shade(plastic);"));
    CHECK(Contains(
        text,
        "float3 _luslSpecialization0_Shade(CarPaint material)\n{\n    return material.Evaluate(float3(1.0f)) + "
        "material.EvaluateClearcoat(float3(1.0f));"));
    CHECK(Contains(
        text,
        "float3 _luslSpecialization1_Shade(Plastic material)\n{\n    return material.Evaluate(float3(1.0f));"));
    // The template itself is not lowered; only its specializations are.
    CHECK(Contains(text, "Shade<") == false);
}

TEST_CASE("LUSL if constexpr checks only the branch it selects", "[shader-language][concepts]")
{
    const std::string text = Lower(R"(template <Material M>
float32x3 Shade(in M material)
{
    float32x3 color = material.Evaluate(float32x3(1.0));
    if constexpr (Layered<M> == true)
    {
        // Valid only for layered materials; Plastic never checks this branch.
        color += material.EvaluateClearcoat(float32x3(1.0));
    }
    else
    {
        color *= float32(M::LOBE_COUNT);
    }
    return color;
}
float32x3 Use(in CarPaint paint, in Plastic plastic)
{
    return Shade(paint) + Shade(plastic);
}
)");
    CHECK(Contains(text, "color += material.EvaluateClearcoat(float3(1.0f));"));
    CHECK(Contains(text, "color *= float(Plastic.LOBE_COUNT);"));

    const CheckedSource variable = Check(R"(float32 Use(in float32 x)
{
    float32 y = x;
    if constexpr (x > 1.0)
    {
        y = 2.0;
    }
    return y;
}
)");
    CHECK(variable.FindDiagnosticOrNull(DiagnosticCode::InvalidConstraint) != nullptr);
}

TEST_CASE("LUSL requires clauses constrain templates", "[shader-language][concepts]")
{
    const std::string text = Lower(R"(template <typename T> requires Opaque<T> == true
float32x3 ShadeOpaque(in T material)
{
    return material.Evaluate(float32x3(0.0));
}
float32x3 Use(in Plastic plastic)
{
    return ShadeOpaque(plastic);
}
)");
    CHECK(Contains(text, "_luslSpecialization0_ShadeOpaque(Plastic material)"));
    const CheckedSource unsatisfied = Check(R"(template <typename T> requires Opaque<T> == true
float32x3 ShadeOpaque(in T material)
{
    return material.Evaluate(float32x3(0.0));
}
float32x3 Use(in CarPaint paint)
{
    return ShadeOpaque(paint);
}
)");
    CHECK(unsatisfied.FindDiagnosticOrNull(DiagnosticCode::TemplateDeduction) != nullptr);
}

TEST_CASE("LUSL reports templates whose constraints do not order", "[shader-language][concepts]")
{
    const CheckedSource ambiguous = Check(R"(concept Shiny<T> = (T : Material) && (T::LOBE_COUNT < 5);
template <Layered M>
float32 Weigh(in M material)
{
    return 1.0;
}
template <Shiny M>
float32 Weigh(in M material)
{
    return 2.0;
}
float32 Use(in CarPaint paint)
{
    return Weigh(paint);
}
)");
    CHECK(ambiguous.FindDiagnosticOrNull(DiagnosticCode::InvalidConstraint) != nullptr);
    const CheckedSource malformed = Check("concept Strange<T> = T::LOBE_COUNT.x;\n");
    CHECK(malformed.FindDiagnosticOrNull(DiagnosticCode::InvalidConstraint) != nullptr);
}

TEST_CASE("LUSL analyzes the flow of each specialization", "[shader-language][concepts]")
{
    const CheckedSource missingReturn = Check(R"(template <Material M>
float32 Weigh(in M material)
{
    if constexpr (Opaque<M> == true)
    {
        return 1.0;
    }
}
float32 Use(in CarPaint paint, in Plastic plastic)
{
    return Weigh(plastic) + Weigh(paint);
}
)");
    // Plastic's specialization returns; CarPaint's discards the only return.
    CHECK(missingReturn.CountDiagnostics(DiagnosticCode::MissingReturn) == 1);
}

TEST_CASE("LUSL reports specializations and groupshared clears", "[shader-language][concepts]")
{
    Snapshot snapshot;
    snapshot.AddFile("main.lusl", std::string(MATERIALS) + R"(static const uint32 TILE = 8;
groupshared float32x2 tile[TILE] = 0;
template <Layered M>
float32x3 Shade(in M material)
{
    return material.EvaluateClearcoat(float32x3(1.0));
}
[stage(compute), threads(TILE, 1, 1)]
void Main(in uint32 groupIndex : GroupIndex)
{
    CarPaint paint = { .Albedo = float32x3(1.0) };
    tile[groupIndex] = Shade(paint).xy;
}
)");
    (void)snapshot.Run();
    language::LoweringResult result;
    REQUIRE(language::LowerToSlang(snapshot.GetRequest(), language::LoweringOptions{}, result) ==
            language::Status::Ready);
    REQUIRE(result.Specializations.GetSize() == 1);
    CHECK(result.Specializations[0].Name.GetView() == "Shade<CarPaint>");
    CHECK(result.Specializations[0].GeneratedName.GetView() == "_luslSpecialization0_Shade");
    REQUIRE(result.GroupsharedClears.GetSize() == 1);
    // Eight float32x2 elements, unpadded.
    CHECK(result.GroupsharedClears[0].Bytes == 64);
    CHECK(result.GroupsharedClears[0].Variable.Line > 0);
}

TEST_CASE("LUSL keeps specializations within the budget", "[shader-language][concepts]")
{
    const std::string source = std::string(MATERIALS) + R"(template <Layered M>
float32x3 Shade(in M material)
{
    return material.EvaluateClearcoat(float32x3(1.0));
}
template <Opaque M>
float32x3 Shade(in M material)
{
    return material.Evaluate(float32x3(1.0));
}
float32x3 Use(in CarPaint paint, in Plastic plastic)
{
    return Shade(paint) + Shade(plastic);
}
)";
    language::CheckOptions options;
    options.SpecializationBudget = 2;
    const CheckedSource fits(source, options);
    INFO(fits.DescribeDiagnostics());
    CHECK(fits.GetStatus() == language::Status::Ready);
    CHECK(fits.GetModel().Specializations.GetSize() == 2);

    options.SpecializationBudget = 1;
    const CheckedSource over(source, options);
    const language::Diagnostic* budgetOrNull = over.FindDiagnosticOrNull(DiagnosticCode::SpecializationBudget);
    REQUIRE(budgetOrNull != nullptr);
    // The message names the template, the count, the budget and both repairs.
    const std::string_view message = budgetOrNull->Message.GetView();
    CHECK(message.find("'Shade' needs specialization 2, over the budget of 1") != std::string_view::npos);
    CHECK(message.find("budgets.specializations") != std::string_view::npos);
    CHECK(message.find("interfaces only") != std::string_view::npos);
}
