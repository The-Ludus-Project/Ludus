#include "internal/browser_folder.h"
#include "internal/editor_files.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QList>
#include <QTemporaryDir>

using namespace ludus::editor;
using ludus::foundation::uint64;

namespace
{
constexpr uint64 MEBIBYTE = 1024ULL * 1024ULL;

/// Dolomites Survival outside its generated folders (2026-11-17): 154 files, 37 MB, the largest 16 MiB.
QList<BrowserFolderEntry> MakeLargeProject()
{
    QList<BrowserFolderEntry> entries{
        { .Path = QStringLiteral("ludus.project.json"), .Size = 600 },
        { .Path = QStringLiteral("assets/objects/9e6c.bin"), .Size = 16 * MEBIBYTE + 12 },
        { .Path = QStringLiteral("assets/terrain/lagazuoi-winter-environment.glb"), .Size = 8245432 },
        { .Path = QStringLiteral("authoring/trail.world.json"), .Size = 90229 },
    };
    for (qsizetype index = static_cast<qsizetype>(entries.size()); index < 154; ++index)
    {
        entries.append({ .Path = QStringLiteral("src/file%1.cpp").arg(index), .Size = MEBIBYTE / 8 });
    }
    return entries;
}
} // namespace

TEST_CASE("The browser file budget derives from the editor's WebAssembly memory", "[editor][browser]")
{
    STATIC_REQUIRE(BROWSER_EDITOR_MAXIMUM_MEMORY_BYTES == 2048ULL * MEBIBYTE);
    STATIC_REQUIRE(BROWSER_FILE_BUDGET_BYTES == BROWSER_EDITOR_MAXIMUM_MEMORY_BYTES / 4);
    // The former per-session total refused projects such as Dolomites Survival.
    STATIC_REQUIRE(BROWSER_FILE_BUDGET_BYTES > 37 * MEBIBYTE);
}

TEST_CASE("Browser project folders larger than the former 32 MiB session total are admitted", "[editor][browser]")
{
    QString entry = QStringLiteral("previous");
    QString problem;
    REQUIRE(AdmitBrowserFolder(QStringLiteral("Dolomites-Survival"),
                               MakeLargeProject(),
                               BrowserFolderIntent::Project,
                               {},
                               entry,
                               problem));
    CHECK(entry == QStringLiteral("Dolomites-Survival/ludus.project.json"));
    CHECK(problem.isEmpty());
}

TEST_CASE("Browser folder refusals name the measured size or count and a next step", "[editor][browser]")
{
    QString entry = QStringLiteral("previous");
    QString problem;

    SECTION("A file larger than the per-file limit")
    {
        CHECK_FALSE(AdmitBrowserFolder(QStringLiteral("Game"),
                                       MakeLargeProject(),
                                       BrowserFolderIntent::Project,
                                       { .MaximumFileBytes = 8 * MEBIBYTE },
                                       entry,
                                       problem));
        CHECK(problem.contains(QStringLiteral("Game/assets/objects/9e6c.bin is 16.0 MiB")));
        CHECK(problem.contains(QStringLiteral("up to 8.0 MiB")));
        CHECK(problem.contains(QStringLiteral("desktop editor")));
    }
    SECTION("Too many files")
    {
        CHECK_FALSE(AdmitBrowserFolder(QStringLiteral("Game"),
                                       MakeLargeProject(),
                                       BrowserFolderIntent::Project,
                                       { .MaximumFiles = 100 },
                                       entry,
                                       problem));
        CHECK(problem.contains(QStringLiteral("Game has 154 files")));
        CHECK(problem.contains(QStringLiteral("at most 100")));
        CHECK(problem.contains(QStringLiteral("out, build, .git, .ludus, node_modules")));
    }
    SECTION("No descriptor at the top level")
    {
        CHECK_FALSE(AdmitBrowserFolder(QStringLiteral("assets"),
                                       {{ .Path = QStringLiteral("nested/ludus.project.json"), .Size = 1 }},
                                       BrowserFolderIntent::Project,
                                       {},
                                       entry,
                                       problem));
        CHECK(problem.contains(QStringLiteral("Choose the project's root folder")));
    }
    SECTION("An unusable path")
    {
        CHECK_FALSE(AdmitBrowserFolder(QStringLiteral("Game"),
                                       {{ .Path = QStringLiteral("ludus.project.json"), .Size = 1 },
                                        { .Path = QStringLiteral("bad\\name.txt"), .Size = 1 }},
                                       BrowserFolderIntent::Project,
                                       {},
                                       entry,
                                       problem));
        CHECK(problem.contains(QStringLiteral("Game/bad\\name.txt cannot be opened")));
        CHECK(problem.contains(QStringLiteral("Rename it")));
    }
    SECTION("An empty folder")
    {
        CHECK_FALSE(AdmitBrowserFolder(QStringLiteral("Game"), {}, BrowserFolderIntent::Scene, {}, entry, problem));
        CHECK(problem.contains(QStringLiteral("no files outside its generated folders")));
    }
    CHECK(entry == QStringLiteral("previous"));
}

