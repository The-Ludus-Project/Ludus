// L5a: resource queries, atomics and group barriers
// (docs/modules/tooling/shader-language.md#resources-and-bindings). Golden lowering and diagnostic
// tests; emitter_target_tests.cpp compiles the same constructs for every portable target.

#include "checker_test_support.hpp"
#include "formatter_test_support.hpp"
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
using language::test::Snapshot;

/// Bindings and `groupshared` storage the rule cases use.
constexpr std::string_view PRELUDE = R"(static const uint32 BIN_COUNT = 4;
Texture2D<float32x4> image;
RWTexture2D<float32x4> target;
StructuredBuffer<float32x4> values;
RWStructuredBuffer<uint32> counters;
RWStructuredBuffer<int32> balances;
RWStructuredBuffer<uint32x4> words;
RWStructuredBuffer<uint32> results;
groupshared uint32 total = 0;
groupshared uint32 bins[BIN_COUNT] = 0;
)";

CheckedSource Check(std::string_view body)
{
    return CheckedSource(std::string(PRELUDE) + std::string(body));
}

/// A compute entry point around `statements`.
CheckedSource CheckCompute(std::string_view statements)
{
    return Check(std::string("[stage(compute), threads(8, 1, 1)]\nvoid Main(in uint32x3 id : DispatchThreadId, in "
                             "uint32 groupIndex : GroupIndex, in uint32x3 group : GroupId)\n{\n") +
                 std::string(statements) + "}\n");
}

std::string Lower(std::string_view text)
{
    Snapshot snapshot;
    snapshot.AddFile("main.lusl", std::string(text));
    (void)snapshot.Run();
    language::LoweringResult result;
    const Status status = language::LowerToSlang(snapshot.GetRequest(), language::LoweringOptions{}, result);
    std::string diagnostics;
    for (const language::Diagnostic& diagnostic : result.Diagnostics)
    {
        diagnostics += diagnostic.Message.GetView();
        diagnostics += '\n';
    }
    INFO(diagnostics);
    REQUIRE(status == Status::Ready);
    return std::string(result.Text.GetData(), result.Text.GetSize());
}

bool Contains(const std::string& text, std::string_view part)
{
    return text.find(part) != std::string::npos;
}

void RequireCode(const CheckedSource& checked, DiagnosticCode code)
{
    INFO(checked.DescribeDiagnostics());
    CHECK(checked.GetStatus() == Status::Failed);
    CHECK(checked.FindDiagnosticOrNull(code) != nullptr);
}

void RequireReady(const CheckedSource& checked)
{
    INFO(checked.DescribeDiagnostics());
    CHECK(checked.GetStatus() == Status::Ready);
}
} // namespace

TEST_CASE("LUSL GetDimensions returns resource sizes by value", "[shader-language][shared-memory]")
{
    const CheckedSource ready = CheckCompute(R"(    uint32x2 imageSize = image.GetDimensions();
    uint32x2 targetSize = target.GetDimensions();
    uint32 count = values.GetDimensions() + counters.GetDimensions();
    results[id.x] = imageSize.x + targetSize.y + count;
)");
    RequireReady(ready);

    RequireCode(CheckCompute("    float32x2 size = image.GetDimensions();\n"), DiagnosticCode::TypeMismatch);
    RequireCode(CheckCompute("    uint32x2 size = image.GetDimensions(0);\n"), DiagnosticCode::NoMatchingOverload);
    RequireCode(Check("SamplerState imageSampler;\nuint32x2 F()\n{\n    return imageSampler.GetDimensions();\n}\n"),
                DiagnosticCode::UnknownMember);

    const std::string slang = Lower(std::string(PRELUDE) + R"([stage(compute), threads(8, 1, 1)]
void Main(in uint32x3 id : DispatchThreadId)
{
    results[id.x] = image.GetDimensions().x + values.GetDimensions() + image.GetDimensions().y;
}
)");
    CHECK(Contains(slang,
                   "uint2 _luslDimensions0(Texture2D<float4> resource)\n{\n    uint width;\n    uint height;\n"
                   "    resource.GetDimensions(width, height);\n    return uint2(width, height);\n}\n"));
    CHECK(Contains(slang,
                   "uint _luslDimensions1(StructuredBuffer<float4> resource)\n{\n    uint count;\n"
                   "    uint stride;\n    resource.GetDimensions(count, stride);\n    return count;\n}\n"));
    // One helper per resource type.
    CHECK(Contains(slang, "_luslDimensions2") == false);
    CHECK(Contains(slang,
                   "results[id.x] = (_luslDimensions0(image).x + _luslDimensions1(values)) + "
                   "_luslDimensions0(image).y;"));
}

