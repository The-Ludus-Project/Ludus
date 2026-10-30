// ludus-cook: native replacement for the retired Python scene cooker and the
// `ludus scene` CLI commands.
//
//   ludus-cook cook SOURCE OUTPUT
//   ludus-cook pbrt ENTRY.pbrt OUTPUT [--workers N]  (best-effort pbrt-v4 import)
//   ludus-cook pbrt ENTRY.pbrt --dry-run
//   ludus-cook gltf ENTRY.{gltf,glb} {OUTPUT|--dry-run} [--workers N]  (best-effort glTF 2.0)
//   ludus-cook obj ENTRY.obj {OUTPUT|--dry-run} [--workers N]          (best-effort OBJ/MTL)
//   ludus-cook import ENTRY {OUTPUT|--dry-run} [--workers N]  (format detected from content)
//   ludus-cook scene new      PROJECT LEVEL [--name NAME] [--json]
//   ludus-cook scene open     PROJECT LEVEL [--json]
//   ludus-cook scene import   PROJECT LEVEL SOURCE [--workers N] [--json]
//   ludus-cook scene dump     PROJECT LEVEL [--json]
//   ludus-cook scene assemble PROJECT LEVEL OUTPUT [--json]
//   ludus-cook scene publish  PROJECT LEVEL OUTPUT [--json]
//   ludus-cook identity migrate ROOT MANIFEST WORLD...
//   ludus-cook identity apply   ROOT MANIFEST SCHEMA WORLD OUTPUT
//
// `cook` writes OUTPUT/scene.luscn, OUTPUT/manifest.json and OUTPUT/LICENSE.txt
// and prints the manifest. `pbrt`, `gltf`, `obj` and `import` write
// OUTPUT/scene.luscn, OUTPUT/import-report.json and OUTPUT/LICENSE.txt and print
// the report; --dry-run only parses and plans and prints the report. `import`
// picks the importer from the entry's bytes, falling back to its extension. --workers N
// decodes texture images on N job workers plus the calling thread (default 0: serial);
// the output bytes do not depend on N. Every `scene` command prints the level's
// dump (the level, its exact resident closure and byte total); --json prints it on one line with "schema": 1, or the
// error as {"schema": 1, "error": {...}}. `identity migrate` mints UUIDs once for version-1 World documents and
// atomically creates MANIFEST; it never replaces an existing manifest or edits a World. `identity apply` replays
// MANIFEST to create the version-2 World record OUTPUT, minting nothing; it never replaces an existing file. Paths are
// relative to ROOT (docs/architecture/object-handles-uuid.md#stage-3-uuid-authoring-records). Exit status: 0 success, 1
// failure, 2 usage, 3 missing host facilities, 130 cancelled.

#include <ludus/content/identity_records/migration_manifest.h>
#include <ludus/content/identity_records/world_records.h>
#include <ludus/content/scene_cook/best_effort_import.h>
#include <ludus/content/scene_cook/scene_assets.h>
#include <ludus/content/scene_cook/scene_cook.h>
#include <ludus/foundation/base/parse_number.hpp>
#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/foundation/threading/jobs.hpp>
#include <ludus/platform/entropy.h>

#include <string_view>
#include <unistd.h>

