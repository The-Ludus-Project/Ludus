#include "internal/code_snippet.h"

#include <ludus/foundation/base/core.h>

#include <QStringView>

// Thanks to Microsoft, Language Server Protocol 3.17, "Snippet Syntax": tab
// stops, nested placeholders, choices, variables and their escapes. Transforms
// are recognized only to reject them.
// https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#snippet_syntax
namespace ludus::editor
{
namespace
{
using namespace foundation;
constexpr int32 DECIMAL_BASE = 10;
// Larger indices are kept at this value; a snippet that needs them exceeds the stop budget anyway.
constexpr int32 MAX_STOP_INDEX = 1 << 20;

bool IsDigit(QChar character)
{
    return character >= QLatin1Char('0') && character <= QLatin1Char('9');
}

bool IsVariableStart(QChar character)
{
    return character == QLatin1Char('_') || (character >= QLatin1Char('a') && character <= QLatin1Char('z')) ||
           (character >= QLatin1Char('A') && character <= QLatin1Char('Z'));
}

bool IsVariablePart(QChar character)
{
    return IsVariableStart(character) == true || IsDigit(character) == true;
}

class SnippetParser final
{
public:
    explicit SnippetParser(QStringView source) : mSource(source) {}

    bool Parse(CodeSnippet& outSnippet)
    {
        parseSequence(false);
        if (mIsUnsupported == true || mStops.size() > CODE_MAX_SNIPPET_STOPS)
        {
            return false;
        }
        outSnippet = { .Text = Move(mText), .Stops = Move(mStops) };
        return true;
    }

private:
    struct Mark final
    {
        qsizetype Position = 0;
        qsizetype TextSize = 0;
        qsizetype StopCount = 0;
    };

    [[nodiscard]] Mark mark() const
    {
        return { .Position = mPosition, .TextSize = mText.size(), .StopCount = mStops.size() };
    }

    void restore(const Mark& state)
    {
        mPosition = state.Position;
        mText.truncate(state.TextSize);
        mStops.resize(state.StopCount);
    }

    [[nodiscard]] bool isAt(QChar character) const
    {
        return mPosition < mSource.size() && mSource.at(mPosition) == character;
    }

    /// Text up to the end, or up to an unescaped `}` when nested. False when a
    /// nested sequence reaches the end without its `}`.
    bool parseSequence(bool isNested)
    {
        while (mPosition < mSource.size() && mIsUnsupported == false)
        {
            const QChar character = mSource.at(mPosition);
            if (character == QLatin1Char('\\') && mPosition + 1 < mSource.size() &&
                QStringView(u"$}\\").contains(mSource.at(mPosition + 1)) == true)
            {
                mText += mSource.at(mPosition + 1);
                mPosition += 2;
            }
            else if (isNested == true && character == QLatin1Char('}'))
            {
                return true;
            }
            else if (character == QLatin1Char('$') && parseDollar() == true)
            {
                continue;
            }
            else
            {
                mText += character;
                ++mPosition;
            }
        }
        return isNested == false;
    }

    [[nodiscard]] int32 readInteger()
    {
        int32 value = 0;
        while (mPosition < mSource.size() && IsDigit(mSource.at(mPosition)) == true)
        {
            if (value < MAX_STOP_INDEX)
            {
                value = value * DECIMAL_BASE + (mSource.at(mPosition).unicode() - u'0');
            }
            ++mPosition;
        }
        return value;
    }

    [[nodiscard]] QString readVariable()
    {
        const qsizetype start = mPosition;
        while (mPosition < mSource.size() && IsVariablePart(mSource.at(mPosition)) == true)
        {
            ++mPosition;
        }
        return mSource.sliced(start, mPosition - start).toString();
    }

    /// A bare `$N` after `${N:text}` repeats that text, as editors show mirrors.
    void addMirror(int32 index)
    {
        const qsizetype start = mText.size();
        for (const CodeSnippetStop& stop : mStops)
        {
            if (stop.Index == index && stop.End > stop.Start)
            {
                mText += QStringView(mText).sliced(stop.Start, stop.End - stop.Start).toString();
                break;
            }
        }
        addStop(index, start);
    }

    void addStop(int32 index, qsizetype start)
    {
        mStops.append({ .Index = index, .Start = static_cast<int32>(start), .End = static_cast<int32>(mText.size()) });
    }

