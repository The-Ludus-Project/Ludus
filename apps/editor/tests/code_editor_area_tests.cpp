// Code tabs: Save, external-change detection and the conflict bar (CE03, CE04).
// Offscreen only. File-system notifications are not awaited: the tests call
// CheckExternalChanges, the same check the watcher and app activation run.

#include "internal/code_conflict_banner.h"
#include "internal/code_document_tab.h"
#include "internal/code_editor.h"
#include "internal/code_editor_area.h"
#include "internal/code_file_document.h"

#include <catch2/catch_test_macros.hpp>

#include <QAbstractButton>
#include <QApplication>
#include <QByteArray>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>
#include <QTest>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
[[nodiscard]] QString WriteFile(const QTemporaryDir& directory, const QString& name, const QByteArray& bytes)
{
    const QString path = QDir(directory.path()).filePath(name);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
    file.close();
    return path;
}

// Rewrite a file as another program would, with a later modification time so
// the change is visible even within the clock's resolution.
void RewriteExternally(const QString& path, const QByteArray& bytes)
{
    constexpr int64 LATER_SECONDS = 2;
    const QDateTime later = QFileInfo(path).lastModified().addSecs(LATER_SECONDS);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate) == true);
    REQUIRE(file.write(bytes) == bytes.size());
    REQUIRE(file.setFileTime(later, QFileDevice::FileModificationTime) == true);
    file.close();
}

[[nodiscard]] QByteArray ReadBytes(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}

void TypeInto(CodeDocumentTab& inoutTab)
{
    inoutTab.GetEditor().setFocus();
    QTest::keyClick(&inoutTab.GetEditor(), Qt::Key_Z);
    QCoreApplication::processEvents();
}

void ClickBannerButton(CodeDocumentTab& inoutTab, const char* objectName)
{
    QAbstractButton* button = inoutTab.GetBanner().findChild<QAbstractButton*>(QString::fromLatin1(objectName));
    REQUIRE(button != nullptr);
    button->click();
}

struct OpenedFile final
{
    QTemporaryDir Project;
    QString Path;
    CodeEditorArea Area;
};

[[nodiscard]] CodeDocumentTab& OpenOne(OpenedFile& inoutOpened, const QByteArray& bytes)
{
    REQUIRE(inoutOpened.Project.isValid() == true);
    inoutOpened.Path = WriteFile(inoutOpened.Project, QStringLiteral("shader.slang"), bytes);
    inoutOpened.Area.SetProjectEpoch(1);
    REQUIRE(inoutOpened.Area.OpenFile(inoutOpened.Path, false) == true);
    CodeDocumentTab* tab = inoutOpened.Area.GetActiveTabOrNull();
    REQUIRE(tab != nullptr);
    return *tab;
}
} // namespace

