// Target tests: lowered LUSL compiled by the pinned Slang through Ludus::ShaderCompiler for every
// portable target (docs/architecture/shading-language.md#testing).

#include <ludus/foundation/containers/array.hpp>
#include <ludus/tooling/shader_compiler/compiler.hpp>
#include <ludus/tooling/shader_language/pipeline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace
{
namespace foundation = ludus::foundation;
namespace compiler = ludus::tooling::shader_compiler;
namespace language = ludus::tooling::shader_language;

constexpr std::string_view ROOT = "shaders/main.lusl";
constexpr std::string_view GENERATED = "shaders/main.lusl.slang";
constexpr std::string_view ENTRY = "Main";

/// Owned files of one snapshot.
class Files final
{
public:
    Files& Add(std::string path, std::string text)
    {
        mPaths.Add(std::move(path));
        mTexts.Add(std::move(text));
        return *this;
    }

    /// Views of every file; valid until the next Add.
    [[nodiscard]] foundation::Array<compiler::SourceFile> GetViews() const
    {
        foundation::Array<compiler::SourceFile> views;
        for (foundation::usize i = 0; i < mPaths.GetSize(); ++i)
        {
            compiler::SourceFile file;
            file.Path = foundation::BorrowedView<const char>(mPaths[i].data(), mPaths[i].size());
            file.Text = foundation::BorrowedView<const char>(mTexts[i].data(), mTexts[i].size());
            views.Add(file);
        }
        return views;
    }

private:
    foundation::Array<std::string> mPaths;
    foundation::Array<std::string> mTexts;
};

/// Lower `files` from ROOT, add the generated module to them and return its text.
std::string LowerInto(Files& inoutFiles, bool shouldCheckBounds)
{
    const foundation::Array<compiler::SourceFile> views = inoutFiles.GetViews();
    language::PreprocessRequest request;
    request.Files = foundation::BorrowedView<const compiler::SourceFile>(views.GetData(), views.GetSize());
    request.Source = foundation::BorrowedView<const char>(ROOT.data(), ROOT.size());
    language::LoweringOptions options;
    options.Emit.ShouldCheckBounds = shouldCheckBounds;
    language::LoweringResult result;
    const language::Status outcome = language::LowerToSlang(request, options, result);
    std::string diagnostics;
    for (const language::Diagnostic& diagnostic : result.Diagnostics)
    {
        diagnostics += diagnostic.Message.GetView();
        diagnostics += '\n';
    }
    INFO(diagnostics);
    REQUIRE(outcome == language::Status::Ready);
    std::string text(result.Text.GetData(), result.Text.GetSize());
    inoutFiles.Add(std::string(GENERATED), text);
    return text;
}

compiler::Status
CompileFor(compiler::Compiler& inoutSlang, const Files& files, compiler::Target target, std::string& outDiagnostics)
{
    const foundation::Array<compiler::SourceFile> views = files.GetViews();
    compiler::Request request;
    request.Files = foundation::BorrowedView<const compiler::SourceFile>(views.GetData(), views.GetSize());
    request.Source = foundation::BorrowedView<const char>(GENERATED.data(), GENERATED.size());
    request.Entry = foundation::BorrowedView<const char>(ENTRY.data(), ENTRY.size());
    request.OutputTarget = target;
    request.EntryStage = compiler::Stage::Compute;
    compiler::Result result;
    const compiler::Status status = inoutSlang.Compile(request, result);
    outDiagnostics.assign(reinterpret_cast<const char*>(result.Diagnostics.GetData()), result.Diagnostics.GetSize());
    if (status == compiler::Status::Ready)
    {
        CHECK(result.Artifact.IsEmpty() == false);
    }
    return status;
}

/// A module that uses every lowering rule.
Files MakeFeatureFiles()
{
    Files files;
    files.Add(
        "shaders/math.slang",
        "float ComputeLuminance(float3 linearRgb) { return dot(linearRgb, float3(0.2126f, 0.7152f, 0.0722f)); }\n");
    files.Add(
        "shaders/math.luslh",
        "#pragma once\nextern slang \"math.slang\"\n{\n    float32 ComputeLuminance(in float32x3 linearRgb);\n};\n");
    files.Add(std::string(ROOT), R"(#include "math.luslh"
static const uint32 TAP_COUNT = 4;
static const uint32 TILE = 8;
static const int32 LOWEST = -2147483647 - 1;
enum class Mode : uint32
{
    Off,
    On = 3,
};
struct Lobe
{
    float32x3 NormalWorld = float32x3(0.0, 0.0, 1.0);
    float32 Weights[TAP_COUNT];
    bool IsFront;
    float32 Evaluate(in float32 cosine, in float32 scale = 1.0)
    {
        return Weights[0] * cosine * scale;
    }
    float32 Twice()
    {
        return Evaluate(scale = Constant(), cosine = 0.5);
    }
    float32 Constant()
    {
        return 2.0;
    }
    [mutating]
    void Flip()
    {
        NormalWorld = -NormalWorld;
    }
};
groupshared float32 tile[TILE] = 0;
groupshared Lobe shared = 0;
float32 Constant()
{
    return 1.0;
}
float32 Scale(in float32 value, in float32 factor = 2.0, in float32 bias = 0.0)
{
    return value * factor + bias;
}
int32 Wrap(in int32 a, in int32 b)
{
    int32 c = a * b - a;
    c += b;
    c++;
    int32x2 v = int32x2(a, b) * 3;
    return -c + v.x;
}
bool Trace(in float32x3 origin, out float32x3 outRadiance)
{
    outRadiance = origin * 0.5;
    return true;
}
float32 Pick(in uint32 index, in uint32 count)
{
    float32 weights[TAP_COUNT] = 0;
    for (uint32 i = 0; i < TAP_COUNT; i++)
    {
        weights[i] = float32(i << count);
    }
    Lobe lobe = { .IsFront = true };
    lobe.Flip();
    float32x4x4 worldToClip = float32x4x4(float32x4(1, 0, 0, 0), float32x4(0, 1, 0, 0), float32x4(0, 0, 1, 0),
                                          float32x4(0, 0, 0, 1));
    float32x4 positionClip = worldToClip * float32x4(lobe.NormalWorld, 1.0);
    float32x3 radiance;
    bool hasHit = Trace(outRadiance = out radiance, origin = float32x3(Constant()));
    Mode mode = Mode::On;
    float32 total = 0;
    switch (index)
    {
        case 0:
            total = 1;
            [[fallthrough]];
        case 1:
            total += 2;
            break;
        default:
            total = 3;
            break;
    }
    if (mode == Mode::On && hasHit == true)
    {
        total += ComputeLuminance(radiance) + positionClip.w + lobe.Twice();
    }
    return weights[index] + Scale(total, bias = 1.0) + Scale(factor = 3.0, value = Constant()) +
           float32(Wrap(int32(index), LOWEST));
}
[stage(compute), threads(TILE, 1, 1)]
void Main(in uint32x3 dispatchThread : DispatchThreadId)
{
    tile[dispatchThread.x] = Pick(index = dispatchThread.x % TAP_COUNT, count = dispatchThread.y);
    shared.Weights[dispatchThread.x % TAP_COUNT] = tile[0];
}
)");
    return files;
}

compiler::Compiler& GetCompiler()
{
    static compiler::Compiler slang;
    static const compiler::Status INITIALIZE_STATUS = slang.Initialize();
    REQUIRE(INITIALIZE_STATUS == compiler::Status::Ready);
    return slang;
}
} // namespace