TEST_CASE("LUSL GetDimensions of a writable resource is uniform", "[shader-language][shared-memory]")
{
    const CheckedSource checked = Check(R"(SamplerState imageSampler;
struct Interpolated
{
    float32x4 Position : ClipPosition;
    float32x2 Uv;
};
[stage(fragment)]
float32x4 FragmentMain(in Interpolated input)
{
    float32x4 color = float32x4(0.0);
    if (counters.GetDimensions() > 4 && target.GetDimensions().x > 1)
    {
        color = image.Sample(imageSampler, input.Uv);
    }
    return color;
}
)");
    RequireReady(checked);
}

TEST_CASE("LUSL atomics update buffer elements and groupshared memory", "[shader-language][shared-memory]")
{
    const CheckedSource ready = CheckCompute(R"(    uint32 original = 0;
    int32 balance = 0;
    InterlockedAdd(total, 1);
    InterlockedAdd(bins[groupIndex % BIN_COUNT], 1, out original);
    InterlockedMax(bins[0], groupIndex);
    InterlockedMin(bins[1], groupIndex, out original);
    InterlockedAnd(counters[0], 0xff);
    InterlockedOr(counters[1], 1, out original);
    InterlockedXor(counters[2], 2);
    InterlockedExchange(counters[3], id.x, out original);
    InterlockedCompareExchange(counters[4], 0, id.x, out original);
    InterlockedCompareStore(counters[5], 0, 7);
    InterlockedAdd(balances[0], -1, out balance);
    counters[6] = total + bins[2];
    total = 3;
    results[id.x] = original + uint32(balance) + counters[7];
)");
    RequireReady(ready);
    const language::SemanticModel& model = ready.GetModel();
    ludus::foundation::usize atomicCount = 0;
    for (const language::Symbol& symbol : model.Symbols)
    {
        atomicCount += symbol.IsAtomic == true ? 1 : 0;
    }
    // total, bins, counters and balances; results is written plainly.
    CHECK(atomicCount == 4);
}

