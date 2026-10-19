#include "internal/external_code_editor.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

using namespace ludus::editor;

TEST_CASE("External code editor paths remain individual arguments", "[editor][source-editor]")
{
    QTemporaryDir temporary;
    const QString path = QDir(temporary.path()).filePath(QStringLiteral("shader $(literal) with spaces.slang"));
    QFile source(path);
    REQUIRE(source.open(QIODevice::WriteOnly) == true);
    source.close();
    ExternalCodeEditorCommand command;
    REQUIRE(MakeExternalCodeEditorCommand(QStringLiteral("/usr/bin/true"), path, command) == true);
    CHECK(command.Program == QStringLiteral("/usr/bin/true"));
    CHECK(command.Arguments == QStringList{QFileInfo(path).canonicalFilePath()});
    const ExternalCodeEditorCommand original = command;
    CHECK(MakeExternalCodeEditorCommand(temporary.path(), path, command) == false);
    CHECK(command.Program == original.Program);
    CHECK(command.Arguments == original.Arguments);
    CHECK(MakeExternalCodeEditorCommand(QStringLiteral("/usr/bin/true"), path + QStringLiteral("missing"), command) ==
          false);
    CHECK(command.Arguments == original.Arguments);
}

#if defined(Q_OS_MACOS)
TEST_CASE("macOS code editor applications open a source without shell quoting", "[editor][source-editor]")
{
    QTemporaryDir temporary;
    const QString application = QDir(temporary.path()).filePath(QStringLiteral("Code Editor.app"));
    REQUIRE(QDir().mkpath(application) == true);
    const QString path = QDir(temporary.path()).filePath(QStringLiteral("shader file.slang"));
    QFile source(path);
    REQUIRE(source.open(QIODevice::WriteOnly) == true);
    source.close();
    ExternalCodeEditorCommand command;
    REQUIRE(MakeExternalCodeEditorCommand(application, path, command) == true);
    CHECK(command.Program == QStringLiteral("/usr/bin/open"));
    const QStringList expected{QStringLiteral("-a"),
                               QFileInfo(application).canonicalFilePath(),
                               QFileInfo(path).canonicalFilePath()};
    CHECK(command.Arguments == expected);
}
#endif
