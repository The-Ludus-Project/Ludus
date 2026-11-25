#pragma once

#include <ludus/foundation/base/types.h>

#include <QList>
#include <QString>

namespace ludus::editor
{
/// How well query matches candidate: -1 when its letters do not appear in order, otherwise higher is better.
/// Case and spaces are ignored. Consecutive letters, letters at the start of a word and a match at the start
/// score higher; an empty query scores 0. The command palette and code completion rank with it.
[[nodiscard]] foundation::int32 ScoreFuzzyMatch(const QString& query, const QString& candidate);

/// Positions in candidate of the letters ScoreFuzzyMatch matched, in order; empty when query is empty or does not
/// match. The palette shows these letters in bold so a person sees why a command was found.
[[nodiscard]] QList<foundation::int32> FindFuzzyMatchPositions(const QString& query, const QString& candidate);
} // namespace ludus::editor
