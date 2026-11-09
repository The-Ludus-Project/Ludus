#include "internal/world_document.h"

#include "internal/builtin_world.h"
#include "internal/editor_identity.h"
#include "internal/material_inspection.h"
#include "internal/world_celestial_light.h"
#include "internal/world_terrain.h"

#include <ludus/content/identity_records/migration_manifest.h>
#include <ludus/content/identity_records/world_records.h>
#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/parse_number.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/parsing/json.hpp>

#include <QJsonDocument>
#include <QSet>
#include <QUuid>

namespace ludus::editor
{
namespace
{
bool Fail(QString& error, const QString& message)
{
    error = message;
    return false;
}
bool Id(const QString& text)
{
    if (text.isEmpty() || text.size() > 64)
    {
        return false;
    }
    for (const auto ch : text)
    {
        const auto c = ch.unicode();
        if ((c < 'a' || c > 'z') && (c < 'A' || c > 'Z') && (c < '0' || c > '9') && c != '_' && c != '-' && c != '.' &&
            c != ':')
        {
            return false;
        }
    }
    return true;
}
bool ResourceId(const QJsonValue& value, const QString& kind)
{
    const auto text = value.toString();
    const auto prefix = kind + QLatin1Char('/');
    const auto uuid = text.mid(prefix.size());
    return value.isString() && text.startsWith(prefix) && uuid.size() == 36 &&
           QUuid(uuid).toString(QUuid::WithoutBraces) == uuid;
}
bool Text(const QJsonValue& value, foundation::int64 limit)
{
    if (value.isString() == false)
    {
        return false;
    }
    const auto text = value.toString();
    return text.toUtf8().size() <= limit && text.contains(QChar(0)) == false &&
           QString::fromUtf8(text.toUtf8()) == text;
}
namespace identity_records = content::identity_records;
/// Longest label minted for a new version-2 entity: it fits the world-demo sample's
/// 32-byte name field with its terminator, and the 64-character World ID rule.
constexpr qsizetype MAX_MINTED_LABEL_CHARACTERS = 31;
/// Bytes per KiB, for reporting the World size limit in the unit users read.
constexpr foundation::int64 BYTES_PER_KIBIBYTE = 1024;
/// Longest UTF-8 schema, component, property, asset, enum choice, entity or scene label.
constexpr foundation::int64 MAX_LABEL_BYTES = 128;
/// Longest UTF-8 component or property help text.
constexpr foundation::int64 MAX_HELP_BYTES = 512;
/// Longest UTF-8 property unit.
constexpr foundation::int64 MAX_UNIT_BYTES = 32;
/// Most named choices of one enum property.
constexpr qsizetype MAX_ENUM_CHOICES = 64;
/// Members of a `scene_transform` binding: component, translation, rotation and scale.
constexpr qsizetype SCENE_TRANSFORM_BINDING_FIELD_COUNT = 4;
/// Components of a vector: transform binding axes, camera vectors and placement vectors.
constexpr qsizetype VECTOR_COMPONENT_COUNT = 3;
/// Elements of a 3x3 placement basis.
constexpr qsizetype BASIS_ELEMENT_COUNT = 9;
/// Largest magnitude of a reference scene camera coordinate.
constexpr double MAX_CAMERA_COORDINATE = 1e12;
/// Largest magnitude of an instance placement value.
constexpr double MAX_PLACEMENT_VALUE = 1000000;
/// Root `version` value of a World document of envelope version `version`.
QJsonValue MakeVersionValue(foundation::uint64 version)
{
    return QJsonValue(static_cast<qint64>(version));
}
/// Member holding an entity's key: the version-2 `uuid` or the version-1 `id`.
QString GetKeyMember(bool hasUuidRecords)
{
    return hasUuidRecords == true ? QStringLiteral("uuid") : QStringLiteral("id");
}
/// Member holding an entity's readable label: the version-2 `label` or the version-1 `id`.
QString GetLabelMember(bool hasUuidRecords)
{
    return hasUuidRecords == true ? QStringLiteral("label") : QStringLiteral("id");
}
/// Canonical lowercase text of a nonnil version-4 UUID, as the identity records write it.
bool IsUuidText(const QJsonValue& value)
{
    namespace identity = foundation::identity;
    if (value.isString() == false)
    {
        return false;
    }
    const QByteArray text = value.toString().toLatin1();
    identity::Uuid uuid;
    if (identity::TryParseUuid(text.constData(),
                               static_cast<foundation::usize>(text.size()),
                               identity::UuidPolicy::RequireVersion4,
                               uuid) != identity::UuidStatus::Success)
    {
        return false;
    }
    foundation::StaticArray<char, identity::UUID_TEXT_BUFFER_BYTES> canonical{};
    return identity::FormatUuid(uuid, canonical.GetData(), canonical.GetSize()) == identity::UuidStatus::Success &&
           text == QByteArray(canonical.GetData(), static_cast<qsizetype>(identity::UUID_TEXT_LENGTH));
}
/// Mint a SourceObjectId for a new version-2 entity. Like the migration manifest,
/// each candidate is checked against every identity of the World and replaced at
/// most MAX_UUID_GENERATION_ATTEMPTS times in total.
bool TryMintObjectUuid(const foundation::identity::EntropySource& entropy,
                       QString& outError,
                       const QJsonObject& world,
                       QString& outUuid)
{
    QSet<QString> used{world.value(QStringLiteral("document")).toString()};
    for (const QJsonValue value : world.value(QStringLiteral("entities")).toArray())
    {
        used.insert(value.toObject().value(QStringLiteral("uuid")).toString());
    }
    for (foundation::usize attempt = 0; attempt < identity_records::MAX_UUID_GENERATION_ATTEMPTS; ++attempt)
    {
        QString candidate;
        if (TryGenerateUuidText(entropy, candidate) == false)
        {
            return Fail(outError, DescribeEntropyUnavailable());
        }
        if (used.contains(candidate) == false)
        {
            outUuid = foundation::Move(candidate);
            return true;
        }
    }
    return Fail(outError,
                QStringLiteral("Could not generate an identity that is unique in this World. Nothing was changed."));
}
/// Remove the trailing separators a truncated label may end with.
QString TrimLabel(QString label)
{
    while (label.endsWith(QLatin1Char('-')) == true)
    {
        label.chop(1);
    }
    return label;
}
/// Derive a readable label for a new version-2 entity from its display name: the
/// name's ASCII letters (lowercased) and digits, with each other run of characters
/// replaced by one `-`. `-2`, `-3`, ... make it unique among the World's labels.
QString MakeUniqueLabel(const QJsonObject& world, const QString& name)
{
    QString base;
    bool isSeparatorPending = false;
    for (const QChar character : name)
    {
        const foundation::uint16 code = character.unicode();
        const foundation::uint16 lower =
            code >= 'A' && code <= 'Z' ? static_cast<foundation::uint16>(code - 'A' + 'a') : code;
        const bool isLabelCharacter = (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9');
        if (isLabelCharacter == false)
        {
            isSeparatorPending = true;
            continue;
        }
        if (isSeparatorPending == true && base.isEmpty() == false)
        {
            base.append(QLatin1Char('-'));
        }
        isSeparatorPending = false;
        base.append(QChar(lower));
    }
    base = TrimLabel(base.left(MAX_MINTED_LABEL_CHARACTERS));
    if (base.isEmpty() == true)
    {
        base = QStringLiteral("entity");
    }
    QSet<QString> used;
    for (const QJsonValue value : world.value(QStringLiteral("entities")).toArray())
    {
        used.insert(value.toObject().value(QStringLiteral("label")).toString());
    }
    QString candidate = base;
    for (foundation::int32 suffix = 2; used.contains(candidate) == true; ++suffix)
    {
        const QString tail = QLatin1Char('-') + QString::number(suffix);
        candidate = TrimLabel(base.left(MAX_MINTED_LABEL_CHARACTERS - tail.size())) + tail;
    }
    return candidate;
}
// Mint a fresh entity ID. World-demo IDs keep their legacy `entity-` prefix and
// the first ENTITY_UUID_CHARACTERS of the UUID text, so that they fit the
// sample's 32-byte name field with its terminator.
bool TryGenerateEntityId(const foundation::identity::EntropySource& entropy, bool isWorldDemo, QString& outId)
{
    constexpr qsizetype ENTITY_UUID_CHARACTERS = 24;
    QString uuid;
    if (TryGenerateUuidText(entropy, uuid) == false)
    {
        return false;
    }
    outId = isWorldDemo == true ? QStringLiteral("entity-") + uuid.left(ENTITY_UUID_CHARACTERS) : uuid;
    return true;
}
bool Fields(const QJsonObject& object, const QStringList& allowed)
{
    for (auto i = object.begin(); i != object.end(); ++i)
    {
        if (allowed.contains(i.key()) == false)
        {
            return false;
        }
    }
    return true;
}
bool ValidateEncodedObject(const QByteArray& bytes, foundation::int64 limit, QString& outError)
{
    namespace parsing = foundation::parsing;
    if (bytes.isEmpty() || bytes.size() > limit)
    {
        return Fail(outError, QStringLiteral("Document exceeds its byte limit or is empty."));
    }
    // The Ludus parser supplies bounded depth and duplicate-key rejection before
    // private Qt presentation values are created; Qt alone collapses duplicates.
    parsing::JsonDocument document;
    parsing::ParseError diagnostic;
    parsing::JsonLimits limits;
    limits.MaxInputBytes = static_cast<foundation::usize>(limit);
    limits.MaxObjectMembers = 64;
    limits.MaxArrayElements = 4096;
    limits.MaxValues = 131072;
    limits.MaxStringBytes = 2048;
    if (document.Read({bytes.constData(), static_cast<foundation::usize>(bytes.size())}, diagnostic, limits) !=
            parsing::ParseStatus::Ok ||
        document.Root().Object() == false)
    {
        return Fail(outError, QStringLiteral("Invalid JSON, duplicate keys, or document limits exceeded."));
    }
    return true;
}
bool Decode(const QByteArray& bytes, foundation::int64 limit, QJsonObject& outObject, QString& outError)
{
    if (ValidateEncodedObject(bytes, limit, outError) == false)
    {
        return false;
    }
    outObject = QJsonDocument::fromJson(bytes).object();
    return true;
}
bool Value(const QJsonObject& p, const QJsonValue& value, QString& error)
{
    const auto type = p.value(QStringLiteral("type")).toString();
    bool valid = false;
    if (type == QStringLiteral("boolean"))
    {
        valid = value.isBool();
    }
    else if (type == QStringLiteral("number") || type == QStringLiteral("integer"))
    {
        const auto n = value.toDouble();
        valid = value.isDouble() && foundation::math::IsFinite(n) && n >= p.value(QStringLiteral("min")).toDouble() &&
                n <= p.value(QStringLiteral("max")).toDouble();
        if (valid && type == QStringLiteral("integer"))
        {
            valid = n == static_cast<foundation::float64>(static_cast<foundation::int32>(n));
        }
    }
    else if (type == QStringLiteral("string"))
    {
        valid = Text(value, 512);
    }
    else if (type == QStringLiteral("enum"))
    {
        valid = value.isString() && p.value(QStringLiteral("choices")).toArray().contains(value);
    }
    else if (type == QStringLiteral("entity") || type == QStringLiteral("asset"))
    {
        valid = value.isString() && (value.toString().isEmpty() || Id(value.toString()));
    }
    return valid || Fail(error,
                         QStringLiteral("%1: value does not match its type, choices or bounds.")
                             .arg(p.value(QStringLiteral("label")).toString()));
}
/// One `disabled_lights` entry: exactly a known `kind` and a nonzero 16-digit lowercase hex `source`.
bool TryReadLightSwitch(const QJsonValue& value, LightSourceKey& outKey)
{
    const QJsonObject entry = value.toObject();
    LightSourceKey key;
    if (value.isObject() == false || entry.size() != 2 ||
        Fields(entry, {QStringLiteral("kind"), QStringLiteral("source")}) == false ||
        TryParseLightSourceKind(entry.value(QStringLiteral("kind")).toString(), key.Kind) == false ||
        TryParseLightSourceId(entry.value(QStringLiteral("source")).toString(), key.SourceId) == false ||
        key.SourceId == 0)
    {
        return false;
    }
    outKey = key;
    return true;
}
/// The root `disabled_lights` list: only beside a scene level, bounded, with unique valid entries.
bool IsLightSwitchListValid(const QJsonObject& candidate, QString& outError)
{
    const QJsonValue value = candidate.value(QStringLiteral("disabled_lights"));
    const QJsonArray entries = value.toArray();
    if (candidate.contains(QStringLiteral("scene")) == false || value.isArray() == false ||
        static_cast<foundation::usize>(entries.size()) > GetMaxDisabledLightSources())
    {
        return Fail(outError,
                    QStringLiteral("Disabled lights need an imported scene and at most %1 sources.")
                        .arg(GetMaxDisabledLightSources()));
    }
    LightSwitchSet seen;
    for (const QJsonValue entry : entries)
    {
        LightSourceKey key;
        if (TryReadLightSwitch(entry, key) == false || seen.IsDisabled(key) == true)
        {
            return Fail(outError,
                        QStringLiteral("Every disabled light names a unique emitter or light by its 16-digit "
                                       "lowercase hexadecimal source ID."));
        }
        seen.Disabled.append(key);
    }
    return true;
}
QJsonObject Defaults(const QJsonObject& component)
{
    QJsonObject out;
    for (const auto value : component.value(QStringLiteral("properties")).toArray())
    {
        const auto p = value.toObject();
        out.insert(p.value(QStringLiteral("id")).toString(), p.value(QStringLiteral("default")));
    }
    return out;
}
[[nodiscard]] bool ValidateCoating(const QJsonValue& value, QString& outError)
{
    const QJsonObject coating = value.toObject();
    if (coating.size() != static_cast<qsizetype>(sizeof(COATING_FIELDS) / sizeof(CoatingField)))
    {
        return Fail(outError, QStringLiteral("A coating instance needs GGX alpha, IOR and optical thickness."));
    }
    for (const CoatingField& field : COATING_FIELDS)
    {
        if (Value(field.MakeNumberProperty(), coating.value(QString::fromLatin1(field.Key)), outError) == false)
        {
            return false;
        }
    }
    return true;
}

} // namespace
bool WorldDocument::LoadSchema(const QByteArray& bytes, QString& error)
{
    if (Loaded_)
    {
        return Fail(error, QStringLiteral("Close the current world before loading another schema."));
    }
    QJsonObject schema;
    if (Decode(bytes, MAX_SCHEMA_BYTES, schema, error) == false)
    {
        return false;
    }
    if (Fields(schema,
               {QStringLiteral("version"),
                QStringLiteral("id"),
                QStringLiteral("label"),
                QStringLiteral("components"),
                QStringLiteral("assets"),
                QStringLiteral("scene_transform")}) == false ||
        schema.value(QStringLiteral("version")) != QJsonValue(1) ||
        Id(schema.value(QStringLiteral("id")).toString()) == false ||
        Text(schema.value(QStringLiteral("label")), MAX_LABEL_BYTES) == false ||
        schema.value(QStringLiteral("components")).isArray() == false ||
        schema.value(QStringLiteral("assets")).isArray() == false)
    {
        return Fail(error, QStringLiteral("Schema needs version 1, stable id, label, components and assets arrays."));
    }
    QJsonArray components = schema.value(QStringLiteral("components")).toArray();
    bool isBuiltInSchema = false;
    if (schema.value(QStringLiteral("id")) == QJsonValue(QStringLiteral("ludus.world.default.v1")))
    {
        QByteArray bundledSchema;
        if (ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), bundledSchema, error) == false)
        {
            return false;
        }
        if (bytes == bundledSchema)
        {
            // Extend the in-memory schema while retaining the base bytes used by World recovery fingerprints.
            QByteArray terrainBytes;
            QJsonObject terrainComponent;
            if (ReadBuiltInWorldAsset(QStringLiteral("terrain.component.json"), terrainBytes, error) == false ||
                Decode(terrainBytes, MAX_SCHEMA_BYTES, terrainComponent, error) == false)
            {
                return false;
            }
            const QJsonArray celestialProperties = MakeWorldCelestialProperties();
            for (qsizetype i = 0; i < components.size(); ++i)
            {
                QJsonObject component = components[i].toObject();
                if (component.value(QStringLiteral("id")) != QJsonValue(QStringLiteral("light")))
                {
                    continue;
                }
                QJsonArray properties = component.value(QStringLiteral("properties")).toArray();
                for (const QJsonValue property : celestialProperties)
                {
                    properties.append(property);
                }
                properties.append(QJsonObject{
                    {QStringLiteral("id"), QStringLiteral("staticTransform")},
                    {QStringLiteral("label"), QStringLiteral("Static Transform")},
                    {QStringLiteral("type"), QStringLiteral("boolean")},
                    {QStringLiteral("default"), false},
                    {QStringLiteral("unit"), QString()},
                    {QStringLiteral("help"), QStringLiteral("This light's transform stays fixed during play.")},
                });
                properties.append(QJsonObject{
                    {QStringLiteral("id"), QStringLiteral("bakeAlwaysOn")},
                    {QStringLiteral("label"), QStringLiteral("Bake always-on lighting")},
                    {QStringLiteral("type"), QStringLiteral("boolean")},
                    {QStringLiteral("default"), false},
                    {QStringLiteral("unit"), QString()},
                    {QStringLiteral("help"),
                     QStringLiteral("Bake this enabled light's diffuse contribution into "
                                    "the native Lit viewport's shared chart.")},
                });
                component.insert(QStringLiteral("properties"), properties);
                components[i] = component;
                break;
            }
            components.append(terrainComponent);
            schema.insert(QStringLiteral("components"), components);
            isBuiltInSchema = true;
        }
    }
    if (components.size() > 16 || schema.value(QStringLiteral("assets")).toArray().size() > 4096)
    {
        return Fail(error, QStringLiteral("Schema supports 0–16 components and at most 4096 declared assets."));
    }
    QSet<QString> ids;
    for (const auto value : components)
    {
        const auto c = value.toObject();
        const auto id = c.value(QStringLiteral("id")).toString();
        if (value.isObject() == false ||
            Fields(c,
                   {QStringLiteral("id"),
                    QStringLiteral("label"),
                    QStringLiteral("help"),
                    QStringLiteral("required"),
                    QStringLiteral("properties")}) == false ||
            Id(id) == false || ids.contains(id) || Text(c.value(QStringLiteral("label")), MAX_LABEL_BYTES) == false ||
            Text(c.value(QStringLiteral("help")), MAX_HELP_BYTES) == false ||
            c.value(QStringLiteral("required")).isBool() == false ||
            c.value(QStringLiteral("properties")).isArray() == false)
        {
            return Fail(error, QStringLiteral("Invalid or duplicate component definition: %1").arg(id));
        }
        ids.insert(id);
        const auto properties = c.value(QStringLiteral("properties")).toArray();
        if (properties.size() > 32)
        {
            return Fail(error, QStringLiteral("Each component supports at most 32 properties."));
        }
        QSet<QString> propertiesSeen;
        for (const auto field : properties)
        {
            const auto p = field.toObject();
            const auto key = p.value(QStringLiteral("id")).toString();
            const auto type = p.value(QStringLiteral("type")).toString();
            if (field.isObject() == false ||
                Fields(p,
                       {QStringLiteral("id"),
                        QStringLiteral("label"),
                        QStringLiteral("type"),
                        QStringLiteral("default"),
                        QStringLiteral("min"),
                        QStringLiteral("max"),
                        QStringLiteral("choices"),
                        QStringLiteral("help"),
                        QStringLiteral("unit"),
                        QStringLiteral("asset_kind")}) == false ||
                Id(key) == false || propertiesSeen.contains(key) ||
                Text(p.value(QStringLiteral("label")), MAX_LABEL_BYTES) == false ||
                Text(p.value(QStringLiteral("help")), MAX_HELP_BYTES) == false ||
                Text(p.value(QStringLiteral("unit")), MAX_UNIT_BYTES) == false)
            {
                return Fail(error, QStringLiteral("Invalid or duplicate property definition: %1.%2").arg(id, key));
            }
            propertiesSeen.insert(key);
            if (type == QStringLiteral("number") || type == QStringLiteral("integer"))
            {
                const auto low = p.value(QStringLiteral("min"));
                const auto high = p.value(QStringLiteral("max"));
                const auto limit =
                    type == QStringLiteral("integer") ? foundation::float64{2147483647} : foundation::float64{1e30};
                if (low.isDouble() == false || high.isDouble() == false ||
                    foundation::math::IsFinite(low.toDouble()) == false ||
                    foundation::math::IsFinite(high.toDouble()) == false || low.toDouble() > high.toDouble() ||
                    low.toDouble() < -limit || high.toDouble() > limit)
                {
                    return Fail(error, QStringLiteral("Numeric properties need finite, ordered min/max bounds."));
                }
            }
            if (type == QStringLiteral("enum"))
            {
                const auto choices = p.value(QStringLiteral("choices")).toArray();
                QSet<QString> unique;
                if (p.value(QStringLiteral("choices")).isArray() == false || choices.isEmpty() ||
                    choices.size() > MAX_ENUM_CHOICES)
                {
                    return Fail(error, QStringLiteral("Enums need 1–64 named choices."));
                }
                for (const auto choice : choices)
                {
                    if (Text(choice, MAX_LABEL_BYTES) == false || unique.contains(choice.toString()))
                    {
                        return Fail(error, QStringLiteral("Invalid or duplicate enum choice."));
                    }
                    unique.insert(choice.toString());
                }
            }
            if ((type == QStringLiteral("asset") && Id(p.value(QStringLiteral("asset_kind")).toString()) == false) ||
                (type != QStringLiteral("asset") && p.contains(QStringLiteral("asset_kind"))))
            {
                return Fail(error,
                            QStringLiteral("Asset properties need an asset_kind; other property types must omit it."));
            }
            if (Value(p, p.value(QStringLiteral("default")), error) == false)
            {
                return false;
            }
            if ((type == QStringLiteral("entity") || type == QStringLiteral("asset")) &&
                p.value(QStringLiteral("default")) != QJsonValue(QString()))
            {
                return Fail(
                    error,
                    QStringLiteral("Reference defaults must be empty; assign an explicit reference after creation."));
            }
        }
    }
    ids.clear();
    for (const auto value : schema.value(QStringLiteral("assets")).toArray())
    {
        const auto asset = value.toObject();
        const auto id = asset.value(QStringLiteral("id")).toString();
        if (value.isObject() == false ||
            Fields(asset, {QStringLiteral("id"), QStringLiteral("label"), QStringLiteral("kind")}) == false ||
            Id(id) == false || ids.contains(id) ||
            Text(asset.value(QStringLiteral("label")), MAX_LABEL_BYTES) == false ||
            Id(asset.value(QStringLiteral("kind")).toString()) == false)
        {
            return Fail(error, QStringLiteral("Invalid or duplicate declared asset."));
        }
        ids.insert(id);
    }
    if (schema.contains(QStringLiteral("scene_transform")))
    {
        const auto binding = schema.value(QStringLiteral("scene_transform")).toObject();
        if (schema.value(QStringLiteral("scene_transform")).isObject() == false ||
            binding.size() != SCENE_TRANSFORM_BINDING_FIELD_COUNT ||
            Fields(binding,
                   {QStringLiteral("component"),
                    QStringLiteral("position"),
                    QStringLiteral("rotation"),
                    QStringLiteral("scale")}) == false)
        {
            return Fail(error, QStringLiteral("Invalid scene transform binding."));
        }
        QJsonObject definition;
        for (const auto value : components)
        {
            if (value.toObject().value(QStringLiteral("id")) == binding.value(QStringLiteral("component")))
            {
                definition = value.toObject();
                break;
            }
        }
        if (definition.value(QStringLiteral("required")).toBool() == false)
        {
            return Fail(error, QStringLiteral("Scene transform binding needs a required component."));
        }
        QSet<QString> names;
        for (const auto& key : {QStringLiteral("position"), QStringLiteral("rotation"), QStringLiteral("scale")})
        {
            const auto axes = binding.value(key).toArray();
            if (binding.value(key).isArray() == false || axes.size() != VECTOR_COMPONENT_COUNT)
            {
                return Fail(error, QStringLiteral("Scene transform bindings need three property IDs per group."));
            }
            for (const auto axis : axes)
            {
                QJsonObject property;
                for (const auto field : definition.value(QStringLiteral("properties")).toArray())
                {
                    if (field.toObject().value(QStringLiteral("id")) == axis)
                    {
                        property = field.toObject();
                        break;
                    }
                }
                if (axis.isString() == false || names.contains(axis.toString()) ||
                    property.value(QStringLiteral("type")) != QJsonValue(QStringLiteral("number")))
                {
                    return Fail(error, QStringLiteral("Scene placement needs nine distinct numeric properties."));
                }
                names.insert(axis.toString());
            }
        }
    }
    Schema_ = foundation::Move(schema);
    mIsBuiltInSchema = isBuiltInSchema;
    error.clear();
    return true;
}
QJsonObject WorldDocument::Component(const QString& id) const
{
    for (const auto value : Schema_.value(QStringLiteral("components")).toArray())
    {
        const auto c = value.toObject();
        if (c.value(QStringLiteral("id")).toString() == id)
        {
            return c;
        }
    }
    return {};
}
bool WorldDocument::HasUuidRecords() const
{
    return Draft_.value(QStringLiteral("version")) == MakeVersionValue(identity_records::WORLD_RECORD_VERSION_2);
}
QString WorldDocument::GetEntityKey(const QJsonObject& entity) const
{
    return entity.value(GetKeyMember(HasUuidRecords())).toString();
}
QString WorldDocument::GetEntityLabel(const QJsonObject& entity) const
{
    return entity.value(GetLabelMember(HasUuidRecords())).toString();
}
QString WorldDocument::FindEntityKey(const QString& label) const
{
    for (const QJsonValue value : Draft_.value(QStringLiteral("entities")).toArray())
    {
        const QJsonObject entity = value.toObject();
        if (GetEntityLabel(entity) == label)
        {
            return GetEntityKey(entity);
        }
    }
    return {};
}
QJsonObject WorldDocument::Entity(const QString& id) const
{
    for (const auto value : Draft_.value(QStringLiteral("entities")).toArray())
    {
        const auto e = value.toObject();
        if (GetEntityKey(e) == id)
        {
            return e;
        }
    }
    return {};
}
bool WorldDocument::Validate(const QJsonObject& candidate, QString& error) const
{
    const QJsonValue version = candidate.value(QStringLiteral("version"));
    const bool hasUuidRecords = version == MakeVersionValue(identity_records::WORLD_RECORD_VERSION_2);
    const bool hasScopedIds = version == MakeVersionValue(identity_records::WORLD_RECORD_VERSION_1);
    if (hasUuidRecords == true && candidate.contains(QStringLiteral("scene")) == true)
    {
        return Fail(error, QStringLiteral("Version-2 World records do not support the scene extension yet."));
    }
    const QStringList rootFields = hasUuidRecords == true ? QStringList{QStringLiteral("version"),
                                                                        QStringLiteral("schema"),
                                                                        QStringLiteral("document"),
                                                                        QStringLiteral("entities")}
                                                          : QStringList{QStringLiteral("version"),
                                                                        QStringLiteral("schema"),
                                                                        QStringLiteral("entities"),
                                                                        QStringLiteral("scene"),
                                                                        QStringLiteral("disabled_lights"),
                                                                        QStringLiteral("shared_coatings")};
    if (HasSchema() == false || (hasScopedIds == false && hasUuidRecords == false) ||
        Fields(candidate, rootFields) == false ||
        candidate.value(QStringLiteral("schema")) != Schema_.value(QStringLiteral("id")) ||
        candidate.value(QStringLiteral("entities")).isArray() == false)
    {
        return Fail(error, QStringLiteral("World version/schema does not match this project's authoring contract."));
    }
    const QString document = candidate.value(QStringLiteral("document")).toString();
    if (hasUuidRecords == true && IsUuidText(candidate.value(QStringLiteral("document"))) == false)
    {
        return Fail(error, QStringLiteral("A version-2 World needs a nonnil version-4 document UUID."));
    }
    const auto entities = candidate.value(QStringLiteral("entities")).toArray();
    const QByteArray encoded =
        entities.size() <= MAX_ENTITIES ? QJsonDocument(candidate).toJson(QJsonDocument::Compact) : QByteArray{};
    if (entities.size() > MAX_ENTITIES || encoded.size() + 1 > MAX_BYTES)
    {
        return Fail(error,
                    QStringLiteral("World supports at most %1 entities and %2 KiB.")
                        .arg(MAX_ENTITIES)
                        .arg(MAX_BYTES / BYTES_PER_KIBIBYTE));
    }
    const QJsonValue sharedCoatings = candidate.value(QStringLiteral("shared_coatings"));
    if (sharedCoatings.isUndefined() == false)
    {
        const QJsonObject instances = sharedCoatings.toObject();
        if (candidate.contains(QStringLiteral("scene")) == false || sharedCoatings.isObject() == false ||
            instances.size() > static_cast<qsizetype>(content::scene_format::IMPORT_LIMITS.Materials))
        {
            return Fail(error, QStringLiteral("Invalid shared coating instances. Check the World's scene and table."));
        }
        for (QJsonObject::const_iterator instance = instances.constBegin(); instance != instances.constEnd();
             ++instance)
        {
            if (ResourceId(QJsonValue(instance.key()), QStringLiteral("material")) == false)
            {
                return Fail(error, QStringLiteral("A shared coating must name an imported material resource ID."));
            }
            if (ValidateCoating(instance.value(), error) == false)
            {
                return false;
            }
        }
    }
    // Entity keys: version-2 UUIDs or version-1 IDs, the values entity-typed properties hold.
    QSet<QString> ids;
    QSet<QString> labels;
    QSet<QString> assets;
    for (const auto value : Schema_.value(QStringLiteral("assets")).toArray())
    {
        assets.insert(value.toObject().value(QStringLiteral("id")).toString());
    }
    for (const QJsonValue value : entities)
    {
        const QJsonObject e = value.toObject();
        const QString id = e.value(GetKeyMember(hasUuidRecords)).toString();
        const QString label = e.value(GetLabelMember(hasUuidRecords)).toString();
        // A version-2 entity is named by its UUID; its label stays a unique, valid scoped ID.
        const bool hasIdentity = hasUuidRecords == true
                                     ? Fields(e,
                                              {QStringLiteral("uuid"),
                                               QStringLiteral("label"),
                                               QStringLiteral("name"),
                                               QStringLiteral("components")}) == true &&
                                           IsUuidText(e.value(QStringLiteral("uuid"))) == true && id != document &&
                                           Id(label) == true && labels.contains(label) == false
                                     : Fields(e,
                                              {QStringLiteral("id"),
                                               QStringLiteral("name"),
                                               QStringLiteral("components"),
                                               QStringLiteral("scene")}) == true &&
                                           Id(id) == true;
        if (value.isObject() == false || hasIdentity == false || ids.contains(id) == true ||
            Text(e.value(QStringLiteral("name")), MAX_LABEL_BYTES) == false ||
            e.value(QStringLiteral("name")).toString().trimmed().isEmpty() == true ||
            e.value(QStringLiteral("components")).isObject() == false)
        {
            return Fail(
                error,
                hasUuidRecords == true
                    ? QStringLiteral("Every entity needs a unique UUID and label, a name and components object.")
                    : QStringLiteral("Every entity needs a unique stable ID, name and components object."));
        }
        ids.insert(id);
        labels.insert(label);
    }
    const QString terrainComponent = GetTerrainComponentId();
    bool hasAuthoredTerrainPayload = false;
    for (const auto value : entities)
    {
        const auto e = value.toObject();
        const auto components = e.value(QStringLiteral("components")).toObject();
        if (HasBuiltInTerrain() == true && components.contains(terrainComponent) == true)
        {
            if (components.contains(QStringLiteral("transform")) == false ||
                e.contains(QStringLiteral("scene")) == true)
            {
                return Fail(error,
                            QStringLiteral("Generated terrain needs a Transform and its own entity. Use "
                                           "Generate Terrain to create it."));
            }
            if (ValidateTerrainRecipe(components.value(terrainComponent).toObject(), error) == false)
            {
                return false;
            }
        }
        for (const auto definition : Schema_.value(QStringLiteral("components")).toArray())
        {
            const auto c = definition.toObject();
            if (c.value(QStringLiteral("required")).toBool() &&
                components.contains(c.value(QStringLiteral("id")).toString()) == false)
            {
                return Fail(
                    error,
                    QStringLiteral("%1 requires %2.")
                        .arg(e.value(QStringLiteral("name")).toString(), c.value(QStringLiteral("label")).toString()));
            }
        }
        for (auto c = components.begin(); c != components.end(); ++c)
        {
            const auto definition = Component(c.key());
            if (definition.isEmpty() || c.value().isObject() == false)
            {
                return Fail(error, QStringLiteral("Unknown component %1; the world was preserved.").arg(c.key()));
            }
            const auto props = c.value().toObject();
            const auto definitions = definition.value(QStringLiteral("properties")).toArray();
            const bool isBuiltInLight = HasBuiltInSchema() == true && c.key() == QStringLiteral("light");
            qsizetype recognizedPropertyCount = 0;
            for (const QJsonValue field : definitions)
            {
                const QJsonObject p = field.toObject();
                const QString property = p.value(QStringLiteral("id")).toString();
                const QJsonValue v = props.value(property);
                if (v.isUndefined() == true)
                {
                    // Older Worlds may omit fields added by the built-in schema extensions.
                    if (isBuiltInLight == true &&
                        (IsWorldCelestialProperty(property) == true || property == QStringLiteral("staticTransform") ||
                         property == QStringLiteral("bakeAlwaysOn")))
                    {
                        continue;
                    }
                    return Fail(error, QStringLiteral("%1 has missing or unknown properties.").arg(c.key()));
                }
                ++recognizedPropertyCount;
                if (Value(p, v, error) == false)
                {
                    error = e.value(QStringLiteral("name")).toString() + QStringLiteral(" / ") +
                            definition.value(QStringLiteral("label")).toString() + QStringLiteral(" / ") + error;
                    return false;
                }
                const auto type = p.value(QStringLiteral("type")).toString();
                if (v.toString().isEmpty() == false &&
                    ((type == QStringLiteral("entity") && ids.contains(v.toString()) == false) ||
                     (type == QStringLiteral("asset") && assets.contains(v.toString()) == false)))
                {
                    return Fail(error,
                                QStringLiteral("%1 / %2: reference %3 is not in this world or the declared asset list.")
                                    .arg(e.value(QStringLiteral("name")).toString(),
                                         p.value(QStringLiteral("label")).toString(),
                                         v.toString()));
                }
                if (type == QStringLiteral("asset") && v.toString().isEmpty() == false)
                {
                    for (const auto entry : Schema_.value(QStringLiteral("assets")).toArray())
                    {
                        const auto asset = entry.toObject();
                        if (asset.value(QStringLiteral("id")) == v &&
                            asset.value(QStringLiteral("kind")) != p.value(QStringLiteral("asset_kind")))
                        {
                            return Fail(error,
                                        QStringLiteral("%1: choose an asset of kind %2.")
                                            .arg(p.value(QStringLiteral("label")).toString(),
                                                 p.value(QStringLiteral("asset_kind")).toString()));
                        }
                    }
                }
            }
            const qsizetype terrainPayloads =
                HasBuiltInTerrain() == true && c.key() == terrainComponent ? CountTerrainPayloadFields(props) : 0;
            hasAuthoredTerrainPayload = hasAuthoredTerrainPayload == true || terrainPayloads != 0;
            if (recognizedPropertyCount + terrainPayloads != props.size())
            {
                return Fail(error, QStringLiteral("%1 has missing or unknown properties.").arg(c.key()));
            }
            if (isBuiltInLight == true && props.value(QStringLiteral("bakeAlwaysOn")).toBool() == true &&
                (props.value(QStringLiteral("staticTransform")).toBool() == false ||
                 props.value(QStringLiteral("enabled")).toBool() == false))
            {
                return Fail(error,
                            QStringLiteral("Always-on baking requires an enabled light with Static Transform. "
                                           "Clear Bake always-on lighting to disable or move it during play."));
            }
            if (isBuiltInLight == true && props.value(QStringLiteral("bakeAlwaysOn")).toBool() == true &&
                props.value(QStringLiteral("kind")).toString() == QStringLiteral("directional") &&
                props.value(WORLD_CELESTIAL_ROLE).toString() == WORLD_CELESTIAL_DYNAMIC)
            {
                return Fail(error, QStringLiteral("Clear Bake always-on lighting before choosing Dynamic Celestial."));
            }
        }
    }
    if (candidate.contains(QStringLiteral("scene")))
    {
        const QJsonObject scene = candidate.value(QStringLiteral("scene")).toObject();
        const QJsonValue mappings = scene.value(QStringLiteral("material_mappings"));
        constexpr qsizetype SCENE_FIELDS = 6;
        if (candidate.value(QStringLiteral("scene")).isObject() == false ||
            Fields(scene,
                   {QStringLiteral("version"),
                    QStringLiteral("id"),
                    QStringLiteral("name"),
                    QStringLiteral("profile"),
                    QStringLiteral("camera"),
                    QStringLiteral("imports"),
                    QStringLiteral("material_mappings")}) == false ||
            scene.size() != SCENE_FIELDS + (mappings.isUndefined() == true ? 0 : 1) ||
            scene.value(QStringLiteral("version")) != QJsonValue(1) ||
            QUuid(scene.value(QStringLiteral("id")).toString()).isNull() ||
            scene.value(QStringLiteral("profile")) != QJsonValue(QStringLiteral("resident-reference-v1")) ||
            Text(scene.value(QStringLiteral("name")), MAX_LABEL_BYTES) == false ||
            scene.value(QStringLiteral("camera")).isObject() == false ||
            scene.value(QStringLiteral("imports")).isArray() == false)
        {
            return Fail(error, QStringLiteral("Invalid cooked-scene metadata."));
        }
        if (mappings.isUndefined() == false)
        {
            const QJsonObject table = mappings.toObject();
            if (mappings.isObject() == false ||
                table.size() > static_cast<qsizetype>(content::scene_format::IMPORT_LIMITS.Materials))
            {
                return Fail(error, QStringLiteral("Invalid material mapping table."));
            }
            for (QJsonObject::const_iterator entry = table.constBegin(); entry != table.constEnd(); ++entry)
            {
                if (ResourceId(QJsonValue(entry.key()), QStringLiteral("material")) == false ||
                    ResourceId(entry.value(), QStringLiteral("material")) == false ||
                    table.contains(entry.value().toString()) == true)
                {
                    return Fail(error, QStringLiteral("Use distinct material IDs and remove chained mappings."));
                }
            }
        }
        const auto camera = scene.value(QStringLiteral("camera")).toObject();
        if (camera.size() != 4 ||
            Fields(camera,
                   {QStringLiteral("eye"), QStringLiteral("target"), QStringLiteral("up"), QStringLiteral("fov")}) ==
                false)
        {
            return Fail(error, QStringLiteral("Invalid scene camera."));
        }
        foundation::math::Vector3 vectors[3];
        foundation::int32 index = 0;
        for (const auto& key : {QStringLiteral("eye"), QStringLiteral("target"), QStringLiteral("up")})
        {
            const auto values = camera.value(key).toArray();
            if (camera.value(key).isArray() == false || values.size() != VECTOR_COMPONENT_COUNT)
            {
                return Fail(error, QStringLiteral("Invalid camera vector."));
            }
            for (const auto number : values)
            {
                if (number.isDouble() == false || foundation::math::IsFinite(number.toDouble()) == false ||
                    number.toDouble() < -MAX_CAMERA_COORDINATE || number.toDouble() > MAX_CAMERA_COORDINATE)
                {
                    return Fail(error, QStringLiteral("Nonfinite or oversized camera vector."));
                }
            }
            vectors[index++] = {static_cast<foundation::float32>(values[0].toDouble()),
                                static_cast<foundation::float32>(values[1].toDouble()),
                                static_cast<foundation::float32>(values[2].toDouble())};
        }
        const auto fov = camera.value(QStringLiteral("fov"));
        const auto forward = vectors[1] - vectors[0];
        const auto cross = foundation::math::Cross(forward, vectors[2]);
        if (fov.isDouble() == false || foundation::math::IsFinite(fov.toDouble()) == false || fov.toDouble() <= 0 ||
            fov.toDouble() >= 3.141592653589793 || foundation::math::Dot(forward, forward) <= 1e-12F ||
            foundation::math::Dot(cross, cross) <= 1e-12F)
        {
            return Fail(error, QStringLiteral("Degenerate scene camera."));
        }
        const auto imports = scene.value(QStringLiteral("imports")).toArray();
        if (imports.size() > 128)
        {
            return Fail(error, QStringLiteral("Too many scene imports."));
        }
        for (const auto resource : imports)
        {
            if (ResourceId(resource, QStringLiteral("scene")) == false)
            {
                return Fail(error, QStringLiteral("Invalid import provenance."));
            }
        }
    }
    if (candidate.contains(QStringLiteral("disabled_lights")) == true &&
        IsLightSwitchListValid(candidate, error) == false)
    {
        return false;
    }
    for (const auto value : entities)
    {
        const auto entity = value.toObject();
        if (entity.contains(QStringLiteral("scene")) == false)
        {
            continue;
        }
        const QJsonObject scene = entity.value(QStringLiteral("scene")).toObject();
        const bool hasCoating = scene.contains(QStringLiteral("coating"));
        const bool hasProjectBrdf = scene.contains(QStringLiteral("project_brdf"));
        constexpr qsizetype BOUND_PLACEMENT_FIELDS = 4;
        constexpr qsizetype INLINE_PLACEMENT_FIELDS = 7;
        const qsizetype placementFields = Schema_.contains(QStringLiteral("scene_transform")) == true
                                              ? BOUND_PLACEMENT_FIELDS
                                              : INLINE_PLACEMENT_FIELDS;
        if (candidate.contains(QStringLiteral("scene")) == false ||
            entity.value(QStringLiteral("scene")).isObject() == false ||
            Fields(scene,
                   {QStringLiteral("mesh"),
                    QStringLiteral("material"),
                    QStringLiteral("parent"),
                    QStringLiteral("basis"),
                    QStringLiteral("position"),
                    QStringLiteral("rotation"),
                    QStringLiteral("scale"),
                    QStringLiteral("coating"),
                    QStringLiteral("project_brdf")}) == false ||
            scene.size() != placementFields + (hasCoating == true ? 1 : 0) + (hasProjectBrdf == true ? 1 : 0) ||
            ResourceId(scene.value(QStringLiteral("mesh")), QStringLiteral("mesh")) == false ||
            ResourceId(scene.value(QStringLiteral("material")), QStringLiteral("material")) == false ||
            scene.value(QStringLiteral("parent")).isString() == false)
        {
            return Fail(error, QStringLiteral("Invalid scene instance."));
        }
        if (hasCoating == true && ValidateCoating(scene.value(QStringLiteral("coating")), error) == false)
        {
            return false;
        }
        if (hasProjectBrdf == true)
        {
            const QJsonObject binding = scene.value(QStringLiteral("project_brdf")).toObject();
            const QJsonValue roughness = binding.value(QStringLiteral("roughness"));
            if (binding.size() != (roughness.isUndefined() == true ? 1 : 2) ||
                ResourceId(binding.value(QStringLiteral("material")), QStringLiteral("material")) == false ||
                (roughness.isUndefined() == false &&
                 Value(MakeProjectBrdfRoughnessProperty(), roughness, error) == false))
            {
                return Fail(error, QStringLiteral("Assign a project BRDF and enter a valid roughness."));
            }
        }
        for (const auto& key :
             {QStringLiteral("basis"), QStringLiteral("position"), QStringLiteral("rotation"), QStringLiteral("scale")})
        {
            const auto placement = ScenePlacement(entity);
            const auto numbers = placement.value(key).toArray();
            if (placement.value(key).isArray() == false ||
                numbers.size() != (key == QStringLiteral("basis") ? BASIS_ELEMENT_COUNT : VECTOR_COMPONENT_COUNT))
            {
                return Fail(error, QStringLiteral("Invalid instance placement."));
            }
            for (const auto number : numbers)
            {
                const auto n = number.toDouble();
                if (number.isDouble() == false || foundation::math::IsFinite(n) == false || n < -MAX_PLACEMENT_VALUE ||
                    n > MAX_PLACEMENT_VALUE || (key == QStringLiteral("scale") && n <= 0) ||
                    (key == QStringLiteral("rotation") && (n < -360 || n > 360)))
                {
                    return Fail(error, QStringLiteral("Instance placement is outside its finite bounds."));
                }
            }
        }
        auto parent = scene.value(QStringLiteral("parent")).toString();
        QSet<QString> seen{entity.value(QStringLiteral("id")).toString()};
        while (parent.isEmpty() == false)
        {
            QJsonObject ancestor;
            for (const auto other : entities)
            {
                if (other.toObject().value(QStringLiteral("id")) == QJsonValue(parent))
                {
                    ancestor = other.toObject();
                    break;
                }
            }
            if (seen.contains(parent) || ancestor.value(QStringLiteral("scene")).isObject() == false)
            {
                return Fail(error, QStringLiteral("Scene parent is missing or forms a cycle."));
            }
            seen.insert(parent);
            parent = ancestor.value(QStringLiteral("scene")).toObject().value(QStringLiteral("parent")).toString();
        }
    }
    // A single heightfield fits the per-array limit. Many embedded grids can still
    // exceed the aggregate parser budget, so reject before committing an unreadable World.
    if (hasAuthoredTerrainPayload == true && ValidateEncodedObject(encoded, MAX_BYTES, error) == false)
    {
        return Fail(error,
                    QStringLiteral("This World exceeds its terrain document budget. Use smaller heightmaps "
                                   "or split the terrain between Worlds before retrying."));
    }
    error.clear();
    return true;
}
bool WorldDocument::Load(const QByteArray& bytes, QString& error)
{
    QJsonObject candidate;
    if (Decode(bytes, MAX_BYTES, candidate, error) == false || Validate(candidate, error) == false)
    {
        return false;
    }
    Draft_ = Saved_ = candidate;
    History_.Reset();
    Loaded_ = true;
    return true;
}
bool WorldDocument::New(QString& error)
{
    QJsonObject candidate{{QStringLiteral("version"), 1},
                          {QStringLiteral("schema"), Schema_.value(QStringLiteral("id"))},
                          {QStringLiteral("entities"), QJsonArray{}}};
    if (Validate(candidate, error) == false)
    {
        return false;
    }
    Draft_ = Saved_ = candidate;
    History_.Reset();
    Loaded_ = true;
    return true;
}
void WorldDocument::Clear()
{
    Draft_ = {};
    Saved_ = {};
    Schema_ = {};
    mIsBuiltInSchema = false;
    Loaded_ = false;
    History_.Reset();
}
bool WorldDocument::Commit(const QJsonObject& candidate, QString& error)
{
    if (Loaded_ == false || Validate(candidate, error) == false)
    {
        return false;
    }
    // A document keeps the version and AssetId it was opened with; nothing converts it implicitly.
    if (candidate.value(QStringLiteral("version")) != Draft_.value(QStringLiteral("version")) ||
        candidate.value(QStringLiteral("document")) != Draft_.value(QStringLiteral("document")))
    {
        return Fail(error, QStringLiteral("The candidate changes the World's version or document identity."));
    }
    if (candidate == Draft_)
    {
        error.clear();
        return true;
    }
    if (History_.Commit(Draft_, candidate) == false)
    {
        return Fail(error, QStringLiteral("History revision exhausted; save and reopen."));
    }
    Draft_ = candidate;
    return true;
}
bool WorldDocument::ReplaceEntity(const QJsonObject& entity, QString& error)
{
    auto entities = Draft_.value(QStringLiteral("entities")).toArray();
    for (foundation::int32 i = 0; i < entities.size(); ++i)
    {
        if (GetEntityKey(entities[i].toObject()) == GetEntityKey(entity))
        {
            entities[i] = entity;
            auto candidate = Draft_;
            candidate.insert(QStringLiteral("entities"), entities);
            return Commit(candidate, error);
        }
    }
    return Fail(error, QStringLiteral("Entity no longer exists."));
}
bool WorldDocument::CreateEntity(const foundation::identity::EntropySource& entropy,
                                 QString& outError,
                                 const QString& name,
                                 QString& outId,
                                 const QJsonObject& initialComponents)
{
    const bool hasUuidRecords = HasUuidRecords();
    const bool isWorldDemo = Schema_.value(QStringLiteral("id")) == QJsonValue(QStringLiteral("ludus.world-demo.v1"));
    QString fresh;
    if (hasUuidRecords == true)
    {
        if (TryMintObjectUuid(entropy, outError, Draft_, fresh) == false)
        {
            return false;
        }
    }
    else if (TryGenerateEntityId(entropy, isWorldDemo, fresh) == false)
    {
        return Fail(outError, DescribeEntropyUnavailable());
    }
    QJsonObject components;
    for (const auto value : Schema_.value(QStringLiteral("components")).toArray())
    {
        const auto c = value.toObject();
        if (c.value(QStringLiteral("required")).toBool())
        {
            components.insert(c.value(QStringLiteral("id")).toString(), Defaults(c));
        }
    }
    for (QJsonObject::const_iterator entry = initialComponents.constBegin(); entry != initialComponents.constEnd();
         ++entry)
    {
        components.insert(entry.key(), entry.value());
    }
    auto candidate = Draft_;
    auto entities = candidate.value(QStringLiteral("entities")).toArray();
    QJsonObject entity{{QStringLiteral("name"), name}, {QStringLiteral("components"), components}};
    entity.insert(GetKeyMember(hasUuidRecords), fresh);
    if (hasUuidRecords == true)
    {
        entity.insert(QStringLiteral("label"), MakeUniqueLabel(Draft_, name));
    }
    entities.append(entity);
    candidate.insert(QStringLiteral("entities"), entities);
    if (Commit(candidate, outError) == false)
    {
        return false;
    }
    outId = foundation::Move(fresh);
    return true;
}
bool WorldDocument::DuplicateEntity(const foundation::identity::EntropySource& entropy,
                                    QString& outError,
                                    const QString& source,
                                    QString& outId)
{
    auto entity = Entity(source);
    if (entity.isEmpty())
    {
        return Fail(outError, QStringLiteral("Select an entity to duplicate."));
    }
    const bool hasUuidRecords = HasUuidRecords();
    const bool isWorldDemo = Schema_.value(QStringLiteral("id")) == QJsonValue(QStringLiteral("ludus.world-demo.v1"));
    QString fresh;
    if (hasUuidRecords == true)
    {
        if (TryMintObjectUuid(entropy, outError, Draft_, fresh) == false)
        {
            return false;
        }
    }
    else if (TryGenerateEntityId(entropy, isWorldDemo, fresh) == false)
    {
        return Fail(outError, DescribeEntropyUnavailable());
    }
    auto components = entity.value(QStringLiteral("components")).toObject();
    for (auto c = components.begin(); c != components.end(); ++c)
    {
        auto props = c.value().toObject();
        for (const auto value : Component(c.key()).value(QStringLiteral("properties")).toArray())
        {
            const auto p = value.toObject();
            const auto key = p.value(QStringLiteral("id")).toString();
            if (p.value(QStringLiteral("type")) == QJsonValue(QStringLiteral("entity")) &&
                props.value(key) == QJsonValue(source))
            {
                props.insert(key, fresh);
            }
        }
        c.value() = props;
    }
    entity.insert(GetKeyMember(hasUuidRecords), fresh);
    auto copyName = entity.value(QStringLiteral("name")).toString();
    while (copyName.toUtf8().size() > 123)
    {
        copyName.chop(copyName.back().isLowSurrogate() ? 2 : 1);
    }
    entity.insert(QStringLiteral("name"), copyName + QStringLiteral(" copy"));
    if (hasUuidRecords == true)
    {
        entity.insert(QStringLiteral("label"), MakeUniqueLabel(Draft_, copyName + QStringLiteral(" copy")));
    }
    entity.insert(QStringLiteral("components"), components);
    auto candidate = Draft_;
    auto entities = candidate.value(QStringLiteral("entities")).toArray();
    entities.append(entity);
    candidate.insert(QStringLiteral("entities"), entities);
    if (Commit(candidate, outError) == false)
    {
        return false;
    }
    outId = foundation::Move(fresh);
    return true;
}
bool WorldDocument::DuplicateEntities(const foundation::identity::EntropySource& entropy,
                                      const QStringList& sources,
                                      QStringList& outIds,
                                      QString& outError)
{
    QStringList uniqueSources = sources;
    uniqueSources.removeDuplicates();
    if (uniqueSources.isEmpty() == true)
    {
        return Fail(outError, QStringLiteral("Select entities to duplicate."));
    }
    WorldDocument candidate = *this;
    QStringList copies;
    for (const QString& source : uniqueSources)
    {
        QString copy;
        if (candidate.DuplicateEntity(entropy, outError, source, copy) == false)
        {
            return false;
        }
        copies.append(copy);
    }
    QJsonObject draft = candidate.Draft();
    QJsonArray entities = draft.value(QStringLiteral("entities")).toArray();
    for (foundation::int32 index = 0; index < entities.size(); ++index)
    {
        QJsonObject entity = entities[index].toObject();
        if (copies.contains(GetEntityKey(entity)) == false)
        {
            continue;
        }
        QJsonObject components = entity.value(QStringLiteral("components")).toObject();
        for (QJsonObject::iterator component = components.begin(); component != components.end(); ++component)
        {
            QJsonObject properties = component.value().toObject();
            for (const QJsonValue value : Component(component.key()).value(QStringLiteral("properties")).toArray())
            {
                const QJsonObject property = value.toObject();
                const QString key = property.value(QStringLiteral("id")).toString();
                const foundation::isize sourceIndex = uniqueSources.indexOf(properties.value(key).toString());
                if (property.value(QStringLiteral("type")) == QJsonValue(QStringLiteral("entity")) && sourceIndex >= 0)
                {
                    properties.insert(key, copies[sourceIndex]);
                }
            }
            component.value() = properties;
        }
        entity.insert(QStringLiteral("components"), components);
        QJsonObject scene = entity.value(QStringLiteral("scene")).toObject();
        const foundation::isize parentIndex = uniqueSources.indexOf(scene.value(QStringLiteral("parent")).toString());
        if (scene.isEmpty() == false && parentIndex >= 0)
        {
            scene.insert(QStringLiteral("parent"), copies[parentIndex]);
            entity.insert(QStringLiteral("scene"), scene);
        }
        entities[index] = entity;
    }
    draft.insert(QStringLiteral("entities"), entities);
    if (Commit(draft, outError) == false)
    {
        return false;
    }
    outIds = copies;
    return true;
}
bool WorldDocument::RemoveEntities(const QStringList& ids, QString& outError)
{
    if (ids.isEmpty() == true)
    {
        return Fail(outError, QStringLiteral("Select entities to delete."));
    }
    for (const QString& id : ids)
    {
        if (Entity(id).isEmpty() == true)
        {
            return Fail(outError, QStringLiteral("Entity no longer exists."));
        }
    }
    QJsonObject candidate = Draft_;
    QJsonArray entities = candidate.value(QStringLiteral("entities")).toArray();
    for (foundation::int32 index = static_cast<foundation::int32>(entities.size()); index > 0; --index)
    {
        if (ids.contains(GetEntityKey(entities[index - 1].toObject())) == true)
        {
            entities.removeAt(index - 1);
        }
    }
    candidate.insert(QStringLiteral("entities"), entities);
    return Commit(candidate, outError);
}
bool WorldDocument::RemoveEntity(const QString& id, QString& error)
{
    auto entities = Draft_.value(QStringLiteral("entities")).toArray();
    for (foundation::int32 i = 0; i < entities.size(); ++i)
    {
        if (GetEntityKey(entities[i].toObject()) == id)
        {
            entities.removeAt(i);
            auto candidate = Draft_;
            candidate.insert(QStringLiteral("entities"), entities);
            return Commit(candidate, error);
        }
    }
    return Fail(error, QStringLiteral("Select an entity to delete."));
}
bool WorldDocument::RenameEntity(const QString& id, QString& error, const QString& name)
{
    auto e = Entity(id);
    if (e.isEmpty())
    {
        return Fail(error, QStringLiteral("Entity no longer exists."));
    }
    e.insert(QStringLiteral("name"), name);
    return ReplaceEntity(e, error);
}
bool WorldDocument::Attach(const QString& entity, QString& error, const QString& component)
{
    auto e = Entity(entity);
    const auto definition = Component(component);
    auto components = e.value(QStringLiteral("components")).toObject();
    if (e.isEmpty() || definition.isEmpty() || components.contains(component))
    {
        return Fail(error, QStringLiteral("Choose an available component and entity."));
    }
    components.insert(component, Defaults(definition));
    e.insert(QStringLiteral("components"), components);
    return ReplaceEntity(e, error);
}
bool WorldDocument::Detach(const QString& entity, QString& error, const QString& component)
{
    auto e = Entity(entity);
    auto components = e.value(QStringLiteral("components")).toObject();
    if (e.isEmpty() || components.contains(component) == false)
    {
        return Fail(error, QStringLiteral("Component no longer exists."));
    }
    components.remove(component);
    e.insert(QStringLiteral("components"), components);
    return ReplaceEntity(e, error);
}
bool WorldDocument::Edit(const QString& entity,
                         const QJsonObject& components,
                         const QString& name,
                         QString& error,
                         const QJsonObject* scene)
{
    auto e = Entity(entity);
    if (e.isEmpty())
    {
        return Fail(error, QStringLiteral("Entity no longer exists."));
    }
    e.insert(QStringLiteral("name"), name);
    e.insert(QStringLiteral("components"), components);
    if (scene != nullptr)
    {
        e.insert(QStringLiteral("scene"), *scene);
    }
    return ReplaceEntity(e, error);
}
bool WorldDocument::ImportScene(const QJsonObject& level, QString& error)
{
    if (HasUuidRecords() == true)
    {
        return Fail(error, QStringLiteral("Version-2 World records do not support the scene extension yet."));
    }
    if (level.value(QStringLiteral("objects")).isArray() == false)
    {
        return Fail(error, QStringLiteral("Missing imported instances."));
    }
    auto candidate = Draft_;
    auto entities = candidate.value(QStringLiteral("entities")).toArray();
    for (const auto value : level.value(QStringLiteral("objects")).toArray())
    {
        const auto object = value.toObject();
        const auto id = object.value(QStringLiteral("id")).toString();
        if (Entity(id).isEmpty() == false)
        {
            if (Entity(id).value(QStringLiteral("scene")).isObject() == false)
            {
                return Fail(error, QStringLiteral("An imported ID collides with an existing gameplay entity."));
            }
            continue; // Existing entities and all their edits remain authoritative.
        }
        const auto transform = object.value(QStringLiteral("transform")).toArray();
        if (transform.size() != 12)
        {
            return Fail(error, QStringLiteral("Invalid imported affine transform."));
        }
        QJsonArray basis, position;
        for (foundation::int32 i = 0; i < 12; ++i)
        {
            if (i < 9)
            {
                basis.append(transform[i]);
            }
            else
            {
                position.append(transform[i]);
            }
        }
        QJsonObject components;
        for (const auto definition : Schema_.value(QStringLiteral("components")).toArray())
        {
            const auto c = definition.toObject();
            if (c.value(QStringLiteral("required")).toBool())
            {
                components.insert(c.value(QStringLiteral("id")).toString(), Defaults(c));
            }
        }
        QJsonObject instance{{QStringLiteral("mesh"), object.value(QStringLiteral("mesh"))},
                             {QStringLiteral("material"), object.value(QStringLiteral("material"))},
                             {QStringLiteral("parent"), object.value(QStringLiteral("parent"))},
                             {QStringLiteral("basis"), basis}};
        if (object.contains(QStringLiteral("coating")) == true)
        {
            instance.insert(QStringLiteral("coating"), object.value(QStringLiteral("coating")));
        }
        if (object.contains(QStringLiteral("project_brdf")) == true)
        {
            instance.insert(QStringLiteral("project_brdf"), object.value(QStringLiteral("project_brdf")));
        }
        const auto binding = Schema_.value(QStringLiteral("scene_transform")).toObject();
        if (binding.isEmpty())
        {
            instance.insert(QStringLiteral("position"), position);
            instance.insert(QStringLiteral("rotation"), QJsonArray{0, 0, 0});
            instance.insert(QStringLiteral("scale"), QJsonArray{1, 1, 1});
        }
        else
        {
            const auto component = binding.value(QStringLiteral("component")).toString();
            auto values = components.value(component).toObject();
            for (const auto& key : {QStringLiteral("position"), QStringLiteral("rotation"), QStringLiteral("scale")})
            {
                const auto axes = binding.value(key).toArray();
                for (foundation::int32 axis = 0; axis < 3; ++axis)
                {
                    values.insert(axes[axis].toString(),
                                  key == QStringLiteral("position")
                                      ? position[axis]
                                      : QJsonValue(key == QStringLiteral("scale") ? 1 : 0));
                }
            }
            components.insert(component, values);
        }
        entities.append(QJsonObject{{QStringLiteral("id"), id},
                                    {QStringLiteral("name"), object.value(QStringLiteral("name"))},
                                    {QStringLiteral("components"), components},
                                    {QStringLiteral("scene"), instance}});
    }
    auto metadata = level;
    metadata.remove(QStringLiteral("objects"));
    const QJsonValue sharedCoatings = metadata.value(QStringLiteral("shared_coatings"));
    metadata.remove(QStringLiteral("shared_coatings"));
    if (sharedCoatings.isUndefined() == false)
    {
        candidate.insert(QStringLiteral("shared_coatings"), sharedCoatings);
    }
    candidate.insert(QStringLiteral("scene"), metadata);
    candidate.insert(QStringLiteral("entities"), entities);
    return Commit(candidate, error);
}
bool WorldDocument::MapSceneMaterial(const QString& source, const QString& replacement, QString& outError)
{
    QJsonObject scene = Draft_.value(QStringLiteral("scene")).toObject();
    if (scene.isEmpty() == true)
    {
        return Fail(outError, QStringLiteral("Import a scene before mapping a material."));
    }
    QJsonObject mappings = scene.value(QStringLiteral("material_mappings")).toObject();
    mappings.insert(source, replacement);
    scene.insert(QStringLiteral("material_mappings"), mappings);
    QJsonObject candidate = Draft_;
    candidate.insert(QStringLiteral("scene"), scene);
    return Commit(candidate, outError);
}

