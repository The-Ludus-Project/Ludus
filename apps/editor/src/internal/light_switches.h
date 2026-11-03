#pragma once

#include <ludus/foundation/base/types.h>

#include <QByteArray>
#include <QString>
#include <QVector>

namespace ludus::editor
{
struct RenderScene;

// Per-World light switches: imported light sources the World turns off without
// changing the cooked scene. A disabled source keeps its identity and authored
// values in the definition; the presented scene emits nothing for it and no light
// sampler can pick it. The World records only the disabled sources
// (docs/development/editor-workspace.md#switching-lights).

/// What a switch names in the presented scene.
enum class LightSourceKind : foundation::uint8
{
    /// An emitter group: every surface whose material carries this AreaSourceId. The importer
    /// merges area lights with equal emission and material into one group.
    Emitter,
    /// An analytic light of the cooked scene, by its SourceId.
    Light,
};

/// One switched-off light source.
struct LightSourceKey final
{
    LightSourceKind Kind = LightSourceKind::Emitter;
    /// Nonzero AreaSourceId (emitter) or light SourceId.
    foundation::uint64 SourceId = 0;
    [[nodiscard]] friend bool operator==(const LightSourceKey& left, const LightSourceKey& right) noexcept
    {
        return left.Kind == right.Kind && left.SourceId == right.SourceId;
    }
};

/// The disabled sources of one World, unique and in ascending (kind, source) order.
struct LightSwitchSet final
{
    QVector<LightSourceKey> Disabled;
    [[nodiscard]] bool IsDisabled(const LightSourceKey& key) const noexcept;
    [[nodiscard]] friend bool operator==(const LightSwitchSet& left, const LightSwitchSet& right) noexcept
    {
        return left.Disabled == right.Disabled;
    }
};

/// Most disabled sources one World records: every material of an import-profile bundle plus
/// every cooked light (content::scene_format::IMPORT_LIMITS and MAX_LIGHTS).
[[nodiscard]] foundation::usize GetMaxDisabledLightSources() noexcept;

/// Sort `inoutSet` into canonical order and drop duplicates and zero sources.
void NormalizeLightSwitches(LightSwitchSet& inoutSet);

/// Lowercase 16-digit hexadecimal text of a source ID, as the World file stores it.
[[nodiscard]] QString FormatLightSourceId(foundation::uint64 sourceId);
/// Parse FormatLightSourceId text: exactly 16 lowercase hexadecimal digits. Failure preserves outSourceId.
[[nodiscard]] bool TryParseLightSourceId(const QString& text, foundation::uint64& outSourceId) noexcept;
/// World file spelling of `kind` ("emitter" or "light").
[[nodiscard]] QString GetLightSourceKindName(LightSourceKind kind);
/// Parse GetLightSourceKindName text. Failure preserves outKind.
[[nodiscard]] bool TryParseLightSourceKind(const QString& text, LightSourceKind& outKind) noexcept;

/// Identity of a scene presenting `lightSwitchIdentity` (RenderScene::LightSwitchIdentity) over
/// content whose identity without switches is `unswitchedIdentity`.
[[nodiscard]] QByteArray ComposeLightSwitchedIdentity(const QByteArray& unswitchedIdentity,
                                                      const QByteArray& lightSwitchIdentity);

/// What ApplyLightSwitches did.
struct LightSwitchReport final
{
    /// Disabled sources the scene does not contain (for example after a re-import regrouped emitters).
    QVector<LightSourceKey> MissingSources;
    /// Surfaces whose emission was switched off.
    foundation::int32 DisabledSurfaceCount = 0;
    /// Analytic lights removed from the presented light table.
    foundation::int32 DisabledLightCount = 0;
};

/// Present `inoutScene` with exactly the sources of `switches` turned off.
///
/// Emission, emission power and the analytic light table are re-derived from the scene's immutable
/// definition each time, so re-enabling needs no stored copy and applying the same set twice gives the
/// same scene. Disabled surfaces get zero emission and no emission power (a power-specified emitter would
/// otherwise resolve a zero colour into an error material); disabled lights leave AuthoredLights.
/// Identity changes with the switch set and returns to the unswitched value when nothing is disabled,
/// so renderers restart accumulation on every change. Scenes without a cooked definition, or whose
/// surfaces do not mirror its materials, are left unchanged and report every switch as missing.
/// The definition itself, and therefore a static-lighting bake that reads it, is not changed.
/// @param switches Disabled sources; need not be normalized.
/// @param inoutScene Scene to present; its Definition is read, never written.
/// @param outReport Replaced with this application's counts and missing sources.
/// @return False only when the definition's light table exceeds the presented capacity; inoutScene
/// is then unchanged. GUI or worker thread.
[[nodiscard]] bool
ApplyLightSwitches(const LightSwitchSet& switches, RenderScene& inoutScene, LightSwitchReport& outReport);
} // namespace ludus::editor