TEST_CASE("LUSL atomics lower to Atomic<T> storage", "[shader-language][shared-memory]")
{
    const std::string slang = Lower(std::string(PRELUDE) + R"([stage(compute), threads(8, 1, 1)]
void Main(in uint32x3 id : DispatchThreadId, in uint32 groupIndex : GroupIndex)
{
    uint32 original = 0;
    InterlockedAdd(bins[groupIndex % BIN_COUNT], 1, out original);
    InterlockedAdd(total, 1);
    InterlockedCompareExchange(counters[0], 0, id.x, out original);
    InterlockedCompareStore(counters[1], 0, 7);
    barrier groupshared;
    counters[2] = total + bins[1];
    results[id.x] = original + counters.GetDimensions();
}
)");
    CHECK(Contains(slang, "[[vk::binding(3, 0)]] RWStructuredBuffer<Atomic<uint>> counters : register(u3);"));
    // Buffers that no atomic updates keep plain elements.
    CHECK(Contains(slang, "[[vk::binding(4, 0)]] RWStructuredBuffer<int> balances : register(u4);"));
    CHECK(Contains(slang, "[[vk::binding(6, 0)]] RWStructuredBuffer<uint> results : register(u6);"));
    CHECK(Contains(slang, "groupshared Atomic<uint> total;"));
    CHECK(Contains(slang, "groupshared Atomic<uint> bins[4];"));
    // The cooperative clears store into the atomic elements.
    CHECK(Contains(slang, "        total.store(0u);"));
    CHECK(Contains(slang, "        bins[(_luslClear / 1u) % 4u].store(0u);"));
    CHECK(Contains(slang, "    original = bins[groupIndex % BIN_COUNT].add(1u);"));
    CHECK(Contains(slang, "    total.add(1u);"));
    CHECK(Contains(slang, "    original = counters[0u].compareExchange(0u, id.x);"));
    CHECK(Contains(slang, "    counters[1u].compareExchange(0u, 7u);"));
    CHECK(Contains(slang, "    GroupMemoryBarrierWithGroupSync();"));
    CHECK(Contains(slang, "    counters[2u].store(total.load() + bins[1u].load());"));
    CHECK(Contains(slang, "uint _luslDimensions0(RWStructuredBuffer<Atomic<uint>> resource)"));
    CHECK(Contains(slang, "results[id.x] = original + _luslDimensions0(counters);"));
}

