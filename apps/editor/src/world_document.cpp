#include "internal/world_document.h"

#include "internal/editor_identity.h"

#include <ludus/content/identity_records/migration_manifest.h>
#include <ludus/content/identity_records/world_records.h>
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
bool Decode(const QByteArray& bytes, foundation::int64 limit, QJsonObject& out, QString& error)
{
    namespace parsing = foundation::parsing;
    if (bytes.isEmpty() || bytes.size() > limit)
    {
        return Fail(error, QStringLiteral("Document exceeds its byte limit or is empty."));
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
        return Fail(error, QStringLiteral("Invalid JSON, duplicate keys, or document limits exceeded."));
    }
    out = QJsonDocument::fromJson(bytes).object();
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
    const auto components = schema.value(QStringLiteral("components")).toArray();
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
                                                                        QStringLiteral("scene")};
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
    if (entities.size() > MAX_ENTITIES || QJsonDocument(candidate).toJson(QJsonDocument::Compact).size() + 1 > MaxBytes)
    {
        return Fail(error,
                    QStringLiteral("World supports at most %1 entities and %2 KiB.")
                        .arg(MAX_ENTITIES)
                        .arg(MaxBytes / BYTES_PER_KIBIBYTE));
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
    for (const auto value : entities)
    {
        const auto e = value.toObject();
        const auto components = e.value(QStringLiteral("components")).toObject();
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
            if (props.size() != definitions.size())
            {
                return Fail(error, QStringLiteral("%1 has missing or unknown properties.").arg(c.key()));
            }
            for (const auto field : definitions)
            {
                const auto p = field.toObject();
                const auto v = props.value(p.value(QStringLiteral("id")).toString());
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
        }
    }
    if (candidate.contains(QStringLiteral("scene")))
    {
        const auto scene = candidate.value(QStringLiteral("scene")).toObject();
        if (candidate.value(QStringLiteral("scene")).isObject() == false ||
            Fields(scene,
                   {QStringLiteral("version"),
                    QStringLiteral("id"),
                    QStringLiteral("name"),
                    QStringLiteral("profile"),
                    QStringLiteral("camera"),
                    QStringLiteral("imports")}) == false ||
            scene.size() != 6 || scene.value(QStringLiteral("version")) != QJsonValue(1) ||
            QUuid(scene.value(QStringLiteral("id")).toString()).isNull() ||
            scene.value(QStringLiteral("profile")) != QJsonValue(QStringLiteral("resident-reference-v1")) ||
            Text(scene.value(QStringLiteral("name")), MAX_LABEL_BYTES) == false ||
            scene.value(QStringLiteral("camera")).isObject() == false ||
            scene.value(QStringLiteral("imports")).isArray() == false)
        {
            return Fail(error, QStringLiteral("Invalid cooked-scene metadata."));
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
    for (const auto value : entities)
    {
        const auto entity = value.toObject();
        if (entity.contains(QStringLiteral("scene")) == false)
        {
            continue;
        }
        const auto scene = entity.value(QStringLiteral("scene")).toObject();
        if (candidate.contains(QStringLiteral("scene")) == false ||
            entity.value(QStringLiteral("scene")).isObject() == false ||
            Fields(scene,
                   {QStringLiteral("mesh"),
                    QStringLiteral("material"),
                    QStringLiteral("parent"),
                    QStringLiteral("basis"),
                    QStringLiteral("position"),
                    QStringLiteral("rotation"),
                    QStringLiteral("scale")}) == false ||
            scene.size() != (Schema_.contains(QStringLiteral("scene_transform")) ? 4 : 7) ||
            ResourceId(scene.value(QStringLiteral("mesh")), QStringLiteral("mesh")) == false ||
            ResourceId(scene.value(QStringLiteral("material")), QStringLiteral("material")) == false ||
            scene.value(QStringLiteral("parent")).isString() == false)
        {
            return Fail(error, QStringLiteral("Invalid scene instance."));
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
    error.clear();
    return true;
}
bool WorldDocument::Load(const QByteArray& bytes, QString& error)
{
    QJsonObject candidate;
    if (Decode(bytes, MaxBytes, candidate, error) == false || Validate(candidate, error) == false)
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
    candidate.insert(QStringLiteral("scene"), metadata);
    candidate.insert(QStringLiteral("entities"), entities);
    return Commit(candidate, error);
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
        const auto basis = scene.value(QStringLiteral("basis")).toArray();
        const auto position = scene.value(QStringLiteral("position")).toArray();
        const auto rotation = scene.value(QStringLiteral("rotation")).toArray();
        const auto scale = scene.value(QStringLiteral("scale")).toArray();
        math::Affine3 base;
        for (foundation::int32 column = 0; column < 3; ++column)
        {
            base.Columns[column] = {
                static_cast<foundation::float32>(basis[static_cast<foundation::isize>(column) * 3].toDouble()),
                static_cast<foundation::float32>(basis[column * 3 + 1].toDouble()),
                static_cast<foundation::float32>(basis[column * 3 + 2].toDouble())};
        }
        math::Quaternion qx, qy, qz;
        constexpr foundation::float32 radians = 0.017453292519943295F;
        (void)math::TryFromAxisAngle({1, 0, 0}, static_cast<foundation::float32>(rotation[0].toDouble()) * radians, qx);
        (void)math::TryFromAxisAngle({0, 1, 0}, static_cast<foundation::float32>(rotation[1].toDouble()) * radians, qy);
        (void)math::TryFromAxisAngle({0, 0, 1}, static_cast<foundation::float32>(rotation[2].toDouble()) * radians, qz);
        math::TransformTRS adjustment;
        (void)math::TryNormalize(qz * qy * qx, adjustment.Rotation);
        adjustment.Scale = {static_cast<foundation::float32>(scale[0].toDouble()),
                            static_cast<foundation::float32>(scale[1].toDouble()),
                            static_cast<foundation::float32>(scale[2].toDouble())};
        const auto affine = math::Compose(base, math::ToAffine3(adjustment));
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
        objects.append(QJsonObject{{QStringLiteral("id"), entity.value(QStringLiteral("id"))},
                                   {QStringLiteral("name"), entity.value(QStringLiteral("name"))},
                                   {QStringLiteral("parent"), scene.value(QStringLiteral("parent"))},
                                   {QStringLiteral("mesh"), scene.value(QStringLiteral("mesh"))},
                                   {QStringLiteral("material"), scene.value(QStringLiteral("material"))},
                                   {QStringLiteral("transform"), transform}});
    }
    level.insert(QStringLiteral("objects"), objects);
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