    /// One `$` construct at mPosition. False, restoring the state, when it is not
    /// one, so the caller keeps the `$` as text.
    bool parseDollar()
    {
        const Mark state = mark();
        ++mPosition;
        if (mPosition < mSource.size() && IsDigit(mSource.at(mPosition)) == true)
        {
            addMirror(readInteger());
            return true;
        }
        if (mPosition < mSource.size() && IsVariableStart(mSource.at(mPosition)) == true)
        {
            appendVariable(readVariable());
            return true;
        }
        if (isAt(QLatin1Char('{')) == false)
        {
            restore(state);
            return false;
        }
        ++mPosition;
        const bool isTabStop = mPosition < mSource.size() && IsDigit(mSource.at(mPosition)) == true;
        const bool isVariable =
            isTabStop == false && mPosition < mSource.size() && IsVariableStart(mSource.at(mPosition)) == true;
        if (isTabStop == false && isVariable == false)
        {
            restore(state);
            return false;
        }
        const int32 index = isTabStop == true ? readInteger() : 0;
        const QString variable = isVariable == true ? readVariable() : QString();
        const qsizetype start = mText.size();
        if (isAt(QLatin1Char('/')) == true)
        {
            mIsUnsupported = true;
            return false;
        }
        if (isAt(QLatin1Char('}')) == true)
        {
            ++mPosition;
            if (isTabStop == true)
            {
                addMirror(index);
            }
            else
            {
                appendVariable(variable);
            }
            return true;
        }
        if (isTabStop == true && isAt(QLatin1Char('|')) == true)
        {
            return parseChoice(index, state);
        }
        if (isAt(QLatin1Char(':')) == false || mDepth == CODE_MAX_SNIPPET_DEPTH)
        {
            mIsUnsupported = mIsUnsupported == true || mDepth == CODE_MAX_SNIPPET_DEPTH;
            restore(state);
            return false;
        }
        ++mPosition;
        ++mDepth;
        const bool isClosed = parseSequence(true) == true && isAt(QLatin1Char('}')) == true;
        --mDepth;
        if (isClosed == false || mIsUnsupported == true)
        {
            restore(state);
            return false;
        }
        ++mPosition;
        if (isTabStop == true)
        {
            addStop(index, start);
        }
        else if (variable.startsWith(QStringLiteral("TM_")) == false)
        {
            // An unknown variable inserts its name rather than its default.
            mText.truncate(start);
            appendVariable(variable);
        }
        return true;
    }

    /// `${N|one,two|}`: the first choice becomes the placeholder text.
    bool parseChoice(int32 index, const Mark& state)
    {
        ++mPosition;
        const qsizetype start = mText.size();
        bool isFirst = true;
        while (mPosition < mSource.size())
        {
            const QChar character = mSource.at(mPosition);
            if (character == QLatin1Char('\\') && mPosition + 1 < mSource.size() &&
                QStringView(u"$}\\,|").contains(mSource.at(mPosition + 1)) == true)
            {
                if (isFirst == true)
                {
                    mText += mSource.at(mPosition + 1);
                }
                mPosition += 2;
                continue;
            }
            if (character == QLatin1Char('|') && mPosition + 1 < mSource.size() &&
                mSource.at(mPosition + 1) == QLatin1Char('}'))
            {
                mPosition += 2;
                addStop(index, start);
                return true;
            }
            if (character == QLatin1Char(','))
            {
                isFirst = false;
            }
            else if (isFirst == true)
            {
                mText += character;
            }
            ++mPosition;
        }
        restore(state);
        return false;
    }

    void appendVariable(const QString& name)
    {
        // The editor resolves no variables: TM_ names (selection, current word,
        // line) are empty here, and others insert their name, as the protocol asks.
        if (name.startsWith(QStringLiteral("TM_")) == false)
        {
            mText += name;
        }
    }

    QStringView mSource;
    qsizetype mPosition = 0;
    QString mText;
    QList<CodeSnippetStop> mStops;
    int32 mDepth = 0;
    bool mIsUnsupported = false;
};
} // namespace

bool ParseCodeSnippet(const QString& source, CodeSnippet& outSnippet)
{
    SnippetParser parser(source);
    return parser.Parse(outSnippet);
}
} // namespace ludus::editor
