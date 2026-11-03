#include "internal/light_switches.h"

#include "internal/render_scene.h"

#include <ludus/content/scene_format/limits.h>
#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <QCryptographicHash>

namespace ludus::editor
{
namespace
{
namespace format = content::scene_format;
namespace rr = graphics::renderer;
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;

/// Hexadecimal digits of a 64-bit source ID in the World file.
constexpr qsizetype SOURCE_ID_DIGITS = 16;
/// Bits one hexadecimal digit encodes.
constexpr uint32 BITS_PER_HEX_DIGIT = 4;
/// Hexadecimal digit values spelled by '0' to '9'; larger values use 'a' to 'f'.
constexpr uint32 DECIMAL_DIGIT_COUNT = 10;
/// Bits in one byte of the little-endian digest input.
constexpr usize BITS_PER_BYTE = 8;
/// Domain tag of the light switch digest, so it cannot equal another identity's input.
constexpr char LIGHT_SWITCH_DIGEST_TAG[] = "ludus-light-switches-v1";

bool IsOrderedBefore(const LightSourceKey& left, const LightSourceKey& right) noexcept
{
    if (left.Kind != right.Kind)
    {
        return static_cast<uint8>(left.Kind) < static_cast<uint8>(right.Kind);
    }
    return left.SourceId < right.SourceId;
}
/// Index of key in the normalized disabled list, or -1.
qsizetype FindDisabled(const LightSwitchSet& switches, const LightSourceKey& key) noexcept
{
    for (qsizetype i = 0; i < switches.Disabled.size(); ++i)
    {
        if (switches.Disabled[i] == key)
        {
            return i;
        }
    }
    return -1;
}
QByteArray MakeLightSwitchDigest(const QVector<LightSourceKey>& applied)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView(LIGHT_SWITCH_DIGEST_TAG));
    for (const LightSourceKey& key : applied)
    {
        // Kind byte, then the source ID in little-endian order.
        foundation::StaticArray<char, sizeof(uint8) + sizeof(uint64)> bytes{};
        bytes[0] = static_cast<char>(key.Kind);
        for (usize i = 0; i < sizeof(uint64); ++i)
        {
            bytes[1 + i] = static_cast<char>(static_cast<uint8>(key.SourceId >> (i * BITS_PER_BYTE)));
        }
        hash.addData(QByteArrayView(bytes.GetData(), static_cast<qsizetype>(bytes.GetSize())));
    }
    return hash.result();
}
} // namespace

