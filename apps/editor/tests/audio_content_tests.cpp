#include "internal/audio_preview.h"
#include "internal/audio_workspace.h"
#include "internal/content_import.h"
#include "internal/main_window.h"
#include "wav_fixture.h"
#include <QApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <ludus/audio/content/definitions.h>

namespace
{
using namespace ludus::foundation;
namespace content = ludus::content;
namespace audio = ludus::audio;
void Save(std::string_view root, std::string_view path, const content::Bytes& bytes)
{
    REQUIRE(content::SaveFile(root, path, bytes.Data(), nullptr) == content::Status::Ok);
}
void Fixture(const QString& path)
{
    const auto root = path.toUtf8();
    content::Catalog catalog;
    content::Resource source, soundResource;
    REQUIRE(source.Id.Set("source/hit"));
    REQUIRE(source.Path.Set("hit.wav"));
    REQUIRE(catalog.Put(source) == content::Status::Ok);
    REQUIRE(soundResource.Id.Set("sound/hit"));
    REQUIRE(soundResource.Path.Set("hit.json"));
    soundResource.Type = content::Kind::Sound;
    REQUIRE(catalog.Put(soundResource) == content::Status::Ok);
    content::Bytes bytes;
    REQUIRE(catalog.Write(bytes) == content::Status::Ok);
    Save(root.constData(), "catalog.json", bytes);
    audio::content::Sound sound;
    REQUIRE(sound.Id.Set("sound/hit"));
    REQUIRE(sound.Bus.Set("sfx"));
    REQUIRE(sound.Group.Set("default"));
    REQUIRE(sound.Variations[0].Set("source/hit"));
    sound.VariationCount = 1;
    REQUIRE(audio::content::WriteSound(sound, bytes) == content::Status::Ok);
    Save(root.constData(), "hit.json", bytes);
    const auto wav = audio::test::MakeSineWav(44100, 1, 4410);
    REQUIRE(content::SaveFile(root.constData(), "hit.wav", wav, nullptr) == content::Status::Ok);
}
void Activate(ludus::editor::AudioWorkspace& workspace)
{
    auto* list = workspace.findChild<QListWidget*>(QStringLiteral("audio-resources"));
    REQUIRE(list != nullptr);
    for (int i = 0; i < list->count(); ++i)
    {
        if (list->item(i)->text() == QStringLiteral("sound/hit"))
        {
            list->setCurrentRow(i);
            Q_EMIT list->itemActivated(list->item(i));
            return;
        }
    }
    FAIL("sound resource missing");
}
void ClickSave(ludus::editor::AudioWorkspace& workspace)
{
    for (auto* button : workspace.findChildren<QPushButton*>())
    {
        if (button->text() == QStringLiteral("Save audio"))
        {
            button->click();
            return;
        }
    }
    FAIL("save action missing");
}
} // namespace
TEST_CASE("Audio editor saves drafts, reopens canonical data and preserves external conflicts", "[editor][audio]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    Fixture(directory.path());
    ludus::editor::AudioWorkspace workspace;
    workspace.SetRoot(directory.path());
    Activate(workspace);
    auto* gain = workspace.findChild<QDoubleSpinBox*>(QStringLiteral("audio-gain"));
    REQUIRE(gain != nullptr);
    gain->setValue(0.25);
    ClickSave(workspace);
    ludus::editor::AudioWorkspace reopened;
    reopened.SetRoot(directory.path());
    Activate(reopened);
    auto* saved = reopened.findChild<QDoubleSpinBox*>(QStringLiteral("audio-gain"));
    REQUIRE(saved != nullptr);
    REQUIRE(saved->value() == 0.25);
    const auto root = directory.path().toUtf8();
    content::Bytes original;
    REQUIRE(content::ReadFile(root.constData(), "hit.json", content::MAX_DOCUMENT_BYTES, original) ==
            content::Status::Ok);
    const auto digest = content::Hash(original.Data());
    const uint8 invalid[] = {'{', '}'};
    REQUIRE(content::SaveFile(root.constData(), "hit.json", invalid, &digest) == content::Status::Ok);
    gain->setValue(0.75);
    ClickSave(workspace);
    REQUIRE(gain->value() == 0.75);
    content::Bytes current;
    REQUIRE(content::ReadFile(root.constData(), "hit.json", 100, current) == content::Status::Ok);
    REQUIRE(current.String() == "{}");
}
TEST_CASE("Audio import runs away from GUI, preserves ID on reimport and quiesces on close",
          "[editor][audio][concurrency]")
{
    QTemporaryDir directory, exported;
    REQUIRE(directory.isValid());
    REQUIRE(exported.isValid());
    const auto wav = audio::test::MakeSineWav(44100, 1, 44100);
    const auto exportRoot = exported.path().toUtf8();
    REQUIRE(content::SaveFile(exportRoot.constData(), "export.wav", wav, nullptr) == content::Status::Ok);
    std::atomic<uint32> imported{0};
    std::atomic<bool> importedOnGui{false};
    ludus::editor::AudioPreview preview;
    const auto* guiThread = QThread::currentThread();
    // Observe the emitting thread directly: a fast import need not outlast a GUI timer.
    QObject::connect(
        &preview,
        &ludus::editor::AudioPreview::Imported,
        &preview,
        [&imported, &importedOnGui, guiThread]() {
            if (QThread::currentThread() == guiThread)
            {
                importedOnGui.store(true, std::memory_order_relaxed);
            }
            imported.fetch_add(1, std::memory_order_release);
        },
        Qt::DirectConnection);
    const ludus::editor::ContentImportSource source
    {
        .Root = directory.path(),
        .File = exported.path() + QStringLiteral("/export.wav"),
        .Id = QStringLiteral("source/hit"),
    };
    preview.Import(source);
    for (int i = 0; i < 500 && imported.load(std::memory_order_acquire) == 0; ++i)
    {
        QTest::qWait(10);
    }
    REQUIRE(imported.load(std::memory_order_acquire) == 1);
    REQUIRE_FALSE(importedOnGui.load(std::memory_order_relaxed));
    preview.Import(source);
    for (int i = 0; i < 500 && imported.load(std::memory_order_acquire) < 2; ++i)
    {
        QTest::qWait(10);
    }
    REQUIRE(imported.load(std::memory_order_acquire) == 2);
    REQUIRE_FALSE(importedOnGui.load(std::memory_order_relaxed));
    const auto root = directory.path().toUtf8();
    content::Bytes bytes;
    content::Catalog catalog;
    content::Diagnostic diagnostic;
    REQUIRE(content::ReadFile(root.constData(), "catalog.json", content::MAX_DOCUMENT_BYTES, bytes) ==
            content::Status::Ok);
    REQUIRE(catalog.Read(bytes.String(), diagnostic) == content::Status::Ok);
    REQUIRE(catalog.Entries().size() == 1);
    REQUIRE(catalog.Find("source/hit") != nullptr);
    preview.Stop();
    preview.Shutdown();
    for (int i = 0; i < 500 && preview.Finished() == false; ++i)
    {
        QTest::qWait(10);
    }
    REQUIRE(preview.Finished());
}