TEST_CASE("Lowered LUSL compiles for every portable target", "[shader-language][emitter-targets]")
{
    compiler::Compiler& slang = GetCompiler();
    for (const bool shouldCheckBounds : {false, true})
    {
        Files files = MakeFeatureFiles();
        (void)LowerInto(files, shouldCheckBounds);
        for (const compiler::Target target : {compiler::Target::Spirv,
                                              compiler::Target::SpirvRayQuery,
                                              compiler::Target::Wgsl,
                                              compiler::Target::Metal})
        {
            std::string diagnostics;
            const compiler::Status status = CompileFor(slang, files, target, diagnostics);
            INFO("target " << static_cast<int>(target) << ", bounds checks " << shouldCheckBounds << "\n"
                           << diagnostics);
            CHECK(status == compiler::Status::Ready);
            // The generated module is warning-free, so its diagnostics stay meaningful.
            CHECK(diagnostics.find("warning") == std::string::npos);
        }
    }
}

TEST_CASE("Slang diagnostics on lowered LUSL name LUSL lines", "[shader-language][emitter-targets]")
{
    compiler::Compiler& slang = GetCompiler();
    Files files;
    // The extern declaration promises a function the Slang file does not define.
    files.Add("shaders/math.slang", "float Other(float x) { return x; }\n");
    files.Add(std::string(ROOT), R"(extern slang "math.slang"
{
    float32 Missing(in float32 x);
};

[stage(compute), threads(1, 1, 1)]
void Main(in uint32x3 dispatchThread : DispatchThreadId)
{
    float32 value = Missing(float32(dispatchThread.x));
}
)");
    (void)LowerInto(files, false);
    std::string diagnostics;
    CHECK(CompileFor(slang, files, compiler::Target::Spirv, diagnostics) == compiler::Status::CompileFailed);
    INFO(diagnostics);
    CHECK(diagnostics.find("shaders/main.lusl(9)") != std::string::npos);
}

TEST_CASE("Lowered 16-bit integers follow the portable target set", "[shader-language][emitter-targets]")
{
    // ShaderCompiler checks one module against every portable target at once, so a type that one
    // target lacks fails them all, with a diagnostic naming that target and the type.
    compiler::Compiler& slang = GetCompiler();
    Files files;
    // Values reach groupshared storage, so the compiler cannot drop them as unused.
    files.Add(std::string(ROOT), R"(groupshared int32 result = 0;
[stage(compute), threads(1, 1, 1)]
void Main(in uint32x3 dispatchThread : DispatchThreadId)
{
    int16 small = int16(dispatchThread.x);
    int16 negated = -small;
    result = int32(negated);
}
)");
    (void)LowerInto(files, false);
    for (const compiler::Target target : {compiler::Target::Spirv, compiler::Target::Wgsl, compiler::Target::Metal})
    {
        std::string diagnostics;
        CHECK(CompileFor(slang, files, target, diagnostics) == compiler::Status::CompileFailed);
        INFO(diagnostics);
        CHECK(diagnostics.find("'int16_t' is not supported by the WGSL backend") != std::string::npos);
    }
}
