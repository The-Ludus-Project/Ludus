#include "internal/editor_files.h"
#include "internal/project_creation_dialog.h"
#include "internal/project_setup_dialog.h"
#include "internal/project_store.h"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>

#include <utility>

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

TEST_CASE("New Project offers GitHub CI only outside an existing work tree", "[editor][setup]")
{
    QTemporaryDir directory;
    ProjectCreationDialog dialog;
    QLineEdit* nameOrNull = dialog.findChild<QLineEdit*>(QStringLiteral("newProjectName"));
    QLineEdit* locationOrNull = dialog.findChild<QLineEdit*>(QStringLiteral("newProjectLocation"));
    QCheckBox* gitHubCiOrNull = dialog.findChild<QCheckBox*>(QStringLiteral("newProjectGitHubCi"));
    QLabel* noteOrNull = dialog.findChild<QLabel*>(QStringLiteral("newProjectGitHubCiNote"));
    REQUIRE(nameOrNull != nullptr);
    REQUIRE(locationOrNull != nullptr);
    REQUIRE(gitHubCiOrNull != nullptr);
    REQUIRE(noteOrNull != nullptr);
    locationOrNull->setText(directory.path());
    nameOrNull->setText(QStringLiteral("Game"));
    CHECK(gitHubCiOrNull->isEnabled());
    CHECK_FALSE(gitHubCiOrNull->isChecked());
    CHECK_FALSE(dialog.Options().ShouldAddGitHubCi);
    gitHubCiOrNull->setChecked(true);
    CHECK(dialog.Options().ShouldAddGitHubCi);

    // A new folder inside a work tree can never be the repository root GitHub reads.
    REQUIRE(QDir(directory.path()).mkdir(QStringLiteral("repository")));
    REQUIRE(QDir(directory.filePath(QStringLiteral("repository"))).mkdir(QStringLiteral(".git")));
    locationOrNull->setText(directory.filePath(QStringLiteral("repository/games")));
    CHECK_FALSE(gitHubCiOrNull->isEnabled());
    CHECK_FALSE(dialog.Options().ShouldAddGitHubCi);
    CHECK(noteOrNull->text().contains(QStringLiteral("inside the Git repository")));
    locationOrNull->setText(directory.path());
    CHECK(gitHubCiOrNull->isEnabled());
    CHECK(dialog.Options().ShouldAddGitHubCi);
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

TEST_CASE("New Project limits names to the descriptor's UTF-8 bytes", "[editor][setup]")
{
    QTemporaryDir directory;
    ProjectCreationDialog dialog;
    QLineEdit* nameOrNull = dialog.findChild<QLineEdit*>(QStringLiteral("newProjectName"));
    QLineEdit* locationOrNull = dialog.findChild<QLineEdit*>(QStringLiteral("newProjectLocation"));
    QDialogButtonBox* buttonsOrNull = dialog.findChild<QDialogButtonBox*>();
    REQUIRE(nameOrNull != nullptr);
    REQUIRE(locationOrNull != nullptr);
    REQUIRE(buttonsOrNull != nullptr);
    locationOrNull->setText(directory.path());
    // A supplementary-plane character is one printable code point, as in the Python tooling.
    nameOrNull->setText(QStringLiteral("Game \U0001F600"));
    CHECK(buttonsOrNull->button(QDialogButtonBox::Ok)->isEnabled());
    // 43 three-byte characters are 129 UTF-8 bytes, beyond the descriptor's 128.
    constexpr qsizetype OVERLONG_HANGUL_CHARACTERS = 43;
    nameOrNull->setText(QString(OVERLONG_HANGUL_CHARACTERS, QChar(u'한')));
    CHECK_FALSE(buttonsOrNull->button(QDialogButtonBox::Ok)->isEnabled());
    QLabel* validationOrNull = dialog.findChild<QLabel*>(QStringLiteral("newProjectValidation"));
    REQUIRE(validationOrNull != nullptr);
    CHECK(validationOrNull->text().contains(QStringLiteral("128 UTF-8 bytes")));
}

TEST_CASE("Project targets derive from names as the Python tooling does", "[editor][setup]")
{
    // Equal to SESSION_TARGET_CASES in scripts/python/test_ludus_tools.py; change both together.
    constexpr qsizetype LONG_NAME_CHARACTERS = 130;
    constexpr qsizetype TARGET_CHARACTERS = 128;
    const QList<std::pair<QString, QString>> cases = {
        {QStringLiteral("New Game"), QStringLiteral("New_Game")},
        {QStringLiteral("Jeux Vidéo 日本語"), QStringLiteral("Jeux_Vid_o")},
        {QStringLiteral("  game  "), QStringLiteral("game")},
        {QStringLiteral("-.+_"), QStringLiteral("app")},
        {QStringLiteral("日本"), QStringLiteral("app")},
        {QStringLiteral("a\U0001F600b"), QStringLiteral("a_b")},
        {QStringLiteral("my.game+1-x"), QStringLiteral("my.game+1-x")},
        {QString(LONG_NAME_CHARACTERS, QLatin1Char('x')), QString(TARGET_CHARACTERS, QLatin1Char('x'))},
    };
    for (const std::pair<QString, QString>& entry : cases)
    {
        INFO(entry.first.toStdString());
        CHECK(DeriveProjectTarget(entry.first) == entry.second);
    }
}

TEST_CASE("New projects render the bundled template without the tooling", "[editor][setup]")
{
    QTemporaryDir projects;
    const QString destination = projects.filePath(QStringLiteral("Quote \"Game\" 日本"));
    QString descriptor = QStringLiteral("previous");
    QString problem;
    REQUIRE(CreateEditorProjectFromTemplate(QStringLiteral("Quote \"Game\" 日本"), destination, descriptor, problem));
    CHECK(problem.isEmpty());
    CHECK(descriptor == QDir(destination).filePath(QStringLiteral("ludus.project.json")));
    QFile descriptorFile(descriptor);
    REQUIRE(descriptorFile.open(QIODevice::ReadOnly));
    const ParseOutcome parsed = ParseDescriptor(descriptorFile.readAll());
    REQUIRE(parsed.Ok());
    CHECK(parsed.Descriptor.Name == QStringLiteral("Quote \"Game\" 日本"));
    CHECK(parsed.Descriptor.Target == QStringLiteral("Quote__Game"));
    CHECK(parsed.Descriptor.Version == 2);
    CHECK(parsed.Descriptor.HasEngine);
    for (const QString& relative : {QStringLiteral("CMakeLists.txt"),
                                    QStringLiteral("CMakePresets.json"),
                                    QStringLiteral("src/main.cpp"),
                                    QStringLiteral("ludus.lock.json"),
                                    QStringLiteral(".gitignore"),
                                    QStringLiteral(".vscode/launch.json")})
    {
        INFO(relative.toStdString());
        QFile file(QDir(destination).filePath(relative));
        REQUIRE(file.open(QIODevice::ReadOnly));
        CHECK_FALSE(file.readAll().contains("{{"));
    }
    QFile cmake(QDir(destination).filePath(QStringLiteral("CMakeLists.txt")));
    REQUIRE(cmake.open(QIODevice::ReadOnly));
    CHECK(cmake.readAll().contains("add_executable(Quote__Game src/main.cpp)"));

    // An existing folder or an invalid name writes nothing and preserves the output.
    const QString created = descriptor;
    CHECK_FALSE(CreateEditorProjectFromTemplate(QStringLiteral("Other"), destination, descriptor, problem));
    CHECK(descriptor == created);
    CHECK(problem.contains(QStringLiteral("already exists")));
    const QString invalid = projects.filePath(QStringLiteral("bad"));
    for (const QString& name : {QString(), QStringLiteral(".."), QStringLiteral("a/b"), QStringLiteral(" padded")})
    {
        problem.clear();
        CHECK_FALSE(CreateEditorProjectFromTemplate(name, invalid, descriptor, problem));
        CHECK_FALSE(problem.isEmpty());
        CHECK_FALSE(QFileInfo::exists(invalid));
    }
    CHECK(descriptor == created);
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
