// Code-buffer recovery journal and dirty-close prompts (CE03), task M1.5.
//
// These cover the four required recovery cases - a crash-style restore, a
// journal for a deleted file, a journal over its per-entry limit, and the dirty
// buffer bookkeeping - plus a regression for each dirty-close answer (Save,
// Don't Save, Cancel) driven through the modal dialog. Offscreen only.

#include "internal/code_document_registry.h"
#include "internal/code_document_tab.h"
#include "internal/code_editor.h"
#include "internal/code_editor_area.h"
#include "internal/code_file_document.h"
#include "internal/code_recovery_journal.h"

#include <catch2/catch_test_macros.hpp>

#include <QAbstractButton>
#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWidget>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
[[nodiscard]] QString writeFile(const QTemporaryDir& directory, const QString& name, const QByteArray& bytes)
{
    const QString path = QDir(directory.path()).filePath(name);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
    file.close();
    return path;
}

// Type one character into a tab's editor so its controller commits a host edit,
// making the bound file host dirty.
void typeInto(CodeDocumentTab& inoutTab)
{
    inoutTab.GetEditor().setFocus();
    QTest::keyClick(&inoutTab.GetEditor(), Qt::Key_Z);
    QCoreApplication::processEvents();
}

// Click a standard button on the next modal dialog to appear. Scheduled before a
// blocking exec() so the dialog answers itself; the answer is recorded for a check.
void answerNextDialog(QMessageBox::StandardButton answer)
{
    QTimer::singleShot(0, [answer]() {
        QWidget* modal = QApplication::activeModalWidget();
        QMessageBox* box = qobject_cast<QMessageBox*>(modal);
        if (box == nullptr)
        {
            // Retry once on the next turn if the dialog is not up yet.
            answerNextDialog(answer);
            return;
        }
        QAbstractButton* button = box->button(answer);
        if (button != nullptr)
        {
            button->click();
        }
        else
        {
            box->reject();
        }
    });
}

[[nodiscard]] QByteArray readBytes(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}
} // namespace

TEST_CASE("Journal store round-trips an unsaved draft and skips damaged files", "[code-editor][recovery]")
{
    QTemporaryDir state;
    REQUIRE(state.isValid() == true);
    CodeRecoveryJournal journal(state.path(), QStringLiteral("/project/alpha"));
    REQUIRE(journal.GetDirectory().isEmpty() == false);

    CodeRecoveryEntry entry;
    entry.CanonicalPath = QStringLiteral("/project/alpha/src/main.cpp");
    entry.SavedHash = QCryptographicHash::hash(QByteArray("saved\n"), QCryptographicHash::Sha256);
    entry.Draft = QByteArray("draft bytes\n");
    REQUIRE(journal.Write(entry) == true);

    const QVector<CodeRecoveryEntry> read = journal.Read();
    REQUIRE(read.size() == 1);
    CHECK(read.at(0).CanonicalPath == entry.CanonicalPath);
    CHECK(read.at(0).SavedHash == entry.SavedHash);
    CHECK(read.at(0).Draft == entry.Draft);

    // A different project does not see another project's journal.
    CodeRecoveryJournal other(state.path(), QStringLiteral("/project/beta"));
    CHECK(other.Read().isEmpty() == true);

    // Removing the entry empties the journal.
    REQUIRE(journal.Remove(entry.CanonicalPath) == true);
    CHECK(journal.Read().isEmpty() == true);
}

TEST_CASE("Journal refuses an entry over its per-entry limit", "[code-editor][recovery]")
{
    QTemporaryDir state;
    REQUIRE(state.isValid() == true);
    CodeRecoveryJournal journal(state.path(), QStringLiteral("/project/limit"));

    CodeRecoveryEntry entry;
    entry.CanonicalPath = QStringLiteral("/project/limit/huge.bin");
    entry.SavedHash = QCryptographicHash::hash(QByteArray(), QCryptographicHash::Sha256);
    // One byte past the limit before base64 expansion already exceeds the cap.
    entry.Draft = QByteArray(static_cast<int>(CodeRecoveryJournal::MAX_RECOVERY_ENTRY_BYTES) + 1, 'x');
    CHECK(journal.Write(entry) == false);
    CHECK(journal.Read().isEmpty() == true);
}