TEST_CASE("LUSL buffer exchanges compare-exchange on SPIR-V", "[shader-language][shared-memory]")
{
    // The pinned Slang crashes emitting SPIR-V 1.3 for a used buffer-element exchange.
    const std::string slang = Lower(std::string(PRELUDE) + R"([stage(compute), threads(8, 1, 1)]
void Main(in uint32x3 id : DispatchThreadId)
{
    uint32 swapped = 0;
    InterlockedExchange(counters[id.x], 5, out swapped);
    InterlockedExchange(total, swapped, out swapped);
    results[id.x] = swapped;
}
)");
    CHECK(Contains(slang, R"(    {
        uint _luslValue)"));
    CHECK(Contains(slang, "        __target_switch\n        {\n        case spirv:\n"));
    CHECK(Contains(slang, " = counters[id.x].load();"));
    CHECK(Contains(slang, " = counters[id.x].compareExchange(_luslExpected"));
    CHECK(Contains(slang, "        default:\n            swapped = counters[id.x].exchange(_luslValue"));
    // groupshared exchanges compile directly.
    CHECK(Contains(slang, "    swapped = total.exchange(swapped);"));
}

TEST_CASE("LUSL atomics on extern slang storage keep Slang's Interlocked calls", "[shader-language][shared-memory]")
{
    const std::string slang = Lower(R"(extern slang
{
    RWStructuredBuffer<uint32x4> GraphBuffer0;
};
[stage(compute), threads(8, 1, 1)]
void Main(in uint32x3 id : DispatchThreadId)
{
    uint32 previous = 0;
    InterlockedAdd(GraphBuffer0[id.x].y, 1, out previous);
    InterlockedCompareExchange(GraphBuffer0[0].x, 0, previous, out previous);
    GraphBuffer0[1].z = previous + GraphBuffer0.GetDimensions();
}
)");
    CHECK(Contains(slang, "InterlockedAdd(GraphBuffer0[id.x].y, 1u, previous);"));
    CHECK(Contains(slang, "InterlockedCompareExchange(GraphBuffer0[0u].x, 0u, previous, previous);"));
    CHECK(Contains(slang, "Atomic<") == false);
    CHECK(Contains(slang, "uint _luslDimensions0(RWStructuredBuffer<uint4> resource)"));
}

TEST_CASE("LUSL rejects invalid atomic calls with a repair", "[shader-language][shared-memory]")
{
    RequireCode(CheckCompute("    uint32 local = 0;\n    InterlockedAdd(local, 1);\n"), DiagnosticCode::InvalidAtomic);
    RequireCode(CheckCompute("    InterlockedAdd(words[0].y, 1);\n"), DiagnosticCode::InvalidAtomic);
    RequireCode(CheckCompute("    InterlockedAdd(values[0].x, 1.0);\n"), DiagnosticCode::InvalidAtomic);
    RequireCode(Check("RWTexture2D<uint32> marks;\n[stage(compute), threads(1, 1, 1)]\nvoid Main()\n{\n    "
                      "InterlockedAdd(marks[uint32x2(0, 0)], 1);\n}\n"),
                DiagnosticCode::InvalidAtomic);
    RequireCode(CheckCompute("    int32 original = 0;\n    InterlockedAdd(counters[0], 1, out original);\n"),
                DiagnosticCode::InvalidAtomic);
    RequireCode(CheckCompute("    InterlockedAdd(counters[0], value = 1);\n"), DiagnosticCode::InvalidAtomic);
    RequireCode(CheckCompute("    InterlockedAdd(counters[0], 1.5);\n"), DiagnosticCode::TypeMismatch);
    RequireCode(CheckCompute("    InterlockedExchange(counters[0], 1);\n"), DiagnosticCode::NoMatchingOverload);
    RequireCode(CheckCompute("    uint32 original = 0;\n    InterlockedCompareStore(counters[0], 0, 1, out "
                             "original);\n"),
                DiagnosticCode::NoMatchingOverload);

    const CheckedSource marked = CheckCompute("    InterlockedAdd(out total, 1);\n");
    RequireCode(marked, DiagnosticCode::InvalidAtomic);
    const language::Diagnostic* markedDiagnostic = marked.FindDiagnosticOrNull(DiagnosticCode::InvalidAtomic);
    REQUIRE(markedDiagnostic != nullptr);
    CHECK(Contains(marked.ApplyFix(markedDiagnostic->Fix), "InterlockedAdd(total, 1);"));

    const CheckedSource unmarked = CheckCompute("    uint32 original = 0;\n    InterlockedAdd(total, 1, original);\n");
    RequireCode(unmarked, DiagnosticCode::DirectionMismatch);
    const language::Diagnostic* unmarkedDiagnostic = unmarked.FindDiagnosticOrNull(DiagnosticCode::DirectionMismatch);
    REQUIRE(unmarkedDiagnostic != nullptr);
    CHECK(Contains(unmarked.ApplyFix(unmarkedDiagnostic->Fix), "InterlockedAdd(total, 1, out original);"));
}

TEST_CASE("LUSL atomic storage is used through loads, stores and atomics", "[shader-language][shared-memory]")
{
    constexpr std::string_view UPDATE = "    InterlockedAdd(total, 1);\n    InterlockedAdd(counters[0], 1);\n";
    RequireCode(CheckCompute(std::string(UPDATE) + "    counters[0] += 1;\n"), DiagnosticCode::AtomicStorageAccess);
    RequireCode(CheckCompute(std::string(UPDATE) + "    total++;\n"), DiagnosticCode::AtomicStorageAccess);
    RequireCode(Check(R"(void Read(out uint32 value)
{
    value = 1;
}
[stage(compute), threads(1, 1, 1)]
void Main()
{
    Read(out total);
    InterlockedAdd(total, 1);
}
)"),
                DiagnosticCode::AtomicStorageAccess);
    RequireCode(Check(R"(uint32 Count(in RWStructuredBuffer<uint32> buffer)
{
    return buffer[0];
}
[stage(compute), threads(1, 1, 1)]
void Main()
{
    results[0] = Count(counters);
    InterlockedAdd(counters[0], 1);
}
)"),
                DiagnosticCode::AtomicStorageAccess);
    RequireCode(CheckCompute("    InterlockedAdd(bins[0], 1);\n    uint32 copy[BIN_COUNT] = bins;\n"),
                DiagnosticCode::AtomicStorageAccess);
    // Storage that no atomic updates keeps every operation.
    RequireReady(CheckCompute("    results[0] += 1;\n    results[1]++;\n"));
}

TEST_CASE("LUSL barriers lower to group barriers", "[shader-language][shared-memory]")
{
    const CheckedSource parsed = CheckCompute("    barrier groupshared;\n    barrier all;\n");
    RequireReady(parsed);
    bool hasBarrier = false;
    for (const language::SyntaxNode& node : parsed.GetTree().Nodes)
    {
        hasBarrier = hasBarrier == true || node.Kind == language::SyntaxKind::BarrierStatement;
    }
    CHECK(hasBarrier == true);
    CHECK(language::GetSyntaxKindName(language::SyntaxKind::BarrierStatement) == "barrier-statement");
    // `barrier` stays a name outside the statement.
    RequireReady(CheckCompute("    uint32 barrier = 1;\n    results[0] = barrier;\n"));

    const std::string slang = Lower(std::string(PRELUDE) + R"([stage(compute), threads(8, 1, 1)]
void Main(in uint32 groupIndex : GroupIndex)
{
    bins[groupIndex % BIN_COUNT] = groupIndex;
    barrier groupshared;
    results[groupIndex] = bins[0];
    barrier all;
}
)");
    CHECK(Contains(slang, "    GroupMemoryBarrierWithGroupSync();\n"));
    CHECK(Contains(slang, "    AllMemoryBarrierWithGroupSync();\n"));
}

TEST_CASE("LUSL barriers need uniform control flow in compute stages", "[shader-language][shared-memory]")
{
    RequireCode(CheckCompute("    if (groupIndex == 0)\n    {\n        barrier groupshared;\n    }\n"),
                DiagnosticCode::NonUniformBarrier);
    RequireCode(Check(R"(void Synchronize()
{
    barrier all;
}
[stage(compute), threads(8, 1, 1)]
void Main(in uint32x3 id : DispatchThreadId)
{
    for (uint32 i = 0; i < id.x; ++i)
    {
        Synchronize();
    }
}
)"),
                DiagnosticCode::NonUniformBarrier);
    // An atomic's original value reads shared memory, so it varies.
    RequireCode(CheckCompute("    uint32 original = 0;\n    InterlockedAdd(total, 1, out original);\n    if (original "
                             "== 0)\n    {\n        barrier groupshared;\n    }\n"),
                DiagnosticCode::NonUniformBarrier);
    // The group's id is uniform within the group.
    RequireReady(CheckCompute("    if (group.x == 0)\n    {\n        barrier groupshared;\n    }\n"));

    const CheckedSource message =
        CheckCompute("    if (groupIndex == 0)\n    {\n        barrier groupshared;\n    }\n");
    const language::Diagnostic* diagnostic = message.FindDiagnosticOrNull(DiagnosticCode::NonUniformBarrier);
    REQUIRE(diagnostic != nullptr);
    INFO(diagnostic->Message.GetView());
    CHECK(Contains(std::string(diagnostic->Message.GetView()), "groupIndex"));
    CHECK(Contains(std::string(diagnostic->Message.GetView()), "move the barrier out of the branch"));

    RequireCode(Check(R"(void Synchronize()
{
    barrier groupshared;
}
struct Interpolated
{
    float32x4 Position : ClipPosition;
};
[stage(fragment)]
float32x4 FragmentMain(in Interpolated input)
{
    Synchronize();
    return input.Position;
}
)"),
                DiagnosticCode::InvalidBarrier);
}

TEST_CASE("LUSL formatter keeps barrier statements", "[shader-language][shared-memory]")
{
    Status status = Status::InvalidRequest;
    const std::string source = "void F()\n{\n    barrier groupshared;\n    barrier all;\n}\n";
    const std::string formatted = language::test::FormatText(source, status);
    REQUIRE(status == Status::Ready);
    CHECK(formatted == source);
}
