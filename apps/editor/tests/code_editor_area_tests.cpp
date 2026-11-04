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
