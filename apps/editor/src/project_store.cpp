#include "internal/project_store.h"

#include "internal/project_descriptor.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QStringList>

namespace ludus::editor
{
namespace
{

// A validation failure carries a code and a field-specific message. Returned by
// value so no exception path is involved.
ParseOutcome Fail(ResultCode code, const QString& message)
{
    ParseOutcome outcome;
    outcome.Code = code;
    outcome.Message = message;
    return outcome;
}

bool WithinBytes(const QString& value, usize maxBytes)
{
    return static_cast<usize>(value.toUtf8().size()) <= maxBytes;
}

bool ContainsNul(const QString& value)
{
    return value.contains(QChar(u'\0'));
}

// A relative path with no scheme/root; intentional ".." is allowed (resolved
// later). An absolute path is rejected here (design section 4).
bool IsRelativePath(const QString& value)
{
    if (value.isEmpty())
    {
        return false;
    }
    return QDir::isRelativePath(value);
}

bool ValidTargetName(const QString& value)
{
    if (value.isEmpty() || value.size() > 256)
    {
        return false;
    }
    const QChar first = value.at(0);
    const auto isWord = [](QChar c) {
        const ushort u = c.unicode();
        return (u >= u'A' && u <= u'Z') || (u >= u'a' && u <= u'z') || (u >= u'0' && u <= u'9') || u == u'_';
    };
    if (isWord(first) == false)
    {
        return false;
    }
    for (const QChar c : value)
    {
        const ushort u = c.unicode();
        const bool extra = u == u'.' || u == u'+' || u == u'-';
        if (isWord(c) == false && extra == false)
        {
            return false;
        }
    }
    return true;
}

// Validate a bounded array of pattern-constrained, unique, nonempty strings.
// Used for engine.components / engine.features. Returns true on success and
// fills `out`; on failure returns false and sets `message`.
struct StringListLimits
{
    usize Count;
    usize Bytes;
};

bool ParseStringList(const QJsonValue& value,
                     const QString& context,
                     const StringListLimits& listLimits,
                     bool (*validChar)(QChar, bool),
                     QStringList& out,
                     QString& message)
{
    if (value.isArray() == false)
    {
        message = QStringLiteral("'%1' must be an array").arg(context);
        return false;
    }
    const QJsonArray array = value.toArray();
    if (static_cast<usize>(array.size()) > listLimits.Count)
    {
        message = QStringLiteral("'%1' exceeds its entry limit").arg(context);
        return false;
    }
    QSet<QString> seen;
    for (const auto& item : array)
    {
        if (item.isString() == false)
        {
            message = QStringLiteral("every '%1' entry must be a string").arg(context);
            return false;
        }
        const QString s = item.toString();
        if (s.isEmpty() || s.contains(QChar(u'\0')) || static_cast<usize>(s.toUtf8().size()) > listLimits.Bytes)
        {
            message = QStringLiteral("invalid '%1' entry").arg(context);
            return false;
        }
        bool first = true;
        for (const QChar c : s)
        {
            if (validChar(c, first) == false)
            {
                message = QStringLiteral("invalid '%1' entry").arg(context);
                return false;
            }
            first = false;
        }
        if (seen.contains(s))
        {
            message = QStringLiteral("duplicate '%1' entry").arg(context);
            return false;
        }
        seen.insert(s);
        out.append(s);
    }
    return true;
}

// Component token: [A-Za-z][A-Za-z0-9]* (matches ludus_tools COMPONENT_RE).
bool ComponentChar(QChar c, bool first)
{
    const ushort u = c.unicode();
    if (first)
    {
        return (u >= u'A' && u <= u'Z') || (u >= u'a' && u <= u'z');
    }
    return (u >= u'A' && u <= u'Z') || (u >= u'a' && u <= u'z') || (u >= u'0' && u <= u'9');
}

// Feature token: [A-Za-z0-9][A-Za-z0-9_.-]* (matches ludus_tools FEATURE_RE).
bool FeatureChar(QChar c, bool first)
{
    const ushort u = c.unicode();
    const bool alnum = (u >= u'A' && u <= u'Z') || (u >= u'a' && u <= u'z') || (u >= u'0' && u <= u'9');
    if (first)
    {
        return alnum;
    }
    return alnum || u == u'_' || u == u'.' || u == u'-';
}

// Engine version / template id token: [A-Za-z0-9][A-Za-z0-9_.+-]* and
// [A-Za-z0-9][A-Za-z0-9_.-]* respectively; both share this permissive first-
// char rule, with the trailing set passed per use.
bool TokenChar(QChar c, bool first, bool allowPlus)
{
    const ushort u = c.unicode();
    const bool alnum = (u >= u'A' && u <= u'Z') || (u >= u'a' && u <= u'z') || (u >= u'0' && u <= u'9');
    if (first)
    {
        return alnum;
    }
    return alnum || u == u'_' || u == u'.' || u == u'-' || (allowPlus && u == u'+');
}

// Reject any object key not in `allowed` (unknown fields fail at every level).
QString UnknownKey(const QJsonObject& object, const QStringList& allowed)
{
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
    {
        if (allowed.contains(it.key()) == false)
        {
            return it.key();
        }
    }
    return QString();
}

} // namespace

QString Sha256Hex(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

ParseOutcome ParseDescriptor(const QByteArray& bytes)
{
    if (static_cast<usize>(bytes.size()) > limits::MaxFileBytes)
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("descriptor exceeds the 64 KiB size limit"));
    }
    // Reject invalid UTF-8 / unpaired surrogates / embedded NUL before JSON.
    // QString::fromUtf8 replaces invalid sequences; detect by round-tripping.
    const QString text = QString::fromUtf8(bytes);
    if (text.toUtf8() != bytes)
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("descriptor is not valid UTF-8"));
    }
    if (text.contains(QChar(u'\0')))
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("descriptor contains a NUL byte"));
    }

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || document.isObject() == false)
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("malformed JSON: %1").arg(error.errorString()));
    }
    const QJsonObject root = document.object();

    // version: a JSON number numerically equal to 1 or 2; reject bool/string/
    // non-integer float. The allowed root keys depend on the version so a v1
    // file still rejects v2's extra fields (old-reader behavior preserved).
    if (root.contains(QStringLiteral("version")) == false)
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("missing field 'version'"));
    }
    const QJsonValue versionValue = root.value(QStringLiteral("version"));
    if (versionValue.type() != QJsonValue::Double)
    {
        return Fail(ResultCode::UnsupportedVersion, QStringLiteral("'version' must be the number 1 or 2"));
    }
    foundation::uint32 schemaVersion = 0;
    {
        const double raw = versionValue.toDouble();
        if (raw == 1.0)
        {
            schemaVersion = 1;
        }
        else if (raw == 2.0)
        {
            schemaVersion = 2;
        }
        else
        {
            return Fail(ResultCode::UnsupportedVersion,
                        QStringLiteral("unsupported descriptor version; supported versions are 1 and 2"));
        }
    }

    QStringList rootKeys{QStringLiteral("version"),
                         QStringLiteral("name"),
                         QStringLiteral("provider"),
                         QStringLiteral("source_dir"),
                         QStringLiteral("preset"),
                         QStringLiteral("target"),
                         QStringLiteral("run")};
    if (schemaVersion == 2)
    {
        rootKeys << QStringLiteral("engine") << QStringLiteral("template");
    }
    const QString unknown = UnknownKey(root, rootKeys);
    if (unknown.isEmpty() == false)
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("unknown field '%1'").arg(unknown));
    }

    ProjectDescriptor descriptor;
    descriptor.Version = schemaVersion;

    // name
    const QJsonValue nameValue = root.value(QStringLiteral("name"));
    if (nameValue.isString() == false)
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("'name' must be a string"));
    }
    descriptor.Name = nameValue.toString();
    if (descriptor.Name.isEmpty())
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("'name' must be nonempty"));
    }
    if (ContainsNul(descriptor.Name) || WithinBytes(descriptor.Name, limits::MaxNameBytes) == false)
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("'name' exceeds 128 UTF-8 bytes or contains NUL"));
    }

    // provider
    const QJsonValue providerValue = root.value(QStringLiteral("provider"));
    if (providerValue.isString() == false)
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("'provider' must be a string"));
    }
    const QString provider = providerValue.toString();
    if (provider == QStringLiteral("ludus"))
    {
        descriptor.ProviderKind = Provider::Ludus;
    }
    else if (provider == QStringLiteral("cmake"))
    {
        descriptor.ProviderKind = Provider::Cmake;
    }
    else
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("'provider' must be exactly 'ludus' or 'cmake'"));
    }

    // source_dir
    const QJsonValue sourceValue = root.value(QStringLiteral("source_dir"));
    if (sourceValue.isString() == false)
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("'source_dir' must be a string"));
    }
    descriptor.SourceDir = sourceValue.toString();
    if (IsRelativePath(descriptor.SourceDir) == false || ContainsNul(descriptor.SourceDir) ||
        WithinBytes(descriptor.SourceDir, limits::MaxPathBytes) == false)
    {
        return Fail(ResultCode::InvalidProject,
                    QStringLiteral("'source_dir' must be a relative path within 4096 UTF-8 bytes"));
    }

    // preset
    const QJsonValue presetValue = root.value(QStringLiteral("preset"));
    if (presetValue.isString() == false)
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("'preset' must be a string"));
    }
    descriptor.Preset = presetValue.toString();
    {
        const bool v1Preset = descriptor.Preset == QStringLiteral("linux-clang-debug") ||
                              descriptor.Preset == QStringLiteral("linux-clang-development");
        // Version 2 permits all native game profiles, including macOS.
        // Version 1 keeps the original Linux two-preset contract.
        const bool v2Native = schemaVersion == 2 && (descriptor.Preset == QStringLiteral("linux-clang-release") ||
                                                     descriptor.Preset == QStringLiteral("macos-clang-debug") ||
                                                     descriptor.Preset == QStringLiteral("macos-clang-development") ||
                                                     descriptor.Preset == QStringLiteral("macos-clang-release"));
        if (v1Preset == false && v2Native == false)
        {
            return Fail(ResultCode::InvalidProject,
                        schemaVersion == 2
                            ? QStringLiteral("'preset' must be {linux,macos}-clang-{debug,development,release}")
                            : QStringLiteral("'preset' must be linux-clang-debug or linux-clang-development"));
        }
    }

    // target
    const QJsonValue targetValue = root.value(QStringLiteral("target"));
    if (targetValue.isString() == false)
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("'target' must be a string"));
    }
    descriptor.Target = targetValue.toString();
    if (ValidTargetName(descriptor.Target) == false || WithinBytes(descriptor.Target, limits::MaxTargetBytes) == false)
    {
        return Fail(ResultCode::InvalidProject,
                    QStringLiteral("'target' must match [A-Za-z0-9_][A-Za-z0-9_.+-]* within 256 bytes"));
    }

    // run { cwd, args }
    if (root.contains(QStringLiteral("run")) == false)
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("missing field 'run'"));
    }
    const QJsonValue runValue = root.value(QStringLiteral("run"));
    if (runValue.isObject() == false)
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("'run' must be an object"));
    }
    const QJsonObject run = runValue.toObject();
    static const QStringList runKeys{QStringLiteral("cwd"), QStringLiteral("args")};
    const QString runUnknown = UnknownKey(run, runKeys);
    if (runUnknown.isEmpty() == false)
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("unknown field 'run.%1'").arg(runUnknown));
    }

    const QJsonValue cwdValue = run.value(QStringLiteral("cwd"));
    if (cwdValue.isString() == false)
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("'run.cwd' must be a string"));
    }
    descriptor.RunCwd = cwdValue.toString();
    if (IsRelativePath(descriptor.RunCwd) == false || ContainsNul(descriptor.RunCwd) ||
        WithinBytes(descriptor.RunCwd, limits::MaxPathBytes) == false)
    {
        return Fail(ResultCode::InvalidProject,
                    QStringLiteral("'run.cwd' must be a relative path within 4096 UTF-8 bytes"));
    }

    if (run.contains(QStringLiteral("args")) == false)
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("missing field 'run.args'"));
    }
    const QJsonValue argsValue = run.value(QStringLiteral("args"));
    if (argsValue.isArray() == false)
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("'run.args' must be an array"));
    }
    const QJsonArray args = argsValue.toArray();
    if (static_cast<usize>(args.size()) > limits::MaxArgCount)
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("'run.args' exceeds 64 entries"));
    }
    usize total = 0;
    for (const auto& item : args)
    {
        if (item.isString() == false)
        {
            return Fail(ResultCode::InvalidProject, QStringLiteral("every 'run.args' entry must be a string"));
        }
        const QString value = item.toString();
        if (ContainsNul(value))
        {
            return Fail(ResultCode::InvalidProject, QStringLiteral("'run.args' entries must not contain NUL"));
        }
        const usize valueBytes = static_cast<usize>(value.toUtf8().size());
        if (valueBytes > limits::MaxArgBytes)
        {
            return Fail(ResultCode::InvalidProject, QStringLiteral("a 'run.args' entry exceeds 4096 UTF-8 bytes"));
        }
        total += valueBytes;
        descriptor.RunArgs.append(value); // empty strings are valid
    }
    if (total > limits::MaxArgsTotalBytes)
    {
        return Fail(ResultCode::InvalidProject, QStringLiteral("'run.args' total exceeds 32 KiB"));
    }

    // Version-2 engine/template objects.
    if (schemaVersion == 2)
    {
        // engine: required for provider cmake, forbidden for provider ludus.
        if (descriptor.ProviderKind == Provider::Cmake)
        {
            if (root.contains(QStringLiteral("engine")) == false)
            {
                return Fail(ResultCode::InvalidProject,
                            QStringLiteral("version-2 'cmake' projects require an 'engine' object"));
            }
            const QJsonValue engineValue = root.value(QStringLiteral("engine"));
            if (engineValue.isObject() == false)
            {
                return Fail(ResultCode::InvalidProject, QStringLiteral("'engine' must be an object"));
            }
            const QJsonObject engine = engineValue.toObject();
            static const QStringList engineKeys{QStringLiteral("version"),
                                                QStringLiteral("components"),
                                                QStringLiteral("features")};
            const QString engineUnknown = UnknownKey(engine, engineKeys);
            if (engineUnknown.isEmpty() == false)
            {
                return Fail(ResultCode::InvalidProject, QStringLiteral("unknown field 'engine.%1'").arg(engineUnknown));
            }
            const QJsonValue engineVersionValue = engine.value(QStringLiteral("version"));
            if (engineVersionValue.isString() == false)
            {
                return Fail(ResultCode::InvalidProject, QStringLiteral("'engine.version' must be a string"));
            }
            const QString engineVersion = engineVersionValue.toString();
            if (engineVersion.isEmpty() || WithinBytes(engineVersion, limits::MaxVersionBytes) == false)
            {
                return Fail(ResultCode::InvalidProject,
                            QStringLiteral("'engine.version' must be an exact release token"));
            }
            bool firstEngine = true;
            for (const QChar c : engineVersion)
            {
                if (TokenChar(c, firstEngine, /*allowPlus=*/true) == false)
                {
                    return Fail(ResultCode::InvalidProject,
                                QStringLiteral("'engine.version' must be an exact release token"));
                }
                firstEngine = false;
            }
            descriptor.Engine.Version = engineVersion;

            if (engine.contains(QStringLiteral("components")))
            {
                QString msg;
                if (ParseStringList(engine.value(QStringLiteral("components")),
                                    QStringLiteral("engine.components"),
                                    { .Count = limits::MaxComponentCount, .Bytes = limits::MaxComponentBytes },
                                    ComponentChar,
                                    descriptor.Engine.Components,
                                    msg) == false)
                {
                    return Fail(ResultCode::InvalidProject, msg);
                }
            }
            if (engine.contains(QStringLiteral("features")))
            {
                QString msg;
                if (ParseStringList(engine.value(QStringLiteral("features")),
                                    QStringLiteral("engine.features"),
                                    { .Count = limits::MaxFeatureCount, .Bytes = limits::MaxFeatureBytes },
                                    FeatureChar,
                                    descriptor.Engine.Features,
                                    msg) == false)
                {
                    return Fail(ResultCode::InvalidProject, msg);
                }
            }
            descriptor.HasEngine = true;
        }
        else // provider ludus must not declare an engine requirement
        {
            if (root.contains(QStringLiteral("engine")))
            {
                return Fail(ResultCode::InvalidProject,
                            QStringLiteral("provider 'ludus' must not declare an 'engine' requirement"));
            }
        }

        // template: optional { id, version }.
        if (root.contains(QStringLiteral("template")))
        {
            const QJsonValue templateValue = root.value(QStringLiteral("template"));
            if (templateValue.isObject() == false)
            {
                return Fail(ResultCode::InvalidProject, QStringLiteral("'template' must be an object"));
            }
            const QJsonObject templateObj = templateValue.toObject();
            static const QStringList templateKeys{QStringLiteral("id"), QStringLiteral("version")};
            const QString templateUnknown = UnknownKey(templateObj, templateKeys);
            if (templateUnknown.isEmpty() == false)
            {
                return Fail(ResultCode::InvalidProject,
                            QStringLiteral("unknown field 'template.%1'").arg(templateUnknown));
            }
            const QJsonValue idValue = templateObj.value(QStringLiteral("id"));
            if (idValue.isString() == false)
            {
                return Fail(ResultCode::InvalidProject, QStringLiteral("'template.id' must be a string"));
            }
            const QString templateId = idValue.toString();
            if (templateId.isEmpty() || WithinBytes(templateId, limits::MaxTemplateIdBytes) == false)
            {
                return Fail(ResultCode::InvalidProject, QStringLiteral("'template.id' must be a bounded token"));
            }
            bool firstTid = true;
            for (const QChar c : templateId)
            {
                if (TokenChar(c, firstTid, /*allowPlus=*/false) == false)
                {
                    return Fail(ResultCode::InvalidProject, QStringLiteral("'template.id' must be a bounded token"));
                }
                firstTid = false;
            }
            if (templateObj.contains(QStringLiteral("version")) == false)
            {
                return Fail(ResultCode::InvalidProject, QStringLiteral("missing field 'template.version'"));
            }
            const QJsonValue templateVersionValue = templateObj.value(QStringLiteral("version"));
            if (templateVersionValue.type() != QJsonValue::Double)
            {
                return Fail(ResultCode::InvalidProject,
                            QStringLiteral("'template.version' must be a positive integer"));
            }
            const double rawTemplateVersion = templateVersionValue.toDouble();
            if (rawTemplateVersion < 1.0 || rawTemplateVersion > 1000000.0 ||
                rawTemplateVersion != static_cast<double>(static_cast<foundation::uint64>(rawTemplateVersion)))
            {
                return Fail(ResultCode::InvalidProject,
                            QStringLiteral("'template.version' must be a positive integer"));
            }
            descriptor.Template.Id = templateId;
            descriptor.Template.Version = static_cast<foundation::uint64>(rawTemplateVersion);
            descriptor.HasTemplate = true;
        }
    }

    ParseOutcome outcome;
    outcome.Code = ResultCode::Ok;
    outcome.Descriptor = descriptor;
    return outcome;
}