TEST_CASE("Cancelling close preserves the audio draft and does not save workspace preferences",
          "[editor][audio][layout]")
{
    QTemporaryDir directory;
    const auto contentRoot = directory.filePath(QStringLiteral("content"));
    REQUIRE(QDir().mkpath(contentRoot));
    Fixture(contentRoot);
    QFile descriptor(directory.filePath(QStringLiteral("ludus.project.json")));
    REQUIRE(descriptor.open(QIODevice::WriteOnly));
    descriptor.write(QByteArrayLiteral(R"json({"version":1,"name":"demo","provider":"cmake",
"source_dir":".","preset":"linux-clang-debug","target":"app","run":{"cwd":".","args":[]}})json"));
    descriptor.close();
    ludus::editor::EditorController controller(ludus::editor::ToolingPaths{});
    const auto preferences = directory.filePath(QStringLiteral("workspace.json"));
    ludus::editor::MainWindow window(&controller, nullptr, preferences);
    controller.OpenProject(descriptor.fileName());
    window.show();
    auto* audio = window.findChild<ludus::editor::AudioWorkspace*>();
    REQUIRE(audio != nullptr);
    Activate(*audio);
    auto* gain = audio->findChild<QDoubleSpinBox*>(QStringLiteral("audio-gain"));
    REQUIRE(gain != nullptr);
    gain->setValue(0.25);
    bool prompted = false;
    QTimer::singleShot(0, &window, [&window, &prompted]() {
        if (auto* dialog = window.findChild<QMessageBox*>())
        {
            prompted = true;
            dialog->done(QMessageBox::Cancel);
        }
    });
    CHECK_FALSE(window.close());
    CHECK(prompted);
    CHECK(window.isVisible());
    CHECK(gain->value() == 0.25);
    CHECK_FALSE(QFileInfo::exists(preferences));
    // A later accepted close can publish the layout after the draft decision.
    QTimer::singleShot(0, &window, [&window]() {
        if (auto* dialog = window.findChild<QMessageBox*>())
        {
            dialog->done(QMessageBox::Discard);
        }
    });
    (void)window.close();
    QTRY_VERIFY_WITH_TIMEOUT(window.isVisible() == false, 3000);
    CHECK(QFileInfo::exists(preferences));
}

TEST_CASE("Audio workspace rejects mesh entries even when their bytes resemble an audio definition",
          "[editor][audio][mesh]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    Fixture(directory.path());
    const auto root = directory.path().toUtf8();
    content::Bytes bytes;
    content::Diagnostic diagnostic;
    content::Catalog catalog;
    REQUIRE(content::ReadFile(root.constData(), "catalog.json", content::MAX_DOCUMENT_BYTES, bytes) ==
            content::Status::Ok);
    const auto catalogDigest = content::Hash(bytes.Data());
    REQUIRE(catalog.Read(bytes.String(), diagnostic) == content::Status::Ok);
    content::Resource mesh;
    mesh.Type = content::Kind::Mesh;
    REQUIRE(mesh.Id.Set("mesh/disguised"));
    REQUIRE(mesh.Path.Set("mesh.json"));
    REQUIRE(catalog.Put(mesh) == content::Status::Ok);
    REQUIRE(catalog.Write(bytes) == content::Status::Ok);
    REQUIRE(content::SaveFile(root.constData(), "catalog.json", bytes.Data(), &catalogDigest) == content::Status::Ok);
    audio::content::Sound disguised;
    REQUIRE(disguised.Id.Set("mesh/disguised"));
    REQUIRE(disguised.Bus.Set("sfx"));
    REQUIRE(disguised.Group.Set("default"));
    REQUIRE(disguised.Variations[0].Set("source/hit"));
    disguised.VariationCount = 1;
    REQUIRE(audio::content::WriteSound(disguised, bytes) == content::Status::Ok);
    Save(root.constData(), "mesh.json", bytes);
    ludus::editor::AudioWorkspace workspace;
    workspace.SetRoot(directory.path());
    CHECK_FALSE(workspace.OpenResource(QStringLiteral("mesh/disguised")));
    CHECK_FALSE(workspace.CanSave());
    CHECK(workspace.OpenResource(QStringLiteral("sound/hit")));
}
