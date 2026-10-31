#include "internal/code_editor.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_recovery.h"
#include "internal/render_graph_recovery_store.h"
#include "internal/renderer_workspace.h"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLockFile>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace ludus::editor;
namespace
{
QString MakeRecoveryPackage(const QTemporaryDir& project)
{
    const QString package = project.filePath(QStringLiteral("graphs/Main"));
    const QDir fixture(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/tests/sdk_consumer/rg2/offscreen"));
    for (const QString& name : {QStringLiteral("main.lurender.json"),
                                QStringLiteral("rasterize.lurender.json"),
                                QStringLiteral("main.lushader.json.in"),
                                QStringLiteral("main_shaders/geometry.slang"),
                                QStringLiteral("main_shaders/color.slang"),
                                QStringLiteral("main_shaders/depth.slang")})
    {
        QFile input(fixture.filePath(name));
        REQUIRE(input.open(QIODevice::ReadOnly) == true);
        QByteArray bytes = input.readAll();
        bytes.replace("@RG2_TEST_PROFILES@", "[\"metal\"]");
        const QString path =
            QDir(package).filePath(name.endsWith(QStringLiteral(".in")) == true ? name.chopped(3) : name);
        REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()) == true);
        QFile output(path);
        REQUIRE(output.open(QIODevice::WriteOnly) == true);
        REQUIRE(output.write(bytes) == bytes.size());
    }
    return package;
}

RenderGraphRecoveryCheckpoint
ReadCheckpoint(const RenderGraphRecoveryStore& store, const QString& path, const RenderGraphDocument& document)
{
    RenderGraphRecoveryCheckpoint checkpoint;
    QString error;
    REQUIRE(store.Read(path, document, checkpoint, error) == true);
    REQUIRE(error.isEmpty() == true);
    return checkpoint;
}
} // namespace

TEST_CASE("Source checkpoints recover incomplete text and structural files as one undo without publishing",
          "[editor][source-recovery]")
{
    QTemporaryDir project;
    QTemporaryDir recovery;
    const QString package = MakeRecoveryPackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, QStringLiteral("main.lurender.json")) == RenderGraphDocumentStatus::Ok);
    const RenderGraphSnapshot baseline = document.CaptureRecoverySnapshot(RenderGraphSource::Saved);
    REQUIRE(document.SetText(QStringLiteral("main.lurender.json"), "{ incomplete JSON") ==
            RenderGraphDocumentStatus::Ok);
    REQUIRE(document.SetText(QStringLiteral("main_shaders/color.slang"), "// unsaved shader\n#error incomplete\n") ==
            RenderGraphDocumentStatus::Ok);
    RenderGraphRecoveryStore writer(recovery.path());
    QString error;
    REQUIRE(writer.Write(document, error) == true);
    const QString path = writer.GetPath(document);
    RenderGraphRecoveryStore reopened(recovery.path());
    CHECK(reopened.Find(document).contains(path) == true);
    RenderGraphDocument restored;
    REQUIRE(restored.Load(package, QStringLiteral("main.lurender.json")) == RenderGraphDocumentStatus::Ok);
    const RenderGraphRecoveryCheckpoint checkpoint = ReadCheckpoint(reopened, path, restored);
    REQUIRE(restored.RestoreRecoverySnapshot(checkpoint.Draft, checkpoint.SavedHashes, checkpoint.IsBuiltIn) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(restored.CaptureRecoverySnapshot(RenderGraphSource::Draft) ==
          document.CaptureRecoverySnapshot(RenderGraphSource::Draft));
    CHECK(restored.CaptureRecoverySnapshot(RenderGraphSource::Saved) == baseline);
    REQUIRE(restored.Undo() == true);
    CHECK(restored.IsDirty() == false);
    REQUIRE(restored.Redo() == true);
    CHECK(restored.IsDirty() == true);
    RenderGraphDocument disk;
    REQUIRE(disk.Load(package, QStringLiteral("main.lurender.json")) == RenderGraphDocumentStatus::Ok);
    CHECK(disk.CaptureRecoverySnapshot(RenderGraphSource::Saved) == baseline);
}

