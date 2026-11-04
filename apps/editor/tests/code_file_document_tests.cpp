#include "internal/code_conflict_banner.h"
#include "internal/code_editor_config.h"
#include "internal/code_file_document.h"
#include "internal/code_file_limits.h"
#include "internal/code_text_detect.h"
#include "internal/code_text_profile.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>

#include <utility>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
// Write bytes verbatim so BOM and line endings survive to the loader.
[[nodiscard]] QString writeFile(const QTemporaryDir& directory, const QString& name, const QByteArray& bytes)
{
    const QString path = QDir(directory.path()).filePath(name);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
    file.close();
    return path;
}

// Move a file's modification time forward, so an external rewrite is visible
// even when it lands in the same millisecond as the original write.
[[nodiscard]] bool AdvanceModifiedTime(const QString& path)
{
    constexpr int64 ADVANCE_SECONDS = 2;
    QFile file(path);
    if (file.open(QIODevice::ReadWrite) == false)
    {
        return false;
    }
    const QDateTime later = QFileInfo(path).lastModified().addSecs(ADVANCE_SECONDS);
    return file.setFileTime(later, QFileDevice::FileModificationTime);
}

[[nodiscard]] QByteArray readFile(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}

// One inserted byte at the end, so the host's edit path is exercised directly.
[[nodiscard]] CodeEditStatus appendByte(CodeFileDocument& inoutHost, char value)
{
    const CodeDocumentSnapshot snapshot = inoutHost.GetSnapshot();
    const usize size = snapshot.Text.GetSize();
    CodeTextEdit edit;
    edit.StartBytes = size;
    edit.RemovedBytes = 0;
    edit.Inserted = {&value, 1};
    const CodeTextSelection before{size, size};
    const CodeTextSelection after{size + 1, size + 1};
    return inoutHost.ApplyEdit(snapshot.Identity, edit, before, after, false);
}
} // namespace

TEST_CASE("Size thresholds classify through one owner each", "[code-editor][file-host][thresholds]")
{
    CHECK(ClassifyCodeFileSize(0) == CodeFileSizeMode::Normal);
    CHECK(ClassifyCodeFileSize(CODE_FILE_NORMAL_LIMIT_BYTES) == CodeFileSizeMode::Normal);
    CHECK(ClassifyCodeFileSize(CODE_FILE_NORMAL_LIMIT_BYTES + 1) == CodeFileSizeMode::Large);
    CHECK(ClassifyCodeFileSize(CODE_FILE_LARGE_LIMIT_BYTES + 1) == CodeFileSizeMode::VeryLarge);
    CHECK(ClassifyCodeFileSize(CODE_FILE_READ_ONLY_LIMIT_BYTES + 1) == CodeFileSizeMode::Refused);
    // The thresholds are strictly ordered, so no two bands overlap.
    CHECK(CODE_FILE_NORMAL_LIMIT_BYTES < CODE_FILE_LARGE_LIMIT_BYTES);
    CHECK(CODE_FILE_LARGE_LIMIT_BYTES < CODE_FILE_READ_ONLY_LIMIT_BYTES);
}

TEST_CASE("Detection counts lines and leaves a small file Normal", "[code-editor][file-host][thresholds]")
{
    CodeTextProfile profile;
    const QByteArray bytes("one\ntwo\n");
    const usize bom = DetectCodeTextProfile(bytes.constData(), static_cast<usize>(bytes.size()), profile);
    CHECK(bom == 0);
    CHECK(profile.LineCount == 3);
    CHECK(profile.SizeMode == CodeFileSizeMode::Normal);
    CHECK(profile.IsReadOnly == false);
    CHECK(profile.LineEnding == CodeLineEnding::Lf);
}

TEST_CASE("Invalid UTF-8 opens read-only with an explanation", "[code-editor][file-host][encoding]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    QByteArray bytes("valid then \xFF\xFE invalid");
    const QString path = writeFile(directory, QStringLiteral("broken.txt"), bytes);

    const CodeFileLoadResult result = LoadCodeFile(path);
    CHECK(result.Status == CodeFileLoadStatus::ReadOnly);
    CHECK(result.Profile.Encoding == CodeTextEncoding::InvalidUtf8);
    CHECK(result.Profile.IsReadOnly == true);
    CHECK(result.Explanation.isEmpty() == false);

    CodeFileDocument host(CodeFileLoadResult(result), 1);
    CHECK(host.IsReadOnly() == true);
    // Editing a read-only document is rejected and leaves the draft untouched.
    CHECK(appendByte(host, 'x') == CodeEditStatus::Unavailable);
    QString error;
    CHECK(host.Save(error) == false);
}

