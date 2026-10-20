// Serial-oracle tests for texture decoding on a caller-owned JobSystem: the importer
// must cook identical bundles, reports and input digests for every worker count and
// without a job system (BestEffortOptions::JobsOrNull).

#include "import_test_support.h"

#include <ludus/content/scene_cook/best_effort_import.h>
#include <ludus/content/scene_format/limits.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/threading/jobs.hpp>

#include "importers/import_diagnostics.h"
#include "importers/scene_ir.h"
#include "importers/texture_decode_queue.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>

namespace
{
using namespace ludus::scene_import_tests;
using namespace ludus::content::scene_cook;
using namespace ludus::content::scene_cook::importers;
namespace threading = ludus::foundation::threading;
using ludus::content::Status;
using ludus::foundation::core::int32;
using ludus::foundation::core::uint32;
using ludus::foundation::core::usize;

/// Worker budgets compared with the serial oracle: the zero-worker executor, one
/// worker, batches smaller than the plan, and the batch cap (seven workers and the caller).
constexpr usize WORKER_BUDGET_COUNT = 4;
const ludus::foundation::StaticArray<uint32, WORKER_BUDGET_COUNT> WORKER_BUDGETS{0, 1, 3, 7};

/// A PNG signature followed by bytes that are not a valid PNG stream.
constexpr std::string_view BROKEN_PNG = "\x89PNG\r\n\x1a\nbroken";

std::string_view GetView(const ludus::foundation::Array<ludus::foundation::core::uint8>& bytes)
{
    return {reinterpret_cast<const char*>(bytes.GetData()), bytes.GetSize()};
}

void CopyTexture(const ScratchDirectory& scratch, std::string_view fixture, std::string_view name)
{
    std::filesystem::copy_file(GetFixture(fixture), scratch.Path / "textures" / name);
}

/// Append one material and a triangle that uses it.
void AddMaterial(std::string& inoutWavefront, std::string& inoutMtl, int32 index, std::string_view statements)
{
    const std::string name = "M" + std::to_string(index);
    inoutMtl += "newmtl " + name + "\n" + std::string(statements);
    const std::string x = std::to_string(index * 2);
    inoutWavefront += "v " + x + " 0 0\nv " + x + ".5 0 0\nv " + x + " 1 0\nusemtl " + name + "\nf -3 -2 -1\n";
}

void Import(const std::string& entry, threading::JobSystem* jobsOrNull, BestEffortResult& outResult)
{
    BestEffortOptions options;
    options.JobsOrNull = jobsOrNull;
    CookError error;
    REQUIRE(ImportObjBestEffort(entry, options, outResult, error) == true);
}

void RequireSameImport(const BestEffortResult& expected, const BestEffortResult& actual)
{
    // Compare as booleans: a mismatch would otherwise print megabytes of bundle bytes.
    const bool isBundleSame = GetView(expected.Bundle) == GetView(actual.Bundle);
    CHECK(isBundleSame == true);
    CHECK(GetView(expected.Report) == GetView(actual.Report));
    REQUIRE(expected.Inputs.GetSize() == actual.Inputs.GetSize());
    REQUIRE(expected.InputDigests.GetSize() == actual.InputDigests.GetSize());
    for (usize i = 0; i < expected.Inputs.GetSize(); ++i)
    {
        CHECK(expected.Inputs[i].GetView() == actual.Inputs[i].GetView());
        CHECK(expected.InputDigests[i] == actual.InputDigests[i]);
    }
}

/// Import entry serially, then on each worker budget, and require identical outputs.
void RequireWorkerCountIndependence(const std::string& entry, BestEffortResult& outSerial)
{
    Import(entry, nullptr, outSerial);
    for (const uint32 workers : WORKER_BUDGETS)
    {
        INFO("workers " << workers);
        threading::JobSystem system;
        REQUIRE(system.Initialize({ .WorkerCount = workers }) == threading::JobStatus::Ok);
        BestEffortResult parallel;
        Import(entry, &system, parallel);
        RequireSameImport(outSerial, parallel);
    }
}

bool HasReportCode(const BestEffortResult& result, std::string_view code)
{
    return GetView(result.Report).find("\"code\": \"" + std::string(code) + "\"") != std::string_view::npos;
}

/// Cancellation that answers false for a fixed number of polls, then true.
struct CountdownCancel final
{
    std::atomic<int32> Remaining{0};
};

bool RequestAfterCountdown(void* context) noexcept
{
    return static_cast<CountdownCancel*>(context)->Remaining.fetch_sub(1) <= 0;
}

threading::JobOutcome SucceedImmediately(void*) noexcept
{
    return threading::JobOutcome::Succeeded;
}

/// Decode texture inline, without a job system, as the reference result.
TextureDecode DecodeSerially(const SceneIr& scene, std::string_view root, uint32 texture, bool shouldHash)
{
    TextureDecodeQueue serial(scene, root, nullptr, nullptr);
    TextureDecode decode;
    REQUIRE(serial.Take(texture, shouldHash, decode) == Status::Ok);
    return decode;
}

void RequireSameDecode(const TextureDecode& expected, const TextureDecode& actual)
{
    CHECK(expected.Read == actual.Read);
    CHECK(expected.IsFileRead == actual.IsFileRead);
    CHECK(expected.ReadDigest == actual.ReadDigest);
    CHECK(expected.Decoded == actual.Decoded);
    CHECK(expected.Message == actual.Message);
    CHECK(expected.Image.Width == actual.Image.Width);
    CHECK(expected.Image.Height == actual.Image.Height);
    CHECK(GetView(expected.Image.Texels) == GetView(actual.Image.Texels));
}

/// Scene IR with file, embedded, missing and undecodable texture images below root.
void MakeQueueScene(const ScratchDirectory& scratch, SceneIr& outScene)
{
    std::filesystem::create_directories(scratch.Path / "textures");
    CopyTexture(scratch, "obj/room/textures/checker.png", "checker.png");
    CopyTexture(scratch, "obj/room/textures/normal.png", "normal.png");
    CopyTexture(scratch, "obj/cutout/textures/leaf.png", "leaf.png");
    WriteFile(scratch.Path / "textures/broken.png", BROKEN_PNG);
    for (const std::string_view file :
         {"textures/checker.png", "textures/normal.png", "textures/missing.png", "textures/broken.png", ""})
    {
        IrTexture* textureOrNull = outScene.Textures.TryAddInPlace();
        REQUIRE(textureOrNull != nullptr);
        REQUIRE(textureOrNull->File.TryAssign(file) == ludus::foundation::StringStatus::Ok);
    }
    // The last texture is an embedded copy of leaf.png.
    IrEmbeddedImage* embeddedOrNull = outScene.EmbeddedImages.TryAddInPlace();
    REQUIRE(embeddedOrNull != nullptr);
    REQUIRE(embeddedOrNull->DecoderName.TryAssign("embedded.png") == ludus::foundation::StringStatus::Ok);
    const std::string leaf = ReadFileText(scratch.Path / "textures/leaf.png");
    REQUIRE(embeddedOrNull->Bytes.TryAddRange(AsBytes(leaf)) == true);
    outScene.Textures[outScene.Textures.GetSize() - 1].EmbeddedImage = 0;
}
} // namespace