QByteArray SerializeDescriptor(const ProjectDescriptor& descriptor)
{
    // Build the object in a stable, documented order. QJsonObject sorts keys
    // alphabetically on output, which is deterministic; to control order and
    // indentation precisely we emit the text ourselves.
    const auto escape = [](const QString& value) -> QString {
        QString out;
        out.reserve(value.size() + 2);
        for (const QChar c : value)
        {
            const ushort u = c.unicode();
            switch (u)
            {
                case u'"':
                    out += QStringLiteral("\\\"");
                    break;
                case u'\\':
                    out += QStringLiteral("\\\\");
                    break;
                case u'\b':
                    out += QStringLiteral("\\b");
                    break;
                case u'\f':
                    out += QStringLiteral("\\f");
                    break;
                case u'\n':
                    out += QStringLiteral("\\n");
                    break;
                case u'\r':
                    out += QStringLiteral("\\r");
                    break;
                case u'\t':
                    out += QStringLiteral("\\t");
                    break;
                default:
                    if (u < 0x20)
                    {
                        out += QStringLiteral("\\u%1").arg(u, 4, 16, QLatin1Char('0'));
                    }
                    else
                    {
                        out += c;
                    }
            }
        }
        return out;
    };

    const QString provider =
        descriptor.ProviderKind == Provider::Ludus ? QStringLiteral("ludus") : QStringLiteral("cmake");
    const foundation::uint32 version = descriptor.Version == 2 ? 2u : 1u;

    QString text;
    text += QStringLiteral("{\n");
    text += QStringLiteral("  \"version\": %1,\n").arg(version);
    text += QStringLiteral("  \"name\": \"%1\",\n").arg(escape(descriptor.Name));
    text += QStringLiteral("  \"provider\": \"%1\",\n").arg(provider);
    text += QStringLiteral("  \"source_dir\": \"%1\",\n").arg(escape(descriptor.SourceDir));
    text += QStringLiteral("  \"preset\": \"%1\",\n").arg(escape(descriptor.Preset));
    text += QStringLiteral("  \"target\": \"%1\",\n").arg(escape(descriptor.Target));
    text += QStringLiteral("  \"run\": {\n");
    text += QStringLiteral("    \"cwd\": \"%1\",\n").arg(escape(descriptor.RunCwd));
    if (descriptor.RunArgs.isEmpty())
    {
        text += QStringLiteral("    \"args\": []\n");
    }
    else
    {
        text += QStringLiteral("    \"args\": [\n");
        for (int i = 0; i < descriptor.RunArgs.size(); ++i)
        {
            const bool last = i + 1 == descriptor.RunArgs.size();
            text += QStringLiteral("      \"%1\"%2\n")
                        .arg(escape(descriptor.RunArgs.at(i)), last ? QString() : QStringLiteral(","));
        }
        text += QStringLiteral("    ]\n");
    }
    // Close run with a comma when a version-2 engine/template object follows.
    const bool hasTrailer = version == 2 && (descriptor.HasEngine || descriptor.HasTemplate);
    text += hasTrailer ? QStringLiteral("  },\n") : QStringLiteral("  }\n");

    const auto emitStringArray = [&](const QString& key, const QStringList& items, bool trailingComma) {
        if (items.isEmpty())
        {
            text += QStringLiteral("    \"%1\": []%2\n").arg(key, trailingComma ? QStringLiteral(",") : QString());
            return;
        }
        text += QStringLiteral("    \"%1\": [\n").arg(key);
        for (int i = 0; i < items.size(); ++i)
        {
            const bool last = i + 1 == items.size();
            text += QStringLiteral("      \"%1\"%2\n").arg(escape(items.at(i)), last ? QString() : QStringLiteral(","));
        }
        text += QStringLiteral("    ]%1\n").arg(trailingComma ? QStringLiteral(",") : QString());
    };

    if (version == 2 && descriptor.HasEngine)
    {
        text += QStringLiteral("  \"engine\": {\n");
        text += QStringLiteral("    \"version\": \"%1\",\n").arg(escape(descriptor.Engine.Version));
        emitStringArray(QStringLiteral("components"), descriptor.Engine.Components, /*trailingComma=*/true);
        emitStringArray(QStringLiteral("features"), descriptor.Engine.Features, /*trailingComma=*/false);
        text += descriptor.HasTemplate ? QStringLiteral("  },\n") : QStringLiteral("  }\n");
    }
    if (version == 2 && descriptor.HasTemplate)
    {
        text += QStringLiteral("  \"template\": {\n");
        text += QStringLiteral("    \"id\": \"%1\",\n").arg(escape(descriptor.Template.Id));
        text += QStringLiteral("    \"version\": %1\n").arg(descriptor.Template.Version);
        text += QStringLiteral("  }\n");
    }
    text += QStringLiteral("}\n");
    return text.toUtf8();
}

