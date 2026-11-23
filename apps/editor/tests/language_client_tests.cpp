#include "internal/language_client.h"
#include "internal/language_positions.h"
#include "internal/language_profiles.h"
#include "internal/language_requests.h"

#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
constexpr int32 RESTART_WAIT_MILLISECONDS = 10000;

/// One started client against language_server_fixture.py, with its message record.
class FakeServer final
{
public:
    explicit FakeServer(const QStringList& options = {}) : mRecord(mDirectory.filePath(QStringLiteral("record.jsonl")))
    {
        REQUIRE(mDirectory.isValid() == true);
        QStringList arguments{QString::fromUtf8(LUDUS_LANGUAGE_SERVER_FIXTURE), QStringLiteral("--record"), mRecord};
        arguments.append(options);
        Client.Start(GetSlangServerProfile(), QString::fromUtf8(LUDUS_TEST_PYTHON), mDirectory.path(), arguments);
    }

    [[nodiscard]] QString GetPath(const QString& name) const
    {
        return mDirectory.filePath(name);
    }

    [[nodiscard]] QString GetUri() const
    {
        return QUrl::fromLocalFile(mDirectory.filePath(QStringLiteral("unit.slang"))).toString();
    }

    /// Every recorded message, in arrival order.
    [[nodiscard]] QList<QJsonObject> ReadRecord() const
    {
        QList<QJsonObject> messages;
        QFile file(mRecord);
        if (file.open(QIODevice::ReadOnly) == false)
        {
            return messages;
        }
        for (const QByteArray& line : file.readAll().split('\n'))
        {
            if (line.isEmpty() == false)
            {
                messages.append(QJsonDocument::fromJson(line).object());
            }
        }
        return messages;
    }

    [[nodiscard]] QStringList ReadMethods() const
    {
        QStringList methods;
        for (const QJsonObject& message : ReadRecord())
        {
            if (message.contains(QStringLiteral("method")) == true)
            {
                methods.append(message.value(QStringLiteral("method")).toString());
            }
        }
        return methods;
    }

    [[nodiscard]] QString ReadLastText() const
    {
        QString text;
        for (const QJsonObject& message : ReadRecord())
        {
            if (message.contains(QStringLiteral("text")) == true)
            {
                text = message.value(QStringLiteral("text")).toString();
            }
        }
        return text;
    }

    [[nodiscard]] bool WaitForReady()
    {
        return QTest::qWaitFor([this]() { return Client.IsReady() == true; });
    }

    LanguageClient Client;

private:
    QTemporaryDir mDirectory;
    QString mRecord;
};

QJsonObject MakePosition(int32 line, int32 character)
{
    return {{QStringLiteral("line"), line}, {QStringLiteral("character"), character}};
}
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
    const LanguageServerProfile& lusl = GetLuslServerProfile();
    REQUIRE(lusl.ConfigurationResponder != nullptr);
    CHECK(lusl.LanguageId == QStringLiteral("lusl"));
    CHECK(lusl.ConfigurationResponder(QStringLiteral("slang.predefinedMacros")).isNull() == true);
}

TEST_CASE("The request table gates each kind on its advertised capability", "[editor][text][assistance]")
{
    for (usize index = 0; index < LANGUAGE_REQUEST_KIND_COUNT; ++index)
    {
        const LanguageRequestKind kind = static_cast<LanguageRequestKind>(index);
        CHECK(DescribeLanguageRequest(kind).Kind == kind);
        CHECK(DescribeLanguageRequest(kind).Method.isEmpty() == false);
        CHECK(IsLanguageRequestAdvertised({}, kind) == false);
    }
    const QJsonObject capabilities{
        {QStringLiteral("hoverProvider"), true},
        {QStringLiteral("definitionProvider"), false},
        {QStringLiteral("completionProvider"), QJsonObject{}},
        {QStringLiteral("renameProvider"), QJsonObject{{QStringLiteral("prepareProvider"), true}}},
        {QStringLiteral("semanticTokensProvider"), QJsonObject{{QStringLiteral("full"), QJsonObject{}}}}};
    CHECK(IsLanguageRequestAdvertised(capabilities, LanguageRequestKind::Hover) == true);
    CHECK(IsLanguageRequestAdvertised(capabilities, LanguageRequestKind::Definition) == false);
    CHECK(IsLanguageRequestAdvertised(capabilities, LanguageRequestKind::Completion) == true);
    CHECK(IsLanguageRequestAdvertised(capabilities, LanguageRequestKind::CompletionResolve) == false);
    CHECK(IsLanguageRequestAdvertised(capabilities, LanguageRequestKind::Rename) == true);
    CHECK(IsLanguageRequestAdvertised(capabilities, LanguageRequestKind::PrepareRename) == true);
    CHECK(IsLanguageRequestAdvertised(capabilities, LanguageRequestKind::SemanticTokens) == true);
    CHECK(DescribeLanguageRequest(LanguageRequestKind::WorkspaceSymbols).IsDocumentScoped == false);
    CHECK(DescribeLanguageRequest(LanguageRequestKind::WorkspaceSymbols).Priority ==
          LanguageRequestPriority::Workspace);
}