TEST_CASE("Texture decoding cooks identical outputs for every worker count", "[scene_cook][obj][threading]")
{
    ScratchDirectory scratch;
    std::filesystem::create_directories(scratch.Path / "textures");
    CopyTexture(scratch, "obj/room/textures/checker.png", "checker.png");
    CopyTexture(scratch, "obj/room/textures/normal.png", "normal.png");
    CopyTexture(scratch, "obj/room/textures/rough.png", "rough.png");
    CopyTexture(scratch, "obj/cutout/textures/leaf.png", "leaf.png");
    CopyTexture(scratch, "obj/cutout/textures/leaf_mask.png", "leaf_mask.png");
    WriteFile(scratch.Path / "textures/broken.png", BROKEN_PNG);
    // Every map kind, shared images, unreadable and undecodable images, repeated so the
    // plan spans several batches for every budget.
    constexpr usize MATERIAL_KIND_COUNT = 10;
    const ludus::foundation::StaticArray<std::string_view, MATERIAL_KIND_COUNT> kinds{
        "Kd 1 1 1\nmap_Kd textures/checker.png\n",
        "Kd 1 1 1\nmap_Kd textures/leaf.png\nmap_d textures/leaf_mask.png\n",
        "Kd 0.5 0.5 0.5\nmap_Kd textures/checker.png\nnorm textures/normal.png\nPr 0.5\n"
        "map_Pr -imfchan g textures/rough.png\nPm 1\nmap_Pm textures/rough.png\n",
        "Kd 0.2 0.2 0.2\nKe 1 1 1\nmap_Ke textures/checker.png\n",
        "Kd 1 1 1\nmap_Kd textures/missing.png\n",
        "Kd 1 1 1\nmap_Kd textures/broken.png\n",
        "Kd 0.3 0.3 0.3\nnorm textures/broken.png\nPr 0.5\nmap_Pr textures/rough.png\n",
        "Kd 0.2 0.2 0.2\nKe 2 2 2\nmap_Ke textures/missing.png\n",
        "Kd 1 0 0\nmap_d textures/missing.png\n",
        "Kd 0 1 0\nmap_Kd textures/leaf.png\nnorm textures/normal.png\nKe 1 0 0\nmap_Ke textures/leaf.png\n",
    };
    constexpr int32 REPEATS = 3;
    std::string wavefront = "mtllib m.mtl\n";
    std::string mtl;
    int32 index = 0;
    for (int32 repeat = 0; repeat < REPEATS; ++repeat)
    {
        for (const std::string_view kind : kinds)
        {
            AddMaterial(wavefront, mtl, index++, kind);
        }
    }
    WriteFile(scratch.Path / "scene.obj", wavefront);
    WriteFile(scratch.Path / "m.mtl", mtl);
    BestEffortResult serial;
    RequireWorkerCountIndependence((scratch.Path / "scene.obj").string(), serial);
    CHECK(HasReportCode(serial, codes::TEXTURE_UNREADABLE) == true);
    CHECK(serial.Summary.CookedTextures > serial.Summary.CookedMaterials); // Surface and emission maps.
}

