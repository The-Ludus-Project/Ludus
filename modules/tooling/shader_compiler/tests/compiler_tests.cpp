#include <ludus/tooling/shader_compiler/compiler.hpp>

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/foundation/time/timers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>
#include <string_view>

using namespace ludus::foundation;
namespace compiler = ludus::tooling::shader_compiler;
namespace threading = ludus::foundation::threading;
namespace timing = ludus::foundation::time;
namespace
{
BorrowedView<const char> View(const char* text)
{
    return {text, std::strlen(text)};
}
constexpr usize FILE_COUNT = 3;
struct Fixture final
{
    StaticArray<compiler::SourceFile, FILE_COUNT> Files{};
    compiler::Request Request;
    Fixture()
    {
        Files[0] = {View("root.slang"),
                    View("#include \"surface.slang\"\nfloat4 FragmentMain() : SV_Target { return GetColor(); }")};
        Files[1] = {View("surface.slang"), View("float4 GetColor() { return float4(1,0,0,1); }")};
        Files[2] = {View("unused.slang"), View("// not included")};
        Request.Files = {Files.GetData(), Files.GetSize()};
        Request.Source = View("root.slang");
        Request.Entry = View("FragmentMain");
        Request.EntryStage = compiler::Stage::Fragment;
        Request.OutputTarget = compiler::Target::Wgsl;
        Request.Identity = 7;
    }
};
compiler::Status Await(compiler::Client& inoutClient, compiler::Result& outResult)
{
    timing::Deadline deadline;
    if (deadline.Arm(timing::Now(), timing::Seconds(10)) != timing::TimeStatus::Ok)
    {
        return compiler::Status::Pending;
    }
    compiler::Status status = compiler::Status::Pending;
    while (status == compiler::Status::Pending && deadline.IsExpired(timing::Now()) == false)
    {
        status = inoutClient.Poll(outResult);
        threading::SleepFor(timing::Milliseconds(1));
    }
    return status;
}
} // namespace
TEST_CASE("Native shader compiler reuses snapshots and invalidates included sources", "[shader][compiler]")
{
    compiler::Compiler worker;
    REQUIRE(worker.Initialize() == compiler::Status::Ready);
    Fixture fixture;
    compiler::Result first;
    REQUIRE(worker.Compile(fixture.Request, first) == compiler::Status::Ready);
    CHECK(first.Identity == fixture.Request.Identity);
    REQUIRE(first.Artifact.IsEmpty() == false);
    REQUIRE(first.SourceReadMask.GetSize() == FILE_COUNT);
    CHECK(first.SourceReadMask[0] == 1);
    CHECK(first.SourceReadMask[1] == 1);
    CHECK(first.SourceReadMask[2] == 0);
    compiler::Result cached;
    REQUIRE(worker.Compile(fixture.Request, cached) == compiler::Status::Ready);
    CHECK(first.Artifact.GetSize() == cached.Artifact.GetSize());
    CHECK(std::memcmp(first.Artifact.GetData(), cached.Artifact.GetData(), first.Artifact.GetSize()) == 0);
    fixture.Files[1].Text = View("float4 GetColor() { return float4(0,1,0,1); }");
    compiler::Result changed;
    REQUIRE(worker.Compile(fixture.Request, changed) == compiler::Status::Ready);
    CHECK(std::string_view(reinterpret_cast<const char*>(first.Artifact.GetData()), first.Artifact.GetSize()) !=
          std::string_view(reinterpret_cast<const char*>(changed.Artifact.GetData()), changed.Artifact.GetSize()));
    fixture.Files[1].Text = View("this is invalid syntax");
    REQUIRE(worker.Compile(fixture.Request, changed) == compiler::Status::CompileFailed);
    CHECK(changed.Artifact.IsEmpty() == true);
    CHECK(changed.Reflection.IsEmpty() == true);
    CHECK(changed.Diagnostics.IsEmpty() == false);
    fixture.Files[1].Text = View("float4 GetColor() { return float4(1,0,0,1); }");
    REQUIRE(worker.Compile(fixture.Request, changed) == compiler::Status::Ready);
    CHECK(std::string_view(reinterpret_cast<const char*>(first.Artifact.GetData()), first.Artifact.GetSize()) ==
          std::string_view(reinterpret_cast<const char*>(changed.Artifact.GetData()), changed.Artifact.GetSize()));
}
TEST_CASE("Native shader compiler has no ambient shader filesystem", "[shader][compiler]")
{
    compiler::Compiler worker;
    REQUIRE(worker.Initialize() == compiler::Status::Ready);
    Fixture fixture;
    fixture.Request.Files = {fixture.Files.GetData(), 1};
    compiler::Result result;
    REQUIRE(worker.Compile(fixture.Request, result) == compiler::Status::CompileFailed);
    CHECK(result.Diagnostics.IsEmpty() == false);
    fixture.Request.Source = View("../root.slang");
    CHECK(worker.Compile(fixture.Request, result) == compiler::Status::InvalidRequest);
    fixture.Request.Source = View("root.slang");
    fixture.Request.Files = {nullptr, compiler::MAX_SOURCE_FILES + 1};
    CHECK(worker.Compile(fixture.Request, result) == compiler::Status::InvalidRequest);
}
TEST_CASE("Native shader compiler emits each portable target from one snapshot", "[shader][compiler]")
{
    compiler::Compiler worker;
    REQUIRE(worker.Initialize() == compiler::Status::Ready);
    Fixture fixture;
    for (const compiler::Target target : {compiler::Target::Spirv, compiler::Target::Wgsl, compiler::Target::Metal})
    {
        fixture.Request.OutputTarget = target;
        compiler::Result result;
        REQUIRE(worker.Compile(fixture.Request, result) == compiler::Status::Ready);
        CHECK(result.Artifact.IsEmpty() == false);
        CHECK(result.Reflection.IsEmpty() == false);
    }
}
TEST_CASE("Native shader compiler compiles an entry that calls another stage entry", "[shader][compiler]")
{
    // Render-graph wrappers call renderer recipe entries. Loaded as a module, this source made
    // Slang 2026.1.2 spin forever in its entry-point parameter transform; slangc compiled it.
    compiler::Compiler worker;
    REQUIRE(worker.Initialize() == compiler::Status::Ready);
    Fixture fixture;
    fixture.Files[0].Text = View(R"(
struct Varying { float4 PositionClip : SV_Position; };
[shader("fragment")] float4 ColorMain(in Varying value) { return value.PositionClip; }
[shader("fragment")] float4 FragmentMain(Varying value) : SV_Target { return ColorMain(value); }
)");
    for (const compiler::Target target : {compiler::Target::Spirv, compiler::Target::Wgsl, compiler::Target::Metal})
    {
        fixture.Request.OutputTarget = target;
        compiler::Result result;
        REQUIRE(worker.Compile(fixture.Request, result) == compiler::Status::Ready);
        CHECK(result.Artifact.IsEmpty() == false);
        CHECK(result.Reflection.IsEmpty() == false);
    }
}
TEST_CASE("Native shader compiler supports ray queries and invalidates macro edits", "[shader][compiler]")
{
    compiler::Compiler worker;
    REQUIRE(worker.Initialize() == compiler::Status::Ready);
    Fixture fixture;
    fixture.Files[0].Text = View(R"(
#include "surface.slang"
RaytracingAccelerationStructure Scene;
RWStructuredBuffer<float4> Results;
[numthreads(1, 1, 1)]
void ComputeMain()
{
    RayDesc ray;
    ray.Origin = float3(0, 0, 0);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0;
    ray.TMax = 1;
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(Scene, RAY_FLAG_NONE, 255, ray);
    while (query.Proceed()) {}
    Results[0] = GetColor() * float(query.CommittedStatus() + 1);
}
)");
    fixture.Files[1].Text = View(R"(
#define DEBUG_COLOR
float4 GetColor()
{
#if defined(DEBUG_COLOR)
    return float4(1, 0, 0, 1);
#else
    return float4(0, 1, 0, 1);
#endif
}
)");
    fixture.Request.Entry = View("ComputeMain");
    fixture.Request.EntryStage = compiler::Stage::Compute;
    fixture.Request.OutputTarget = compiler::Target::SpirvRayQuery;
    compiler::Result first;
    const compiler::Status compiled = worker.Compile(fixture.Request, first);
    INFO(std::string_view(reinterpret_cast<const char*>(first.Diagnostics.GetData()), first.Diagnostics.GetSize()));
    REQUIRE(compiled == compiler::Status::Ready);
    REQUIRE(first.Artifact.GetSize() > 7);
    CHECK(first.Artifact[5] == 5); // SPIR-V 1.5, little-endian version word.
    CHECK(first.Artifact[6] == 1);
    fixture.Files[1].Text = View("float4 GetColor() { return float4(0,1,0,1); }");
    compiler::Result changed;
    REQUIRE(worker.Compile(fixture.Request, changed) == compiler::Status::Ready);
    CHECK(std::string_view(reinterpret_cast<const char*>(first.Artifact.GetData()), first.Artifact.GetSize()) !=
          std::string_view(reinterpret_cast<const char*>(changed.Artifact.GetData()), changed.Artifact.GetSize()));
    fixture.Request.EntryStage = compiler::Stage::Fragment;
    CHECK(worker.Compile(fixture.Request, changed) == compiler::Status::InvalidRequest);
    fixture.Request.EntryStage = compiler::Stage::Compute;
    fixture.Files[1].Text = View("invalid shader");
    CHECK(worker.Compile(fixture.Request, changed) == compiler::Status::CompileFailed);
    CHECK(changed.Artifact.IsEmpty() == true);
}

