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
using language::test::CheckedSource;

/// Bindings, a stage interface and helpers shared by the fragment tests.
constexpr std::string_view PRELUDE = R"(Texture2D<float32x4> image;
SamplerState imageSampler;
struct Settings
{
    uint32 Mode;
    uint32 Count;
};
ConstantBuffer<Settings> settings;
RWStructuredBuffer<uint32> counts;
struct Interpolated
{
    float32x4 Position : ClipPosition;
    float32x2 Uv;
};
float32x4 Shade(in uniform uint32 mode, in float32x2 uv)
{
    if (mode == 1)
    {
        return image.Sample(imageSampler, uv);
    }
    return float32x4(0.0);
}
float32x4 ShadeTwice(in uniform uint32 mode, in float32x2 uv)
{
    return Shade(mode, uv) + Shade(mode, uv);
}
float32 Weigh(in uniform uint32 mode)
{
    return float32(mode);
}
uint32 ReadCount()
{
    return counts[0];
}
[stage(vertex)]
Interpolated VertexMain(in float32x4 positionUv)
{
    Interpolated output = { .Position = float32x4(positionUv.xy, 0.0, 1.0), .Uv = positionUv.zw };
    return output;
}
)";

/// A fragment entry point with `body`, which may use `input` and `color`.
[[nodiscard]] std::string MakeFragment(std::string_view body)
{
    return std::string(PRELUDE) +
           "[stage(fragment)]\nfloat32x4 FragmentMain(in Interpolated input)\n{\n    float32x4 color = 0;\n" +
           std::string(body) + "\n    return color;\n}\n";
}

void RequireUniform(std::string_view body)
{
    const CheckedSource checked(MakeFragment(body));
    INFO(body << "\n" << checked.DescribeDiagnostics());
    CHECK(checked.GetStatus() == Status::Ready);
}