TEST_CASE("Texture decoding stays serial-identical where decode failures shift the texture limit",
          "[scene_cook][obj][threading]")
{
    namespace format = ludus::content::scene_format;
    // Three free texture slots after the base swatches. The first two slots' only surface
    // source fails, so the plan, which assumes success, expects the limit two maps early:
    // later slots take their maps off the plan and are decoded inline.
    constexpr int32 MATERIAL_COUNT = static_cast<int32>(format::IMPORT_LIMITS.Textures) - 3;
    constexpr int32 FAILING_SLOTS = 2;
    ScratchDirectory scratch;
    std::filesystem::create_directories(scratch.Path / "textures");
    CopyTexture(scratch, "obj/room/textures/normal.png", "normal.png");
    CopyTexture(scratch, "obj/room/textures/rough.png", "rough.png");
    WriteFile(scratch.Path / "textures/broken.png", BROKEN_PNG);
    std::string wavefront = "mtllib m.mtl\n";
    std::string mtl;
    for (int32 i = 0; i < MATERIAL_COUNT; ++i)
    {
        AddMaterial(wavefront,
                    mtl,
                    i,
                    i < FAILING_SLOTS ? "Kd 0.5 0.5 0.5\nnorm textures/broken.png\n"
                                      : "Kd 0.5 0.5 0.5\nnorm textures/normal.png\nPr 0.5\n"
                                        "map_Pr -imfchan g textures/rough.png\nPm 1\nmap_Pm textures/rough.png\n");
    }
    WriteFile(scratch.Path / "scene.obj", wavefront);
    WriteFile(scratch.Path / "m.mtl", mtl);
    BestEffortResult serial;
    RequireWorkerCountIndependence((scratch.Path / "scene.obj").string(), serial);
    CHECK(serial.Summary.CookedTextures == format::IMPORT_LIMITS.Textures);
    CHECK(HasReportCode(serial, codes::TEXTURE_APPROXIMATED) == true);
    CHECK(HasReportCode(serial, codes::NORMALMAP_DROPPED) == true);
}