TEST_CASE("Recovery cannot replace dirty or externally changed sources and retains inspectable checkpoints",
          "[editor][source-recovery]")
{
    QTemporaryDir project;
    QTemporaryDir recovery;
    const QString package = MakeRecoveryPackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, QStringLiteral("main.lurender.json")) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.SetText(QStringLiteral("main_shaders/color.slang"), "// recovered\n") ==
            RenderGraphDocumentStatus::Ok);
    RenderGraphRecoveryStore store(recovery.path());
    QString error;
    REQUIRE(store.Write(document, error) == true);
    const RenderGraphRecoveryCheckpoint checkpoint = ReadCheckpoint(store, store.GetPath(document), document);
    const RenderGraphSnapshot draft = document.CaptureRecoverySnapshot(RenderGraphSource::Draft);
    CHECK(document.RestoreRecoverySnapshot(checkpoint.Draft, checkpoint.SavedHashes, false) ==
          RenderGraphDocumentStatus::Conflict);
    CHECK(document.CaptureRecoverySnapshot(RenderGraphSource::Draft) == draft);
    REQUIRE(document.Undo() == true);
    QFile external(QDir(package).filePath(QStringLiteral("main_shaders/color.slang")));
    REQUIRE(external.open(QIODevice::WriteOnly | QIODevice::Truncate) == true);
    external.write("// external edit\n");
    external.close();
    const RenderGraphSnapshot accepted = document.CaptureRecoverySnapshot(RenderGraphSource::Draft);
    CHECK(document.RestoreRecoverySnapshot(checkpoint.Draft, checkpoint.SavedHashes, false) ==
          RenderGraphDocumentStatus::Conflict);
    CHECK(document.CaptureRecoverySnapshot(RenderGraphSource::Draft) == accepted);
    CHECK(QFileInfo::exists(store.GetPath(document)) == true);
    bool hasChanged = false;
    REQUIRE(document.RefreshFromDisk(hasChanged) == RenderGraphDocumentStatus::Ok);
    CHECK(document.RestoreRecoverySnapshot(checkpoint.Draft, checkpoint.SavedHashes, false) ==
          RenderGraphDocumentStatus::Conflict);
}

TEST_CASE("Recovery slots isolate concurrent editors and checksum deletion protects newer checkpoints",
          "[editor][source-recovery]")
{
    QTemporaryDir project;
    QTemporaryDir recovery;
    const QString package = MakeRecoveryPackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, QStringLiteral("main.lurender.json")) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.SetText(QStringLiteral("main_shaders/color.slang"), "// first\n") ==
            RenderGraphDocumentStatus::Ok);
    RenderGraphRecoveryStore first(recovery.path());
    RenderGraphRecoveryStore second(recovery.path());
    QString error;
    REQUIRE(first.Write(document, error) == true);
    REQUIRE(second.Write(document, error) == true);
    CHECK(first.GetPath(document) != second.GetPath(document));
    const RenderGraphRecoveryCheckpoint original = ReadCheckpoint(first, first.GetPath(document), document);
    REQUIRE(document.SetText(QStringLiteral("main_shaders/color.slang"), "// newer\n") ==
            RenderGraphDocumentStatus::Ok);
    REQUIRE(first.Write(document, error) == true);
    CHECK(first.Remove(first.GetPath(document), original.Digest, error) == false);
    CHECK(QFileInfo::exists(first.GetPath(document)) == true);
    const RenderGraphRecoveryCheckpoint current = ReadCheckpoint(first, first.GetPath(document), document);
    REQUIRE(first.Remove(first.GetPath(document), current.Digest, error) == true);
    CHECK(QFileInfo::exists(second.GetPath(document)) == true);
}

