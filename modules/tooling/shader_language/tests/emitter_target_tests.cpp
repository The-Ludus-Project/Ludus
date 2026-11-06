// Target tests: lowered LUSL compiled by the pinned Slang through Ludus::ShaderCompiler for every
// portable target (docs/architecture/shading-language.md#testing).

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/tooling/shader_compiler/compiler.hpp>
#include <ludus/tooling/shader_language/pipeline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
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

namespace
{
/// Compile ROOT as a `.lusl` Source through ShaderCompiler, which lowers it first.
compiler::Status CompileLanguage(compiler::Compiler& inoutSlang,
                                 const Files& files,
                                 const compiler::Request& options,
                                 compiler::Result& outResult)
{
    const foundation::Array<compiler::SourceFile> views = files.GetViews();
    compiler::Request request = options;
    request.Files = foundation::BorrowedView<const compiler::SourceFile>(views.GetData(), views.GetSize());
    request.Source = foundation::BorrowedView<const char>(ROOT.data(), ROOT.size());
    request.Entry = foundation::BorrowedView<const char>(ENTRY.data(), ENTRY.size());
    request.EntryStage = compiler::Stage::Compute;
    return inoutSlang.Compile(request, outResult);
}

std::string GetDiagnostics(const compiler::Result& result)
{
    return std::string(reinterpret_cast<const char*>(result.Diagnostics.GetData()), result.Diagnostics.GetSize());
}
} // namespace

TEST_CASE("ShaderCompiler lowers a .lusl source before Slang", "[shader-language][emitter-targets]")
{
    compiler::Compiler& slang = GetCompiler();
    Files files;
    files.Add("shaders/unused.slang", "float Unused() { return 0.0f; }\n");
    files.Add("shaders/math.slang", "float Twice(float x) { return x * 2.0f; }\n");
    files.Add("shaders/math.luslh",
              "#pragma once\nextern slang \"math.slang\"\n{\n    float32 Twice(in float32 x);\n};\n");
    // A stale generated module in the snapshot is replaced.
    files.Add(std::string(GENERATED), "this does not compile\n");
    files.Add(std::string(ROOT), R"(#include "math.luslh"
groupshared float32 result = 0;
[stage(compute), threads(1, 1, 1)]
void Main(in uint32x3 dispatchThread : DispatchThreadId)
{
#if MODE == 2
    result = Twice(float32(dispatchThread.x));
#endif
}
)");
    const std::string_view modeName = "MODE";
    const std::string_view modeValue = "2";
    const std::string_view unusedName = "UNUSED";
    const std::string_view unusedValue = "1";
    const foundation::StaticArray<compiler::ConfigurationValue, 2> configuration = {{
        {{modeName.data(), modeName.size()}, {modeValue.data(), modeValue.size()}},
        {{unusedName.data(), unusedName.size()}, {unusedValue.data(), unusedValue.size()}},
    }};
    compiler::Request options;
    options.Configuration = foundation::BorrowedView<const compiler::ConfigurationValue>(configuration.GetData(), 2);
    for (const compiler::Target target : {compiler::Target::Spirv, compiler::Target::Wgsl, compiler::Target::Metal})
    {
        options.OutputTarget = target;
        compiler::Result result;
        INFO("target " << static_cast<int>(target));
        REQUIRE(CompileLanguage(slang, files, options, result) == compiler::Status::Ready);
        INFO(GetDiagnostics(result));
        CHECK(result.Artifact.IsEmpty() == false);
        const std::string generated(result.GeneratedSource.GetData(), result.GeneratedSource.GetSize());
        CHECK(generated.find("#include \"math.slang\"") != std::string::npos);
        // One mask byte per request file: the unused Slang file and the stale module stay unread.
        REQUIRE(result.SourceReadMask.GetSize() == 5);
        CHECK(result.SourceReadMask[0] == 0);
        CHECK(result.SourceReadMask[1] == 1);
        CHECK(result.SourceReadMask[2] == 1);
        CHECK(result.SourceReadMask[3] == 0);
        CHECK(result.SourceReadMask[4] == 1);
        REQUIRE(result.ConfigurationReadMask.GetSize() == 2);
        CHECK(result.ConfigurationReadMask[0] == 1);
        CHECK(result.ConfigurationReadMask[1] == 0);
    }
}