TEST_CASE("Texture decode queue matches inline decoding in and out of planned order", "[scene_cook][threading]")
{
    ScratchDirectory scratch;
    SceneIr scene;
    MakeQueueScene(scratch, scene);
    const std::string root = scratch.Path.string();
    const uint32 textureCount = static_cast<uint32>(scene.Textures.GetSize());
    for (const uint32 workers : WORKER_BUDGETS)
    {
        INFO("workers " << workers);
        threading::JobSystem system;
        REQUIRE(system.Initialize({ .WorkerCount = workers }) == threading::JobStatus::Ok);
        TextureDecodeQueue queue(scene, root, &system, nullptr);
        // Plan every texture twice, with and without a digest, then take them in order
        // except for one skipped and one swapped pair.
        for (uint32 texture = 0; texture < textureCount; ++texture)
        {
            REQUIRE(queue.TryPlan(texture, true) == true);
            REQUIRE(queue.TryPlan(texture, false) == true);
        }
        constexpr usize TAKE_COUNT = 9;
        const ludus::foundation::StaticArray<uint32, TAKE_COUNT> order{0, 0, 1, 2, 1, 3, 3, 4, 2};
        const ludus::foundation::StaticArray<bool, TAKE_COUNT>
            hashes{true, false, true, true, false, true, false, true, false};
        for (usize i = 0; i < TAKE_COUNT; ++i)
        {
            INFO("take " << i);
            TextureDecode decode;
            REQUIRE(queue.Take(order[i], hashes[i], decode) == Status::Ok);
            RequireSameDecode(DecodeSerially(scene, root, order[i], hashes[i]), decode);
        }
    }
    const TextureDecode checker = DecodeSerially(scene, root, 0, true);
    CHECK(checker.IsFileRead == true);
    CHECK(checker.Decoded == ImageResult::Ok);
    CHECK(checker.ReadDigest == ludus::content::Hash(AsBytes(ReadFileText(scratch.Path / "textures/checker.png"))));
    CHECK(DecodeSerially(scene, root, 0, false).ReadDigest == ludus::content::Digest{});
    const TextureDecode missing = DecodeSerially(scene, root, 2, true);
    CHECK(missing.Read == Status::NotFound);
    CHECK(missing.Message == "cannot read the image file");
    CHECK(DecodeSerially(scene, root, 3, true).Decoded == ImageResult::Invalid);
    const TextureDecode embedded = DecodeSerially(scene, root, 4, true);
    CHECK(embedded.IsFileRead == false);
    CHECK(embedded.Decoded == ImageResult::Ok);
}