TEST_CASE("Damaged oversized and escaping recovery data preserve accepted history and files",
          "[editor][source-recovery]")
{
    QTemporaryDir project;
    QTemporaryDir recovery;
    const QString package = MakeRecoveryPackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, QStringLiteral("main.lurender.json")) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.SetText(QStringLiteral("main_shaders/color.slang"), "// recover\n") ==
            RenderGraphDocumentStatus::Ok);
    RenderGraphRecoveryStore store(recovery.path());
    QString error;
    REQUIRE(store.Write(document, error) == true);
    const QString path = store.GetPath(document);
    const RenderGraphRecoveryCheckpoint checkpoint = ReadCheckpoint(store, path, document);
    REQUIRE(document.Undo() == true);
    const RenderGraphSnapshot before = document.CaptureRecoverySnapshot(RenderGraphSource::Draft);
    for (const QString& name : {QStringLiteral("../outside.slang"),
                                QStringLiteral("/outside.slang"),
                                QStringLiteral("out/generated.slang"),
                                QStringLiteral(".hidden/source.slang"),
                                QStringLiteral("C:outside.slang")})
    {
        RenderGraphSnapshot invalid = checkpoint.Draft;
        invalid.Files.insert(name, "unsafe");
        CHECK(document.RestoreRecoverySnapshot(invalid, checkpoint.SavedHashes, false) ==
              RenderGraphDocumentStatus::InvalidEdit);
        CHECK(document.CaptureRecoverySnapshot(RenderGraphSource::Draft) == before);
    }
    RenderGraphSnapshot oversized = checkpoint.Draft;
    oversized.Files[QStringLiteral("main_shaders/color.slang")] =
        QByteArray(RenderGraphDocument::MAX_FILE_BYTES + 1, 'x');
    CHECK(document.RestoreRecoverySnapshot(oversized, checkpoint.SavedHashes, false) ==
          RenderGraphDocumentStatus::Capacity);
    CHECK(document.CaptureRecoverySnapshot(RenderGraphSource::Draft) == before);
    QFile damaged(path);
    REQUIRE(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate) == true);
    damaged.write("damaged");
    damaged.close();
    RenderGraphRecoveryCheckpoint output = checkpoint;
    CHECK(store.Read(path, document, output, error) == false);
    CHECK(output.Digest == checkpoint.Digest);
    CHECK(store.Remove(path, checkpoint.Digest, error) == false);
}

TEST_CASE("Built-in shader recovery creates no project package and rejects a changed baseline",
          "[editor][source-recovery]")
{
    QTemporaryDir project;
    QTemporaryDir recovery;
    const QString source = MakeRecoveryPackage(project);
    const QString destination = project.filePath(QStringLiteral("not-created/Main"));
    RenderGraphDocument document;
    REQUIRE(document.LoadBuiltIn(source, QStringLiteral("main.lurender.json"), destination) ==
            RenderGraphDocumentStatus::Ok);
    REQUIRE(document.SetText(QStringLiteral("main_shaders/color.slang"), "// builtin draft\n") ==
            RenderGraphDocumentStatus::Ok);
    RenderGraphRecoveryStore store(recovery.path());
    QString error;
    REQUIRE(store.Write(document, error) == true);
    const RenderGraphRecoveryCheckpoint checkpoint = ReadCheckpoint(store, store.GetPath(document), document);
    REQUIRE(document.Undo() == true);
    REQUIRE(document.RestoreRecoverySnapshot(checkpoint.Draft, checkpoint.SavedHashes, true) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(document.IsBuiltIn() == true);
    CHECK(QFileInfo::exists(destination) == false);
    REQUIRE(document.Undo() == true);
    QMap<QString, QByteArray> different = checkpoint.SavedHashes;
    different[QStringLiteral("main_shaders/color.slang")] = "different";
    CHECK(document.RestoreRecoverySnapshot(checkpoint.Draft, different, true) == RenderGraphDocumentStatus::Conflict);
}

TEST_CASE("Workspace destruction checkpoints the current draft and recovery stays explicit",
          "[editor][source-recovery][recovery-workspace]")
{
    QTemporaryDir project;
    const QString package = MakeRecoveryPackage(project);
    const QString graph = QDir(package).filePath(QStringLiteral("main.lurender.json"));
    {
        RendererWorkspace workspace(ToolingPaths{});
        workspace.SetProject(project.path(), 1);
        REQUIRE(workspace.OpenGraph(graph) == true);
        REQUIRE(workspace.ShowSourceView() == true);
        CodeEditor* sourceOrNull = workspace.findChild<CodeEditor*>(QStringLiteral("renderGraphSource"));
        REQUIRE(sourceOrNull != nullptr);
        QTest::keyClicks(sourceOrNull, QStringLiteral("// preserved at shutdown "));
        REQUIRE(workspace.IsDirty() == true);
    }
    RendererWorkspace reopened(ToolingPaths{});
    reopened.SetProject(project.path(), 1);
    REQUIRE(reopened.OpenGraph(graph) == true);
    CHECK(reopened.IsDirty() == false);
    RenderGraphRecovery* recoveryOrNull = reopened.findChild<RenderGraphRecovery*>();
    REQUIRE(recoveryOrNull != nullptr);
    RenderGraphRecoveryStore inventory(qEnvironmentVariable("LUDUS_EDITOR_RECOVERY_DIRECTORY"));
    const QStringList paths = inventory.Find(reopened.GetDocument());
    REQUIRE(paths.isEmpty() == false);
    REQUIRE(recoveryOrNull->Restore(paths.first()) == true);
    CHECK(reopened.IsDirty() == true);
    REQUIRE(reopened.CanUndo() == true);
    reopened.Undo();
    CHECK(reopened.IsDirty() == false);
}

TEST_CASE("A recovery transaction retains newly authored and removed files without saving them",
          "[editor][source-recovery]")
{
    QTemporaryDir project;
    QTemporaryDir recovery;
    const QString package = MakeRecoveryPackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, QStringLiteral("main.lurender.json")) == RenderGraphDocumentStatus::Ok);
    RenderGraphSnapshot edited = document.CaptureRecoverySnapshot(RenderGraphSource::Draft);
    edited.Files.insert(QStringLiteral("new/nested.slang"), "// new unsaved file\n");
    edited.Files.remove(QStringLiteral("main_shaders/depth.slang"));
    QMap<QString, QByteArray> hashes;
    for (const QString& file : document.GetFiles(RenderGraphSource::Saved))
    {
        hashes.insert(file, QCryptographicHash::hash(document.GetSaved(file), QCryptographicHash::Sha256).toHex());
    }
    REQUIRE(document.RestoreRecoverySnapshot(edited, hashes, false) == RenderGraphDocumentStatus::Ok);
    RenderGraphRecoveryStore store(recovery.path());
    QString error;
    REQUIRE(store.Write(document, error) == true);
    const RenderGraphRecoveryCheckpoint checkpoint = ReadCheckpoint(store, store.GetPath(document), document);
    REQUIRE(document.Undo() == true);
    REQUIRE(document.RestoreRecoverySnapshot(checkpoint.Draft, checkpoint.SavedHashes, false) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(document.GetFiles().contains(QStringLiteral("new/nested.slang")) == true);
    CHECK(document.GetFiles().contains(QStringLiteral("main_shaders/depth.slang")) == false);
    CHECK(QFileInfo::exists(QDir(package).filePath(QStringLiteral("new/nested.slang"))) == false);
    CHECK(QFileInfo::exists(QDir(package).filePath(QStringLiteral("main_shaders/depth.slang"))) == true);
}