TEST_CASE("Browser scene folders select their .pbrt entry first", "[editor][browser]")
{
    QString entry;
    QString problem;
    REQUIRE(AdmitBrowserFolder(QStringLiteral("Staircase"),
                               {{ .Path = QStringLiteral("models/stairs.obj"), .Size = 10 },
                                { .Path = QStringLiteral("scene-v4.pbrt"), .Size = 10 },
                                { .Path = QStringLiteral("textures/wood.png"), .Size = 10 }},
                               BrowserFolderIntent::Scene,
                               {},
                               entry,
                               problem));
    CHECK(entry == QStringLiteral("Staircase/scene-v4.pbrt"));
    CHECK_FALSE(AdmitBrowserFolder(QStringLiteral("Textures"),
                                   {{ .Path = QStringLiteral("wood.png"), .Size = 10 }},
                                   BrowserFolderIntent::Scene,
                                   {},
                                   entry,
                                   problem));
    CHECK(problem.contains(QStringLiteral("no .pbrt, .gltf, .glb, .obj or .luscene.json")));
    QString flat;
    CHECK(ValidateSceneImportPaths({QStringLiteral("archive.zip")}, false, flat));
    CHECK(flat == QStringLiteral("archive.zip"));
    CHECK_FALSE(ValidateSceneImportPaths({QStringLiteral("archive.zip"), QStringLiteral("a.pbrt")}, false, flat));
}

TEST_CASE("Browser folder failures explain the next step", "[editor][browser]")
{
    const auto describe = [](const QString& kind) {
        return DescribeBrowserFolderFailure(
            {
                .Kind = kind,
                .Name = QStringLiteral("Game"),
                .Path = QStringLiteral("assets/a.bin"),
                .Error = QStringLiteral("NotFoundError: gone"),
            },
            BrowserFolderIntent::Project,
            MAX_BROWSER_FOLDER_FILES);
    };
    CHECK(describe(QStringLiteral("activation")).contains(QStringLiteral("File → Open Project... again")));
    CHECK(describe(QStringLiteral("permission")).contains(QStringLiteral("allow editing")));
    CHECK(describe(QStringLiteral("no-remembered")).contains(QStringLiteral("Chrome or Edge")));
    CHECK(describe(QStringLiteral("too-many")).contains(QStringLiteral("more than 4096 files")));
    CHECK(describe(QStringLiteral("read")).contains(QStringLiteral("Could not read assets/a.bin from Game")));
    CHECK(describe(QStringLiteral("write")).contains(QStringLiteral("Your edits are kept in this tab")));
    CHECK(describe(QStringLiteral("unreadable")).contains(QStringLiteral("(NotFoundError: gone)")));
}

TEST_CASE("Browser byte sizes use binary units", "[editor][browser]")
{
    CHECK(FormatBrowserByteSize(600) == QStringLiteral("600 bytes"));
    CHECK(FormatBrowserByteSize(16 * MEBIBYTE + 12) == QStringLiteral("16.0 MiB"));
    CHECK(FormatBrowserByteSize(BROWSER_FILE_BUDGET_BYTES) == QStringLiteral("512.0 MiB"));
    CHECK(IsGeneratedProjectPath(QStringLiteral("out/build/game.o")));
    CHECK(IsGeneratedProjectPath(QStringLiteral(".git")));
    CHECK_FALSE(IsGeneratedProjectPath(QStringLiteral("outline/a.json")));
}

TEST_CASE("Project archives honor their byte limit before reading files", "[editor][browser]")
{
    QTemporaryDir project;
    REQUIRE(QDir(project.path()).mkpath(QStringLiteral("assets")));
    QFile asset(project.filePath(QStringLiteral("assets/data.bin")));
    REQUIRE(asset.open(QIODevice::WriteOnly));
    REQUIRE(asset.write(QByteArray(4096, 'x')) == 4096);
    asset.close();
    QByteArray archive("previous");
    CHECK_FALSE(EncodeEditorProjectArchive(project.path(), archive, 4096));
    CHECK(archive == QByteArray("previous"));
    REQUIRE(EncodeEditorProjectArchive(project.path(), archive, MEBIBYTE));
    CHECK(archive.size() > 4096);
}