QString WorldDocument::GetInheritedMaterialId(const QJsonObject& placement) const
{
    const QString source = placement.value(QStringLiteral("material")).toString();
    const QJsonObject mappings =
        Draft_.value(QStringLiteral("scene")).toObject().value(QStringLiteral("material_mappings")).toObject();
    return mappings.value(source).toString(source);
}

QJsonObject WorldDocument::GetSharedCoating(const QString& material) const
{
    return Draft_.value(QStringLiteral("shared_coatings")).toObject().value(material).toObject();
}

bool WorldDocument::SetSharedCoating(const QString& material, const QJsonObject& coating, QString& outError)
{
    QJsonObject candidate = Draft_;
    QJsonObject instances = candidate.value(QStringLiteral("shared_coatings")).toObject();
    instances.insert(material, coating);
    candidate.insert(QStringLiteral("shared_coatings"), instances);
    return Commit(candidate, outError);
}

bool WorldDocument::SetSceneStartView(const QJsonObject& camera, QString& outError)
{
    QJsonObject scene = Draft_.value(QStringLiteral("scene")).toObject();
    if (scene.isEmpty() == true)
    {
        return Fail(outError, QStringLiteral("This World has no imported scene to start from."));
    }
    scene.insert(QStringLiteral("camera"), camera);
    QJsonObject candidate = Draft_;
    candidate.insert(QStringLiteral("scene"), scene);
    return Commit(candidate, outError);
}
LightSwitchSet WorldDocument::GetLightSwitches() const
{
    LightSwitchSet switches;
    for (const QJsonValue entry : Draft_.value(QStringLiteral("disabled_lights")).toArray())
    {
        LightSourceKey key;
        // Validation admitted every entry; an unreadable one cannot occur in a loaded draft.
        if (TryReadLightSwitch(entry, key) == true)
        {
            switches.Disabled.append(key);
        }
    }
    NormalizeLightSwitches(switches);
    return switches;
}
bool WorldDocument::SetLightSwitches(const LightSwitchSet& switches,
                                     const QJsonObject& lightBinding,
                                     const QVector<WorldLightEnable>& worldLights,
                                     QString& outError)
{
    LightSwitchSet normalized = switches;
    NormalizeLightSwitches(normalized);
    if (normalized.Disabled.isEmpty() == false && Draft_.contains(QStringLiteral("scene")) == false)
    {
        return Fail(outError, QStringLiteral("This World has no imported scene whose lights could be switched."));
    }
    QJsonObject candidate = Draft_;
    if (worldLights.isEmpty() == false)
    {
        const QString component = lightBinding.value(QStringLiteral("component")).toString();
        const QString property =
            lightBinding.value(QStringLiteral("properties")).toObject().value(QStringLiteral("enabled")).toString();
        if (component.isEmpty() == true || property.isEmpty() == true)
        {
            return Fail(outError, QStringLiteral("This World binds no light component to switch."));
        }
        QJsonArray entities = candidate.value(QStringLiteral("entities")).toArray();
        for (const WorldLightEnable& light : worldLights)
        {
            qsizetype found = -1;
            for (qsizetype i = 0; i < entities.size(); ++i)
            {
                if (GetEntityKey(entities[i].toObject()) == light.EntityKey)
                {
                    found = i;
                    break;
                }
            }
            QJsonObject entity = found >= 0 ? entities[found].toObject() : QJsonObject{};
            QJsonObject components = entity.value(QStringLiteral("components")).toObject();
            if (found < 0 || components.value(component).isObject() == false)
            {
                return Fail(outError, QStringLiteral("A switched light entity no longer exists. Nothing was changed."));
            }
            QJsonObject values = components.value(component).toObject();
            values.insert(property, light.IsEnabled);
            components.insert(component, values);
            entity.insert(QStringLiteral("components"), components);
            entities[found] = entity;
        }
        candidate.insert(QStringLiteral("entities"), entities);
    }
    if (normalized.Disabled.isEmpty() == true)
    {
        candidate.remove(QStringLiteral("disabled_lights"));
    }
    else
    {
        QJsonArray entries;
        for (const LightSourceKey& key : normalized.Disabled)
        {
            entries.append(QJsonObject{{QStringLiteral("kind"), GetLightSourceKindName(key.Kind)},
                                       {QStringLiteral("source"), FormatLightSourceId(key.SourceId)}});
        }
        candidate.insert(QStringLiteral("disabled_lights"), entries);
    }
    return Commit(candidate, outError);
}
QJsonObject WorldDocument::ScenePlacement(const QJsonObject& entity) const
{
    auto scene = entity.value(QStringLiteral("scene")).toObject();
    const auto binding = Schema_.value(QStringLiteral("scene_transform")).toObject();
    if (binding.isEmpty() == false && scene.isEmpty() == false)
    {
        const auto values = entity.value(QStringLiteral("components"))
                                .toObject()
                                .value(binding.value(QStringLiteral("component")).toString())
                                .toObject();
        for (const auto& key : {QStringLiteral("position"), QStringLiteral("rotation"), QStringLiteral("scale")})
        {
            QJsonArray axes;
            for (const auto property : binding.value(key).toArray())
            {
                axes.append(values.value(property.toString()));
            }
            scene.insert(key, axes);
        }
    }
    return scene;
}
QJsonObject WorldDocument::MakeDraftWithoutScenePlacements() const
{
    const QJsonObject binding = Schema_.value(QStringLiteral("scene_transform")).toObject();
    const QString component = binding.value(QStringLiteral("component")).toString();
    QJsonArray entities = Draft_.value(QStringLiteral("entities")).toArray();
    for (qsizetype i = 0; i < entities.size(); ++i)
    {
        QJsonObject entity = entities[i].toObject();
        QJsonObject scene = entity.value(QStringLiteral("scene")).toObject();
        if (scene.isEmpty() == true)
        {
            continue;
        }
        QJsonObject components = entity.value(QStringLiteral("components")).toObject();
        QJsonObject values = components.value(component).toObject();
        for (const QString& key : {QStringLiteral("position"), QStringLiteral("rotation"), QStringLiteral("scale")})
        {
            scene.remove(key);
            for (const QJsonValue property : binding.value(key).toArray())
            {
                values.remove(property.toString());
            }
        }
        entity.insert(QStringLiteral("scene"), scene);
        if (components.contains(component) == true)
        {
            components.insert(component, values);
            entity.insert(QStringLiteral("components"), components);
        }
        entities[i] = entity;
    }
    QJsonObject draft = Draft_;
    draft.insert(QStringLiteral("entities"), entities);
    return draft;
}
foundation::math::Affine3 WorldDocument::MakeSceneMeshToParent(const QJsonObject& placement) noexcept
{
    namespace math = foundation::math;
    const QJsonArray basis = placement.value(QStringLiteral("basis")).toArray();
    const QJsonArray position = placement.value(QStringLiteral("position")).toArray();
    const QJsonArray rotation = placement.value(QStringLiteral("rotation")).toArray();
    const QJsonArray scale = placement.value(QStringLiteral("scale")).toArray();
    const auto read = [](const QJsonArray& values, qsizetype index) noexcept {
        return static_cast<foundation::float32>(values[index].toDouble());
    };
    constexpr qsizetype AXIS_COUNT = 3;
    math::Affine3 importedMeshToParent;
    for (qsizetype column = 0; column < AXIS_COUNT; ++column)
    {
        const qsizetype first = column * AXIS_COUNT;
        importedMeshToParent.Columns[column] = {read(basis, first), read(basis, first + 1), read(basis, first + 2)};
    }
    math::Quaternion rotationX;
    math::Quaternion rotationY;
    math::Quaternion rotationZ;
    static_cast<void>(math::TryFromAxisAngle({1, 0, 0}, math::DegreesToRadians(read(rotation, 0)), rotationX));
    static_cast<void>(math::TryFromAxisAngle({0, 1, 0}, math::DegreesToRadians(read(rotation, 1)), rotationY));
    static_cast<void>(math::TryFromAxisAngle({0, 0, 1}, math::DegreesToRadians(read(rotation, 2)), rotationZ));
    math::TransformTRS adjustmentMesh;
    static_cast<void>(math::TryNormalize(rotationZ * rotationY * rotationX, adjustmentMesh.Rotation));
    adjustmentMesh.Scale = {read(scale, 0), read(scale, 1), read(scale, 2)};
    math::Affine3 meshToParent = math::Compose(importedMeshToParent, math::ToAffine3(adjustmentMesh));
    meshToParent.Translation = {read(position, 0), read(position, 1), read(position, 2)};
    return meshToParent;
}
QJsonObject WorldDocument::SceneLevel() const
{
    auto level = Draft_.value(QStringLiteral("scene")).toObject();
    if (level.isEmpty())
    {
        return {};
    }
    namespace math = foundation::math;
    QJsonArray objects;
    for (const auto value : Draft_.value(QStringLiteral("entities")).toArray())
    {
        const auto entity = value.toObject();
        const auto scene = ScenePlacement(entity);
        if (scene.isEmpty())
        {
            continue;
        }
        const auto position = scene.value(QStringLiteral("position")).toArray();
        const math::Affine3 affine = MakeSceneMeshToParent(scene);
        QJsonArray transform;
        for (const auto column : affine.Columns)
        {
            transform.append(static_cast<foundation::float64>(column.X));
            transform.append(static_cast<foundation::float64>(column.Y));
            transform.append(static_cast<foundation::float64>(column.Z));
        }
        for (const auto number : position)
        {
            transform.append(number);
        }
        QJsonObject object{{QStringLiteral("id"), entity.value(QStringLiteral("id"))},
                           {QStringLiteral("name"), entity.value(QStringLiteral("name"))},
                           {QStringLiteral("parent"), scene.value(QStringLiteral("parent"))},
                           {QStringLiteral("mesh"), scene.value(QStringLiteral("mesh"))},
                           {QStringLiteral("material"), scene.value(QStringLiteral("material"))},
                           {QStringLiteral("transform"), transform}};
        if (scene.contains(QStringLiteral("coating")) == true)
        {
            object.insert(QStringLiteral("coating"), scene.value(QStringLiteral("coating")));
        }
        if (scene.contains(QStringLiteral("project_brdf")) == true)
        {
            object.insert(QStringLiteral("project_brdf"), scene.value(QStringLiteral("project_brdf")));
        }
        objects.append(object);
    }
    level.insert(QStringLiteral("objects"), objects);
    if (Draft_.contains(QStringLiteral("shared_coatings")) == true)
    {
        level.insert(QStringLiteral("shared_coatings"), Draft_.value(QStringLiteral("shared_coatings")));
    }
    return level;
}
QByteArray WorldDocument::Encode() const
{
    return QJsonDocument(Draft_).toJson(QJsonDocument::Compact) + '\n';
}
void WorldDocument::AcknowledgeSave()
{
    Saved_ = Draft_;
    History_.BreakGroup();
}
void WorldDocument::Discard()
{
    Draft_ = Saved_;
    History_.Reset();
}
bool WorldDocument::ParseValue(const QJsonObject& p, const QString& text, QJsonValue& value, QString& error)
{
    const auto type = p.value(QStringLiteral("type")).toString();
    QJsonValue candidate = text;
    if (type == QStringLiteral("number"))
    {
        const auto bytes = text.toLatin1();
        foundation::float32 n = 0;
        if (foundation::ParseFloat32(bytes.constData(), static_cast<foundation::usize>(bytes.size()), n) !=
            foundation::NumberParseStatus::Success)
        {
            return Fail(
                error,
                QStringLiteral("%1: enter a complete finite number.").arg(p.value(QStringLiteral("label")).toString()));
        }
        candidate = static_cast<foundation::float64>(n);
    }
    else if (type == QStringLiteral("integer"))
    {
        const auto bytes = text.toLatin1();
        const bool negative = bytes.startsWith('-');
        foundation::uint64 n = 0;
        const auto offset = negative ? 1 : 0;
        if (foundation::ParseUint64(bytes.constData() + offset,
                                    static_cast<foundation::usize>(bytes.size() - offset),
                                    n) != foundation::NumberParseStatus::Success ||
            n > 2147483647u)
        {
            return Fail(error,
                        QStringLiteral("%1: enter a signed integer within its bounds.")
                            .arg(p.value(QStringLiteral("label")).toString()));
        }
        const auto signedValue = static_cast<foundation::int32>(n);
        candidate = negative ? -signedValue : signedValue;
    }
    else if (type == QStringLiteral("boolean"))
    {
        if (text != QStringLiteral("true") && text != QStringLiteral("false"))
        {
            return Fail(error, QStringLiteral("Choose true or false."));
        }
        candidate = text == QStringLiteral("true");
    }
    if (Value(p, candidate, error) == false)
    {
        return false;
    }
    value = foundation::Move(candidate);
    return true;
}
} // namespace ludus::editor
