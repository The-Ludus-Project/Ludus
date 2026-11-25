#include "internal/code_workspace_edit.h"

#include <catch2/catch_test_macros.hpp>

#include <QJsonArray>
#include <QJsonObject>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
const QString SOURCE_URI = QStringLiteral("file:///unit.slang");
const QString HELPER_URI = QStringLiteral("file:///capture/helper.slang");
const QString GENERATED_URI = QStringLiteral("file:///capture/generated.slang");

QJsonObject Range(int32 line, int32 start, int32 end)
{
    return {
        {QStringLiteral("start"), QJsonObject{{QStringLiteral("line"), line}, {QStringLiteral("character"), start}}},
        {QStringLiteral("end"), QJsonObject{{QStringLiteral("line"), line}, {QStringLiteral("character"), end}}},
    };
}

QJsonObject Edit(int32 line, int32 start, int32 end, const QString& text)
{
    return {{QStringLiteral("range"), Range(line, start, end)}, {QStringLiteral("newText"), text}};
}

/// The prefix is the unit's line 0; the authored source starts on line 1.
CodeWorkspaceEditContext MakeContext(bool canEditOtherFiles)
{
    CodeWorkspaceEditContext context;
    const QString prefix = QStringLiteral("// generated value\n");
    const QString source = QStringLiteral("float value = 1;\nfloat b = value;\n");
    context.Uri = SOURCE_URI;
    context.Unit = prefix + source;
    context.SourceStart = static_cast<int32>(prefix.size());
    context.SourceEnd = static_cast<int32>(context.Unit.size());
    context.SentVersion = 4;
    context.Sources.insert(HELPER_URI, {QStringLiteral("shaders/helper.slang"), QStringLiteral("value();\n")});
    context.Sources.insert(GENERATED_URI, {QString(), QStringLiteral("float value;\n")});
    context.CanEditOtherFiles = canEditOtherFiles;
    return context;
}

QJsonObject Changes(const QString& uri, const QJsonArray& edits)
{
    return {{QStringLiteral("changes"), QJsonObject{{uri, edits}}}};
}

/// Reads `edit` expecting refusal; the message must mention `phrase` and the output survive.
void CheckRefused(const QJsonObject& edit, const CodeWorkspaceEditContext& context, const QString& phrase)
{
    QList<CodeWorkspaceFileChange> changes{{QStringLiteral("kept"), {}, {}}};
    QString message;
    CHECK(ReadCodeWorkspaceEdit(edit, context, changes, message) == false);
    INFO(message.toStdString());
    CHECK(message.contains(phrase) == true);
    CHECK(message.contains(QStringLiteral("nothing was changed")) == true);
    REQUIRE(changes.size() == 1);
    CHECK(changes.constFirst().File == QStringLiteral("kept"));
}
} // namespace

TEST_CASE("Workspace edits map to authored files in order", "[editor][code-editor][assistance]")
{
    // Out of order and across two documents, as servers may send them.
    QJsonObject edit{
        {QStringLiteral("changes"),
         QJsonObject{
             {SOURCE_URI,
              QJsonArray{Edit(2, 10, 15, QStringLiteral("amount")), Edit(1, 6, 11, QStringLiteral("amount"))}},
             {HELPER_URI, QJsonArray{Edit(0, 0, 5, QStringLiteral("amount"))}},
         }},
    };
    QList<CodeWorkspaceFileChange> changes;
    QString message;
    REQUIRE(ReadCodeWorkspaceEdit(edit, MakeContext(true), changes, message) == true);
    REQUIRE(changes.size() == 2);
    const CodeWorkspaceFileChange& source = changes.at(0).File.isEmpty() == true ? changes.at(0) : changes.at(1);
    const CodeWorkspaceFileChange& helper = changes.at(0).File.isEmpty() == true ? changes.at(1) : changes.at(0);
    CHECK(source.Text == QStringLiteral("float value = 1;\nfloat b = value;\n"));
    REQUIRE(source.Edits.size() == 2);
    CHECK(source.Edits.at(0) == CodeWorkspaceTextEdit{6, 11, QStringLiteral("amount")});
    CHECK(ApplyCodeWorkspaceTextEdits(source.Text, source.Edits) ==
          QStringLiteral("float amount = 1;\nfloat b = amount;\n"));
    // A caret inside a renamed name moves to its start; later positions shift by the growth.
    CHECK(MapCodeWorkspacePosition(source.Edits, 8) == 6);
    CHECK(MapCodeWorkspacePosition(source.Edits, 17) == 18);
    CHECK(helper.File == QStringLiteral("shaders/helper.slang"));
    CHECK(ApplyCodeWorkspaceTextEdits(helper.Text, helper.Edits) == QStringLiteral("amount();\n"));

    // The draft keeps its CRLF line endings.
    QByteArray bytes;
    REQUIRE(ApplyCodeWorkspaceEditBytes("float value = 1;\r\nvalue;\r\n",
                                        {{6, 11, QStringLiteral("a\nb")}, {17, 22, QStringLiteral("a")}},
                                        bytes) == true);
    CHECK(bytes == "float a\r\nb = 1;\r\na;\r\n");
}

TEST_CASE("Workspace edits that touch generated, read-only or other files are refused whole",
          "[editor][code-editor][assistance]")
{
    const CodeWorkspaceEditContext context = MakeContext(true);
    CheckRefused(Changes(SOURCE_URI, {Edit(0, 12, 17, QStringLiteral("amount"))}),
                 context,
                 QStringLiteral("generated"));
    CheckRefused(Changes(GENERATED_URI, {Edit(0, 6, 11, QStringLiteral("amount"))}),
                 context,
                 QStringLiteral("generated or read-only"));
    CheckRefused(Changes(QStringLiteral("file:///elsewhere.slang"), {Edit(0, 0, 1, QStringLiteral("x"))}),
                 context,
                 QStringLiteral("not part of this shader's context"));
    CheckRefused(Changes(HELPER_URI, {Edit(0, 0, 5, QStringLiteral("amount"))}),
                 MakeContext(false),
                 QStringLiteral("shaders/helper.slang"));
    CheckRefused(Changes(SOURCE_URI, {Edit(1, 6, 11, QStringLiteral("a")), Edit(1, 8, 12, QStringLiteral("b"))}),
                 context,
                 QStringLiteral("overlapping"));
    CheckRefused(Changes(SOURCE_URI, {Edit(1, 6, 99, QStringLiteral("a"))}), context, QStringLiteral("invalid"));
    CheckRefused({{QStringLiteral("documentChanges"),
                   QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("create")},
                                          {QStringLiteral("uri"), QStringLiteral("file:///new.slang")}}}}},
                 context,
                 QStringLiteral("create, move or delete"));
    CheckRefused({{QStringLiteral("documentChanges"),
                   QJsonArray{QJsonObject{
                       {QStringLiteral("textDocument"),
                        QJsonObject{{QStringLiteral("uri"), SOURCE_URI}, {QStringLiteral("version"), 3}}},
                       {QStringLiteral("edits"), QJsonArray{Edit(1, 6, 11, QStringLiteral("a"))}},
                   }}}},
                 context,
                 QStringLiteral("earlier version"));
}
