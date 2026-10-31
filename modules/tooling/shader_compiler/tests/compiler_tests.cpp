#include <ludus/tooling/shader_compiler/compiler.hpp>

#include <ludus/foundation/containers/static_array.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>

using namespace ludus::foundation;
namespace compiler = ludus::tooling::shader_compiler;
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
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    compiler::Status status = compiler::Status::Pending;
    while (status == compiler::Status::Pending && std::chrono::steady_clock::now() < deadline)
    {
        status = inoutClient.Poll(outResult);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
