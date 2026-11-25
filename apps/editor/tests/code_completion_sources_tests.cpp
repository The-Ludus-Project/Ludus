#include "internal/code_completion_sources.h"
#include "internal/code_snippet.h"

#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
using namespace ludus::foundation;

TEST_CASE("Built-in snippets parse and end at a final stop", "[editor][code-editor][assistance]")
{
    CHECK(GetCodeSnippetTemplates(CodeLanguage::PlainText).IsEmpty() == true);
    for (const CodeLanguage language : {CodeLanguage::Slang, CodeLanguage::Lusl})
    {
        REQUIRE(GetCodeSnippetTemplates(language).IsEmpty() == false);
        for (const CodeSnippetTemplate& snippet : GetCodeSnippetTemplates(language))
        {
            INFO(QString(snippet.Prefix).toStdString());
            CodeSnippet parsed;
            REQUIRE(ParseCodeSnippet(QString(snippet.Body), parsed) == true);
            bool hasFinal = false;
            for (const CodeSnippetStop& stop : parsed.Stops)
            {
                hasFinal = hasFinal == true || stop.Index == 0;
            }
            CHECK(hasFinal == true);
            CHECK(snippet.Description.isEmpty() == false);
        }
    }
    CHECK(IndentCodeSnippetBody(u"if (x)\n{\n    $0\n}", u"    ") ==
          QStringLiteral("if (x)\n    {\n        $0\n    }"));
}

TEST_CASE("Completion words are distinct identifiers that match the prefix", "[editor][code-editor][assistance]")
{
    const QList<QString> texts{
        QStringLiteral("float accumulated = acc + 1; // ab x_acc9 accumulated"),
        QStringLiteral("uint8 accent; acceptance"),
    };
    // In the order found, without the prefix itself, short words or non-matches.
    CHECK(CollectCodeCompletionWords(texts, QStringLiteral("acc")) == QStringList{QStringLiteral("accumulated"),
                                                                                  QStringLiteral("x_acc9"),
                                                                                  QStringLiteral("accent"),
                                                                                  QStringLiteral("acceptance")});
    // A text that would exceed the reading budget is skipped.
    const QString large(CODE_MAX_COMPLETION_WORD_SCAN_UNITS + 1, QLatin1Char('a'));
    CHECK(CollectCodeCompletionWords({large, QStringLiteral("accent")}, QStringLiteral("acc")) ==
          QStringList{QStringLiteral("accent")});
}