TEST_CASE("Throttled checkpoints advance and explicit discard cannot resurrect on shutdown",
          "[editor][source-recovery]")
{
    QTemporaryDir project;
    const QString package = MakeRecoveryPackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, QStringLiteral("main.lurender.json")) == RenderGraphDocumentStatus::Ok);
    RenderGraphRecovery recovery(document);
    const QString root = qEnvironmentVariable("LUDUS_EDITOR_RECOVERY_DIRECTORY");
    RenderGraphRecoveryStore inventory(root);
    REQUIRE(document.SetText(QStringLiteral("main_shaders/color.slang"), "// one\n") == RenderGraphDocumentStatus::Ok);
    recovery.Update();
    REQUIRE(QTest::qWaitFor([&]() { return inventory.Find(document).isEmpty() == false; }, 5000) == true);
    const QString path = inventory.Find(document).first();
    const RenderGraphRecoveryCheckpoint first = ReadCheckpoint(inventory, path, document);
    REQUIRE(document.SetText(QStringLiteral("main_shaders/color.slang"), "// two\n") == RenderGraphDocumentStatus::Ok);
    recovery.Update();
    REQUIRE(recovery.CheckpointNow() == true);
    const RenderGraphRecoveryCheckpoint second = ReadCheckpoint(inventory, path, document);
    CHECK(second.Digest != first.Digest);
    QString error;
    REQUIRE(inventory.Remove(path, second.Digest, error) == true);
    REQUIRE(QTest::qWaitFor([&]() { return QFileInfo::exists(path); }, 5000) == true);
    recovery.DiscardCurrent();
    recovery.Leave();
    QTest::qWait(3100);
    CHECK(QFileInfo::exists(path) == false);
}