bool LightSwitchSet::IsDisabled(const LightSourceKey& key) const noexcept
{
    for (const LightSourceKey& disabled : Disabled)
    {
        if (disabled == key)
        {
            return true;
        }
    }
    return false;
}
usize GetMaxDisabledLightSources() noexcept
{
    return usize{format::IMPORT_LIMITS.Materials} + usize{format::MAX_LIGHTS};
}
void NormalizeLightSwitches(LightSwitchSet& inoutSet)
{
    foundation::Sort(
        inoutSet.Disabled.data(),
        static_cast<usize>(inoutSet.Disabled.size()),
        [](const LightSourceKey& left, const LightSourceKey& right) noexcept { return IsOrderedBefore(left, right); });
    QVector<LightSourceKey> unique;
    unique.reserve(inoutSet.Disabled.size());
    for (const LightSourceKey& key : inoutSet.Disabled)
    {
        if (key.SourceId != 0 && (unique.isEmpty() == true || (unique.last() == key) == false))
        {
            unique.append(key);
        }
    }
    inoutSet.Disabled = foundation::Move(unique);
}
QString FormatLightSourceId(uint64 sourceId)
{
    QString text(SOURCE_ID_DIGITS, QLatin1Char('0'));
    for (qsizetype i = 0; i < SOURCE_ID_DIGITS; ++i)
    {
        const uint32 shift = static_cast<uint32>(SOURCE_ID_DIGITS - 1 - i) * BITS_PER_HEX_DIGIT;
        const uint32 digit = static_cast<uint32>((sourceId >> shift) & 0xFU);
        text[i] = QLatin1Char(
            static_cast<char>(digit < DECIMAL_DIGIT_COUNT ? '0' + digit : 'a' + (digit - DECIMAL_DIGIT_COUNT)));
    }
    return text;
}
bool TryParseLightSourceId(const QString& text, uint64& outSourceId) noexcept
{
    if (text.size() != SOURCE_ID_DIGITS)
    {
        return false;
    }
    uint64 value = 0;
    for (const QChar character : text)
    {
        const char16_t code = character.unicode();
        uint32 digit = 0;
        if (code >= u'0' && code <= u'9')
        {
            digit = static_cast<uint32>(code - u'0');
        }
        else if (code >= u'a' && code <= u'f')
        {
            digit = static_cast<uint32>(code - u'a') + DECIMAL_DIGIT_COUNT;
        }
        else
        {
            return false;
        }
        value = (value << BITS_PER_HEX_DIGIT) | digit;
    }
    outSourceId = value;
    return true;
}
QString GetLightSourceKindName(LightSourceKind kind)
{
    return kind == LightSourceKind::Emitter ? QStringLiteral("emitter") : QStringLiteral("light");
}
bool TryParseLightSourceKind(const QString& text, LightSourceKind& outKind) noexcept
{
    if (text == QLatin1String("emitter"))
    {
        outKind = LightSourceKind::Emitter;
        return true;
    }
    if (text == QLatin1String("light"))
    {
        outKind = LightSourceKind::Light;
        return true;
    }
    return false;
}
QByteArray ComposeLightSwitchedIdentity(const QByteArray& unswitchedIdentity, const QByteArray& lightSwitchIdentity)
{
    return QCryptographicHash::hash(unswitchedIdentity + lightSwitchIdentity, QCryptographicHash::Sha256);
}
bool ApplyLightSwitches(const LightSwitchSet& switches, RenderScene& inoutScene, LightSwitchReport& outReport)
{
    LightSwitchSet normalized = switches;
    NormalizeLightSwitches(normalized);
    LightSwitchReport report;
    const resources::SceneDefinition* definitionOrNull = inoutScene.Definition.Get();
    if (definitionOrNull == nullptr ||
        definitionOrNull->Materials.GetSize() != static_cast<usize>(inoutScene.Surfaces.size()))
    {
        report.MissingSources = normalized.Disabled;
        outReport = foundation::Move(report);
        return true;
    }
    const resources::SceneDefinition& definition = *definitionOrNull;
    rr::LightSelection cookedLights;
    QString error;
    if (RenderScene::LoadCookedLights(definition, cookedLights, error) == false)
    {
        return false;
    }
    QVector<bool> isPresent(normalized.Disabled.size(), false);
    QVector<PreviewSurface> surfaces = inoutScene.Surfaces;
    for (usize i = 0; i < definition.Materials.GetSize(); ++i)
    {
        const resources::SceneMaterial& material = definition.Materials[i];
        PreviewSurface& surface = surfaces[static_cast<qsizetype>(i)];
        // LoadCooked copies these three fields unchanged; restoring them re-enables a source.
        surface.Emission = material.Emission;
        surface.HasEmissionPower = material.HasEmissionPower;
        surface.EmissionPower = material.EmissionPower;
        if (material.AreaSourceId == 0)
        {
            continue;
        }
        const qsizetype index = FindDisabled(normalized, {LightSourceKind::Emitter, material.AreaSourceId});
        if (index < 0)
        {
            continue;
        }
        isPresent[index] = true;
        surface.Emission = {};
        surface.HasEmissionPower = false;
        surface.EmissionPower = 0;
        ++report.DisabledSurfaceCount;
    }
    for (const resources::SceneLight& light : definition.Lights)
    {
        const qsizetype index = FindDisabled(normalized, {LightSourceKind::Light, light.SourceId});
        if (index >= 0)
        {
            isPresent[index] = true;
        }
    }
    rr::LightSelection presentedLights;
    for (uint32 i = 0; i < cookedLights.Count; ++i)
    {
        if (normalized.IsDisabled({LightSourceKind::Light, cookedLights.Lights[i].SourceId}) == true)
        {
            ++report.DisabledLightCount;
            continue;
        }
        presentedLights.Lights[presentedLights.Count++] = cookedLights.Lights[i];
    }
    QVector<LightSourceKey> applied;
    for (qsizetype i = 0; i < normalized.Disabled.size(); ++i)
    {
        if (isPresent[i] == true)
        {
            applied.append(normalized.Disabled[i]);
        }
        else
        {
            report.MissingSources.append(normalized.Disabled[i]);
        }
    }
    const QByteArray unswitchedIdentity =
        inoutScene.UnswitchedIdentity.isEmpty() == true ? inoutScene.Identity : inoutScene.UnswitchedIdentity;
    inoutScene.Surfaces = foundation::Move(surfaces);
    inoutScene.AuthoredLights = presentedLights;
    if (applied.isEmpty() == true)
    {
        inoutScene.Identity = unswitchedIdentity;
        inoutScene.UnswitchedIdentity.clear();
        inoutScene.LightSwitchIdentity.clear();
    }
    else
    {
        inoutScene.UnswitchedIdentity = unswitchedIdentity;
        inoutScene.LightSwitchIdentity = MakeLightSwitchDigest(applied);
        inoutScene.Identity = ComposeLightSwitchedIdentity(unswitchedIdentity, inoutScene.LightSwitchIdentity);
    }
    outReport = foundation::Move(report);
    return true;
}
} // namespace ludus::editor
