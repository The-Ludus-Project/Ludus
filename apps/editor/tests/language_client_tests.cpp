#include "internal/language_client.h"
#include "internal/language_profiles.h"

#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
// Answers initialize with UTF-16 incremental sync and completion, but no
// signature help, which the Slang profile requires.
constexpr char UNSUPPORTED_SERVER[] = R"(import json, sys
while True:
    header = sys.stdin.buffer.readline()
    if not header:
        break
    length = int(header.split(b':')[1])
    sys.stdin.buffer.readline()
    message = json.loads(sys.stdin.buffer.read(length))
    if message.get('method') == 'initialize':
        body = json.dumps({'jsonrpc': '2.0', 'id': message['id'], 'result': {'capabilities': {
            'positionEncoding': 'utf-16', 'textDocumentSync': {'change': 2}, 'completionProvider': {}}}}).encode()
        sys.stdout.buffer.write(b'Content-Length: ' + str(len(body)).encode() + b'\r\n\r\n' + body)
        sys.stdout.buffer.flush()
)";
} // namespace

TEST_CASE("The Slang profile answers the editor's workspace configuration", "[editor][text][assistance]")
{
    const LanguageServerProfile& profile = GetSlangServerProfile();
    REQUIRE(profile.ConfigurationResponder != nullptr);
    CHECK(profile.LanguageId == QStringLiteral("slang"));
    CHECK(profile.ConfigurationResponder(QStringLiteral("slang.predefinedMacros")) == QJsonValue(QJsonArray{}));
    CHECK(profile.ConfigurationResponder(QStringLiteral("slang.additionalSearchPaths")) == QJsonValue(QJsonArray{}));
    CHECK(profile.ConfigurationResponder(QStringLiteral("slang.searchInAllWorkspaceDirectories")) == QJsonValue(false));
    CHECK(profile.ConfigurationResponder(QStringLiteral("slang.unknown")).isNull() == true);
}

TEST_CASE("The language client reports a missing server with the profile's repair", "[editor][text][assistance]")
{
    LanguageClient client;
    QSignalSpy failures(&client, &LanguageClient::Failed);
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid() == true);
    client.Start(GetSlangServerProfile(), temporary.filePath(QStringLiteral("missing-slangd")), temporary.path());
    REQUIRE(QTest::qWaitFor([&failures]() { return failures.isEmpty() == false; }));
    CHECK(failures.first().first().toString() ==
          QStringLiteral("Cannot start slangd. Repair project shader tools or check the compiler override."));
    CHECK(client.CanRequest(QStringLiteral("textDocument/completion")) == false);
}

TEST_CASE("The language client rejects a server without the profile's required capabilities",
          "[editor][text][assistance]")
{
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid() == true);
    QFile script(temporary.filePath(QStringLiteral("unsupported_server.py")));
    REQUIRE(script.open(QIODevice::WriteOnly) == true);
    REQUIRE(script.write(UNSUPPORTED_SERVER) == static_cast<qint64>(sizeof(UNSUPPORTED_SERVER) - 1));
    script.close();
    LanguageClient client;
    QSignalSpy failures(&client, &LanguageClient::Failed);
    QSignalSpy ready(&client, &LanguageClient::Ready);
    client.Start(GetSlangServerProfile(), QString::fromUtf8(LUDUS_TEST_PYTHON), temporary.path(), {script.fileName()});
    REQUIRE(QTest::qWaitFor([&failures]() { return failures.isEmpty() == false; }));
    CHECK(ready.isEmpty() == true);
    CHECK(failures.first().first().toString() ==
          QStringLiteral("This slangd does not support the required UTF-16 completion/signature protocol. Use the "
                         "project's pinned tools."));
    CHECK(client.Request(QStringLiteral("textDocument/completion"), {}) == 0);
}

TEST_CASE("The language client opens the profile's language and answers requests", "[editor][text][assistance]")
{
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid() == true);
    LanguageClient client;
    QSignalSpy ready(&client, &LanguageClient::Ready);
    QSignalSpy replies(&client, &LanguageClient::Replied);
    client.Start(GetSlangServerProfile(),
                 QString::fromUtf8(LUDUS_TEST_PYTHON),
                 temporary.path(),
                 {QString::fromUtf8(LUDUS_SLANG_FIXTURE)});
    const QString uri = QUrl::fromLocalFile(temporary.filePath(QStringLiteral("unit.slang"))).toString();
    client.SetDocument(uri, QStringLiteral("float Value() { return 1; }\nscene.Ve"));
    REQUIRE(QTest::qWaitFor([&ready]() { return ready.isEmpty() == false; }));
    CHECK(client.CanRequest(QStringLiteral("textDocument/references")) == true);
    const int32 request =
        client.Request(QStringLiteral("textDocument/completion"),
                       {{QStringLiteral("line"), 1},
                        {QStringLiteral("character"), static_cast<int32>(QStringLiteral("scene.Ve").size())}});
    REQUIRE(request != 0);
    REQUIRE(QTest::qWaitFor([&replies]() { return replies.isEmpty() == false; }));
    CHECK(replies.first().at(0).toInt() == request);
    const QJsonArray items = replies.first().at(1).toJsonValue().toArray();
    REQUIRE(items.size() == 1);
    CHECK(items.first().toObject().value(QStringLiteral("label")).toString() == QStringLiteral("Version"));
}
