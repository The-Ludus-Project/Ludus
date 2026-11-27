#pragma once

#include <ludus/content/scene_cook/scene_cook.h>

#include <QStringList>

namespace ludus::editor
{
class WorldDocument;
struct RenderScene;

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

/// Export a saved World's cooked render scene for an external installed-SDK consumer. It runs the same
/// Load(no lighting composition) -> AssembleImports -> RenderScene::Merge path as Play, but emits the
/// bare LUSCN bundle (the bytes resources::SceneDefinition::Decode consumes) to @p sceneDestination and
/// a sidecar manifest to @p manifestDestination. The manifest records the World and cooked-scene
/// SHA-256 digests, the cook dependency artifacts, the saved camera (eye/target/up/fov) and each
/// imported material slot's resolved scattering model, so a consumer can prove same-World parity. It
/// does not require a built-in schema, a Play spawn or entity ownership; it rejects an authored
/// atmosphere error the same way Play does. All paths are native. @p sceneDestination must end in
/// `.luscn`; @p manifestDestination in `.json`.
/// @param outSceneOrNull When nonnull, receives the composed render scene whose bundle was written, so
/// a caller can render exactly the exported packet; it is assigned only after both files are written.
[[nodiscard]] bool ExportCookedWorldScene(const QString& root,
                                          const WorldDocument& world,
                                          const QString& sceneDestination,
                                          const QString& manifestDestination,
                                          QString& outError,
                                          RenderScene* outSceneOrNull = nullptr);
} // namespace ludus::editor