#if defined(Q_OS_UNIX)
TEST_CASE("Linked recovery locations and checkpoint files are never followed", "[editor][source-recovery]")
{
    QTemporaryDir project;
    QTemporaryDir recovery;
    QTemporaryDir outside;
    const QString package = MakeRecoveryPackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, QStringLiteral("main.lurender.json")) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.SetText(QStringLiteral("main_shaders/color.slang"), "// recover\n") ==
            RenderGraphDocumentStatus::Ok);
    const QString linked = recovery.filePath(QStringLiteral("linked"));
    REQUIRE(QFile::link(outside.path(), linked) == true);
    RenderGraphRecoveryStore badDirectory(linked);
    QString error;
    CHECK(badDirectory.Write(document, error) == false);
    CHECK(QDir(outside.path()).entryList(QDir::Files).isEmpty() == true);
    RenderGraphRecoveryStore store(recovery.path());
    const QString target = outside.filePath(QStringLiteral("untouched"));
    QFile file(target);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    file.write("original");
    file.close();
    REQUIRE(QFile::link(target, store.GetPath(document)) == true);
    CHECK(store.Write(document, error) == false);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    CHECK(file.readAll() == QByteArrayLiteral("original"));
}
#endif

TEST_CASE("Recovery export cannot overwrite source files and unavailable storage is explicit",
          "[editor][source-recovery]")
{
    QTemporaryDir directory;
    const QString destination = directory.filePath(QStringLiteral("copy.slang"));
    const QByteArray bytes = QByteArrayLiteral("// recovered\0raw bytes");
    QString error;
    REQUIRE(WriteRecoveryCopy(bytes, destination, error) == true);
    CHECK(WriteRecoveryCopy(QByteArrayLiteral("replacement"), destination, error) == false);
    QFile file(destination);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    CHECK(file.readAll() == bytes);
    RenderGraphRecoveryStore unavailable(QString{});
    QTemporaryDir project;
    const QString package = MakeRecoveryPackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, QStringLiteral("main.lurender.json")) == RenderGraphDocumentStatus::Ok);
    CHECK(unavailable.Write(document, error) == false);
    CHECK(unavailable.GetPath(document).isEmpty() == true);
}

TEST_CASE("A busy checkpoint slot preserves its last completed bytes and cannot be discarded concurrently",
          "[editor][source-recovery]")
{
    QTemporaryDir project;
    QTemporaryDir recovery;
    const QString package = MakeRecoveryPackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, QStringLiteral("main.lurender.json")) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.SetText(QStringLiteral("main_shaders/color.slang"), "// first\n") ==
            RenderGraphDocumentStatus::Ok);
    RenderGraphRecoveryStore store(recovery.path());
    QString error;
    REQUIRE(store.Write(document, error) == true);
    const QString path = store.GetPath(document);
    const RenderGraphRecoveryCheckpoint first = ReadCheckpoint(store, path, document);
    QLockFile lock(path + QStringLiteral(".lock"));
    REQUIRE(lock.tryLock(0) == true);
    REQUIRE(document.SetText(QStringLiteral("main_shaders/color.slang"), "// newer\n") ==
            RenderGraphDocumentStatus::Ok);
    CHECK(store.Write(document, error) == false);
    CHECK(store.Remove(path, first.Digest, error) == false);
    CHECK(ReadCheckpoint(store, path, document).Digest == first.Digest);
    lock.unlock();
    REQUIRE(store.Write(document, error) == true);
    CHECK(ReadCheckpoint(store, path, document).Digest != first.Digest);
}

TEST_CASE("A rejected recovery remains visible in its review dialog with the accepted draft intact",
          "[editor][source-recovery][recovery-review]")
{
    QTemporaryDir project;
    const QString package = MakeRecoveryPackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, QStringLiteral("main.lurender.json")) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.SetText(QStringLiteral("main_shaders/color.slang"), "// recovered\n") ==
            RenderGraphDocumentStatus::Ok);
    RenderGraphRecoveryStore store(qEnvironmentVariable("LUDUS_EDITOR_RECOVERY_DIRECTORY"));
    QString error;
    REQUIRE(store.Write(document, error) == true);
    REQUIRE(document.Undo() == true);
    const RenderGraphSnapshot accepted = document.CaptureRecoverySnapshot(RenderGraphSource::Draft);
    QFile external(QDir(package).filePath(QStringLiteral("main_shaders/color.slang")));
    REQUIRE(external.open(QIODevice::WriteOnly | QIODevice::Truncate) == true);
    external.write("// external\n");
    external.close();
    RenderGraphRecovery recovery(document);
    bool hasReportedFailure = false;
    QTimer::singleShot(0, &recovery, [&]() {
        QDialog* dialogOrNull = recovery.findChild<QDialog*>(QStringLiteral("renderGraphRecoveryDialog"));
        if (dialogOrNull != nullptr)
        {
            QPushButton* restoreOrNull = dialogOrNull->findChild<QPushButton*>(QStringLiteral("restoreRecoveryDraft"));
            QLabel* messageOrNull = dialogOrNull->findChild<QLabel*>(QStringLiteral("recoveryReviewMessage"));
            if (restoreOrNull != nullptr && messageOrNull != nullptr)
            {
                restoreOrNull->click();
                hasReportedFailure =
                    dialogOrNull->isVisible() == true && messageOrNull->text().contains(QStringLiteral("baseline"));
            }
            dialogOrNull->reject();
        }
    });
    recovery.Review();
    CHECK(hasReportedFailure == true);
    CHECK(document.CaptureRecoverySnapshot(RenderGraphSource::Draft) == accepted);
    CHECK(QFileInfo::exists(store.GetPath(document)) == true);
}