TEST_CASE("Protocol positions map UTF-16 offsets in both encodings", "[editor][text][assistance]")
{
    // "é" is one UTF-16 unit and two UTF-8 bytes; the emoji is two units and four bytes.
    const QString text = QStringLiteral("aé\U0001f600b\nnext");
    const int32 beforeB = static_cast<int32>(text.indexOf(QLatin1Char('b')));
    CHECK(MakeLanguagePosition(text, beforeB, LanguagePositionEncoding::Utf16) == MakePosition(0, 4));
    CHECK(MakeLanguagePosition(text, beforeB, LanguagePositionEncoding::Utf8) == MakePosition(0, 7));
    const int32 next = static_cast<int32>(text.indexOf(QStringLiteral("next"))) + 2;
    CHECK(MakeLanguagePosition(text, next, LanguagePositionEncoding::Utf8) == MakePosition(1, 2));
    int32 offset = -1;
    REQUIRE(GetLanguageOffset(text, MakePosition(0, 7), LanguagePositionEncoding::Utf8, offset) == true);
    CHECK(offset == beforeB);
    REQUIRE(GetLanguageOffset(text, MakePosition(0, 4), LanguagePositionEncoding::Utf16, offset) == true);
    CHECK(offset == beforeB);
    offset = -1;
    // Inside "é" in UTF-8 and inside the surrogate pair in UTF-16: rejected, output kept.
    CHECK(GetLanguageOffset(text, MakePosition(0, 2), LanguagePositionEncoding::Utf8, offset) == false);
    CHECK(GetLanguageOffset(text, MakePosition(0, 3), LanguagePositionEncoding::Utf16, offset) == false);
    CHECK(GetLanguageOffset(text, MakePosition(2, 0), LanguagePositionEncoding::Utf8, offset) == false);
    CHECK(GetLanguageOffset(text, MakePosition(0, 99), LanguagePositionEncoding::Utf8, offset) == false);
    CHECK(offset == -1);
    LanguagePositionEncoding encoding = LanguagePositionEncoding::Utf8;
    CHECK(ReadLanguagePositionEncoding({}, encoding) == true);
    CHECK(encoding == LanguagePositionEncoding::Utf16);
    CHECK(ReadLanguagePositionEncoding(QStringLiteral("utf-32"), encoding) == false);
    CHECK(encoding == LanguagePositionEncoding::Utf16);
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
    CHECK(client.CanRequest(LanguageRequestKind::Completion) == false);
}

TEST_CASE("The language client rejects unsupported synchronization and position encodings",
          "[editor][text][assistance]")
{
    for (const QStringList& options : {QStringList{QStringLiteral("--sync"), QStringLiteral("0")},
                                       QStringList{QStringLiteral("--encoding"), QStringLiteral("utf-32")}})
    {
        FakeServer server(options);
        QSignalSpy failures(&server.Client, &LanguageClient::Failed);
        REQUIRE(QTest::qWaitFor([&failures]() { return failures.isEmpty() == false; }));
        CHECK(failures.first().first().toString().contains(QStringLiteral("Use the project's pinned tools")) == true);
        CHECK(server.Client.IsReady() == false);
        CHECK(server.Client.Request(LanguageRequestKind::Completion, {}) == 0);
    }
}

TEST_CASE("The language client negotiates UTF-8 positions and synchronizes in them", "[editor][text][assistance]")
{
    FakeServer server({QStringLiteral("--encoding"), QStringLiteral("utf-8")});
    const QString uri = server.GetUri();
    server.Client.SetDocument(uri, QStringLiteral("float aé = 1;\n// \U0001f600 end\n"));
    REQUIRE(server.WaitForReady() == true);
    CHECK(server.Client.GetPositionEncoding() == LanguagePositionEncoding::Utf8);
    const QString edited = QStringLiteral("float aéé = 1;\n// \U0001f600 tail end\n");
    server.Client.SetDocument(uri, edited);
    REQUIRE(QTest::qWaitFor([&server, &edited]() { return server.ReadLastText() == edited; }));
    const QList<QJsonObject> record = server.ReadRecord();
    const QJsonObject initialize = record.first().value(QStringLiteral("params")).toObject();
    CHECK(initialize.value(QStringLiteral("capabilities"))
              .toObject()
              .value(QStringLiteral("general"))
              .toObject()
              .value(QStringLiteral("positionEncodings"))
              .toArray()
              .first()
              .toString() == QStringLiteral("utf-8"));
    for (const QJsonObject& message : record)
    {
        if (message.value(QStringLiteral("method")).toString() == QStringLiteral("textDocument/didOpen"))
        {
            CHECK(message.value(QStringLiteral("params"))
                      .toObject()
                      .value(QStringLiteral("textDocument"))
                      .toObject()
                      .value(QStringLiteral("languageId"))
                      .toString() == QStringLiteral("slang"));
        }
    }
}