TEST_CASE("Texture decode queue decodes inline when the job system is busy", "[scene_cook][threading]")
{
    ScratchDirectory scratch;
    SceneIr scene;
    MakeQueueScene(scratch, scene);
    const std::string root = scratch.Path.string();
    threading::JobSystem system;
    REQUIRE(system.Initialize({ .WorkerCount = 2 }) == threading::JobStatus::Ok);
    // Another graph stays admitted until its Wait, so the queue's Submit is refused.
    threading::JobGraph other;
    threading::JobHandle handle;
    REQUIRE(other.Initialize({ .MaxJobs = 1, .MaxDependencies = 0 }) == threading::JobStatus::Ok);
    REQUIRE(other.Add(SucceedImmediately, nullptr, handle) == threading::JobStatus::Ok);
    REQUIRE(other.Seal() == threading::JobStatus::Ok);
    REQUIRE(system.Submit(other) == threading::JobStatus::Ok);
    {
        TextureDecodeQueue queue(scene, root, &system, nullptr);
        for (uint32 texture = 0; texture < scene.Textures.GetSize(); ++texture)
        {
            REQUIRE(queue.TryPlan(texture, true) == true);
        }
        for (uint32 texture = 0; texture < scene.Textures.GetSize(); ++texture)
        {
            TextureDecode decode;
            REQUIRE(queue.Take(texture, true, decode) == Status::Ok);
            RequireSameDecode(DecodeSerially(scene, root, texture, true), decode);
        }
    }
    threading::JobOutcome outcome = threading::JobOutcome::Pending;
    REQUIRE(system.Wait(outcome) == threading::JobStatus::Ok);
    CHECK(outcome == threading::JobOutcome::Succeeded);
}

TEST_CASE("Texture decoding honours cancellation before reading images", "[scene_cook][threading]")
{
    ScratchDirectory scratch;
    SceneIr scene;
    MakeQueueScene(scratch, scene);
    const std::string root = scratch.Path.string();
    threading::JobSystem system;
    REQUIRE(system.Initialize({ .WorkerCount = 1 }) == threading::JobStatus::Ok);
    for (threading::JobSystem* jobsOrNull : {static_cast<threading::JobSystem*>(nullptr), &system})
    {
        CountdownCancel countdown;
        countdown.Remaining = 1;
        const Cancellation cancel{RequestAfterCountdown, &countdown};
        TextureDecodeQueue queue(scene, root, jobsOrNull, &cancel);
        REQUIRE(queue.TryPlan(0, true) == true);
        REQUIRE(queue.TryPlan(1, true) == true);
        TextureDecode decode;
        CHECK(queue.Take(0, true, decode) == Status::Ok);
        CHECK(queue.Take(1, true, decode) == Status::Cancelled);
    }

    // The importer polls once more per texture image, and reports a cancellation there.
    WriteFile(scratch.Path / "scene.obj", "mtllib m.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl M\nf 1 2 3\n");
    const std::string entry = (scratch.Path / "scene.obj").string();
    for (threading::JobSystem* jobsOrNull : {static_cast<threading::JobSystem*>(nullptr), &system})
    {
        BestEffortOptions options;
        options.JobsOrNull = jobsOrNull;
        const auto countPolls = [&](std::string_view mtl) {
            WriteFile(scratch.Path / "m.mtl", mtl);
            constexpr int32 POLL_ALLOWANCE = 1000000;
            CountdownCancel counter;
            counter.Remaining = POLL_ALLOWANCE;
            const Cancellation counting{RequestAfterCountdown, &counter};
            BestEffortResult result;
            CookError error;
            REQUIRE(ImportObjBestEffort(entry, options, result, error, &counting) == true);
            return POLL_ALLOWANCE - counter.Remaining.load();
        };
        const int32 untexturedPolls = countPolls("newmtl M\nKd 1 1 1\n");
        const int32 texturedPolls = countPolls("newmtl M\nmap_Kd textures/checker.png\n");
        REQUIRE(texturedPolls == untexturedPolls + 1);
        // Cancel at the texture's poll: the last one, since none follows the texture stage.
        CountdownCancel countdown;
        countdown.Remaining = texturedPolls - 1;
        const Cancellation cancel{RequestAfterCountdown, &countdown};
        BestEffortResult cancelled;
        CookError error;
        CHECK(ImportObjBestEffort(entry, options, cancelled, error, &cancel) == false);
        CHECK(error.Result == Status::Cancelled);
        CHECK(cancelled.Bundle.IsEmpty() == true);
    }
}