TEST_CASE("Review can copy the in-memory draft without a checkpoint or overwriting it",
          "[editor][source-recovery][recovery-review]")
{
    QTemporaryDir project;
    const QString package = MakeRecoveryPackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, QStringLiteral("main.lurender.json")) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.SetText(QStringLiteral("main_shaders/color.slang"), "// in memory\n") ==
            RenderGraphDocumentStatus::Ok);
    const RenderGraphSnapshot accepted = document.CaptureRecoverySnapshot(RenderGraphSource::Draft);
    RenderGraphRecovery recovery(document);
    bool canCopyCurrent = false;
    QTimer::singleShot(0, &recovery, [&]() {
        QDialog* dialogOrNull = recovery.findChild<QDialog*>(QStringLiteral("renderGraphRecoveryDialog"));
        if (dialogOrNull != nullptr)
        {
            QPushButton* copyOrNull = dialogOrNull->findChild<QPushButton*>(QStringLiteral("saveRecoveryCopy"));
            QPushButton* restoreOrNull = dialogOrNull->findChild<QPushButton*>(QStringLiteral("restoreRecoveryDraft"));
            QComboBox* choicesOrNull = dialogOrNull->findChild<QComboBox*>(QStringLiteral("recoveryCheckpoints"));
            canCopyCurrent = copyOrNull != nullptr && copyOrNull->isEnabled() == true && restoreOrNull != nullptr &&
                             restoreOrNull->isEnabled() == false && choicesOrNull != nullptr &&
                             choicesOrNull->currentData().toString() == QStringLiteral("current-draft");
            dialogOrNull->reject();
        }
    });
    recovery.Review();
    CHECK(canCopyCurrent == true);
    CHECK(document.CaptureRecoverySnapshot(RenderGraphSource::Draft) == accepted);
}

TEST_CASE("Discard waits for the remaining close prompts and resumes only after cancellation",
          "[editor][source-recovery][recovery-close]")
{
    QTemporaryDir project;
    const QString package = MakeRecoveryPackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, QStringLiteral("main.lurender.json")) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.SetText(QStringLiteral("main_shaders/color.slang"), "// unsaved\n") ==
            RenderGraphDocumentStatus::Ok);
    QWidget host;
    RenderGraphRecovery recovery(document, &host);
    host.show();
    REQUIRE(recovery.CheckpointNow() == true);
    RenderGraphRecoveryStore observer(qEnvironmentVariable("LUDUS_EDITOR_RECOVERY_DIRECTORY"));
    const QStringList checkpoints = observer.Find(document);
    REQUIRE(checkpoints.size() == 1);
    const QString path = checkpoints.first();
    recovery.DiscardCurrent();
    QDialog otherOwnerPrompt(&host);
    bool hasKeptDiscard = false;
    QTimer::singleShot(3300, &otherOwnerPrompt, [&]() {
        hasKeptDiscard = QFileInfo::exists(path) == false;
        otherOwnerPrompt.reject();
    });
    static_cast<void>(otherOwnerPrompt.exec());
    REQUIRE(hasKeptDiscard == true);
    SECTION("The close was accepted")
    {
        host.hide();
        QTest::qWait(3100);
        CHECK(QFileInfo::exists(path) == false);
        recovery.Leave();
        CHECK(QFileInfo::exists(path) == false);
    }
    SECTION("Another owner cancelled the close")
    {
        REQUIRE(QTest::qWaitFor([&]() { return QFileInfo::exists(path); }, 5000) == true);
        CHECK(document.IsDirty() == true);
    }
}
