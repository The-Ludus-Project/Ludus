#pragma once

#include <ludus/content/scene_cook/scene_cook.h>

#include <QStringList>

namespace ludus::editor
{
class WorldDocument;

/// Prepare an applied built-in World on a worker with its own frozen document copy.
/// Output is a compact UTF-8 manifest line followed by the exact, decoded LUSCN scene.
/// Cache and source verification read trusted local project inputs. Failure before commit
/// preserves the previously published snapshot. The caller serializes requests for a destination.
[[nodiscard]] bool PreparePlayableWorld(const QString& root,
                                        const QString& relativeWorld,
                                        const WorldDocument& world,
                                        const QString& renderer,
                                        const QString& destination,
                                        const content::scene_cook::Cancellation* cancelOrNull,
                                        QString& outError,
                                        QStringList* warningsOrNull = nullptr);

/// Headless equivalent for a saved project World. All paths except relativeWorld are native.
[[nodiscard]] bool PrepareSavedPlayableWorld(const QString& root,
                                             const QString& relativeWorld,
                                             const QString& renderer,
                                             const QString& destination,
                                             QString& outError,
                                             QStringList* warningsOrNull = nullptr);
} // namespace ludus::editor