TEST_CASE("ShaderCompiler reports LUSL diagnostics in Slang's form", "[shader-language][emitter-targets]")
{
    compiler::Compiler& slang = GetCompiler();
    Files files;
    files.Add(std::string(ROOT), R"([stage(compute), threads(1, 1, 1)]
void Main(in uint32x3 dispatchThread : DispatchThreadId)
{
    float32 x = 1;
    uint32 y = x;
}
)");
    compiler::Request options;
    compiler::Result failed;
    CHECK(CompileLanguage(slang, files, options, failed) == compiler::Status::CompileFailed);
    CHECK(failed.Artifact.IsEmpty() == true);
    CHECK(GetDiagnostics(failed).find("shaders/main.lusl(5,16): error LUSL0504: expected uint32, found float32") !=
          std::string::npos);

    // Style findings follow the request's style setting.
    Files styled;
    styled.Add(std::string(ROOT), R"(static const uint32 count = 4;
[stage(compute), threads(1, 1, 1)]
void Main(in uint32x3 dispatchThread : DispatchThreadId)
{
}
)");
    compiler::Result strict;
    CHECK(CompileLanguage(slang, styled, options, strict) == compiler::Status::CompileFailed);
    CHECK(GetDiagnostics(strict).find("shaders/main.lusl(1,21): error LUSL0601") != std::string::npos);
    options.Style = compiler::LanguageStyle::Warn;
    compiler::Result warned;
    CHECK(CompileLanguage(slang, styled, options, warned) == compiler::Status::Ready);
    CHECK(GetDiagnostics(warned).find("shaders/main.lusl(1,21): warning LUSL0601") != std::string::npos);
    options.Style = compiler::LanguageStyle::Off;
    compiler::Result quiet;
    CHECK(CompileLanguage(slang, styled, options, quiet) == compiler::Status::Ready);
    CHECK(GetDiagnostics(quiet).find("LUSL") == std::string::npos);
}

namespace
{
/// The text of `name` in FoundationMath's shader directory.
std::string ReadMathShader(std::string_view name)
{
    foundation::filesystem::Directory directory;
    REQUIRE(directory.Open(LUDUS_MATH_SHADER_DIRECTORY).Succeeded() == true);
    foundation::filesystem::File file;
    REQUIRE(directory.OpenRead(name, file).Succeeded() == true);
    std::string text(static_cast<foundation::usize>(file.Size()), '\0');
    const foundation::filesystem::ReadResult read =
        file.ReadAt(0, std::span<foundation::uint8>(reinterpret_cast<foundation::uint8*>(text.data()), text.size()));
    REQUIRE(read.Outcome.Succeeded() == true);
    return text;
}
} // namespace

