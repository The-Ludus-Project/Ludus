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

/// A composite shader: two bindings, one attribute and a stage interface.
constexpr std::string_view COMPOSITE = R"(Texture2D<float32x4> image;
extern slang
{
    SamplerState sharedSampler;
};
SamplerState imageSampler;
struct CompositeInput
{
    float32x4 PositionUv;
};
struct CompositeVarying
{
    float32x4 Position : ClipPosition;
    float32x2 Uv;
    uint32 Layer;
};
[stage(vertex)]
CompositeVarying VertexMain(in CompositeInput input, in uint32 vertexIndex : VertexIndex)
{
    CompositeVarying output = { .Position = input.PositionUv, .Uv = input.PositionUv.zw, .Layer = vertexIndex };
    return output;
}
[stage(fragment)]
float32x4 FragmentMain(in CompositeVarying input)
{
    return image.Sample(imageSampler, input.Uv);
}
)";

language::LoweringResult Lower(std::string_view text, std::string& outSlang)
{
    Snapshot snapshot;
    snapshot.AddFile("main.lusl", std::string(text));
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
    outSlang.assign(result.Text.GetData(), result.Text.GetSize());
    return result;
}

bool Contains(const std::string& text, std::string_view part)
{
    return text.find(part) != std::string::npos;
}

/// Check `body` after a struct and bindings that most rule cases use.
CheckedSource Check(std::string_view body)
{
    return CheckedSource(std::string(R"(struct Material
{
    float32x4 Tint;
};
Texture2D<float32x4> image;
SamplerState imageSampler;
StructuredBuffer<float32x4> values;
RWStructuredBuffer<float32x4> results;
ConstantBuffer<Material> material;
)") + std::string(body));
}
} // namespace

TEST_CASE("LUSL numbers bindings and stage locations in declaration order", "[shader-language][bindings]")
{
    const CheckedSource checked{std::string(COMPOSITE)};
    INFO(checked.DescribeDiagnostics());
    REQUIRE(checked.GetStatus() == language::Status::Ready);
    const language::SemanticModel& model = checked.GetModel();
    // The extern sampler is the including Slang's binding and takes no slot.
    REQUIRE(model.Bindings.GetSize() == 2);
    CHECK(model.Symbols[model.Bindings[0]].Slot == 0);
    CHECK(model.Symbols[model.Bindings[1]].Slot == 1);

    std::string slang;
    const language::LoweringResult result = Lower(COMPOSITE, slang);
    CHECK(Contains(slang, "[[vk::binding(0, 0)]] Texture2D<float4> image : register(t0);"));
    CHECK(Contains(slang, "[[vk::binding(1, 0)]] SamplerState imageSampler : register(s1);"));
    CHECK(Contains(slang, "SamplerState sharedSampler") == false);
    CHECK(Contains(slang, "    [[vk::location(0)]] float4 PositionUv : TEXCOORD0;"));
    CHECK(Contains(slang, "    float4 Position : SV_Position;"));
    CHECK(Contains(slang, "    [[vk::location(0)]] float2 Uv : TEXCOORD0;"));
    // Integer varyings are interpolated flat.
    CHECK(Contains(slang, "    [[vk::location(1)]] nointerpolation uint Layer : TEXCOORD1;"));
    CHECK(Contains(slang,
                   "[shader(\"vertex\")]\nCompositeVarying VertexMain(CompositeInput input, uint "
                   "vertexIndex : _LUSL_VERTEX_INDEX)"));
    CHECK(Contains(slang, "#define _LUSL_VERTEX_INDEX SV_VulkanVertexID"));
    CHECK(Contains(slang, "[shader(\"fragment\")]\nfloat4 FragmentMain(CompositeVarying input) : SV_Target0"));
    CHECK(Contains(slang, "return image.Sample(imageSampler, input.Uv);"));
    CHECK(Contains(slang, "numthreads") == false);

    REQUIRE(result.Bindings.GetSize() == 2);
    CHECK(result.Bindings[0].Name.GetView() == "image");
    CHECK(result.Bindings[0].Kind == language::ResourceKind::Texture2D);
    CHECK(result.Bindings[1].Name.GetView() == "imageSampler");
    CHECK(result.Bindings[1].Slot == 1);
    REQUIRE(result.VertexInputs.GetSize() == 1);
    CHECK(result.VertexInputs[0].Name.GetView() == "input.PositionUv");
    CHECK(result.VertexInputs[0].Location == 0);
}