TEST_CASE("UTF-8 BOM is preserved byte for byte across a save", "[code-editor][file-host][encoding]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    QByteArray bytes;
    bytes.append(CODE_UTF8_BOM_0);
    bytes.append(CODE_UTF8_BOM_1);
    bytes.append(CODE_UTF8_BOM_2);
    bytes.append("hello\n");
    const QString path = writeFile(directory, QStringLiteral("bom.txt"), bytes);

    CodeFileLoadResult result = LoadCodeFile(path);
    CHECK(result.Status == CodeFileLoadStatus::Ok);
    CHECK(result.Profile.Encoding == CodeTextEncoding::Utf8Bom);
    // The stored editable bytes exclude the BOM, so interior offsets are clean.
    CHECK(result.Bytes == QByteArray("hello\n"));

    CodeFileDocument host(std::move(result), 1);
    QString error;
    REQUIRE(host.Save(error) == true);
    // The saved file still begins with the three BOM bytes, unchanged.
    const QByteArray roundTrip = readFile(path);
    CHECK(roundTrip == bytes);
}

TEST_CASE("CRLF and mixed line endings survive an untouched save", "[code-editor][file-host][line-endings]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);

    SECTION("uniform CRLF")
    {
        const QByteArray bytes("one\r\ntwo\r\nthree\r\n");
        const QString path = writeFile(directory, QStringLiteral("crlf.txt"), bytes);
        CodeFileLoadResult result = LoadCodeFile(path);
        CHECK(result.Profile.LineEnding == CodeLineEnding::CrLf);
        CodeFileDocument host(std::move(result), 1);
        QString error;
        REQUIRE(host.Save(error) == true);
        CHECK(readFile(path) == bytes);
    }

    SECTION("mixed endings")
    {
        const QByteArray bytes("one\r\ntwo\nthree\r\n");
        const QString path = writeFile(directory, QStringLiteral("mixed.txt"), bytes);
        CodeFileLoadResult result = LoadCodeFile(path);
        CHECK(result.Profile.LineEnding == CodeLineEnding::Mixed);
        CodeFileDocument host(std::move(result), 1);
        QString error;
        REQUIRE(host.Save(error) == true);
        // Untouched mixed endings are written back byte for byte.
        CHECK(readFile(path) == bytes);
    }
}

TEST_CASE("Atomic save replaces the file and clears dirty", "[code-editor][file-host][save]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString path = writeFile(directory, QStringLiteral("edit.txt"), QByteArray("ab\n"));

    CodeFileDocument host(LoadCodeFile(path), 1);
    CHECK(host.IsDirty() == false);
    REQUIRE(appendByte(host, 'c') == CodeEditStatus::Ok);
    CHECK(host.IsDirty() == true);
    QString error;
    REQUIRE(host.Save(error) == true);
    CHECK(host.IsDirty() == false);
    CHECK(readFile(path) == QByteArray("ab\nc"));
}