TEST_CASE("The language client coalesces text changes to the debounce in either sync kind",
          "[editor][text][assistance]")
{
    for (const QString& kind : {QStringLiteral("1"), QStringLiteral("2")})
    {
        FakeServer server({QStringLiteral("--sync"), kind});
        const QString uri = server.GetUri();
        server.Client.SetDocument(uri, QStringLiteral("first\n"));
        REQUIRE(server.WaitForReady() == true);
        QString text = QStringLiteral("first\n");
        for (int32 index = 0; index < 5; ++index)
        {
            text += QStringLiteral("line\n");
            server.Client.SetDocument(uri, text);
        }
        REQUIRE(QTest::qWaitFor([&server, &text]() { return server.ReadLastText() == text; }));
        CHECK(server.ReadMethods().count(QStringLiteral("textDocument/didChange")) == 1);
    }
}

TEST_CASE("The language client never sends a request the server does not advertise", "[editor][text][assistance]")
{
    FakeServer server;
    server.Client.SetDocument(server.GetUri(), QStringLiteral("text\n"));
    REQUIRE(server.WaitForReady() == true);
    QSignalSpy unavailable(&server.Client, &LanguageClient::RequestUnavailable);
    CHECK(server.Client.CanRequest(LanguageRequestKind::Hover) == false);
    CHECK(server.Client.GetUnavailableReason(LanguageRequestKind::Hover) ==
          QStringLiteral("The selected slangd does not support hover."));
    CHECK(server.Client.GetUnavailableReason(LanguageRequestKind::Completion).isEmpty() == false);
    CHECK(server.Client.Request(LanguageRequestKind::Hover, {{QStringLiteral("position"), MakePosition(0, 0)}}) == 0);
    REQUIRE(unavailable.size() == 1);
    CHECK(unavailable.first().at(0).value<LanguageRequestKind>() == LanguageRequestKind::Hover);
    QTest::qWait(100);
    CHECK(server.ReadMethods().contains(QStringLiteral("textDocument/hover")) == false);
    CHECK(server.Client.IsReady() == true);
}

TEST_CASE("A newer request cancels the older one of its kind and drops its reply", "[editor][text][assistance]")
{
    FakeServer server({QStringLiteral("--capabilities"),
                       QStringLiteral(R"({"hoverProvider": true})"),
                       QStringLiteral("--hold"),
                       QStringLiteral("textDocument/hover")});
    server.Client.SetDocument(server.GetUri(), QStringLiteral("text\n"));
    REQUIRE(server.WaitForReady() == true);
    QSignalSpy replies(&server.Client, &LanguageClient::Replied);
    const QJsonObject parameters{{QStringLiteral("position"), MakePosition(0, 1)}};
    const int32 first = server.Client.Request(LanguageRequestKind::Hover, parameters);
    REQUIRE(first != 0);
    QTest::qWait(100);
    const int32 second = server.Client.Request(LanguageRequestKind::Hover, parameters);
    REQUIRE(second != 0);
    REQUIRE(QTest::qWaitFor([&replies]() { return replies.isEmpty() == false; }));
    QTest::qWait(100);
    REQUIRE(replies.size() == 1);
    CHECK(replies.first().at(0).toInt() == second);
    CHECK(replies.first().at(1).value<LanguageRequestKind>() == LanguageRequestKind::Hover);
    bool hasCancel = false;
    for (const QJsonObject& message : server.ReadRecord())
    {
        hasCancel = hasCancel ||
                    (message.value(QStringLiteral("method")).toString() == QStringLiteral("$/cancelRequest") &&
                     message.value(QStringLiteral("params")).toObject().value(QStringLiteral("id")).toInt() == first);
    }
    CHECK(hasCancel == true);
}

