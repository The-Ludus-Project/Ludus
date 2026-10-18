#include "internal/editor_files.h"
#include "internal/project_creation_dialog.h"
#include "internal/project_setup_dialog.h"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>

using namespace ludus::editor;

TEST_CASE("New Project uses a parent picker and protects existing destinations", "[editor][setup]")
{
    QTemporaryDir directory;
    ProjectCreationDialog dialog;
    dialog.show();
    auto* name = dialog.findChild<QLineEdit*>(QStringLiteral("newProjectName"));
    auto* location = dialog.findChild<QLineEdit*>(QStringLiteral("newProjectLocation"));
    auto* sdk = dialog.findChild<QLineEdit*>(QStringLiteral("newProjectSdk"));
    auto* browse = dialog.findChild<QPushButton*>(QStringLiteral("browseProjectLocation"));
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    REQUIRE(name != nullptr);
    REQUIRE(location != nullptr);
    REQUIRE(sdk != nullptr);
    REQUIRE(browse != nullptr);
    REQUIRE(buttons != nullptr);
    CHECK_FALSE(sdk->isVisible());
    location->setText(directory.path());
    name->setText(QStringLiteral("My Game"));
    CHECK(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    CHECK(dialog.Options().Destination == directory.filePath(QStringLiteral("My Game")));
    CHECK(dialog.Options().Sdk.isEmpty());
    CHECK_FALSE(dialog.Options().PrepareEngine);
    REQUIRE(QDir(directory.path()).mkdir(QStringLiteral("My Game")));
    name->setText(QStringLiteral("Other"));
    name->setText(QStringLiteral("My Game"));
    CHECK_FALSE(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    for (const auto& invalid : {QString(), QStringLiteral("../escape"), QStringLiteral("."), QStringLiteral("..")})
    {
        name->setText(invalid);
        CHECK_FALSE(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    }
    name->setText(QStringLiteral("New"));
    location->setText(QStringLiteral("relative"));
    CHECK_FALSE(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    browse->click();
    QApplication::processEvents();
    auto* picker = dialog.findChild<QFileDialog*>();
    REQUIRE(picker != nullptr);
    CHECK(picker->fileMode() == QFileDialog::Directory);
    picker->reject();
    CHECK(location->text() == QStringLiteral("relative"));
    dialog.reject();
    CHECK(QDir(directory.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot) ==
          QStringList{QStringLiteral("My Game")});
}

TEST_CASE("Project repair reuses the selected engine without asking for paths", "[editor][setup]")
{
    QTemporaryDir project;
    ProjectSetupDialog dialog(project.path());
    dialog.show();
    auto* engine = dialog.findChild<QComboBox*>(QStringLiteral("setupEngineChoice"));
    auto* native = dialog.findChild<QLineEdit*>(QStringLiteral("setupSdk"));
    auto* web = dialog.findChild<QLineEdit*>(QStringLiteral("setupWebSdk"));
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    REQUIRE(engine != nullptr);
    REQUIRE(native != nullptr);
    REQUIRE(web != nullptr);
    REQUIRE(buttons != nullptr);
    CHECK_FALSE(native->isVisible());
    CHECK_FALSE(web->isVisible());
    CHECK(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    CHECK(dialog.Options().Sdk.isEmpty());
    CHECK_FALSE(dialog.Options().PrepareEngine);
    CHECK(dialog.Options().DisableWeb);
    CHECK(QDir(project.path()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());

    engine->setCurrentIndex(2);
    CHECK(native->isVisible());
    CHECK_FALSE(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    native->setText(QStringLiteral(" /installed/desktop "));
    CHECK_FALSE(buttons->button(QDialogButtonBox::Ok)->isEnabled()); // missing folders are not installs
    QTemporaryDir install;
    REQUIRE(QDir(install.path()).mkpath(QStringLiteral("share/Ludus")));
    QFile manifest(QDir(install.path()).filePath(QStringLiteral("share/Ludus/LudusSdkManifest.json")));
    REQUIRE(manifest.open(QIODevice::WriteOnly));
    const QByteArray manifestBytes = QByteArrayLiteral("{\"name\":\"Ludus\",\"build_flavor\":\"Development\"}");
    REQUIRE(manifest.write(manifestBytes) == manifestBytes.size());
    manifest.close();
    native->setText(QStringLiteral(" %1 ").arg(install.path()));
    CHECK(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    CHECK(dialog.Options().Sdk == install.path());
    engine->setCurrentIndex(1);
    CHECK_FALSE(native->isVisible());
    CHECK(dialog.Options().Sdk.isEmpty());
    CHECK(dialog.Options().PrepareEngine);
    engine->setCurrentIndex(0);
    CHECK(dialog.Options().Sdk.isEmpty());
    CHECK_FALSE(dialog.Options().PrepareEngine);
}

TEST_CASE("Browser repair needs a browser engine only when enabled", "[editor][setup]")
{
    QTemporaryDir project;
    ProjectSetupDialog dialog(project.path());
    dialog.show();
    auto* browser = dialog.findChild<QCheckBox*>(QStringLiteral("setupBrowser"));
    auto* web = dialog.findChild<QLineEdit*>(QStringLiteral("setupWebSdk"));
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    REQUIRE(browser != nullptr);
    REQUIRE(web != nullptr);
    REQUIRE(buttons != nullptr);
    browser->setChecked(true);
    CHECK(web->isVisible());
    CHECK_FALSE(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    web->setText(QStringLiteral(" /installed/browser "));
    CHECK(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    CHECK(dialog.Options().WebSdk == QStringLiteral("/installed/browser"));
    CHECK_FALSE(dialog.Options().DisableWeb);
    browser->setChecked(false);
    CHECK_FALSE(web->isVisible());
    CHECK(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    CHECK(dialog.Options().WebSdk.isEmpty());
    CHECK(dialog.Options().DisableWeb);
}

TEST_CASE("Project repair displays saved browser setup and can turn it off", "[editor][setup]")
{
    QTemporaryDir project;
    REQUIRE(QDir(project.path()).mkdir(QStringLiteral(".ludus")));
    QFile saved(project.filePath(QStringLiteral(".ludus/setup.json")));
    REQUIRE(saved.open(QIODevice::WriteOnly));
    const QByteArray metadata = QByteArrayLiteral("{\"web_sdk\":\"/old/browser-engine\",\"signature\":\"previous\"}");
    REQUIRE(saved.write(metadata) == metadata.size());
    saved.close();
    ProjectSetupDialog dialog(project.path());
    dialog.show();
    auto* browser = dialog.findChild<QCheckBox*>(QStringLiteral("setupBrowser"));
    auto* web = dialog.findChild<QLineEdit*>(QStringLiteral("setupWebSdk"));
    REQUIRE(browser != nullptr);
    REQUIRE(web != nullptr);
    CHECK(browser->isChecked());
    CHECK(web->isVisible());
    CHECK(dialog.Options().WebSdk == QStringLiteral("/old/browser-engine"));
    browser->setChecked(false);
    CHECK(dialog.Options().DisableWeb);
    REQUIRE(saved.open(QIODevice::ReadOnly));
    CHECK(saved.readAll() == metadata);
}

TEST_CASE("Bundled samples are complete writable copies", "[editor][setup]")
{
    QTemporaryDir samples;
    QString first;
    QString second;
    REQUIRE(CopyEditorSample(QStringLiteral("cornell-box"), first, samples.path()));
    REQUIRE(CopyEditorSample(QStringLiteral("cornell-box"), second, samples.path()));
    CHECK(first != second);
    const auto root = QFileInfo(first).absolutePath();
    CHECK(QFileInfo::exists(QDir(root).filePath(QStringLiteral("src/main.cpp"))));
    CHECK(QFileInfo::exists(QDir(root).filePath(QStringLiteral("shaders/cornell.slang"))));
    CHECK(QFileInfo::exists(QDir(root).filePath(QStringLiteral("CMakeLists.txt"))));
    QFile descriptor(first);
    REQUIRE(descriptor.open(QIODevice::WriteOnly | QIODevice::Append));
    REQUIRE(descriptor.write("\n") == 1);
    descriptor.close();
    QString preserved = first;
    CHECK_FALSE(CopyEditorSample(QStringLiteral("../escape"), preserved, samples.path()));
    CHECK(preserved == first);
    REQUIRE(QDir(root).removeRecursively());
    REQUIRE(QDir(QFileInfo(second).absolutePath()).removeRecursively());
}

TEST_CASE("Project exports round trip binary assets and source paths", "[editor][setup]")
{
    QTemporaryDir project;
    REQUIRE(QDir(project.path()).mkpath(QStringLiteral("assets/nested")));
    REQUIRE(QDir(project.path()).mkpath(QStringLiteral(".ludus")));
    const QByteArray original("\0asset\xff", 7);
    QFile asset(project.filePath(QStringLiteral("assets/nested/image.bin")));
    REQUIRE(asset.open(QIODevice::WriteOnly));
    REQUIRE(asset.write(original) == original.size());
    asset.close();
    QFile local(project.filePath(QStringLiteral(".ludus/setup.json")));
    REQUIRE(local.open(QIODevice::WriteOnly));
    local.write("{}");
    local.close();
    QByteArray archive;
    REQUIRE(EncodeEditorProjectArchive(project.path(), archive));
    QTemporaryDir output;
    const auto archivePath = output.filePath(QStringLiteral("project.tar"));
    QFile saved(archivePath);
    REQUIRE(saved.open(QIODevice::WriteOnly));
    REQUIRE(saved.write(archive) == archive.size());
    saved.close();
    QProcess tar;
    tar.start(QStringLiteral("/usr/bin/tar"), {QStringLiteral("-tf"), archivePath});
    REQUIRE(tar.waitForFinished());
    CHECK(tar.exitCode() == 0);
    CHECK_FALSE(tar.readAllStandardOutput().contains("setup.json"));
    tar.start(QStringLiteral("/usr/bin/tar"),
              {QStringLiteral("-xOf"), archivePath, QStringLiteral("assets/nested/image.bin")});
    REQUIRE(tar.waitForFinished());
    CHECK(tar.exitCode() == 0);
    CHECK(tar.readAllStandardOutput() == original);
    const auto previous = archive;
    CHECK_FALSE(EncodeEditorProjectArchive(project.filePath(QStringLiteral("missing")), archive));
    CHECK(archive == previous);
    REQUIRE(QFile::link(asset.fileName(), project.filePath(QStringLiteral("link.bin"))));
    CHECK_FALSE(EncodeEditorProjectArchive(project.path(), archive));
    CHECK(archive == previous);
}

TEST_CASE("Project folder imports reject escaping or incomplete trees before reading files", "[editor][setup]")
{
    QString descriptor = QStringLiteral("previous");
    REQUIRE(ValidateEditorImportPaths({QStringLiteral("Game/src/game.cpp"),
                                       QStringLiteral("Game/ludus.project.json"),
                                       QStringLiteral("Game/assets/image.png")},
                                      true,
                                      descriptor));
    CHECK(descriptor == QStringLiteral("Game/ludus.project.json"));
    const auto previous = descriptor;
    for (const auto& invalid : {QStringLiteral("Game/../outside.cpp"),
                                QStringLiteral("/absolute.cpp"),
                                QStringLiteral("Game//empty.cpp"),
                                QStringLiteral("Other/game.cpp"),
                                QStringLiteral("Game/./game.cpp"),
                                QStringLiteral("Game\\escape.cpp"),
                                QStringLiteral("Game/ludus.project.json")})
    {
        CHECK_FALSE(ValidateEditorImportPaths({QStringLiteral("Game/ludus.project.json"), invalid}, true, descriptor));
        CHECK(descriptor == previous);
    }
    CHECK_FALSE(ValidateEditorImportPaths({QStringLiteral("Game/nested/ludus.project.json")}, true, descriptor));
    CHECK_FALSE(ValidateEditorImportPaths({}, true, descriptor));
    CHECK(descriptor == previous);
    CHECK(ValidateEditorImportPaths({QStringLiteral("preferences.json")}, false, descriptor));
    CHECK(descriptor == QStringLiteral("preferences.json"));
}