void RequireError(std::string_view body, DiagnosticCode code)
{
    const CheckedSource checked(MakeFragment(body));
    INFO(body << "\n" << checked.DescribeDiagnostics());
    CHECK(checked.GetStatus() == Status::Failed);
    CHECK(checked.HasDiagnostic(code) == true);
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

TEST_CASE("LUSL accepts derivatives in uniform control flow", "[shader-language][uniformity]")
{
    RequireUniform(
        "    color = image.Sample(imageSampler, input.Uv) + float32x4(ddx(input.Uv.x), ddy(input.Uv.y), 0.0, 1.0);");
    // A decision on a ConstantBuffer field, a constant or a uniform loop counter stays uniform.
    RequireUniform("    if (settings.Mode == 1)\n    {\n        color = image.Sample(imageSampler, input.Uv);\n    }");
    RequireUniform("    for (uint32 i = 0; i < settings.Count; ++i)\n    {\n"
                   "        color = color + image.Sample(imageSampler, input.Uv);\n    }");
    // `discard` demotes to a helper invocation, so derivatives after it stay defined.
    RequireUniform("    if (input.Uv.x > 0.5)\n    {\n        discard;\n    }\n"
                   "    color = image.Sample(imageSampler, input.Uv);");
    // An explicit level needs no derivatives.
    RequireUniform("    if (input.Uv.x > 0.5)\n    {\n        color = image.SampleLevel(imageSampler, input.Uv, 0.0);\n"
                   "    }");
    // Control flow reconverges after the statement.
    RequireUniform("    if (input.Uv.x > 0.5)\n    {\n        color = float32x4(1.0);\n    }\n"
                   "    color = color + image.Sample(imageSampler, input.Uv);");
}

TEST_CASE("LUSL rejects derivatives in non-uniform control flow", "[shader-language][uniformity]")
{
    RequireError("    if (input.Uv.x > 0.5)\n    {\n        color = image.Sample(imageSampler, input.Uv);\n    }",
                 DiagnosticCode::NonUniformDerivative);
    RequireError("    if (input.Uv.x > 0.5)\n    {\n        color = float32x4(ddx(input.Uv.x));\n    }",
                 DiagnosticCode::NonUniformDerivative);
    RequireError("    while (color.x < input.Uv.x)\n    {\n        color = color + float32x4(ddy(input.Uv.y));\n    }",
                 DiagnosticCode::NonUniformDerivative);
    RequireError(
        "    switch (uint32(input.Uv.x))\n    {\n    case 0:\n        color = image.Sample(imageSampler, input.Uv);\n"
        "        break;\n    default:\n        break;\n    }",
        DiagnosticCode::NonUniformDerivative);
    RequireError("    color = input.Uv.x > 0.5 ? image.Sample(imageSampler, input.Uv) : float32x4(0.0);",
                 DiagnosticCode::NonUniformDerivative);
    RequireError("    bool isBright = input.Uv.x > 0.5 && image.Sample(imageSampler, input.Uv).x > 0.5;",
                 DiagnosticCode::NonUniformDerivative);
    // The rest of the function is non-uniform after a return in non-uniform flow.
    RequireError("    if (input.Uv.x > 0.5)\n    {\n        return color;\n    }\n"
                 "    color = image.Sample(imageSampler, input.Uv);",
                 DiagnosticCode::NonUniformDerivative);
    // Later iterations run in non-uniform flow after a varying `break`, even for code before it.
    RequireError("    for (uint32 i = 0; i < settings.Count; ++i)\n    {\n"
                 "        color = color + image.Sample(imageSampler, input.Uv);\n"
                 "        if (input.Uv.x > 0.5)\n        {\n            break;\n        }\n    }",
                 DiagnosticCode::NonUniformDerivative);
    // A read of writable memory varies.
    RequireError("    if (counts[0] > 0)\n    {\n        color = image.Sample(imageSampler, input.Uv);\n    }",
                 DiagnosticCode::NonUniformDerivative);
}

TEST_CASE("LUSL derivative errors name the decision and the repair", "[shader-language][uniformity]")
{
    const CheckedSource checked(
        MakeFragment("    uint32 mode = settings.Mode;\n    if (input.Uv.x > 0.5)\n    {\n        mode = 0;\n    }\n"
                     "    if (mode == 1)\n    {\n        color = image.Sample(imageSampler, input.Uv);\n    }"));
    INFO(checked.DescribeDiagnostics());
    CHECK(checked.GetStatus() == Status::Failed);
    const std::string message = GetMessage(checked, DiagnosticCode::NonUniformDerivative);
    INFO(message);
    // A variable assigned in non-uniform flow varies everywhere in its function.
    CHECK(Contains(message, "'Sample' needs uniform control flow") == true);
    CHECK(Contains(message, "reads 'mode', which depends on 'input' (an entry-point input)") == true);
    CHECK(Contains(message, "use 'SampleLevel' with an explicit level") == true);
}

TEST_CASE("LUSL functions that sample need uniform control flow at their calls", "[shader-language][uniformity]")
{
    RequireUniform("    color = ShadeTwice(settings.Mode, input.Uv);");
    const CheckedSource checked(
        MakeFragment("    if (input.Uv.x > 0.5)\n    {\n        color = ShadeTwice(settings.Mode, input.Uv);\n    }"));
    INFO(checked.DescribeDiagnostics());
    CHECK(checked.GetStatus() == Status::Failed);
    CHECK(checked.CountDiagnostics(DiagnosticCode::NonUniformDerivative) == 1);
    const std::string message = GetMessage(checked, DiagnosticCode::NonUniformDerivative);
    INFO(message);
    CHECK(Contains(message, "'ShadeTwice' needs uniform control flow because it calls 'Shade'") == true);
    CHECK(Contains(message, "call it before the branch") == true);

    // A function that decides on a parameter not declared `uniform` reports in its own body.
    const CheckedSource helper(std::string(PRELUDE) + R"(float32x4 Pick(in uint32 mode, in float32x2 uv)
{
    if (mode == 1)
    {
        return image.Sample(imageSampler, uv);
    }
    return float32x4(0.0);
}
)");
    INFO(helper.DescribeDiagnostics());
    CHECK(helper.GetStatus() == Status::Failed);
    const std::string helperMessage = GetMessage(helper, DiagnosticCode::NonUniformDerivative);
    CHECK(Contains(helperMessage, "reads 'mode' (a parameter not declared 'uniform')") == true);
}

TEST_CASE("LUSL uniform parameters take uniform arguments", "[shader-language][uniformity]")
{
    RequireUniform("    color = float32x4(Weigh(settings.Mode) + Weigh(2));");
    // A local declared in non-uniform flow is uniform within that flow.
    RequireUniform("    if (input.Uv.x > 0.5)\n    {\n        uint32 mode = 1;\n"
                   "        color = float32x4(Weigh(mode));\n    }");
    RequireError("    color = float32x4(Weigh(uint32(input.Uv.x)));", DiagnosticCode::NonUniformArgument);
    RequireError("    color = float32x4(Weigh(ReadCount()));", DiagnosticCode::NonUniformArgument);
    // A nested decision on a varying value makes the outer region's local vary.
    RequireError("    if (input.Uv.x > 0.5)\n    {\n        uint32 mode = 1;\n        if (input.Uv.y > 0.5)\n"
                 "        {\n            mode = 2;\n        }\n        color = float32x4(Weigh(mode));\n    }",
                 DiagnosticCode::NonUniformArgument);
    // A result of a function that reads writable memory varies.
    RequireError("    if (ReadCount() > 0)\n    {\n        color = image.Sample(imageSampler, input.Uv);\n    }",
                 DiagnosticCode::NonUniformDerivative);

    const CheckedSource checked(MakeFragment("    color = Shade(uint32(input.Uv.x), input.Uv);"));
    const std::string message = GetMessage(checked, DiagnosticCode::NonUniformArgument);
    INFO(message);
    CHECK(Contains(message, "parameter 'mode' of 'Shade' is 'uniform'") == true);
    CHECK(Contains(message, "remove 'uniform' from 'mode'") == true);
}

TEST_CASE("LUSL compute inputs other than GroupId vary", "[shader-language][uniformity]")
{
    const std::string prelude = R"(static const uint32 GROUP_WIDTH = 8;
RWStructuredBuffer<uint32> counts;
groupshared uint32 total = 0;
uint32 Keep(in uniform uint32 value)
{
    return value;
}
[stage(compute), threads(GROUP_WIDTH, 1, 1)]
void ComputeMain(in uint32x3 group : GroupId, in uint32x3 thread : DispatchThreadId)
{
)";
    const CheckedSource valid(prelude + "    counts[thread.x] = Keep(group.x);\n}\n");
    INFO(valid.DescribeDiagnostics());
    CHECK(valid.GetStatus() == Status::Ready);
    for (const std::string_view argument : {"thread.x", "counts[0]", "total"})
    {
        const CheckedSource checked(prelude + "    counts[thread.x] = Keep(" + std::string(argument) + ");\n}\n");
        INFO(argument << "\n" << checked.DescribeDiagnostics());
        CHECK(checked.HasDiagnostic(DiagnosticCode::NonUniformArgument) == true);
    }
}

TEST_CASE("LUSL rejects misplaced uniform qualifiers", "[shader-language][uniformity]")
{
    const CheckedSource output("void Store(out uniform float32 outValue)\n{\n    outValue = 0;\n}\n");
    INFO(output.DescribeDiagnostics());
    CHECK(output.HasDiagnostic(DiagnosticCode::InvalidUniformParameter) == true);
    const CheckedSource entry(R"(static const uint32 GROUP_WIDTH = 8;
[stage(compute), threads(GROUP_WIDTH, 1, 1)]
void ComputeMain(in uniform uint32x3 group : GroupId)
{
}
)");
    INFO(entry.DescribeDiagnostics());
    CHECK(entry.HasDiagnostic(DiagnosticCode::InvalidUniformParameter) == true);
}

TEST_CASE("LUSL checks uniformity in each specialization once", "[shader-language][uniformity]")
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
float32x4 Layer(in T material, in float32x2 uv)
{
    if (uv.x > 0.5)
    {
        return image.Sample(imageSampler, uv);
    }
    return float32x4(float32(T::LOBE_COUNT));
}
float32x4 Combine(in uniform uint32 mode, in float32x2 uv)
{
    const Plastic plastic = 0;
    const CarPaint paint = 0;
    return Layer(plastic, uv) + Layer(paint, uv);
}
)");
    INFO(checked.DescribeDiagnostics());
    CHECK(checked.GetModel().Specializations.GetSize() == 2);
    CHECK(checked.CountDiagnostics(DiagnosticCode::NonUniformDerivative) == 1);
}

TEST_CASE("LUSL lowering erases the uniform qualifier", "[shader-language][uniformity]")
{
    language::test::Snapshot snapshot;
    snapshot.AddFile("main.lusl", "float32 Weigh(in uniform uint32 mode)\n{\n    return float32(mode);\n}\n");
    (void)snapshot.Run();
    language::LoweringResult result;
    const Status status = language::LowerToSlang(snapshot.GetRequest(), language::LoweringOptions{}, result);
    REQUIRE(status == Status::Ready);
    const std::string text(result.Text.GetData(), result.Text.GetSize());
    INFO(text);
    CHECK(Contains(text, "Weigh(") == true);
    // Slang's own `uniform` would make the parameter a constant-buffer parameter.
    CHECK(Contains(text, "uniform") == false);
}