TEST_CASE("Native shader compiler separates portable and ray query session profiles", "[shader][compiler]")
{
    compiler::Compiler worker;
    REQUIRE(worker.Initialize() == compiler::Status::Ready);
    Fixture fixture;
    fixture.Files[0].Text =
        View("RWStructuredBuffer<float> Values; [numthreads(1,1,1)] void ComputeMain() { Values[0] = 1; }");
    fixture.Request.Entry = View("ComputeMain");
    fixture.Request.EntryStage = compiler::Stage::Compute;
    for (const compiler::Target target :
         {compiler::Target::SpirvRayQuery, compiler::Target::Spirv, compiler::Target::SpirvRayQuery})
    {
        fixture.Request.OutputTarget = target;
        compiler::Result result;
        REQUIRE(worker.Compile(fixture.Request, result) == compiler::Status::Ready);
        REQUIRE(result.Artifact.GetSize() > 7);
        CHECK(result.Artifact[5] == (target == compiler::Target::SpirvRayQuery ? 5 : 3));
    }
}

#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
namespace
{
bool IsSignedDxil(const compiler::Result& result)
{
    // DXBC header: code, 16-byte hash, version and total size; unsigned output has a constant hash.
    constexpr usize HEADER_BYTES = 32;
    constexpr usize HASH_OFFSET = 4;
    constexpr usize HASH_BYTES = 16;
    constexpr usize SIZE_OFFSET = 24;
    if (result.Artifact.GetSize() < HEADER_BYTES || std::memcmp(result.Artifact.GetData(), "DXBC", HASH_OFFSET) != 0)
    {
        return false;
    }
    uint32 size = 0;
    std::memcpy(&size, result.Artifact.GetData() + SIZE_OFFSET, sizeof(size));
    bool isConstant = true;
    for (usize i = 1; i < HASH_BYTES; ++i)
    {
        isConstant = isConstant == true && result.Artifact[HASH_OFFSET + i] == result.Artifact[HASH_OFFSET];
    }
    return size == result.Artifact.GetSize() && isConstant == false;
}
} // namespace
TEST_CASE("Native shader compiler emits signed DXIL and reports DXIL diagnostics", "[shader][compiler][dxil]")
{
    compiler::Compiler worker;
    REQUIRE(worker.Initialize() == compiler::Status::Ready);
    Fixture fixture;
    fixture.Request.OutputTarget = compiler::Target::Dxil;
    compiler::Result first;
    REQUIRE(worker.Compile(fixture.Request, first) == compiler::Status::Ready);
    CHECK(IsSignedDxil(first) == true);
    CHECK(first.Reflection.IsEmpty() == false);
    REQUIRE(first.SourceReadMask.GetSize() == FILE_COUNT);
    CHECK(first.SourceReadMask[1] == 1);
    // An include edit replaces the snapshot and changes the container.
    fixture.Files[1].Text = View("float4 GetColor() { return float4(0,1,0,1); }");
    compiler::Result edited;
    REQUIRE(worker.Compile(fixture.Request, edited) == compiler::Status::Ready);
    CHECK(IsSignedDxil(edited) == true);
    CHECK(std::string_view(reinterpret_cast<const char*>(first.Artifact.GetData()), first.Artifact.GetSize()) !=
          std::string_view(reinterpret_cast<const char*>(edited.Artifact.GetData()), edited.Artifact.GetSize()));
    // Syntax errors and a missing entry are diagnosed without publishing an artifact.
    fixture.Files[1].Text = View("float4 GetColor() { return }");
    REQUIRE(worker.Compile(fixture.Request, edited) == compiler::Status::CompileFailed);
    CHECK(edited.Artifact.IsEmpty() == true);
    CHECK(edited.Diagnostics.IsEmpty() == false);
    fixture.Files[1].Text = View("float4 GetColor() { return float4(1,0,0,1); }");
    fixture.Request.Entry = View("MissingMain");
    REQUIRE(worker.Compile(fixture.Request, edited) == compiler::Status::CompileFailed);
    CHECK(edited.Artifact.IsEmpty() == true);
    // DXIL sources see LUDUS_DXIL as offline builds do; portable targets in the same compiler do not.
    fixture.Files[0].Text = View("#ifndef LUDUS_DXIL\n#error expected DXIL\n#endif\n"
                                 "[numthreads(1,1,1)] void ComputeMain() {}");
    fixture.Request.Entry = View("ComputeMain");
    fixture.Request.EntryStage = compiler::Stage::Compute;
    REQUIRE(worker.Compile(fixture.Request, edited) == compiler::Status::Ready);
    CHECK(IsSignedDxil(edited) == true);
    fixture.Request.OutputTarget = compiler::Target::Spirv;
    CHECK(worker.Compile(fixture.Request, edited) == compiler::Status::CompileFailed);
}
#else
TEST_CASE("Native shader compiler reports DXIL as unavailable off Windows", "[shader][compiler][dxil]")
{
    compiler::Compiler worker;
    REQUIRE(worker.Initialize() == compiler::Status::Ready);
    Fixture fixture;
    fixture.Request.OutputTarget = compiler::Target::Dxil;
    compiler::Result result;
    CHECK(worker.Compile(fixture.Request, result) == compiler::Status::Unavailable);
    CHECK(result.Artifact.IsEmpty() == true);
    fixture.Request.OutputTarget = compiler::Target::Wgsl;
    CHECK(worker.Compile(fixture.Request, result) == compiler::Status::Ready);
}
#endif

