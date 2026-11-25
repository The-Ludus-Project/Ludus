#include "internal/fuzzy_match.h"

#include <ludus/foundation/base/core.h>

namespace ludus::editor
{
namespace
{
using namespace foundation;
constexpr int32 MATCH_SCORE = 10;
constexpr int32 CONSECUTIVE_BONUS = 15;
constexpr int32 WORD_START_BONUS = 12;
constexpr int32 SUBSTRING_BONUS = 30;
constexpr int32 PREFIX_BONUS = 20;

/// The greedy in-order match behind ScoreFuzzyMatch, also reporting where each query letter landed.
int32 MatchFuzzy(const QString& query, const QString& candidate, QList<int32>& outPositions)
{
    outPositions.clear();
    QString needle = query.toLower();
    needle.remove(QLatin1Char(' '));
    if (needle.isEmpty() == true)
    {
        return 0;
    }
    const QString haystack = candidate.toLower();
    int32 score = 0;
    int32 queryIndex = 0;
    int32 previousMatch = -2;
    for (int32 index = 0; index < haystack.size() && queryIndex < needle.size(); ++index)
    {
        if (haystack.at(index) != needle.at(queryIndex))
        {
            continue;
        }
        outPositions.append(index);
        score += MATCH_SCORE;
        if (index == previousMatch + 1)
        {
            score += CONSECUTIVE_BONUS;
        }
        if (index == 0 || haystack.at(index - 1).isLetterOrNumber() == false)
        {
            score += WORD_START_BONUS;
        }
        previousMatch = index;
        ++queryIndex;
    }
    if (queryIndex < needle.size())
    {
        outPositions.clear();
        return -1;
    }
    if (haystack.contains(needle) == true)
    {
        score += SUBSTRING_BONUS;
    }
    if (haystack.startsWith(needle) == true)
    {
        score += PREFIX_BONUS;
    }
    // Shorter candidates win ties: the same letters cover more of them. A match never scores below zero, which
    // means no match.
    return qMax(0, score - static_cast<int32>(haystack.size() / MATCH_SCORE));
}

} // namespace

int32 ScoreFuzzyMatch(const QString& query, const QString& candidate)
{
    QList<int32> positions;
    return MatchFuzzy(query, candidate, positions);
}

QList<int32> FindFuzzyMatchPositions(const QString& query, const QString& candidate)
{
    QList<int32> positions;
    static_cast<void>(MatchFuzzy(query, candidate, positions));
    return positions;
}
} // namespace ludus::editor