TEST_CASE("LUSL lays out vertex inputs, fragment targets and depth", "[shader-language][bindings]")
{
    std::string slang;
    const language::LoweringResult result = Lower(R"(struct Input
{
    float32x3 PositionMesh;
    float32x2 Uv;
};
struct Varying
{
    float32x4 Position : ClipPosition;
};
struct Output
{
    float32x4 Color;
    float32 Depth : Depth;
    uint32x2 Identity;
};
[stage(vertex)]
Varying VertexMain(in float32 scale, in Input input, in uint32 instance : InstanceIndex)
{
    Varying output = { .Position = float32x4(input.PositionMesh * scale, float32(instance)) };
    return output;
}
[stage(fragment)]
Output FragmentMain(in Varying input, in bool isFrontFacing : FrontFacing)
{
    Output output = { .Color = input.Position, .Depth = 0.5, .Identity = uint32x2(isFrontFacing == true ? 1 : 0) };
    return output;
}
)",
                                                  slang);
    // Attributes flatten in declaration order: the scalar first, then the struct's fields.
    CHECK(Contains(slang,
                   "VertexMain([[vk::location(0)]] float scale : TEXCOORD0, Input input, uint instance : "
                   "SV_InstanceID)"));
    CHECK(Contains(slang, "    [[vk::location(1)]] float3 PositionMesh : TEXCOORD1;"));
    CHECK(Contains(slang, "    [[vk::location(2)]] float2 Uv : TEXCOORD2;"));
    CHECK(Contains(slang, "    float4 Color : SV_Target0;"));
    CHECK(Contains(slang, "    float Depth : SV_Depth;"));
    CHECK(Contains(slang, "    uint2 Identity : SV_Target1;"));
    CHECK(Contains(slang, "bool isFrontFacing : SV_IsFrontFace"));
    REQUIRE(result.VertexInputs.GetSize() == 3);
    CHECK(result.VertexInputs[0].Name.GetView() == "scale");
    CHECK(result.VertexInputs[2].Name.GetView() == "input.Uv");
    CHECK(result.VertexInputs[2].Location == 2);
}

TEST_CASE("LUSL resources are handles with their own operations", "[shader-language][bindings]")
{
    const CheckedSource reads = Check(R"(float32x4 Read(in Texture2D<float32x4> texture, in uint32 index)
{
    return texture.Sample(imageSampler, float32x2(0.5)) + texture.SampleLevel(imageSampler, float32x2(0.5), 0) +
           texture.Load(int32x3(0, 0, 0)) + values[index] + results[index] + material.Tint;
}
void Write(in uint32 index)
{
    results[index] = values[0];
}
)");
    INFO(reads.DescribeDiagnostics());
    CHECK(reads.GetStatus() == language::Status::Ready);

    const CheckedSource local = Check("void F()\n{\n    Texture2D<float32x4> copy = image;\n}\n");
    CHECK(local.FindDiagnosticOrNull(DiagnosticCode::TypeMismatch) != nullptr);
    const CheckedSource field = Check("struct Holder\n{\n    SamplerState Sampler;\n};\n");
    CHECK(field.FindDiagnosticOrNull(DiagnosticCode::TypeMismatch) != nullptr);
    const CheckedSource returned = Check("SamplerState Get()\n{\n    return imageSampler;\n}\n");
    CHECK(returned.FindDiagnosticOrNull(DiagnosticCode::TypeMismatch) != nullptr);
    const CheckedSource written = Check("void F(out SamplerState result)\n{\n}\n");
    CHECK(written.FindDiagnosticOrNull(DiagnosticCode::TypeMismatch) != nullptr);
    const CheckedSource array = Check("static const uint32 COUNT = 2;\nTexture2D<float32x4> images[COUNT];\n");
    CHECK(array.FindDiagnosticOrNull(DiagnosticCode::TypeMismatch) != nullptr);
    const CheckedSource initialized = Check("SamplerState other = 0;\n");
    CHECK(initialized.FindDiagnosticOrNull(DiagnosticCode::InvalidInitializer) != nullptr);
    const CheckedSource assigned = Check(R"(void F()
{
    values[0] = float32x4(1.0);
    material.Tint = float32x4(1.0);
}
)");
    CHECK(assigned.CountDiagnostics(DiagnosticCode::NotAssignable) == 2);
    const CheckedSource indexed = Check("float32x4 F()\n{\n    return image[0];\n}\n");
    CHECK(indexed.FindDiagnosticOrNull(DiagnosticCode::InvalidOperands) != nullptr);
    const CheckedSource unknown = Check("float32x4 F()\n{\n    return image.Gather(imageSampler);\n}\n");
    CHECK(unknown.FindDiagnosticOrNull(DiagnosticCode::UnknownMember) != nullptr);
    const CheckedSource sampler =
        Check("float32x4 F()\n{\n    return image.Sample(float32x2(0.5), float32x2(0.5));\n}\n");
    CHECK(sampler.FindDiagnosticOrNull(DiagnosticCode::TypeMismatch) != nullptr);
    for (const std::string_view element : {"Texture2D<bool> a;\n",
                                           "Texture2D<float32x4x4> a;\n",
                                           "ConstantBuffer<float32> a;\n",
                                           "StructuredBuffer<bool> a;\n",
                                           "struct Flag\n{\n    bool IsOn;\n};\nConstantBuffer<Flag> a;\n"})
    {
        const CheckedSource invalid{std::string(element)};
        INFO(element);
        CHECK(invalid.FindDiagnosticOrNull(DiagnosticCode::TypeMismatch) != nullptr);
    }
    const CheckedSource arguments{std::string("SamplerState<float32> a;\nTexture2D b;\n")};
    CHECK(arguments.CountDiagnostics(DiagnosticCode::UnknownType) == 2);
}