LoadOutcome ProjectStore::Load(const QString& descriptorPath) const
{
    LoadOutcome outcome;
    QFile file(descriptorPath);
    const QFileInfo info(descriptorPath);
    if (info.size() > static_cast<qint64>(limits::MaxFileBytes))
    {
        outcome.Parse = Fail(ResultCode::InvalidProject, QStringLiteral("descriptor exceeds the 64 KiB size limit"));
        return outcome;
    }
    if (file.open(QIODevice::ReadOnly) == false)
    {
        outcome.Parse =
            Fail(ResultCode::InvalidProject, QStringLiteral("cannot open descriptor: %1").arg(file.errorString()));
        return outcome;
    }
    outcome.Bytes = file.readAll();
    file.close();
    outcome.Digest = Sha256Hex(outcome.Bytes);
    outcome.Parse = ParseDescriptor(outcome.Bytes);
    return outcome;
}

SaveOutcome
ProjectStore::Save(const QString& descriptorPath, const ProjectDescriptor& draft, const QString& expectedDigest) const
{
    SaveOutcome outcome;

    // Validate the complete draft by serializing then reparsing: the only bytes
    // ever written are bytes that parse back to the same descriptor.
    const QByteArray bytes = SerializeDescriptor(draft);
    const ParseOutcome check = ParseDescriptor(bytes);
    if (check.Ok() == false || (check.Descriptor == draft) == false)
    {
        outcome.Code = ResultCode::InvalidProject;
        outcome.Message = check.Ok() ? QStringLiteral("draft failed round-trip validation") : check.Message;
        return outcome;
    }

    const QFileInfo info(descriptorPath);
    const QDir settingsDir = info.absoluteDir();
    const QString lockDirPath = settingsDir.filePath(QStringLiteral(".ludus"));
    QDir().mkpath(lockDirPath);
    QLockFile lock(QDir(lockDirPath).filePath(info.fileName() + QStringLiteral(".lock")));
    lock.setStaleLockTime(0);
    if (lock.tryLock(250) == false)
    {
        outcome.Code = ResultCode::Busy;
        outcome.Message = QStringLiteral("another editor holds the project settings lock");
        return outcome;
    }

    // Optimistic conflict detection: the current disk bytes must still match the
    // digest recorded at Open/last Save. External change/deletion => Conflict,
    // keep the draft and offer explicit Reload.
    if (expectedDigest.isEmpty() == false)
    {
        QFile current(descriptorPath);
        if (current.exists() == false)
        {
            outcome.Code = ResultCode::Conflict;
            outcome.Message = QStringLiteral("the saved descriptor was deleted on disk; reload or save as new");
            return outcome;
        }
        if (current.open(QIODevice::ReadOnly))
        {
            const QByteArray diskBytes = current.readAll();
            current.close();
            if (Sha256Hex(diskBytes) != expectedDigest)
            {
                outcome.Code = ResultCode::Conflict;
                outcome.Message = QStringLiteral("the descriptor changed on disk since it was opened");
                return outcome;
            }
        }
        else
        {
            outcome.Code = ResultCode::Conflict;
            outcome.Message = QStringLiteral("cannot read the current descriptor to check for conflicts");
            return outcome;
        }
    }

    QSaveFile save(descriptorPath);
    // Direct-write fallback disabled: without the temporary-file replacement we
    // would lose atomic-replacement protection (design section 4).
    save.setDirectWriteFallback(false);
    if (save.open(QIODevice::WriteOnly) == false)
    {
        outcome.Code = ResultCode::InvalidProject;
        outcome.Message = QStringLiteral("cannot open descriptor for writing: %1").arg(save.errorString());
        return outcome;
    }

    // Honor a test-injected short write, otherwise write all bytes.
    QByteArray toWrite = bytes;
    if (Hooks_.TruncateWriteTo)
    {
        const foundation::isize truncate = Hooks_.TruncateWriteTo(bytes);
        if (truncate >= 0 && truncate < static_cast<foundation::isize>(bytes.size()))
        {
            toWrite = bytes.left(static_cast<int>(truncate));
        }
    }
    const qint64 written = save.write(toWrite);
    if (written != bytes.size())
    {
        // Short/failed write: abandon the temporary file; destination unchanged.
        save.cancelWriting();
        outcome.Code = ResultCode::InvalidProject;
        outcome.Message =
            QStringLiteral("short write while saving descriptor (%1 of %2 bytes)").arg(written).arg(bytes.size());
        return outcome;
    }

    if (Hooks_.FailCommit && Hooks_.FailCommit())
    {
        save.cancelWriting();
        outcome.Code = ResultCode::InvalidProject;
        outcome.Message = QStringLiteral("commit failed while saving descriptor");
        return outcome;
    }

    if (save.commit() == false)
    {
        // Commit failure preserves the previous destination and the dirty draft.
        outcome.Code = ResultCode::InvalidProject;
        outcome.Message = QStringLiteral("commit failed while saving descriptor: %1").arg(save.errorString());
        return outcome;
    }

    outcome.Code = ResultCode::Ok;
    outcome.Digest = Sha256Hex(bytes);
    return outcome;
}

} // namespace ludus::editor
