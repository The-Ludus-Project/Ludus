#include "internal/code_completion_sources.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>

#include "internal/fuzzy_match.h"

#include <QSet>

namespace ludus::editor
{
namespace
{
using namespace foundation;
using Template = CodeSnippetTemplate;

constexpr usize SLANG_SNIPPET_COUNT = 5;
constexpr StaticArray<Template, SLANG_SNIPPET_COUNT> SLANG_SNIPPETS{{
    {QLatin1StringView("if"),
     QLatin1StringView("if statement"),
     QLatin1StringView("if (${1:condition})\n{\n    $0\n}")},
    {QLatin1StringView("for"),
     QLatin1StringView("for loop"),
     QLatin1StringView("for (uint ${1:i} = 0; $1 < ${2:count}; ++$1)\n{\n    $0\n}")},
    {QLatin1StringView("struct"),
     QLatin1StringView("struct declaration"),
     QLatin1StringView("struct ${1:Name}\n{\n    $0\n};")},
    {QLatin1StringView("func"),
     QLatin1StringView("function"),
     QLatin1StringView("${1:float} ${2:Name}(${3})\n{\n    $0\n}")},
    {QLatin1StringView("compute"),
     QLatin1StringView("compute entry point"),
     QLatin1StringView("[shader(\"compute\")]\n[numthreads(${1:8}, ${2:8}, ${3:1})]\nvoid ${4:main}(uint3 "
                       "${5:id} : SV_DispatchThreadID)\n{\n    $0\n}")},
}};

constexpr usize LUSL_SNIPPET_COUNT = 5;
constexpr StaticArray<Template, LUSL_SNIPPET_COUNT> LUSL_SNIPPETS{{
    {QLatin1StringView("if"),
     QLatin1StringView("if statement"),
     QLatin1StringView("if (${1:condition})\n{\n    $0\n}")},
    {QLatin1StringView("for"),
     QLatin1StringView("for loop"),
     QLatin1StringView("for (uint32 ${1:i} = 0; $1 < ${2:count}; ++$1)\n{\n    $0\n}")},
    {QLatin1StringView("struct"),
     QLatin1StringView("struct declaration"),
     QLatin1StringView("struct ${1:Name}\n{\n    $0\n};")},
    {QLatin1StringView("func"),
     QLatin1StringView("function"),
     QLatin1StringView("${1:float32} ${2:Name}(${3})\n{\n    $0\n}")},
    {QLatin1StringView("compute"),
     QLatin1StringView("compute entry point"),
     QLatin1StringView("[stage(compute), threads(${1:8}, ${2:8}, ${3:1})]\nvoid ${4:Main}(uint32x3 ${5:id} : "
                       "DispatchThreadId)\n{\n    $0\n}")},
}};

bool IsWordStart(QChar character)
{
    return character.isLetter() == true || character == QLatin1Char('_');
}

bool IsWordPart(QChar character)
{
    return character.isLetterOrNumber() == true || character == QLatin1Char('_');
}
} // namespace

BorrowedView<const CodeSnippetTemplate> GetCodeSnippetTemplates(CodeLanguage language) noexcept
{
    switch (language)
    {
        case CodeLanguage::Slang:
            return {SLANG_SNIPPETS.GetData(), SLANG_SNIPPETS.GetSize()};
        case CodeLanguage::Lusl:
            return {LUSL_SNIPPETS.GetData(), LUSL_SNIPPETS.GetSize()};
        case CodeLanguage::PlainText:
        case CodeLanguage::Luau:
        case CodeLanguage::C:
        case CodeLanguage::Cpp:
        case CodeLanguage::Json:
        case CodeLanguage::CMake:
        case CodeLanguage::Markdown:
        case CodeLanguage::Count:
            break;
    }
    return {};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the text, then what each new line starts with.
QString IndentCodeSnippetBody(QStringView body, QStringView indentation)
{
    QString result;
    result.reserve(body.size());
    for (const QChar character : body)
    {
        result += character;
        if (character == QLatin1Char('\n'))
        {
            result += indentation;
        }
    }
    return result;
}

QStringList CollectCodeCompletionWords(const QList<QString>& texts, const QString& prefix)
{
    QStringList words;
    QSet<QString> seen;
    qsizetype scanned = 0;
    for (const QString& text : texts)
    {
        if (scanned + text.size() > CODE_MAX_COMPLETION_WORD_SCAN_UNITS)
        {
            continue;
        }
        scanned += text.size();
        qsizetype position = 0;
        while (position < text.size() && words.size() < CODE_MAX_COMPLETION_WORDS)
        {
            if (IsWordStart(text.at(position)) == false || (position > 0 && IsWordPart(text.at(position - 1)) == true))
            {
                ++position;
                continue;
            }
            qsizetype end = position + 1;
            while (end < text.size() && IsWordPart(text.at(end)) == true)
            {
                ++end;
            }
            const qsizetype length = end - position;
            if (length >= CODE_MIN_COMPLETION_WORD_UNITS && length <= CODE_MAX_COMPLETION_WORD_UNITS)
            {
                const QString word = text.sliced(position, length);
                if (word != prefix && seen.contains(word) == false && ScoreFuzzyMatch(prefix, word) >= 0)
                {
                    seen.insert(word);
                    words.append(word);
                }
            }
            position = end;
        }
    }
    return words;
}
} // namespace ludus::editor