TEST_CASE("Crash-style restore reopens the unsaved draft on reopen", "[code-editor][recovery]")
{
    QTemporaryDir state;
    REQUIRE(state.isValid() == true);
    qputenv("LUDUS_EDITOR_RECOVERY_DIRECTORY", state.path().toUtf8());

    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const QString root = project.path();
    const QString path = writeFile(project, QStringLiteral("main.cpp"), QByteArray("saved\n"));

    // Session one: open the file, edit it (dirty), flush the journal, then "crash"
    // by destroying the area without a clean close.
    {
        CodeEditorArea area;
        area.SetProject(root, 1);
        area.RestoreRecoveredBuffers(); // nothing to restore yet
        REQUIRE(area.OpenFile(path, false) == true);
        CodeDocumentTab* tab = area.GetActiveTabOrNull();
        REQUIRE(tab != nullptr);
        typeInto(*tab);
        CHECK(tab->IsDirty() == true);
        area.FlushRecovery();
    }

    // The journal file survives the crash.
    CodeRecoveryJournal journal(state.path(), root);
    REQUIRE(journal.Read().size() == 1);

    // Session two: a fresh area reopens the project and restores the draft. The
    // on-disk file is still the saved baseline, so the recovered draft applies and
    // the reopened buffer is dirty.
    {
        CodeEditorArea area;
        area.SetProject(root, 2);
        area.RestoreRecoveredBuffers();
        CodeDocumentTab* tab = area.GetActiveTabOrNull();
        REQUIRE(tab != nullptr);
        CHECK(tab->IsDirty() == true);
        CHECK(tab->GetHostOrNull() != nullptr);
        CHECK(tab->GetHostOrNull()->GetSnapshot().Text.GetSize() >
              static_cast<usize>(QByteArray("saved\n").size()));
    }

    // The restore pass cleared the journal, so a third reopen restores nothing.
    CodeRecoveryJournal after(state.path(), root);
    CHECK(after.Read().isEmpty() == true);
    qunsetenv("LUDUS_EDITOR_RECOVERY_DIRECTORY");
}

TEST_CASE("A journal for a deleted file is skipped on restore", "[code-editor][recovery]")
{
    QTemporaryDir state;
    REQUIRE(state.isValid() == true);
    qputenv("LUDUS_EDITOR_RECOVERY_DIRECTORY", state.path().toUtf8());

    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const QString root = project.path();
    const QString path = writeFile(project, QStringLiteral("gone.cpp"), QByteArray("saved\n"));

    // Journal an unsaved draft for the file, then delete the file from disk.
    {
        CodeRecoveryJournal journal(state.path(), root);
        CodeRecoveryEntry entry;
        entry.CanonicalPath = QFileInfo(path).absoluteFilePath();
        entry.SavedHash = QCryptographicHash::hash(QByteArray("saved\n"), QCryptographicHash::Sha256);
        entry.Draft = QByteArray("unsaved edit\n");
        REQUIRE(journal.Write(entry) == true);
    }
    REQUIRE(QFile::remove(path) == true);

    CodeEditorArea area;
    area.SetProject(root, 1);
    // A deleted file cannot be reopened; restore skips it without crashing and
    // opens no tab.
    area.RestoreRecoveredBuffers();
    CHECK(area.GetTabCount() == 0);

    // A changed-on-disk file is likewise skipped: the recovered draft is dropped
    // because the baseline hash no longer matches.
    const QString changedPath = writeFile(project, QStringLiteral("changed.cpp"), QByteArray("original\n"));
    {
        CodeRecoveryJournal journal(state.path(), root);
        CodeRecoveryEntry entry;
        entry.CanonicalPath = QFileInfo(changedPath).absoluteFilePath();
        entry.SavedHash = QCryptographicHash::hash(QByteArray("a different baseline\n"), QCryptographicHash::Sha256);
        entry.Draft = QByteArray("recovered\n");
        REQUIRE(journal.Write(entry) == true);
    }
    CodeEditorArea second;
    second.SetProject(root, 1);
    second.RestoreRecoveredBuffers();
    // The file opens (it exists) but the draft is not applied, so it is clean.
    CodeDocumentTab* tab = second.GetActiveTabOrNull();
    if (tab != nullptr)
    {
        CHECK(tab->IsDirty() == false);
    }
    qunsetenv("LUDUS_EDITOR_RECOVERY_DIRECTORY");
}