TEST_CASE("Interactive requests go first and workspace queries last", "[editor][text][assistance]")
{
    FakeServer server(
        {QStringLiteral("--capabilities"), QStringLiteral(R"({"hoverProvider": true, "documentSymbolProvider": true,
                                          "workspaceSymbolProvider": true,
                                          "semanticTokensProvider": {"full": true}})")});
    server.Client.SetDocument(server.GetUri(), QStringLiteral("text\n"));
    REQUIRE(server.WaitForReady() == true);
    QSignalSpy replies(&server.Client, &LanguageClient::Replied);
    REQUIRE(server.Client.Request(LanguageRequestKind::WorkspaceSymbols, {{QStringLiteral("query"), QString()}}) != 0);
    REQUIRE(server.Client.Request(LanguageRequestKind::SemanticTokens, {}) != 0);
    REQUIRE(server.Client.Request(LanguageRequestKind::DocumentSymbols, {}) != 0);
    REQUIRE(server.Client.Request(LanguageRequestKind::Hover, {{QStringLiteral("position"), MakePosition(0, 0)}}) != 0);
    REQUIRE(QTest::qWaitFor([&replies]() { return replies.size() == 4; }));
    QStringList requests;
    for (const QString& method : server.ReadMethods())
    {
        if (method.startsWith(QStringLiteral("textDocument/did")) == false && method != QStringLiteral("initialize") &&
            method != QStringLiteral("initialized"))
        {
            requests.append(method);
        }
    }
    CHECK(requests == QStringList{QStringLiteral("textDocument/hover"),
                                  QStringLiteral("textDocument/documentSymbol"),
                                  QStringLiteral("textDocument/semanticTokens/full"),
                                  QStringLiteral("workspace/symbol")});
}

TEST_CASE("A request error keeps the connection ready", "[editor][text][assistance]")
{
    FakeServer server({QStringLiteral("--capabilities"),
                       QStringLiteral(R"({"completionProvider": {}})"),
                       QStringLiteral("--error"),
                       QStringLiteral("textDocument/completion")});
    server.Client.SetDocument(server.GetUri(), QStringLiteral("text\n"));
    REQUIRE(server.WaitForReady() == true);
    QSignalSpy errors(&server.Client, &LanguageClient::RequestFailed);
    QSignalSpy failures(&server.Client, &LanguageClient::Failed);
    const int32 request =
        server.Client.Request(LanguageRequestKind::Completion, {{QStringLiteral("position"), MakePosition(0, 0)}});
    REQUIRE(QTest::qWaitFor([&errors]() { return errors.isEmpty() == false; }));
    CHECK(errors.first().at(0).toInt() == request);
    CHECK(errors.first().at(2).toString() ==
          QStringLiteral("Slang could not answer this request. Repair the shader syntax and retry."));
    CHECK(failures.isEmpty() == true);
    CHECK(server.Client.IsReady() == true);
}

TEST_CASE("A crashed server restarts with backoff, then stops after three crashes in a minute",
          "[editor][text][assistance]")
{
    SECTION("Two crashes, then a stable start")
    {
        QTemporaryDir counter;
        FakeServer server(
            {QStringLiteral("--crash-starts"), counter.filePath(QStringLiteral("starts")) + QStringLiteral(":2")});
        QSignalSpy restarts(&server.Client, &LanguageClient::Restarting);
        QSignalSpy failures(&server.Client, &LanguageClient::Failed);
        REQUIRE(QTest::qWaitFor([&restarts]() { return restarts.size() == 2; }, RESTART_WAIT_MILLISECONDS));
        CHECK(restarts.first().first().toString() ==
              QStringLiteral("Slang stopped. Restarting shader assistance in 1 s; editing is unaffected."));
        CHECK(restarts.last().first().toString().contains(QStringLiteral("in 2 s")) == true);
        REQUIRE(QTest::qWaitFor([&server]() { return server.Client.IsReady() == true; }, RESTART_WAIT_MILLISECONDS));
        QTest::qWait(200);
        CHECK(server.Client.IsReady() == true);
        CHECK(failures.isEmpty() == true);
    }
    SECTION("Three crashes")
    {
        QTemporaryDir counter;
        FakeServer server(
            {QStringLiteral("--crash-starts"), counter.filePath(QStringLiteral("starts")) + QStringLiteral(":9")});
        QSignalSpy restarts(&server.Client, &LanguageClient::Restarting);
        QSignalSpy failures(&server.Client, &LanguageClient::Failed);
        REQUIRE(QTest::qWaitFor([&failures]() { return failures.isEmpty() == false; }, RESTART_WAIT_MILLISECONDS));
        CHECK(restarts.size() == 2);
        CHECK(failures.first().first().toString().contains(QStringLiteral("3 times within a minute")) == true);
        CHECK(server.Client.IsReady() == false);
    }
}