TEST_CASE("Project Explorer can stage a line-dense file without changing its draft", "[code-editor][staged-open]")
{
    constexpr int32 SOURCE_LINE_COUNT = 40000;
    constexpr int32 OPEN_TIMEOUT_MILLISECONDS = 10000;
    const QByteArray line = QStringLiteral("void Paint(); // 日😀\r\n").toUtf8();
    const QByteArray source = line.repeated(SOURCE_LINE_COUNT);
    OpenedFile opened;
    REQUIRE(opened.Project.isValid() == true);
    opened.Path = WriteFile(opened.Project, QStringLiteral("dense.cpp"), source);
    opened.Area.resize(900, 640);
    REQUIRE(opened.Area.OpenFileFromProjectExplorer(opened.Path) == true);
    CodeDocumentTab* tabOrNull = opened.Area.GetActiveTabOrNull();
    REQUIRE(tabOrNull != nullptr);
    CodeDocumentTab& tab = *tabOrNull;
    REQUIRE(tab.IsOpening() == true);
    CHECK(tab.GetEditor().isReadOnly() == true);
    CHECK(tab.GetEditor().blockCount() < SOURCE_LINE_COUNT);
    CHECK(tab.GetBanner().isVisibleTo(&tab) == true);
    CHECK(tab.IsDirty() == false);

    SECTION("Completes after the visible prefix paints")
    {
        opened.Area.show();
        REQUIRE(QTest::qWaitFor([&tab]() { return tab.IsOpening() == false; }, OPEN_TIMEOUT_MILLISECONDS));
        CHECK(tab.GetBanner().isVisibleTo(&tab) == false);
        CHECK(tab.GetController().Capture().Text == source);
        CHECK(tab.IsDirty() == false);
    }
    SECTION("An early key press edits the complete draft and can be undone")
    {
        TypeInto(tab);
        REQUIRE(tab.IsOpening() == false);
        CHECK(tab.GetController().Capture().Text == QByteArrayLiteral("z") + source);
        CHECK(tab.IsDirty() == true);
        tab.GetEditor().Undo();
        CHECK(tab.GetController().Capture().Text == source);
        CHECK(tab.IsDirty() == false);
    }
    SECTION("A nearby search result paints at its destination before editing is ready")
    {
        constexpr int32 REQUESTED_LINE = 8;
        REQUIRE(opened.Area.OpenFileAtLocation(opened.Path, REQUESTED_LINE, 1) == true);
        CHECK(opened.Area.GetActiveTabOrNull() == &tab);
        REQUIRE(tab.IsOpening() == true);
        CHECK(tab.GetEditor().textCursor().blockNumber() == REQUESTED_LINE - 1);
        opened.Area.show();
        REQUIRE(QTest::qWaitFor([&tab]() { return tab.IsOpening() == false; }, OPEN_TIMEOUT_MILLISECONDS));
        CHECK(tab.GetEditor().textCursor().blockNumber() == REQUESTED_LINE - 1);
        CHECK(tab.IsPreview() == false);
    }
    SECTION("A distant search result receives the full projection before navigation")
    {
        REQUIRE(opened.Area.OpenFileAtLocation(opened.Path, SOURCE_LINE_COUNT, 1) == true);
        CHECK(opened.Area.GetActiveTabOrNull() == &tab);
        REQUIRE(tab.IsOpening() == false);
        CHECK(tab.GetEditor().textCursor().blockNumber() == SOURCE_LINE_COUNT - 1);
        CHECK(tab.IsPreview() == false);
    }
    REQUIRE(tab.GetEditor().isReadOnly() == false);
    QString projected = QString::fromUtf8(source);
    projected.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    CHECK(tab.GetEditor().toPlainText() == projected);
    CHECK(ReadBytes(opened.Path) == source);
}

TEST_CASE("Replacing a staged preview releases its host before deferred work runs", "[code-editor][staged-open]")
{
    constexpr int32 SOURCE_LINE_COUNT = 40000;
    OpenedFile opened;
    REQUIRE(opened.Project.isValid() == true);
    const QString dense = WriteFile(opened.Project,
                                    QStringLiteral("dense.cpp"),
                                    QByteArrayLiteral("int value;\n").repeated(SOURCE_LINE_COUNT));
    const QString small = WriteFile(opened.Project, QStringLiteral("small.cpp"), QByteArrayLiteral("int small;\n"));
    REQUIRE(opened.Area.OpenFileFromProjectExplorer(dense) == true);
    REQUIRE(opened.Area.GetActiveTabOrNull() != nullptr);
    REQUIRE(opened.Area.GetActiveTabOrNull()->IsOpening() == true);
    REQUIRE(opened.Area.OpenFileFromProjectExplorer(small) == true);
    opened.Area.show();
    QCoreApplication::processEvents();
    REQUIRE(opened.Area.GetTabCount() == 1);
    REQUIRE(opened.Area.GetRegistry().GetHostCount() == 1);
    REQUIRE(opened.Area.GetActiveTabOrNull() != nullptr);
    CHECK(opened.Area.GetActiveTabOrNull()->GetEditor().toPlainText() == QStringLiteral("int small;\n"));
}

TEST_CASE("Saving a code tab writes the file and clears its dirty state", "[code-editor][save]")
{
    OpenedFile opened;
    CodeDocumentTab& tab = OpenOne(opened, QByteArray("saved\n"));
    TypeInto(tab);
    REQUIRE(tab.IsDirty() == true);
    REQUIRE(opened.Area.CanSaveActiveTab() == true);

    REQUIRE(opened.Area.SaveTab(&tab) == true);
    CHECK(tab.IsDirty() == false);
    CHECK(ReadBytes(opened.Path) == QByteArray("zsaved\n"));
    // The editor's own save is not an external change.
    opened.Area.CheckExternalChanges();
    CHECK(tab.GetBanner().IsShowingConflict() == false);
}