TEST_CASE("The SDK math declarations match math.slang on every target", "[shader-language][emitter-targets]")
{
    compiler::Compiler& slang = GetCompiler();
    Files files;
    files.Add("shaders/math.slang", ReadMathShader("math.slang"));
    files.Add("shaders/math.luslh", ReadMathShader("math.luslh"));
    // Every declaration is called, so Slang checks each against its definition.
    files.Add(std::string(ROOT), R"(#include "math.luslh"
groupshared float32 result = 0;
[stage(compute), threads(1, 1, 1)]
void Main(in uint32x3 dispatchThread : DispatchThreadId)
{
    uint32 low;
    uint32 high;
    MultiplyWide(dispatchThread.x, FLOAT_EXPONENT_BITS, out low, out high);
    RandomAddress address = MakeRandomAddress(scope = dispatchThread.xy, event = RANDOM_ADDRESS_LAYOUT_VERSION,
                                              dimension = RANDOM_MAX_DIMENSION);
    uint32 bounded;
    bool isBounded = TrySampleRandomBounded(dispatchThread.xy, address,
                                            RANDOM_ATTEMPT_COUNT + RANDOM_BOUNDED_MAPPING_VERSION, out bounded);
    float32x3 normal = ApplyNormalWorld(NormalizeOrZero(float32x3(1.0)), float32x3(0.0, 0.0, 1.0),
                                        float32x3(1.0, 0.0, 0.0), float32x3(0.0, 1.0, 0.0), float32x2(1.0, 0.0),
                                        float32x2(0.0, 1.0));
    float32 total = PI + ComputeErrorFunction(0.5) + ComputeArctangent(1.0) + ComputeStandardNormalQuantile(0.25) +
                    DecodeSrgb(0.5) + RandomFloat01(low ^ high) + SampleRandomFloat01(dispatchThread.xy, address);
    total += float32(Philox4x32(PackRandomCounter(address, 0), dispatchThread.xy).x) +
             float32(SampleRandomUInt32(dispatchThread.xy, address)) + normal.x + float32(bounded);
    if (IsFiniteBits(total) == true && IsFiniteBits(normal) == true && HasPositiveComponent(normal) == true &&
        isBounded == true && IsRandomAddressInRange(address) == true)
    {
        result = total;
    }
}
)");
    compiler::Request options;
    for (const compiler::Target target : {compiler::Target::Spirv, compiler::Target::Wgsl, compiler::Target::Metal})
    {
        options.OutputTarget = target;
        compiler::Result result;
        const compiler::Status status = CompileLanguage(slang, files, options, result);
        INFO("target " << static_cast<int>(target) << "\n" << GetDiagnostics(result));
        CHECK(status == compiler::Status::Ready);
    }
}

TEST_CASE("Lowered LUSL interfaces and templates compile for every portable target",
          "[shader-language][emitter-targets]")
{
    compiler::Compiler& slang = GetCompiler();
    Files files;
    files.Add("shaders/ggx.slang", "struct FoundationGgx { float Roughness; };\n");
    files.Add(std::string(ROOT), R"(extern slang "ggx.slang"
{
    struct FoundationGgx
    {
        float32 Roughness;
    };
};

interface Material
{
    static const uint32 LOBE_COUNT;
    float32x3 Evaluate(in float32x3 normalWorld, in float32x3 viewWorld, in float32x3 lightWorld);
};

interface ClearcoatMaterial : Material
{
    float32x3 EvaluateClearcoat(in float32x3 normalWorld, in float32x3 viewWorld);
};

struct CarPaint : ClearcoatMaterial
{
    static const uint32 LOBE_COUNT = 3;
    float32x3 Albedo;
    float32x3 Evaluate(in float32x3 normalWorld, in float32x3 viewWorld, in float32x3 lightWorld)
    {
        return Albedo * dot(normalWorld, lightWorld) * float32(LOBE_COUNT);
    }
    float32x3 EvaluateClearcoat(in float32x3 normalWorld, in float32x3 viewWorld)
    {
        return float32x3(dot(normalWorld, viewWorld));
    }
};

extension FoundationGgx : Material
{
    static const uint32 LOBE_COUNT = 1;
    float32x3 Evaluate(in float32x3 normalWorld, in float32x3 viewWorld, in float32x3 lightWorld)
    {
        return float32x3(Roughness);
    }
};

template <Material M>
float32x3 Shade(in M material, in float32x3 normalWorld)
{
    float32x3 color = material.Evaluate(normalWorld, normalWorld, lightWorld = normalWorld);
    return color * float32(M::LOBE_COUNT);
}

template <ClearcoatMaterial M>
float32x3 ShadeClearcoat(in M material, in float32x3 normalWorld)
{
    return Shade(material, normalWorld) + material.EvaluateClearcoat(normalWorld, normalWorld);
}

groupshared float32x3 result = 0;

[stage(compute), threads(1, 1, 1)]
void Main(in uint32x3 dispatchThread : DispatchThreadId)
{
    CarPaint paint = { .Albedo = float32x3(1.0) };
    FoundationGgx ggx = { .Roughness = 0.5 };
    float32x3 normalWorld = float32x3(0.0, 0.0, 1.0);
    result = Shade(material = paint, normalWorld = normalWorld) + Shade(ggx, normalWorld) +
             ShadeClearcoat(paint, normalWorld) + float32x3(float32(CarPaint::LOBE_COUNT));
}
)");
    compiler::Request options;
    for (const compiler::Target target : {compiler::Target::Spirv, compiler::Target::Wgsl, compiler::Target::Metal})
    {
        options.OutputTarget = target;
        compiler::Result result;
        const compiler::Status status = CompileLanguage(slang, files, options, result);
        INFO("target " << static_cast<int>(target) << "\n" << GetDiagnostics(result));
        CHECK(status == compiler::Status::Ready);
        const std::string generated(result.GeneratedSource.GetData(), result.GeneratedSource.GetSize());
        CHECK(generated.find("float3 Shade<M : Material>(M material, float3 normalWorld)") != std::string::npos);
    }
}