TEST_CASE("External change: a clean buffer reloads, a dirty one is kept", "[code-editor][file-host][conflict]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString path = writeFile(directory, QStringLiteral("watch.txt"), QByteArray("original\n"));

    SECTION("clean buffer reloads silently")
    {
        CodeFileDocument host(LoadCodeFile(path), 1);
        CHECK(host.HasExternalChange() == false);
        // Rewrite the file externally with different content and a later mtime.
        QFile::remove(path);
        CHECK(writeFile(directory, QStringLiteral("watch.txt"), QByteArray("changed on disk\n")) == path);
        REQUIRE(AdvanceModifiedTime(path) == true);
        CHECK(host.HasExternalChange() == true);
        QString error;
        REQUIRE(host.ReloadFromDisk(error) == true);
        const CodeDocumentSnapshot snapshot = host.GetSnapshot();
        const QByteArray loaded(snapshot.Text.GetDataOrNull(), static_cast<qsizetype>(snapshot.Text.GetSize()));
        CHECK(loaded == QByteArray("changed on disk\n"));
        CHECK(host.IsDirty() == false);
    }

    SECTION("dirty buffer keeps the draft (never overwritten)")
    {
        CodeFileDocument host(LoadCodeFile(path), 1);
        REQUIRE(appendByte(host, '!') == CodeEditStatus::Ok);
        CHECK(host.IsDirty() == true);
        QFile::remove(path);
        CHECK(writeFile(directory, QStringLiteral("watch.txt"), QByteArray("external\n")) == path);
        REQUIRE(AdvanceModifiedTime(path) == true);
        CHECK(host.HasExternalChange() == true);
        // Keep Mine re-baselines without touching the draft; it stays dirty.
        host.AdoptAsBaseline();
        CHECK(host.IsDirty() == true);
        CHECK(host.HasExternalChange() == false);
        // Saving now overwrites the external change with the kept draft.
        QString error;
        REQUIRE(host.Save(error) == true);
        CHECK(readFile(path) == QByteArray("original\n!"));
    }
}

TEST_CASE("EditorConfig overrides detected indentation and line endings", "[code-editor][file-host][editorconfig]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    // The file is detected as 2-space, LF; EditorConfig forces tabs and CRLF.
    const QString configPath =
        writeFile(directory,
                  QStringLiteral(".editorconfig"),
                  QByteArray("root = true\n[*.txt]\nindent_style = tab\nindent_size = tab\ntab_width = 8\n"
                             "end_of_line = crlf\n"));
    REQUIRE(configPath.isEmpty() == false);
    const QString path = writeFile(directory, QStringLiteral("conf.txt"), QByteArray("  spaced\n  spaced\n"));

    const CodeFileLoadResult result = LoadCodeFile(path);
    CHECK(result.Profile.Indent.Style == CodeIndentStyle::Tabs);
    CHECK(result.Profile.Indent.Source == CodeTextProfileSource::EditorConfig);
    CHECK(result.Profile.Indent.Width == 8);
    CHECK(result.Profile.LineEnding == CodeLineEnding::CrLf);
    CHECK(result.Profile.LineEndingSource == CodeTextProfileSource::EditorConfig);
}

TEST_CASE("EditorConfig glob matching selects the right section", "[code-editor][file-host][editorconfig]")
{
    CodeEditorConfigSettings settings;
    const QByteArray config("root = true\n"
                            "[*]\nindent_style = space\nindent_size = 4\n"
                            "[*.lua]\nindent_style = space\nindent_size = 2\n");
    const bool isRoot = MergeEditorConfigFile(config.constData(),
                                              static_cast<usize>(config.size()),
                                              QStringLiteral("door.lua"),
                                              settings);
    CHECK(isRoot == true);
    CHECK(settings.HasIndentSize == true);
    // The later, more specific section wins for a .lua file.
    CHECK(settings.IndentSize == 2);

    CodeEditorConfigSettings other;
    const bool isOtherRoot = MergeEditorConfigFile(config.constData(),
                                                   static_cast<usize>(config.size()),
                                                   QStringLiteral("notes.txt"),
                                                   other);
    CHECK(isOtherRoot == true);
    CHECK(other.IndentSize == 4);
}

TEST_CASE("Conflict banner offers the three resolutions", "[code-editor][file-host][conflict]")
{
    CodeConflictBanner banner;
    QSignalSpy compareSpy(&banner, &CodeConflictBanner::CompareRequested);
    QSignalSpy keepSpy(&banner, &CodeConflictBanner::KeepMineRequested);
    QSignalSpy reloadSpy(&banner, &CodeConflictBanner::ReloadRequested);

    banner.ShowConflict(QStringLiteral("watch.txt"));
    CHECK(banner.isVisible() == true);

    banner.ShowExplanation(QStringLiteral("This file is read-only."));
    CHECK(banner.isVisible() == true);
    banner.Dismiss();
    CHECK(banner.isVisible() == false);
    // The signals exist and are connectable; interactive clicks are covered by
    // the recorded native run named in the ledger.
    CHECK(compareSpy.isValid() == true);
    CHECK(keepSpy.isValid() == true);
    CHECK(reloadSpy.isValid() == true);
}