TEST_CASE("Native shader client rejects malformed endpoints and never waits for a missing worker", "[shader][client]")
{
    compiler::Client client;
    Fixture fixture;
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    // Only local pipe names are endpoints; a missing pipe is reported without waiting.
    CHECK(client.Submit(View("C:\\worker.sock"), fixture.Request) == compiler::Status::InvalidRequest);
    CHECK(client.Submit(View("\\\\server\\pipe\\worker"), fixture.Request) == compiler::Status::InvalidRequest);
    CHECK(client.Submit(View("\\\\.\\pipe\\"), fixture.Request) == compiler::Status::InvalidRequest);
    CHECK(client.Submit(View("\\\\.\\pipe\\ludus-missing-shader-worker"), fixture.Request) ==
          compiler::Status::Unavailable);
#else
    CHECK(client.Submit(View("/nonexistent/ludus-missing-shader-worker.sock"), fixture.Request) ==
          compiler::Status::Unavailable);
#endif
    CHECK(client.Submit({"", 0}, fixture.Request) == compiler::Status::InvalidRequest);
    compiler::Result result;
    CHECK(client.Poll(result) == compiler::Status::InvalidState);
}

TEST_CASE("Native shader client copies requests and supports cancellation and recovery",
          "[shader][client][.native-client]")
{
    const char* endpointOrNull = std::getenv("LUDUS_TEST_SHADER_SOCKET");
    REQUIRE(endpointOrNull != nullptr);
    compiler::Client client;
    Fixture fixture;
    REQUIRE(client.Submit(View(endpointOrNull), fixture.Request) == compiler::Status::Pending);
    CHECK(client.Submit(View(endpointOrNull), fixture.Request) == compiler::Status::InvalidState);
    fixture.Files[1].Text = View("invalid caller mutation after Submit");
    compiler::Result result;
    result.Identity = 99;
    REQUIRE(Await(client, result) == compiler::Status::Ready);
    CHECK(result.Identity == 7);
    CHECK(result.Artifact.IsEmpty() == false);
    REQUIRE(client.Submit(View(endpointOrNull), fixture.Request) == compiler::Status::Pending);
    client.Cancel();
    CHECK(client.Poll(result) == compiler::Status::InvalidState);
    fixture.Files[1].Text = View("float4 GetColor() { return float4(0,0,1,1); }");
    fixture.Request.Identity = 8;
    REQUIRE(client.Submit(View(endpointOrNull), fixture.Request) == compiler::Status::Pending);
    REQUIRE(Await(client, result) == compiler::Status::Ready);
    CHECK(result.Identity == 8);
}
TEST_CASE("Native shader compiler reflects module declarations for LUSL headers", "[shader][compiler]")
{
    compiler::Compiler worker;
    REQUIRE(worker.Initialize() == compiler::Status::Ready);
    StaticArray<compiler::SourceFile, 2> files{};
    files[0] = {View("library.slang"), View(R"(#include "detail.slang"
static const float PI = 3.14159265f;
static const uint COUNT = 4u;
enum Mode : uint
{
    Off = 0,
    On = 7,
};
struct Pair
{
    uint2 First;
    float Values[4];
    Mode State;
};
bool IsFinite(float value) { return value == value; }
bool IsFinite(float3 value) { return all(value == value); }
void Split(uint value, out uint low, inout uint high) { low = value & 0xffffu; high += value >> 16u; }
float4 Read(Texture2D<float4> image) { return image.Load(int3(0, 0, 0)); }
float3x4 Identity() { return float3x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0); }
T Echo<T>(T value) { return value; }
)")};
    files[1] = {View("detail.slang"), View("half Halve(half value) { return value * 0.5h; }\n")};
    compiler::Request request;
    request.Files = {files.GetData(), files.GetSize()};
    request.Source = View("library.slang");
    request.Entry = View("Unused");
    compiler::DeclarationResult result;
    REQUIRE(worker.ReflectDeclarations(request, result) == compiler::Status::Ready);
    const auto text = [&result](compiler::ReflectedText span) { // ludus-style: inferred-type: lambda closure
        return std::string_view(result.Text.GetData() + span.Offset, span.Size);
    };
    const auto find = [&](std::string_view name, usize skip) { // ludus-style: inferred-type: lambda closure
        for (const compiler::ReflectedDeclaration& declaration : result.Declarations)
        {
            if (text(declaration.Name) == name && skip-- == 0)
            {
                return &declaration;
            }
        }
        return static_cast<const compiler::ReflectedDeclaration*>(nullptr);
    };
    // Included declarations are part of the module; generics are skipped.
    const compiler::ReflectedDeclaration* halveOrNull = find("Halve", 0);
    REQUIRE(halveOrNull != nullptr);
    CHECK(halveOrNull->Type.Scalar == compiler::ReflectedScalar::Float16);
    CHECK(find("Echo", 0) == nullptr);
    const compiler::ReflectedDeclaration* piOrNull = find("PI", 0);
    REQUIRE(piOrNull != nullptr);
    CHECK(piOrNull->Kind == compiler::DeclarationKind::Constant);
    CHECK(piOrNull->Type.Shape == compiler::ReflectedShape::Scalar);
    const compiler::ReflectedDeclaration* pairOrNull = find("Pair", 0);
    REQUIRE(pairOrNull != nullptr);
    REQUIRE(pairOrNull->MemberCount == 3);
    const compiler::ReflectedMember& state = result.Members[pairOrNull->FirstMember + 2];
    CHECK(state.Type.Shape == compiler::ReflectedShape::Enum);
    CHECK(text(state.Type.Name) == "Mode");
    const compiler::ReflectedDeclaration* modeOrNull = find("Mode", 0);
    REQUIRE(modeOrNull != nullptr);
    CHECK(modeOrNull->Kind == compiler::DeclarationKind::Enum);
    REQUIRE(modeOrNull->MemberCount == 2);
    CHECK(text(result.Members[modeOrNull->FirstMember + 1].Name) == "On");
    CHECK(result.Members[modeOrNull->FirstMember + 1].HasValue == true);
    CHECK(result.Members[modeOrNull->FirstMember + 1].Value == 7);
    const compiler::ReflectedDeclaration* countOrNull = find("COUNT", 0);
    REQUIRE(countOrNull != nullptr);
    CHECK(countOrNull->HasValue == true);
    CHECK(countOrNull->Value == 4);
    const compiler::ReflectedMember& first = result.Members[pairOrNull->FirstMember];
    CHECK(text(first.Name) == "First");
    CHECK(first.Type.Shape == compiler::ReflectedShape::Vector);
    CHECK(first.Type.Rows == 2);
    CHECK(first.Type.Scalar == compiler::ReflectedScalar::Uint32);
    CHECK(result.Members[pairOrNull->FirstMember + 1].Type.ArrayCount == 4);
    // Overloads are separate records, in declaration order.
    const compiler::ReflectedDeclaration* secondOrNull = find("IsFinite", 1);
    REQUIRE(secondOrNull != nullptr);
    CHECK(result.Members[secondOrNull->FirstMember].Type.Rows == 3);
    const compiler::ReflectedDeclaration* splitOrNull = find("Split", 0);
    REQUIRE(splitOrNull != nullptr);
    CHECK(splitOrNull->Type.Shape == compiler::ReflectedShape::Void);
    REQUIRE(splitOrNull->MemberCount == 3);
    CHECK(result.Members[splitOrNull->FirstMember].Direction == compiler::ReflectedDirection::In);
    CHECK(result.Members[splitOrNull->FirstMember + 1].Direction == compiler::ReflectedDirection::Out);
    CHECK(result.Members[splitOrNull->FirstMember + 2].Direction == compiler::ReflectedDirection::Inout);
    const compiler::ReflectedDeclaration* readOrNull = find("Read", 0);
    REQUIRE(readOrNull != nullptr);
    CHECK(result.Members[readOrNull->FirstMember].Type.Shape == compiler::ReflectedShape::Unsupported);
    const compiler::ReflectedDeclaration* identityOrNull = find("Identity", 0);
    REQUIRE(identityOrNull != nullptr);
    CHECK(identityOrNull->Type.Shape == compiler::ReflectedShape::Matrix);
    CHECK(identityOrNull->Type.Rows == 3);
    CHECK(identityOrNull->Type.Columns == 4);

    // Reflecting again from an edited snapshot sees the edit.
    files[1] = {View("detail.slang"), View("half Third(half value) { return value / 3.0h; }\n")};
    REQUIRE(worker.ReflectDeclarations(request, result) == compiler::Status::Ready);
    CHECK(find("Third", 0) != nullptr);
    CHECK(find("Halve", 0) == nullptr);

    files[0] = {View("library.slang"), View("float Broken( { return 0; }\n")};
    CHECK(worker.ReflectDeclarations(request, result) == compiler::Status::CompileFailed);
    CHECK(result.Diagnostics.IsEmpty() == false);
    CHECK(result.Declarations.IsEmpty() == true);
    request.Source = View("library.lusl");
    CHECK(worker.ReflectDeclarations(request, result) == compiler::Status::InvalidRequest);
}
