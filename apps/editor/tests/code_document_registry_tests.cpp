#include "internal/code_document_registry.h"
#include "internal/code_document_tab.h"
#include "internal/code_editor.h"
#include "internal/code_editor_area.h"
#include "internal/code_file_document.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <QTest>

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

// Type one character into a tab's editor so its controller commits a host edit.
void typeInto(CodeDocumentTab& inoutTab)
{
    inoutTab.GetEditor().setFocus();
    QTest::keyClick(&inoutTab.GetEditor(), Qt::Key_Z);
    QCoreApplication::processEvents();
}
} // namespace

TEST_CASE("Registry keeps one host per canonical path and epoch", "[code-editor][registry]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString path = writeFile(directory, QStringLiteral("a.txt"), QByteArray("alpha\n"));

    CodeDocumentRegistry registry;
    registry.SetProjectEpoch(1);
    CodeFileLoadStatus firstStatus = CodeFileLoadStatus::Failed;
    QString explanation;
    CodeFileDocument* first = registry.AcquireHost(path, firstStatus, explanation);
    REQUIRE(first != nullptr);
    CodeFileLoadStatus secondStatus = CodeFileLoadStatus::Failed;
    CodeFileDocument* second = registry.AcquireHost(path, secondStatus, explanation);
    // Two acquires of the same path share one host.
    CHECK(first == second);
    CHECK(registry.GetHostCount() == 1);

    // Releasing once keeps the host (still referenced by the second acquire).
    registry.ReleaseHost(first);
    CHECK(registry.GetHostCount() == 1);
    registry.ReleaseHost(second);
    CHECK(registry.GetHostCount() == 0);
}

TEST_CASE("Registry drops clean hosts when the epoch changes", "[code-editor][registry]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString path = writeFile(directory, QStringLiteral("b.txt"), QByteArray("beta\n"));

    CodeDocumentRegistry registry;
    registry.SetProjectEpoch(1);
    CodeFileLoadStatus status = CodeFileLoadStatus::Failed;
    QString explanation;
    CodeFileDocument* host = registry.AcquireHost(path, status, explanation);
    REQUIRE(host != nullptr);
    registry.ReleaseHost(host); // no views now, but it is clean so it is freed on release
    CHECK(registry.GetHostCount() == 0);

    // Re-acquire, then bump the epoch with no references: the host is dropped.
    host = registry.AcquireHost(path, status, explanation);
    registry.ReleaseHost(host);
    registry.SetProjectEpoch(2);
    CHECK(registry.GetHostCount() == 0);
    CHECK(registry.GetProjectEpoch() == 2);
}

TEST_CASE("A failed load reports through the status and never creates a host", "[code-editor][registry]")
{
    CodeDocumentRegistry registry;
    registry.SetProjectEpoch(1);
    CodeFileLoadStatus status = CodeFileLoadStatus::Ok;
    QString explanation;
    CodeFileDocument* host = registry.AcquireHost(QStringLiteral("/this/path/does/not/exist.txt"), status, explanation);
    CHECK(host == nullptr);
    CHECK(status == CodeFileLoadStatus::Failed);
    CHECK(explanation.isEmpty() == false);
    CHECK(registry.GetHostCount() == 0);
}

TEST_CASE("Preview tab is replaced by the next preview, editing makes it permanent", "[code-editor][registry][tabs]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString first = writeFile(directory, QStringLiteral("one.txt"), QByteArray("one\n"));
    const QString second = writeFile(directory, QStringLiteral("two.txt"), QByteArray("two\n"));

    CodeEditorArea area;
    area.GetRegistry().SetProjectEpoch(1);

    REQUIRE(area.OpenFile(first, true) == true);
    CHECK(area.GetTabCount() == 1);
    REQUIRE(area.GetActiveTabOrNull() != nullptr);
    CHECK(area.GetActiveTabOrNull()->IsPreview() == true);

    // A second preview open replaces the first preview rather than adding a tab.
    REQUIRE(area.OpenFile(second, true) == true);
    CHECK(area.GetTabCount() == 1);
    CHECK(area.GetActiveTabOrNull()->GetPath() == second);

    // Editing the preview makes it permanent; a later preview opens a new tab.
    CodeDocumentTab* tab = area.GetActiveTabOrNull();
    typeInto(*tab);
    CHECK(tab->IsPreview() == false);
    REQUIRE(area.OpenFile(first, true) == true);
    CHECK(area.GetTabCount() == 2);
}

TEST_CASE("Two groups mirror edits but keep separate views", "[code-editor][registry][mirroring]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString path = writeFile(directory, QStringLiteral("shared.txt"), QByteArray("shared line\n"));

    CodeEditorArea area;
    area.GetRegistry().SetProjectEpoch(1);
    area.resize(800, 400);
    area.show();

    REQUIRE(area.OpenFile(path, false) == true);
    REQUIRE(area.OpenFileInSecondGroup(path) == true);
    CHECK(area.GetGroupCount() == 2);
    CHECK(area.GetTabCount() == 2);

    const QList<CodeDocumentTab*> tabs = area.GetTabs();
    REQUIRE(tabs.size() == 2);
    // Both tabs bind the same shared host.
    CHECK(tabs.at(0)->GetHostOrNull() == tabs.at(1)->GetHostOrNull());

    // Edit in the first view; the second view mirrors the same text.
    typeInto(*tabs.at(0));
    QCoreApplication::processEvents();
    const QString firstText = tabs.at(0)->GetEditor().toPlainText();
    const QString secondText = tabs.at(1)->GetEditor().toPlainText();
    CHECK(firstText == secondText);
    CHECK(firstText.contains(QLatin1Char('z')) == true);
}

TEST_CASE("Tabs are saved and restored per project", "[code-editor][registry][tabs]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString first = writeFile(directory, QStringLiteral("keep1.txt"), QByteArray("first\nsecond\nthird\n"));
    const QString second = writeFile(directory, QStringLiteral("keep2.txt"), QByteArray("other\n"));

    QJsonObject saved;
    {
        CodeEditorArea area;
        area.GetRegistry().SetProjectEpoch(1);
        REQUIRE(area.OpenFile(first, false) == true);
        REQUIRE(area.OpenFile(second, false) == true);
        saved = area.SaveState();
    }
    CHECK(saved.value(QStringLiteral("tabs")).toArray().size() == 2);

    CodeEditorArea restored;
    restored.GetRegistry().SetProjectEpoch(1);
    restored.RestoreState(saved);
    CHECK(restored.GetTabCount() == 2);
    // A missing file in the saved state is skipped, not an error.
    QFile::remove(second);
    CodeEditorArea partial;
    partial.GetRegistry().SetProjectEpoch(1);
    partial.RestoreState(saved);
    CHECK(partial.GetTabCount() == 1);
}

TEST_CASE("A dirty tab close is routed to the owner, not discarded", "[code-editor][registry][tabs]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString path = writeFile(directory, QStringLiteral("dirty.txt"), QByteArray("x\n"));

    CodeEditorArea area;
    area.GetRegistry().SetProjectEpoch(1);
    area.show();
    REQUIRE(area.OpenFile(path, false) == true);
    typeInto(*area.GetActiveTabOrNull());
    CHECK(area.GetActiveTabOrNull()->IsDirty() == true);

    QSignalSpy dirtySpy(&area, &CodeEditorArea::DirtyCloseRequested);
    area.CloseActiveTab();
    // The dirty tab is not closed; its owner is asked first (CE03).
    CHECK(dirtySpy.count() == 1);
    CHECK(area.GetTabCount() == 1);
}