TEST_CASE("LUSL checks stage interfaces", "[shader-language][bindings]")
{
    constexpr std::string_view STRUCTS = R"(struct Varying
{
    float32x4 Position : ClipPosition;
    float32x2 Uv;
};
struct Other
{
    float32x4 Position : ClipPosition;
};
)";
    const auto check = [&STRUCTS](std::string_view body) { // ludus-style: inferred-type: lambda closure
        return CheckedSource(std::string(STRUCTS) + std::string(body));
    };
    const CheckedSource noPosition = CheckedSource(std::string(R"(struct Bare
{
    float32x2 Uv;
};
[stage(vertex)]
Bare Main(in float32x4 position)
{
    Bare output = { .Uv = position.xy };
    return output;
}
)"));
    CHECK(noPosition.FindDiagnosticOrNull(DiagnosticCode::InvalidStageInterface) != nullptr);

    const CheckedSource twoInterfaces = check(R"([stage(vertex)]
Varying VertexMain(in float32x4 position)
{
    Varying output = { .Position = position, .Uv = position.xy };
    return output;
}
[stage(fragment)]
float32x4 FragmentMain(in Other input)
{
    return input.Position;
}
)");
    CHECK(twoInterfaces.FindDiagnosticOrNull(DiagnosticCode::InvalidStageInterface) != nullptr);

    const CheckedSource twoRoles = check(R"([stage(vertex)]
Varying VertexMain(in Varying input)
{
    return input;
}
)");
    CHECK(twoRoles.FindDiagnosticOrNull(DiagnosticCode::InvalidStageInterface) != nullptr);

    const CheckedSource looseFragmentInput = check(R"([stage(fragment)]
float32x4 FragmentMain(in Varying input, in float32x2 uv)
{
    return input.Position;
}
)");
    CHECK(looseFragmentInput.FindDiagnosticOrNull(DiagnosticCode::InvalidStageInterface) != nullptr);

    const CheckedSource boolAttribute = check(R"([stage(vertex)]
Varying VertexMain(in bool isVisible)
{
    Varying output = 0;
    return output;
}
)");
    CHECK(boolAttribute.FindDiagnosticOrNull(DiagnosticCode::InvalidStageInterface) != nullptr);

    const CheckedSource semantics = CheckedSource(std::string(R"(struct Wrong
{
    float32x2 Position : ClipPosition;
    float32x4 Color : Target0;
};
[stage(vertex), threads(1, 1, 1)]
void Main(in uint32 index : DispatchThreadId)
{
}
[stage(raster)]
void Other()
{
}
)"));
    CHECK(semantics.CountDiagnostics(DiagnosticCode::InvalidSemantic) == 3);
    CHECK(semantics.CountDiagnostics(DiagnosticCode::InvalidAttribute) == 2);

    const CheckedSource resourceParameter =
        CheckedSource(std::string("[stage(fragment)]\nfloat32x4 Main(in SamplerState state)\n{\n    return "
                                  "float32x4(0.0);\n}\n"));
    CHECK(resourceParameter.FindDiagnosticOrNull(DiagnosticCode::TypeMismatch) != nullptr);
}