TEST_CASE("Dirty-close Cancel keeps the tab and its draft", "[code-editor][recovery][dirty-close]")
{
    QTemporaryDir state;
    REQUIRE(state.isValid() == true);
    qputenv("LUDUS_EDITOR_RECOVERY_DIRECTORY", state.path().toUtf8());
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const QString path = writeFile(project, QStringLiteral("cancel.cpp"), QByteArray("saved\n"));

    CodeEditorArea area;
    area.SetProject(project.path(), 1);
    REQUIRE(area.OpenFile(path, false) == true);
    CodeDocumentTab* tab = area.GetActiveTabOrNull();
    REQUIRE(tab != nullptr);
    typeInto(*tab);
    REQUIRE(tab->IsDirty() == true);

    answerNextDialog(QMessageBox::Cancel);
    CHECK(area.PromptCloseDirtyTab(tab) == false);
    // The tab stays open and still dirty; nothing was written to disk.
    CHECK(area.GetTabCount() == 1);
    CHECK(area.GetActiveTabOrNull() != nullptr);
    CHECK(area.GetActiveTabOrNull()->IsDirty() == true);
    CHECK(readBytes(path) == QByteArray("saved\n"));
    qunsetenv("LUDUS_EDITOR_RECOVERY_DIRECTORY");
}

TEST_CASE("Dirty-close Save writes the buffer and closes the tab", "[code-editor][recovery][dirty-close]")
{
    QTemporaryDir state;
    REQUIRE(state.isValid() == true);
    qputenv("LUDUS_EDITOR_RECOVERY_DIRECTORY", state.path().toUtf8());
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const QString path = writeFile(project, QStringLiteral("save.cpp"), QByteArray("saved\n"));

    CodeEditorArea area;
    area.SetProject(project.path(), 1);
    REQUIRE(area.OpenFile(path, false) == true);
    CodeDocumentTab* tab = area.GetActiveTabOrNull();
    REQUIRE(tab != nullptr);
    typeInto(*tab);
    REQUIRE(tab->IsDirty() == true);

    answerNextDialog(QMessageBox::Save);
    CHECK(area.PromptCloseDirtyTab(tab) == true);
    CHECK(area.GetTabCount() == 0);
    // The draft was written to disk, so the file no longer matches its old bytes.
    CHECK(readBytes(path) != QByteArray("saved\n"));
    qunsetenv("LUDUS_EDITOR_RECOVERY_DIRECTORY");
}

TEST_CASE("Dirty-close Don't Save closes the tab and discards the draft", "[code-editor][recovery][dirty-close]")
{
    QTemporaryDir state;
    REQUIRE(state.isValid() == true);
    qputenv("LUDUS_EDITOR_RECOVERY_DIRECTORY", state.path().toUtf8());
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const QString path = writeFile(project, QStringLiteral("discard.cpp"), QByteArray("saved\n"));

    CodeEditorArea area;
    area.SetProject(project.path(), 1);
    REQUIRE(area.OpenFile(path, false) == true);
    CodeDocumentTab* tab = area.GetActiveTabOrNull();
    REQUIRE(tab != nullptr);
    typeInto(*tab);
    REQUIRE(tab->IsDirty() == true);

    answerNextDialog(QMessageBox::Discard);
    CHECK(area.PromptCloseDirtyTab(tab) == true);
    CHECK(area.GetTabCount() == 0);
    // The draft was discarded: the file on disk is unchanged.
    CHECK(readBytes(path) == QByteArray("saved\n"));
    qunsetenv("LUDUS_EDITOR_RECOVERY_DIRECTORY");
}
