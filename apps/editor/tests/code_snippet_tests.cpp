#include "internal/code_snippet.h"

#include <catch2/catch_test_macros.hpp>

#include <QList>
#include <QString>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
CodeSnippet Parse(const QString& source)
{
    CodeSnippet snippet;
    REQUIRE(ParseCodeSnippet(source, snippet) == true);
    return snippet;
}

CodeSnippetStop Stop(int32 index, int32 start, int32 end)
{
    CodeSnippetStop stop;
    stop.Index = index;
    stop.Start = start;
    stop.End = end;
    return stop;
}
} // namespace

TEST_CASE("Snippets expand tab stops, placeholders, choices and mirrors", "[editor][code-editor][assistance]")
{
    CodeSnippet call = Parse(QStringLiteral("Call(${1:value}, ${2:factor})$0"));
    CHECK(call.Text == QStringLiteral("Call(value, factor)"));
    CHECK(call.Stops == QList<CodeSnippetStop>{Stop(1, 5, 10), Stop(2, 12, 18), Stop(0, 19, 19)});

    // An inner placeholder is listed before the one that contains it.
    CodeSnippet nested = Parse(QStringLiteral("${1:outer ${2:inner}}"));
    CHECK(nested.Text == QStringLiteral("outer inner"));
    CHECK(nested.Stops == QList<CodeSnippetStop>{Stop(2, 6, 11), Stop(1, 0, 11)});

    CodeSnippet choice = Parse(QStringLiteral("${1|float,int|} x"));
    CHECK(choice.Text == QStringLiteral("float x"));
    CHECK(choice.Stops == QList<CodeSnippetStop>{Stop(1, 0, 5)});

    // A later bare stop repeats its placeholder's text.
    CodeSnippet mirror = Parse(QStringLiteral("${1:a} = $1;"));
    CHECK(mirror.Text == QStringLiteral("a = a;"));
    CHECK(mirror.Stops == QList<CodeSnippetStop>{Stop(1, 0, 1), Stop(1, 4, 5)});
}

TEST_CASE("Snippet escapes, literals and variables follow the protocol", "[editor][code-editor][assistance]")
{
    CHECK(Parse(QStringLiteral("\\$1 \\} \\\\")).Text == QStringLiteral("$1 } \\"));
    CHECK(Parse(QStringLiteral("\\$1 \\} \\\\")).Stops.isEmpty() == true);
    // Constructs that do not parse stay literal text.
    CHECK(Parse(QStringLiteral("$ ${ ${x")).Text == QStringLiteral("$ ${ ${x"));
    CHECK(Parse(QStringLiteral("${1:open")).Text == QStringLiteral("${1:open"));
    // Editor variables are empty here and keep their default; others insert their name.
    CHECK(Parse(QStringLiteral("${TM_SELECTED_TEXT:default} $TM_FILENAME ${NAME}")).Text ==
          QStringLiteral("default  NAME"));
}

TEST_CASE("Snippet transforms and oversized snippets are rejected", "[editor][code-editor][assistance]")
{
    CodeSnippet kept;
    kept.Text = QStringLiteral("kept");
    CHECK(ParseCodeSnippet(QStringLiteral("${1/(.*)/x/}"), kept) == false);
    CHECK(ParseCodeSnippet(QStringLiteral("${TM_FILENAME/(.*)/x/}"), kept) == false);
    QString many;
    for (int32 index = 0; index <= CODE_MAX_SNIPPET_STOPS; ++index)
    {
        many += QStringLiteral("$1");
    }
    CHECK(ParseCodeSnippet(many, kept) == false);
    QString deep;
    for (int32 depth = 0; depth <= CODE_MAX_SNIPPET_DEPTH; ++depth)
    {
        deep += QStringLiteral("${1:");
    }
    deep += QString(CODE_MAX_SNIPPET_DEPTH + 1, QLatin1Char('}'));
    CHECK(ParseCodeSnippet(deep, kept) == false);
    CHECK(kept.Text == QStringLiteral("kept"));
}