TEST_CASE("Opening a Slang file repairs its draft before Save publishes the change", "[code-editor][save]")
{
    OpenedFile opened;
    const QByteArray original = QStringLiteral("void Main()\u2028{}\n").toUtf8();
    CodeDocumentTab& tab = OpenOne(opened, original);
    QCoreApplication::processEvents();
    REQUIRE(tab.IsDirty() == true);
    CHECK(ReadBytes(opened.Path) == original);

    REQUIRE(opened.Area.SaveTab(&tab) == true);
    CHECK(ReadBytes(opened.Path) == QByteArrayLiteral("void Main()\n{}\n"));
    CHECK(tab.IsDirty() == false);
}

TEST_CASE("A clean buffer follows the file when it changes on disk", "[code-editor][conflict]")
{
    OpenedFile opened;
    CodeDocumentTab& tab = OpenOne(opened, QByteArray("first\n"));
    RewriteExternally(opened.Path, QByteArray("second version\n"));
    opened.Area.CheckExternalChanges();
    CHECK(tab.GetEditor().toPlainText() == QStringLiteral("second version\n"));
    CHECK(tab.IsDirty() == false);
    CHECK(tab.GetBanner().IsShowingConflict() == false);
}

TEST_CASE("A dirty buffer is never overwritten by a change on disk", "[code-editor][conflict]")
{
    OpenedFile opened;
    CodeDocumentTab& tab = OpenOne(opened, QByteArray("first\n"));
    TypeInto(tab);
    RewriteExternally(opened.Path, QByteArray("theirs\n"));
    opened.Area.CheckExternalChanges();
    CHECK(tab.GetEditor().toPlainText() == QStringLiteral("zfirst\n"));
    REQUIRE(tab.GetBanner().IsShowingConflict() == true);

    SECTION("Save refuses until the conflict is decided, then Keep Mine overwrites")
    {
        CHECK(opened.Area.SaveTab(&tab) == false);
        CHECK(ReadBytes(opened.Path) == QByteArray("theirs\n"));
        CHECK(tab.GetBanner().IsShowingConflict() == true);
        ClickBannerButton(tab, "codeConflictKeepMine");
        CHECK(tab.GetBanner().IsShowingConflict() == false);
        CHECK(tab.IsDirty() == true);
        REQUIRE(opened.Area.SaveTab(&tab) == true);
        CHECK(ReadBytes(opened.Path) == QByteArray("zfirst\n"));
    }

    SECTION("Reload takes the file and discards the draft")
    {
        ClickBannerButton(tab, "codeConflictReload");
        CHECK(tab.GetBanner().IsShowingConflict() == false);
        CHECK(tab.IsDirty() == false);
        CHECK(tab.GetEditor().toPlainText() == QStringLiteral("theirs\n"));
    }

    SECTION("Compare shows the draft beside the file and leaves the decision open")
    {
        ClickBannerButton(tab, "codeConflictCompare");
        QDialog* dialog = opened.Area.findChild<QDialog*>(QStringLiteral("codeCompareDialog"));
        REQUIRE(dialog != nullptr);
        CodeEditor* mine = dialog->findChild<CodeEditor*>(QStringLiteral("codeCompareMine"));
        CodeEditor* disk = dialog->findChild<CodeEditor*>(QStringLiteral("codeCompareDisk"));
        REQUIRE(mine != nullptr);
        REQUIRE(disk != nullptr);
        CHECK(mine->toPlainText() == QStringLiteral("zfirst\n"));
        CHECK(disk->toPlainText() == QStringLiteral("theirs\n"));
        CHECK(mine->isReadOnly() == true);
        CHECK(tab.GetBanner().IsShowingConflict() == true);
        dialog->close();
    }
}

TEST_CASE("A deleted file keeps its buffer and explains how to recover it", "[code-editor][conflict]")
{
    OpenedFile opened;
    CodeDocumentTab& tab = OpenOne(opened, QByteArray("keep me\n"));
    REQUIRE(QFile::remove(opened.Path) == true);
    opened.Area.CheckExternalChanges();
    CHECK(tab.GetEditor().toPlainText() == QStringLiteral("keep me\n"));
    CHECK(tab.GetBanner().isVisibleTo(&tab) == true);
    CHECK(tab.GetBanner().IsShowingConflict() == false);
    // Save writes the buffer back to the same path.
    REQUIRE(opened.Area.SaveTab(&tab) == true);
    CHECK(ReadBytes(opened.Path) == QByteArray("keep me\n"));
}