namespace
{
using ludus::foundation::Array;
using ludus::foundation::core::uint32;
using ludus::foundation::core::uint64;
using ludus::foundation::core::uint8;
using ludus::foundation::core::usize;
namespace threading = ludus::foundation::threading;
using namespace ludus::content::scene_cook;

constexpr int EXIT_OK = 0;
constexpr int EXIT_FAILED = 1;
constexpr int EXIT_USAGE = 2;
constexpr int EXIT_UNAVAILABLE = 3;
constexpr int EXIT_CANCELLED = 130;

void WriteAll(int descriptor, std::string_view text) noexcept
{
    while (text.empty() == false)
    {
        const ssize_t written = write(descriptor, text.data(), text.size());
        if (written <= 0)
        {
            return;
        }
        text.remove_prefix(static_cast<usize>(written));
    }
}

[[nodiscard]] std::string_view GetView(const Array<uint8>& bytes) noexcept
{
    return std::string_view(reinterpret_cast<const char*>(bytes.GetData()), bytes.GetSize());
}

[[nodiscard]] std::span<const uint8> GetSpan(const Array<uint8>& bytes) noexcept
{
    return std::span<const uint8>(bytes.GetData(), bytes.GetSize());
}

int PrintUsage() noexcept
{
    WriteAll(2,
             "usage: ludus-cook cook SOURCE OUTPUT\n"
             "       ludus-cook cook-materials SOURCE OUTPUT\n"
             "       ludus-cook pbrt ENTRY.pbrt {OUTPUT|--dry-run} [--workers N]\n"
             "       ludus-cook gltf ENTRY.{gltf,glb} {OUTPUT|--dry-run} [--workers N]\n"
             "       ludus-cook obj ENTRY.obj {OUTPUT|--dry-run} [--workers N]\n"
             "       ludus-cook import ENTRY {OUTPUT|--dry-run} [--workers N]\n"
             "       ludus-cook scene new PROJECT LEVEL [--name NAME] [--json]\n"
             "       ludus-cook scene {open|dump} PROJECT LEVEL [--json]\n"
             "       ludus-cook scene import PROJECT LEVEL SOURCE [--workers N] [--json]\n"
             "       ludus-cook scene {assemble|publish} PROJECT LEVEL OUTPUT [--json]\n"
             "       ludus-cook identity migrate ROOT MANIFEST WORLD...\n"
             "       ludus-cook identity apply ROOT MANIFEST SCHEMA WORLD OUTPUT\n");
    return EXIT_USAGE;
}

/// Write one JSON string literal (ASCII-escaped) for error reports.
void WriteJsonString(std::string_view text) noexcept
{
    static constexpr char HEX[] = "0123456789abcdef";
    WriteAll(1, "\"");
    for (const char item : text)
    {
        const uint8 value = static_cast<uint8>(item);
        if (item == '"' || item == '\\')
        {
            const char escaped[2] = {'\\', item};
            WriteAll(1, std::string_view(escaped, 2));
        }
        else if (value < 0x20)
        {
            const char escaped[6] = {'\\', 'u', '0', '0', HEX[value >> 4], HEX[value & 0xf]};
            WriteAll(1, std::string_view(escaped, 6));
        }
        else
        {
            WriteAll(1, std::string_view(&item, 1));
        }
    }
    WriteAll(1, "\"");
}

int ReportFailure(const AssetError& error, bool isJson) noexcept
{
    if (isJson == true)
    {
        WriteAll(1, "{\"schema\": 1, \"error\": {\"code\": ");
        WriteJsonString(GetAssetErrorCodeName(error.Code));
        WriteAll(1, ", \"message\": ");
        WriteJsonString(error.Message.View());
        WriteAll(1, "}}\n");
    }
    else
    {
        WriteAll(2, "error [");
        WriteAll(2, GetAssetErrorCodeName(error.Code));
        WriteAll(2, "]: ");
        WriteAll(2, error.Message.View());
        WriteAll(2, "\n");
    }
    if (error.Code == AssetErrorCode::InstallCancelled)
    {
        return EXIT_CANCELLED;
    }
    return error.Code == AssetErrorCode::MissingTools ? EXIT_UNAVAILABLE : EXIT_FAILED;
}

int RunCook(int argc, char** argv) noexcept
{
    constexpr usize OUTPUT_ARGUMENT = 3;
    if (argc != 4)
    {
        return PrintUsage();
    }
    CookResult result;
    CookError error;
    const bool shouldPreserveMaterials = std::string_view(argv[1]) == "cook-materials";
    const bool isCooked =
        shouldPreserveMaterials == true ? CookMaterials(argv[2], result, error) : Cook(argv[2], result, error);
    if (isCooked == false || WriteCook(argv[OUTPUT_ARGUMENT], result, error) == false)
    {
        WriteAll(2, "conversion failed: ");
        WriteAll(2, error.Message.View());
        WriteAll(2, "\n");
        return EXIT_FAILED;
    }
    WriteAll(1, GetView(result.Manifest));
    return EXIT_OK;
}

/// Largest `--workers` value: JobSystemConfig::WorkerCount's range.
constexpr uint64 MAX_DECODE_WORKERS = 64;

/// Parse the value of `--workers`.
[[nodiscard]] bool ParseWorkers(std::string_view text, uint32& outWorkers) noexcept
{
    uint64 value = 0;
    if (ludus::foundation::ParseUint64(text.data(), text.size(), value) !=
            ludus::foundation::NumberParseStatus::Success ||
        value > MAX_DECODE_WORKERS)
    {
        return false;
    }
    outWorkers = static_cast<uint32>(value);
    return true;
}

/// Start the texture decode job system that `--workers` asks for; zero workers keeps it
/// uninitialized so the importer decodes serially.
[[nodiscard]] bool StartDecodeWorkers(uint32 workers, threading::JobSystem& outSystem) noexcept
{
    if (workers == 0)
    {
        return true;
    }
    if (outSystem.Initialize({ .WorkerCount = workers }) != threading::JobStatus::Ok)
    {
        WriteAll(2, "cannot start the texture decode workers\n");
        return false;
    }
    return true;
}

/// Signature shared by the best-effort importers.
using BestEffortImporter = bool (*)(std::string_view entryFile,
                                    const BestEffortOptions& options,
                                    BestEffortResult& outResult,
                                    CookError& outError,
                                    const Cancellation* cancelOrNull) noexcept;

int RunBestEffort(int argc, char** argv, BestEffortImporter importer) noexcept
{
    // ludus-cook FORMAT ENTRY {OUTPUT|--dry-run} [--workers N]
    constexpr int ARGUMENT_COUNT = 4;
    constexpr int WORKERS_FLAG_INDEX = ARGUMENT_COUNT;
    constexpr int WORKERS_VALUE_INDEX = WORKERS_FLAG_INDEX + 1;
    constexpr int ARGUMENT_COUNT_WITH_WORKERS = WORKERS_VALUE_INDEX + 1;
    uint32 workers = 0;
    const bool hasWorkers = argc == ARGUMENT_COUNT_WITH_WORKERS;
    if ((argc != ARGUMENT_COUNT && hasWorkers == false) ||
        (hasWorkers == true && (std::string_view(argv[WORKERS_FLAG_INDEX]) != "--workers" ||
                                ParseWorkers(argv[WORKERS_VALUE_INDEX], workers) == false)))
    {
        return PrintUsage();
    }
    threading::JobSystem decodeJobs;
    if (StartDecodeWorkers(workers, decodeJobs) == false)
    {
        return EXIT_UNAVAILABLE;
    }
    BestEffortOptions options;
    options.IsDryRun = std::string_view(argv[3]) == "--dry-run";
    options.JobsOrNull = workers == 0 ? nullptr : &decodeJobs;
    BestEffortResult result;
    CookError error;
    const bool isImported = importer(argv[2], options, result, error, nullptr);
    WriteAll(1, GetView(result.Report));
    if (isImported == false || (options.IsDryRun == false && WriteBestEffortImport(argv[3], result, error) == false))
    {
        WriteAll(2, "import failed: ");
        WriteAll(2, error.Message.View());
        WriteAll(2, "\n");
        return error.Result == ludus::content::Status::Cancelled ? EXIT_CANCELLED : EXIT_FAILED;
    }
    return EXIT_OK;
}

/// Parsed `scene` command line.
struct SceneCommand
{
    std::string_view Action;
    std::string_view Project;
    std::string_view Level;
    std::string_view Operand;
    std::string_view Name = "Scene";
    uint32 Workers = 0;
    bool IsJson = false;
};

[[nodiscard]] bool ParseScene(int argc, char** argv, SceneCommand& outCommand) noexcept
{
    std::string_view positional[4];
    usize count = 0;
    for (int i = 2; i < argc; ++i)
    {
        const std::string_view argument = argv[i];
        if (argument == "--json")
        {
            outCommand.IsJson = true;
        }
        else if (argument == "--name" && i + 1 < argc)
        {
            outCommand.Name = argv[++i];
        }
        else if (argument == "--workers" && i + 1 < argc)
        {
            if (ParseWorkers(argv[++i], outCommand.Workers) == false)
            {
                return false;
            }
        }
        else if (count < 4 && argument.starts_with("--") == false)
        {
            positional[count++] = argument;
        }
        else
        {
            return false;
        }
    }
    if (count < 3)
    {
        return false;
    }
    outCommand.Action = positional[0];
    outCommand.Project = positional[1];
    outCommand.Level = positional[2];
    outCommand.Operand = positional[3];
    const std::string_view action = outCommand.Action;
    const bool hasOperand = action == "import" || action == "assemble" || action == "publish";
    const bool isKnown = hasOperand == true || action == "new" || action == "open" || action == "dump";
    if (action != "import" && outCommand.Workers != 0)
    {
        return false;
    }
    return isKnown == true && (count == 4) == hasOperand && (action == "new" || outCommand.Name == "Scene");
}

int RunScene(int argc, char** argv) noexcept
{
    SceneCommand command;
    if (ParseScene(argc, argv, command) == false)
    {
        return PrintUsage();
    }
    threading::JobSystem decodeJobs;
    if (StartDecodeWorkers(command.Workers, decodeJobs) == false)
    {
        return EXIT_UNAVAILABLE;
    }
    const ludus::foundation::identity::EntropySource entropy = ludus::platform::GetSystemEntropySource();
    AssetOptions options;
    options.EntropyOrNull = &entropy;
    options.JobsOrNull = command.Workers == 0 ? nullptr : &decodeJobs;
    AssetError error;
    Array<uint8> level;
    bool isOk = true;
    if (command.Action == "new")
    {
        Array<uint8> created;
        isOk = CreateLevel(command.Name, options, created, error) == true &&
               SaveLevel(command.Project, command.Level, GetSpan(created), nullptr, level, error) == true;
    }
    else
    {
        isOk = OpenLevel(command.Project, command.Level, level, error);
        if (isOk == true && command.Action == "import")
        {
            isOk = ImportSavedLevel(command.Project, command.Level, command.Operand, options, level, error);
        }
        else if (isOk == true && command.Action == "assemble")
        {
            Array<uint8> bundle;
            isOk = AssembleLevel(command.Project, GetSpan(level), {}, bundle, error) == true &&
                   WriteFileAtomically(command.Operand, GetSpan(bundle), error) == true;
        }
        else if (isOk == true && command.Action == "publish")
        {
            Array<uint8> published;
            isOk =
                PublishLevel(command.Project, GetSpan(level), command.Operand, DumpLayout::Canonical, published, error);
        }
    }
    Array<uint8> dump;
    const DumpLayout layout = command.IsJson == true ? DumpLayout::Line : DumpLayout::Indented;
    if (isOk == false || DumpLevel(command.Project, GetSpan(level), {}, layout, dump, error) == false)
    {
        return ReportFailure(error, command.IsJson);
    }
    if (command.IsJson == true)
    {
        // {"schema": 1, **payload}
        WriteAll(1, "{\"schema\": 1, ");
        WriteAll(1, GetView(dump).substr(1));
    }
    else
    {
        WriteAll(1, GetView(dump));
    }
    return EXIT_OK;
}
namespace identity_records = ludus::content::identity_records;

int ReportIdentityFailure(const identity_records::IdentityRecordError& error) noexcept
{
    WriteAll(2, "error [");
    WriteAll(2, identity_records::GetIdentityRecordStatusName(error.Status));
    WriteAll(2, "]: ");
    WriteAll(2, error.Message.View());
    WriteAll(2, "\n");
    return error.Status == identity_records::IdentityRecordStatus::EntropyUnavailable ? EXIT_UNAVAILABLE : EXIT_FAILED;
}

/// Read one file below ROOT, describing a failure in `outError`.
[[nodiscard]] bool ReadIdentityInput(std::string_view root,
                                     std::string_view path,
                                     usize cap,
                                     ludus::content::Bytes& outBytes,
                                     identity_records::IdentityRecordError& outError) noexcept
{
    const ludus::content::Status status = ludus::content::ReadFile(root, path, cap, outBytes);
    if (status == ludus::content::Status::Ok)
    {
        return true;
    }
    outError.Status = status == ludus::content::Status::Limit ? identity_records::IdentityRecordStatus::Limit
                                                              : identity_records::IdentityRecordStatus::IoError;
    const bool isSet = outError.Message.Set(path);
    static_cast<void>(isSet);
    return false;
}

int RunIdentityMigrate(int argc, char** argv) noexcept
{
    if (argc < 6)
    {
        return PrintUsage();
    }
    const std::string_view root = argv[3];
    const std::string_view manifestPath = argv[4];
    identity_records::IdentityRecordError error;
    ludus::content::Bytes existing;
    if (ludus::content::ReadFile(root, manifestPath, identity_records::MAX_MANIFEST_BYTES, existing) !=
        ludus::content::Status::NotFound)
    {
        // Checked before minting; PublishMigrationManifest still refuses a manifest that appears later.
        error.Status = identity_records::IdentityRecordStatus::AlreadyExists;
        const bool isSet =
            error.Message.Set("the manifest already exists (or cannot be inspected); it is never regenerated");
        static_cast<void>(isSet);
        return ReportIdentityFailure(error);
    }
    const usize count = static_cast<usize>(argc - 5);
    Array<ludus::content::Bytes> documents;
    Array<identity_records::MigrationSource> sources;
    if (documents.TryResize(count) == false || sources.TryResize(count) == false)
    {
        error.Status = identity_records::IdentityRecordStatus::OutOfMemory;
        return ReportIdentityFailure(error);
    }
    for (usize i = 0; i < count; ++i)
    {
        const std::string_view path = argv[5 + i];
        if (ReadIdentityInput(root, path, identity_records::MAX_WORLD_DOCUMENT_BYTES, documents[i], error) == false)
        {
            return ReportIdentityFailure(error);
        }
        sources[i] = { .Path = path, .WorldJson = documents[i].String() };
    }
    const ludus::foundation::identity::EntropySource entropy = ludus::platform::GetSystemEntropySource();
    identity_records::MigrationManifest manifest;
    if (identity_records::GenerateMigrationManifest({sources.GetData(), sources.GetSize()}, entropy, manifest, error) !=
            identity_records::IdentityRecordStatus::Success ||
        identity_records::PublishMigrationManifest(root, manifestPath, manifest, error) !=
            identity_records::IdentityRecordStatus::Success)
    {
        return ReportIdentityFailure(error);
    }
    return EXIT_OK;
}

int RunIdentityApply(int argc, char** argv) noexcept
{
    if (argc != 8)
    {
        return PrintUsage();
    }
    const std::string_view root = argv[3];
    const std::string_view worldPath = argv[6];
    const std::string_view outputPath = argv[7];
    identity_records::IdentityRecordError error;
    ludus::content::Bytes manifestJson;
    ludus::content::Bytes schemaJson;
    ludus::content::Bytes worldJson;
    if (ReadIdentityInput(root, argv[4], identity_records::MAX_MANIFEST_BYTES, manifestJson, error) == false ||
        ReadIdentityInput(root, argv[5], identity_records::MAX_WORLD_SCHEMA_BYTES, schemaJson, error) == false ||
        ReadIdentityInput(root, worldPath, identity_records::MAX_WORLD_DOCUMENT_BYTES, worldJson, error) == false)
    {
        return ReportIdentityFailure(error);
    }
    identity_records::MigrationManifest manifest;
    Array<identity_records::EntityReferenceProperty> references;
    ludus::content::Bytes output;
    if (manifest.Read(manifestJson.String(), error) != identity_records::IdentityRecordStatus::Success ||
        identity_records::ReadEntityReferenceProperties(schemaJson.String(), references, error) !=
            identity_records::IdentityRecordStatus::Success ||
        identity_records::ApplyWorldMigration(manifest,
                                              { .Path = worldPath, .WorldJson = worldJson.String() },
                                              {references.GetData(), references.GetSize()},
                                              output,
                                              error) != identity_records::IdentityRecordStatus::Success)
    {
        return ReportIdentityFailure(error);
    }
    // A null expected digest creates OUTPUT and refuses to replace any existing file, including WORLD.
    const ludus::content::Status saved = ludus::content::SaveFile(root, outputPath, output.Data(), nullptr);
    if (saved != ludus::content::Status::Ok)
    {
        error.Status = saved == ludus::content::Status::Conflict ? identity_records::IdentityRecordStatus::AlreadyExists
                                                                 : identity_records::IdentityRecordStatus::IoError;
        const bool isSet = error.Message.Set(outputPath);
        static_cast<void>(isSet);
        return ReportIdentityFailure(error);
    }
    return EXIT_OK;
}

int RunIdentity(int argc, char** argv) noexcept
{
    const std::string_view action = argc >= 3 ? std::string_view(argv[2]) : std::string_view{};
    if (action == "migrate")
    {
        return RunIdentityMigrate(argc, argv);
    }
    if (action == "apply")
    {
        return RunIdentityApply(argc, argv);
    }
    return PrintUsage();
}
} // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 && (std::string_view(argv[1]) == "cook" || std::string_view(argv[1]) == "cook-materials"))
    {
        return RunCook(argc, argv);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "pbrt")
    {
        return RunBestEffort(argc, argv, ImportPbrtBestEffort);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "gltf")
    {
        return RunBestEffort(argc, argv, ImportGltfBestEffort);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "obj")
    {
        return RunBestEffort(argc, argv, ImportObjBestEffort);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "import")
    {
        return RunBestEffort(argc, argv, ImportSceneBestEffort);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "scene")
    {
        return RunScene(argc, argv);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "identity")
    {
        return RunIdentity(argc, argv);
    }
    return PrintUsage();
}