TEST_CASE("Lowered LUSL concepts and specializations compile for every portable target",
          "[shader-language][emitter-targets]")
{
    // The design's material example: concepts, constraint ordering, if constexpr and static_assert.
    compiler::Compiler& slang = GetCompiler();
    Files files;
    files.Add(std::string(ROOT), R"(interface Material
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
        return Albedo * dot(normalWorld, lightWorld);
    }
    float32x3 EvaluateClearcoat(in float32x3 normalWorld)
    {
        return normalWorld * 0.5;
    }
};

struct Plastic : Material
{
    static const uint32 LOBE_COUNT = 1;
    float32 Roughness;
    float32x3 Evaluate(in float32x3 normalWorld, in float32x3 lightWorld)
    {
        return float32x3(Roughness);
    }
};

concept Opaque<T> = (T : Material) && (T::LOBE_COUNT == 1);
concept Layered<T> = (T : ClearcoatMaterial) && (T::LOBE_COUNT > 1);

static_assert(Layered<CarPaint> == true, "CarPaint must stay layered");
static_assert(Opaque<CarPaint> == false, "CarPaint is not opaque");
static_assert(Opaque<Plastic> == true, "Plastic is opaque");

struct FogParameters
{
    float32 Density;
};

float32x3 ApplyFog(in float32x3 color, in FogParameters fog)
{
    return color * (1.0 - fog.Density);
}

template <Material M>
float32x3 Shade(in M material, in float32x3 normalWorld)
{
    return material.Evaluate(normalWorld, normalWorld);
}

template <Layered M>
float32x3 Shade(in M material, in float32x3 normalWorld)
{
    return material.Evaluate(normalWorld, normalWorld) + material.EvaluateClearcoat(normalWorld);
}

template <Material M>
float32x3 ShadeWithFog(in M material, in float32x3 normalWorld, in FogParameters fog)
{
    float32x3 color = Shade(material = material, normalWorld = normalWorld);
    if constexpr (Opaque<M> == true)
    {
        color = ApplyFog(color = color, fog = fog);
    }
    return color;
}

groupshared float32x3 result = 0;

[stage(compute), threads(1, 1, 1)]
void Main(in uint32x3 dispatchThread : DispatchThreadId)
{
    CarPaint paint = { .Albedo = float32x3(1.0) };
    Plastic plastic = { .Roughness = 0.5 };
    FogParameters fog = { .Density = 0.25 };
    float32x3 normalWorld = float32x3(0.0, 0.0, 1.0);
    result = ShadeWithFog(paint, normalWorld, fog) + ShadeWithFog(plastic, normalWorld, fog);
}
)");
    compiler::Request options;
    for (const compiler::Target target : {compiler::Target::Spirv, compiler::Target::Wgsl, compiler::Target::Metal})
    {
        options.OutputTarget = target;
        compiler::Result result;
        const compiler::Status status = CompileLanguage(slang, files, options, result);
        INFO("target " << static_cast<int>(target) << "\n" << GetDiagnostics(result));
        CHECK(status == compiler::Status::Ready);
    }
}
