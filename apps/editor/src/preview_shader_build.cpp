#include "internal/preview_shader_build.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>

#include <cstring>

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QThread>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace rhi = graphics::rhi;
namespace compiler = tooling::shader_compiler;
/// Name of the renderer manifest inside PreviewShaderSources::RecipeDirectory.
const QString RECIPE_MANIFEST = QStringLiteral("recipes.json");
/// How long a just-launched worker may take to initialize Slang and listen.
constexpr qint64 WORKER_START_MILLISECONDS = 10000;
/// Pause between connection attempts while the worker starts.
constexpr uint32 WORKER_RETRY_MILLISECONDS = 20;
/// Pause between nonblocking polls of one compilation.
constexpr uint32 WORKER_POLL_MILLISECONDS = 1;
/// Upper bound of one stage compilation, matching the build's native worker timeout.
constexpr qint64 COMPILE_TIMEOUT_MILLISECONDS = 120000;
/// Upper bound of one spirv-val run.
constexpr int32 VALIDATOR_TIMEOUT_MILLISECONDS = 30000;
/// SPIR-V header words before the first instruction, and the magic number in word 0.
constexpr qsizetype SPIRV_HEADER_WORDS = 5;
constexpr uint32 SPIRV_MAGIC = 0x07230203U;
/// OpEntryPoint opcode and the word index of its literal name.
constexpr uint32 SPIRV_OP_ENTRY_POINT = 15;
constexpr uint32 SPIRV_ENTRY_NAME_WORD = 3;
/// Bits of an instruction's first word holding its opcode; the rest hold its word count.
constexpr uint32 SPIRV_OPCODE_MASK = 0xffffU;
constexpr uint32 SPIRV_WORD_COUNT_SHIFT = 16;
/// Bytes of one little-endian SPIR-V word holding name characters.
constexpr uint32 SPIRV_WORD_BYTES = 4;
constexpr uint32 BITS_PER_BYTE = 8;
constexpr uint32 BYTE_MASK = 0xffU;
/// Vector widths and matrix size the uniform shape reflection admits.
constexpr int32 VECTOR_WIDTH_THREE = 3;
constexpr int32 VECTOR_WIDTH_FOUR = 4;
constexpr int32 MATRIX_SIZE = 4;

compiler::Stage ToCompilerStage(PreviewShaderStage stage) noexcept
{
    return stage == PreviewShaderStage::Vertex ? compiler::Stage::Vertex : compiler::Stage::Fragment;
}
QString GetStageName(PreviewShaderStage stage)
{
    return stage == PreviewShaderStage::Vertex ? QStringLiteral("vertex") : QStringLiteral("fragment");
}
BorrowedView<const char> ToView(const QByteArray& bytes) noexcept
{
    return {bytes.constData(), static_cast<usize>(bytes.size())};
}
QByteArray HashText(const QByteArray& text)
{
    return QCryptographicHash::hash(text, QCryptographicHash::Sha256);
}
QString DescribeStatus(compiler::Status status)
{
    switch (status)
    {
        case compiler::Status::Ready:
            return QStringLiteral("ready");
        case compiler::Status::Pending:
            return QStringLiteral("still pending");
        case compiler::Status::InvalidRequest:
            return QStringLiteral("rejected the source snapshot");
        case compiler::Status::CompileFailed:
            return QStringLiteral("reported a compile error");
        case compiler::Status::Unavailable:
            return QStringLiteral("is not running");
        case compiler::Status::OutOfMemory:
            return QStringLiteral("ran out of memory");
        case compiler::Status::TransportFailed:
            return QStringLiteral("disconnected or sent a malformed reply");
        case compiler::Status::InvalidState:
            return QStringLiteral("was cancelled");
    }
    return QStringLiteral("failed");
}
QString GetBindingKindName(rhi::RasterBindingKind kind)
{
    switch (kind)
    {
        case rhi::RasterBindingKind::UniformBuffer:
            return QStringLiteral("uniform buffer");
        case rhi::RasterBindingKind::Texture2D:
            return QStringLiteral("2D texture");
        case rhi::RasterBindingKind::Texture2DArray:
            return QStringLiteral("2D texture array");
        case rhi::RasterBindingKind::TextureCube:
            return QStringLiteral("cube texture");
        case rhi::RasterBindingKind::Texture3D:
            return QStringLiteral("3D texture");
        case rhi::RasterBindingKind::TextureDepth2D:
            return QStringLiteral("depth texture");
        case rhi::RasterBindingKind::TextureStencil2D:
            return QStringLiteral("stencil texture");
        case rhi::RasterBindingKind::Sampler:
            return QStringLiteral("sampler");
        case rhi::RasterBindingKind::RayScene:
            return QStringLiteral("native ray scene");
        case rhi::RasterBindingKind::StorageRead:
        case rhi::RasterBindingKind::StorageReadWrite:
            return QStringLiteral("storage buffer");
    }
    return QStringLiteral("resource");
}
/// Port of compile_raster.uniform_shape: the authored shape of one uniform member type.
rhi::RasterUniformShape ReadUniformShape(const QJsonObject& type)
{
    const QString kind = type.value(QStringLiteral("kind")).toString();
    const QJsonObject element = type.value(QStringLiteral("elementType")).toObject();
    const bool isFloatElement = element.value(QStringLiteral("scalarType")).toString() == QStringLiteral("float32");
    if (kind == QStringLiteral("scalar") &&
        type.value(QStringLiteral("scalarType")).toString() == QStringLiteral("float32"))
    {
        return rhi::RasterUniformShape::Float32;
    }
    if (kind == QStringLiteral("vector") && isFloatElement == true)
    {
        const int32 count = type.value(QStringLiteral("elementCount")).toInt();
        if (count == 2)
        {
            return rhi::RasterUniformShape::Float2;
        }
        if (count == VECTOR_WIDTH_THREE)
        {
            return rhi::RasterUniformShape::Float3;
        }
        if (count == VECTOR_WIDTH_FOUR)
        {
            return rhi::RasterUniformShape::Float4;
        }
    }
    if (kind == QStringLiteral("matrix") && isFloatElement == true &&
        type.value(QStringLiteral("rowCount")).toInt() == MATRIX_SIZE &&
        type.value(QStringLiteral("columnCount")).toInt() == MATRIX_SIZE)
    {
        return rhi::RasterUniformShape::Float4x4;
    }
    if (kind == QStringLiteral("array") && type.value(QStringLiteral("elementCount")).toInt() > 0)
    {
        const rhi::RasterUniformShape shape = ReadUniformShape(element);
        if (shape == rhi::RasterUniformShape::Float4)
        {
            return rhi::RasterUniformShape::Float4Array;
        }
        if (shape == rhi::RasterUniformShape::Float4x4)
        {
            return rhi::RasterUniformShape::Float4x4Array;
        }
    }
    return rhi::RasterUniformShape::Opaque;
}
/// Kind of one reflected group-zero resource, or false outside the raster profile.
bool ReadBindingKind(const QJsonObject& type, rhi::RasterBindingKind& outKind)
{
    const QString kind = type.value(QStringLiteral("kind")).toString();
    if (kind == QStringLiteral("constantBuffer"))
    {
        outKind = rhi::RasterBindingKind::UniformBuffer;
        return true;
    }
    if (kind == QStringLiteral("samplerState"))
    {
        outKind = rhi::RasterBindingKind::Sampler;
        return true;
    }
    if (kind != QStringLiteral("resource"))
    {
        return false;
    }
    const QString shape = type.value(QStringLiteral("baseShape")).toString();
    const QJsonObject result = type.value(QStringLiteral("resultType")).toObject();
    const bool isScalar = result.value(QStringLiteral("kind")).toString() == QStringLiteral("scalar");
    const QString scalar =
        isScalar == true
            ? result.value(QStringLiteral("scalarType")).toString()
            : result.value(QStringLiteral("elementType")).toObject().value(QStringLiteral("scalarType")).toString();
    const bool isArray = type.value(QStringLiteral("array")).toBool();
    if (isScalar == true && shape == QStringLiteral("texture2D") && isArray == false)
    {
        outKind = scalar == QStringLiteral("float32") ? rhi::RasterBindingKind::TextureDepth2D
                                                      : rhi::RasterBindingKind::TextureStencil2D;
        return true;
    }
    if (shape == QStringLiteral("texture2D"))
    {
        outKind = isArray == true ? rhi::RasterBindingKind::Texture2DArray : rhi::RasterBindingKind::Texture2D;
        return true;
    }
    if (shape == QStringLiteral("textureCube"))
    {
        outKind = rhi::RasterBindingKind::TextureCube;
        return true;
    }
    if (shape == QStringLiteral("texture3D"))
    {
        outKind = rhi::RasterBindingKind::Texture3D;
        return true;
    }
    return false;
}
/// Group-zero binding index of one reflected parameter, as compile_raster selects it.
bool ReadBindingIndex(const QJsonObject& parameter, bool isMetal, uint32& outIndex)
{
    QJsonArray bindings = parameter.value(QStringLiteral("bindings")).toArray();
    if (bindings.isEmpty() == true)
    {
        bindings.append(parameter.value(QStringLiteral("binding")).toObject());
    }
    QJsonObject selected;
    int32 matches = 0;
    for (const QJsonValue& value : bindings)
    {
        const QJsonObject binding = value.toObject();
        if (binding.value(QStringLiteral("kind")).toString() == QStringLiteral("descriptorTableSlot"))
        {
            selected = binding;
            ++matches;
        }
    }
    if (matches == 0 && isMetal == true)
    {
        for (const QJsonValue& value : bindings)
        {
            const QJsonObject binding = value.toObject();
            const QString kind = binding.value(QStringLiteral("kind")).toString();
            if (kind == QStringLiteral("constantBuffer") || kind == QStringLiteral("shaderResource") ||
                kind == QStringLiteral("samplerState"))
            {
                selected = binding;
                ++matches;
            }
        }
    }
    if (matches != 1 || selected.value(QStringLiteral("space")).toInt() != 0 ||
        selected.value(QStringLiteral("index")).toInt(-1) < 0)
    {
        return false;
    }
    outIndex = static_cast<uint32>(selected.value(QStringLiteral("index")).toInt());
    return true;
}
/// Names of the parameters the compiled entry point uses.
QHash<QString, bool> ReadResourceUsage(const QJsonObject& document, PreviewShaderStage stage)
{
    QHash<QString, bool> used;
    const QJsonValue native = document.value(QStringLiteral("nativeEntryPointResourceUsage"));
    if (native.isObject() == true)
    {
        const QJsonObject usage = native.toObject();
        for (QJsonObject::const_iterator it = usage.begin(); it != usage.end(); ++it)
        {
            used.insert(it.key(), it.value().toBool());
        }
        return used;
    }
    for (const QJsonValue& point : document.value(QStringLiteral("entryPoints")).toArray())
    {
        if (point.toObject().value(QStringLiteral("stage")).toString() != GetStageName(stage))
        {
            continue;
        }
        for (const QJsonValue& resource : point.toObject().value(QStringLiteral("bindings")).toArray())
        {
            const QJsonObject entry = resource.toObject();
            bool isUsed = entry.value(QStringLiteral("binding")).toObject().value(QStringLiteral("used")).toBool();
            for (const QJsonValue& binding : entry.value(QStringLiteral("bindings")).toArray())
            {
                isUsed = isUsed || binding.toObject().value(QStringLiteral("used")).toBool();
            }
            used.insert(entry.value(QStringLiteral("name")).toString(), isUsed);
        }
    }
    return used;
}
/// Append the float vector inputs of one vertex entry parameter, flattening structs.
bool ReadVertexInputs(const QJsonObject& field, QList<rhi::RasterShaderInput>& inoutInputs, QString& outReason)
{
    if (field.value(QStringLiteral("semanticName")).toString().startsWith(QStringLiteral("SV_")) == true)
    {
        return true;
    }
    const QJsonObject type = field.value(QStringLiteral("type")).toObject();
    if (type.value(QStringLiteral("kind")).toString() == QStringLiteral("struct"))
    {
        for (const QJsonValue& child : type.value(QStringLiteral("fields")).toArray())
        {
            if (ReadVertexInputs(child.toObject(), inoutInputs, outReason) == false)
            {
                return false;
            }
        }
        return true;
    }
    const QJsonObject binding = field.value(QStringLiteral("binding")).toObject();
    const int32 count = type.value(QStringLiteral("elementCount")).toInt();
    if (binding.value(QStringLiteral("kind")).toString() != QStringLiteral("varyingInput") ||
        type.value(QStringLiteral("kind")).toString() != QStringLiteral("vector") || count < 2 ||
        count > VECTOR_WIDTH_FOUR ||
        type.value(QStringLiteral("elementType")).toObject().value(QStringLiteral("scalarType")).toString() !=
            QStringLiteral("float32"))
    {
        outReason = QStringLiteral("vertex input '%1' is not a float2/3/4 attribute")
                        .arg(field.value(QStringLiteral("name")).toString());
        return false;
    }
    rhi::RasterShaderInput input;
    input.Location = static_cast<uint32>(binding.value(QStringLiteral("index")).toInt());
    input.Format = count == 2                    ? rhi::RasterVertexFormat::Float2
                   : count == VECTOR_WIDTH_THREE ? rhi::RasterVertexFormat::Float3
                                                 : rhi::RasterVertexFormat::Float4;
    inoutInputs.append(input);
    return true;
}
const rhi::RasterBinding* FindBuiltInBindingOrNull(const rhi::RasterShaderDescription& builtIn, uint32 index)
{
    for (const rhi::RasterBinding& binding : builtIn.Bindings)
    {
        if (binding.Binding == index)
        {
            return &binding;
        }
    }
    return nullptr;
}
/// Compare one used uniform buffer's members with the built-in schema of the same binding.
bool AreUniformMembersCompatible(const QJsonObject& type,
                                 uint32 index,
                                 const rhi::RasterShaderDescription& builtIn,
                                 const QString& name,
                                 QString& outReason)
{
    const QJsonArray fields =
        type.value(QStringLiteral("elementType")).toObject().value(QStringLiteral("fields")).toArray();
    qsizetype builtInCount = 0;
    for (const rhi::RasterUniformMember& member : builtIn.UniformMembers)
    {
        builtInCount += member.Binding == index ? 1 : 0;
    }
    if (builtIn.UniformMembers.empty() == false && builtInCount != fields.size())
    {
        outReason = QStringLiteral("uniform buffer '%1' now has %2 members instead of %3")
                        .arg(name)
                        .arg(fields.size())
                        .arg(builtInCount);
        return false;
    }
    for (const QJsonValue& value : fields)
    {
        const QJsonObject field = value.toObject();
        const QString fieldName = field.value(QStringLiteral("name")).toString();
        const usize offset = static_cast<usize>(
            field.value(QStringLiteral("binding")).toObject().value(QStringLiteral("offset")).toInt());
        const rhi::RasterUniformShape shape = ReadUniformShape(field.value(QStringLiteral("type")).toObject());
        bool isFound = builtIn.UniformMembers.empty() == true;
        for (const rhi::RasterUniformMember& member : builtIn.UniformMembers)
        {
            if (member.Binding != index ||
                QLatin1StringView(member.Name.data(), static_cast<qsizetype>(member.Name.size())) != fieldName)
            {
                continue;
            }
            if (member.Offset != offset || member.Shape != shape)
            {
                outReason = QStringLiteral("uniform member '%1.%2' moved from offset %3 to %4 or changed type")
                                .arg(name, fieldName)
                                .arg(member.Offset)
                                .arg(offset);
                return false;
            }
            isFound = true;
        }
        if (isFound == false)
        {
            outReason = QStringLiteral("uniform buffer '%1' gained member '%2'").arg(name, fieldName);
            return false;
        }
    }
    return true;
}
} // namespace

bool ReadPreviewShaderRecipes(const PreviewShaderSources& sources, PreviewShaderRecipes& outRecipes, QString& outError)
{
    const QString path = QDir(sources.RecipeDirectory).filePath(RECIPE_MANIFEST);
    QFile file(path);
    if (file.open(QIODevice::ReadOnly) == false)
    {
        outError = QStringLiteral("Cannot read the renderer shader manifest %1.").arg(QDir::toNativeSeparators(path));
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (document.isObject() == false)
    {
        outError = QStringLiteral("The renderer shader manifest %1 is not valid JSON: %2")
                       .arg(QDir::toNativeSeparators(path), parseError.errorString());
        return false;
    }
    PreviewShaderRecipes recipes{};
    for (usize i = 0; i < PREVIEW_SHADER_PROGRAM_COUNT; ++i)
    {
        const QString name = QString::fromLatin1(GetPreviewShaderRecipeName(static_cast<PreviewShaderProgram>(i)));
        const QJsonObject recipe = document.object().value(name).toObject();
        recipes[i].Source = recipe.value(QStringLiteral("source")).toString();
        recipes[i].VertexEntry = recipe.value(QStringLiteral("vertex")).toString().toUtf8();
        recipes[i].FragmentEntry = recipe.value(QStringLiteral("fragment")).toString().toUtf8();
        if (recipes[i].Source.isEmpty() == true || recipes[i].VertexEntry.isEmpty() == true ||
            recipes[i].FragmentEntry.isEmpty() == true)
        {
            outError = QStringLiteral("The renderer shader manifest has no raster recipe %1.").arg(name);
            return false;
        }
    }
    outRecipes = recipes;
    return true;
}

bool CapturePreviewShaderSnapshot(const PreviewShaderSources& sources,
                                  PreviewShaderSnapshot& outSnapshot,
                                  QString& outError)
{
    PreviewShaderSnapshot snapshot;
    QHash<QString, qsizetype> indices;
    usize totalBytes = 0;
    QStringList roots{sources.RecipeDirectory};
    roots.append(sources.IncludeDirectories);
    for (const QString& root : roots)
    {
        const QDir directory(root);
        if (directory.exists() == false)
        {
            outError = QStringLiteral("Shader source directory %1 does not exist.").arg(QDir::toNativeSeparators(root));
            return false;
        }
        QStringList names;
        QDirIterator iterator(root,
                              {QStringLiteral("*.slang"), QStringLiteral("*.h")},
                              QDir::Files | QDir::System | QDir::Hidden,
                              QDirIterator::Subdirectories);
        while (iterator.hasNext() == true)
        {
            const QString relative = directory.relativeFilePath(iterator.next());
            bool isSkipped = false;
            for (const QString& part : relative.split(QLatin1Char('/')))
            {
                isSkipped = isSkipped || part.startsWith(QLatin1Char('.')) || part == QStringLiteral("out");
            }
            if (isSkipped == false)
            {
                names.append(relative);
            }
        }
        names.sort();
        for (const QString& name : names)
        {
            const QString path = directory.absoluteFilePath(name);
            if (QFileInfo(path).isSymLink() == true)
            {
                outError = QStringLiteral("Shader include roots cannot contain symbolic links: %1")
                               .arg(QDir::toNativeSeparators(path));
                return false;
            }
            QFile file(path);
            if (file.open(QIODevice::ReadOnly) == false)
            {
                outError = QStringLiteral("Cannot read shader source %1.").arg(QDir::toNativeSeparators(path));
                return false;
            }
            const QByteArray text = file.readAll();
            const QHash<QString, qsizetype>::const_iterator existing = indices.constFind(name);
            if (existing != indices.constEnd())
            {
                if (snapshot.Texts[existing.value()] != text)
                {
                    outError = QStringLiteral("Ambiguous shader include %1 differs between source roots.").arg(name);
                    return false;
                }
                continue;
            }
            totalBytes += static_cast<usize>(text.size());
            if (static_cast<usize>(snapshot.Names.size()) >= compiler::MAX_SOURCE_FILES ||
                totalBytes > compiler::MAX_SOURCE_BYTES)
            {
                outError = QStringLiteral("Shader sources exceed the compiler snapshot limit of %1 files.")
                               .arg(compiler::MAX_SOURCE_FILES);
                return false;
            }
            indices.insert(name, snapshot.Names.size());
            snapshot.Names.append(name);
            snapshot.Texts.append(text);
            snapshot.Paths.append(path);
        }
    }
    outSnapshot = snapshot;
    return true;
}

PreviewShaderDiagnostic ParsePreviewShaderDiagnostic(const QByteArray& diagnostics,
                                                     const PreviewShaderSnapshot& snapshot)
{
    // Slang reports "name(line): error 12345: message", followed by the source line and a caret.
    static const QRegularExpression HEADER(QStringLiteral(
        R"(^(?<file>.+?)\((?<line>\d+)(?:,\s*\d+)?\)\s*:\s*(?:fatal\s+)?error\b[^:]*:\s*(?<message>.*)$)"));
    static const QRegularExpression ANY_HEADER(QStringLiteral(R"(^.*?\(\d+(?:,\s*\d+)?\)\s*:\s*\w)"));
    const QStringList lines = QString::fromUtf8(diagnostics).split(QLatin1Char('\n'));
    PreviewShaderDiagnostic diagnostic;
    qsizetype first = -1;
    for (qsizetype i = 0; i < lines.size(); ++i)
    {
        const QRegularExpressionMatch match = HEADER.match(lines[i].trimmed());
        if (match.hasMatch() == true)
        {
            const QString name = match.captured(QStringLiteral("file"));
            const qsizetype index = snapshot.Names.indexOf(name);
            diagnostic.Path = index >= 0 ? snapshot.Paths[index] : name;
            diagnostic.Line = match.captured(QStringLiteral("line")).toInt();
            diagnostic.Message = match.captured(QStringLiteral("message")).trimmed();
            first = i;
            break;
        }
    }
    if (first < 0)
    {
        for (const QString& line : lines)
        {
            if (line.trimmed().isEmpty() == false)
            {
                diagnostic.Message = line.trimmed();
                break;
            }
        }
        if (diagnostic.Message.isEmpty() == true)
        {
            diagnostic.Message = QStringLiteral("The shader compiler reported an error without diagnostics.");
        }
        return diagnostic;
    }
    for (qsizetype i = first + 1; i < lines.size() && diagnostic.Excerpt.size() < PREVIEW_SHADER_EXCERPT_LINES; ++i)
    {
        QString line = lines[i];
        line.remove(QLatin1Char('\r'));
        if (line.trimmed().isEmpty() == true || ANY_HEADER.match(line.trimmed()).hasMatch() == true)
        {
            break;
        }
        diagnostic.Excerpt.append(line);
    }
    return diagnostic;
}

QString FormatPreviewShaderDiagnostic(const PreviewShaderDiagnostic& diagnostic)
{
    QString text = diagnostic.Path.isEmpty() == true ? QString() : QDir::toNativeSeparators(diagnostic.Path);
    if (diagnostic.Line > 0)
    {
        text += QStringLiteral(":%1").arg(diagnostic.Line);
    }
    // Location and message on separate lines, so a narrow overlay shortening the path keeps the
    // file name, the line and the whole message readable.
    if (text.isEmpty() == false)
    {
        text += QLatin1Char('\n');
    }
    text += diagnostic.Message;
    for (const QString& line : diagnostic.Excerpt)
    {
        text += QLatin1Char('\n') + line;
    }
    return text;
}

bool FindSpirvEntry(const QList<uint32>& words, QByteArray& outEntry)
{
    if (words.size() <= SPIRV_HEADER_WORDS || words[0] != SPIRV_MAGIC)
    {
        return false;
    }
    QByteArray entry;
    int32 count = 0;
    qsizetype offset = SPIRV_HEADER_WORDS;
    while (offset < words.size())
    {
        const uint32 opcode = words[offset] & SPIRV_OPCODE_MASK;
        const qsizetype wordCount = static_cast<qsizetype>(words[offset] >> SPIRV_WORD_COUNT_SHIFT);
        if (wordCount == 0 || offset + wordCount > words.size())
        {
            return false;
        }
        if (opcode == SPIRV_OP_ENTRY_POINT && wordCount > SPIRV_ENTRY_NAME_WORD)
        {
            ++count;
            entry.clear();
            bool isTerminated = false;
            for (qsizetype word = SPIRV_ENTRY_NAME_WORD; word < wordCount && isTerminated == false; ++word)
            {
                for (uint32 byte = 0; byte < SPIRV_WORD_BYTES; ++byte)
                {
                    const char character =
                        static_cast<char>((words[offset + word] >> (byte * BITS_PER_BYTE)) & BYTE_MASK);
                    if (character == '\0')
                    {
                        isTerminated = true;
                        break;
                    }
                    entry.append(character);
                }
            }
        }
        offset += wordCount;
    }
    if (count != 1 || entry.isEmpty() == true)
    {
        return false;
    }
    outEntry = entry;
    return true;
}

bool FindMetalEntry(const QByteArray& code, PreviewShaderStage stage, QByteArray& outEntry)
{
    static const QRegularExpression VERTEX(QStringLiteral(R"(\[\[vertex\]\]\s+\w+\s+(\w+)\s*\()"));
    static const QRegularExpression FRAGMENT(QStringLiteral(R"(\[\[fragment\]\]\s+\w+\s+(\w+)\s*\()"));
    const QString text = QString::fromUtf8(code);
    QRegularExpressionMatchIterator matches =
        (stage == PreviewShaderStage::Vertex ? VERTEX : FRAGMENT).globalMatch(text);
    QByteArray entry;
    int32 count = 0;
    while (matches.hasNext() == true)
    {
        entry = matches.next().captured(1).toUtf8();
        ++count;
    }
    if (count != 1)
    {
        return false;
    }
    outEntry = entry;
    return true;
}

bool IsPreviewShaderInterfaceCompatible(const QByteArray& reflection,
                                        PreviewShaderStage stage,
                                        bool isMetal,
                                        const rhi::RasterShaderDescription& builtIn,
                                        QString& outReason)
{
    const QJsonDocument document = QJsonDocument::fromJson(reflection);
    if (document.isObject() == false)
    {
        outReason = QStringLiteral("the compiler returned unreadable reflection");
        return false;
    }
    const QJsonObject root = document.object();
    const QHash<QString, bool> used = ReadResourceUsage(root, stage);
    for (const QJsonValue& value : root.value(QStringLiteral("parameters")).toArray())
    {
        const QJsonObject parameter = value.toObject();
        const QString name = parameter.value(QStringLiteral("name")).toString();
        if (used.value(name, false) == false)
        {
            continue;
        }
        uint32 index = 0;
        rhi::RasterBindingKind kind = rhi::RasterBindingKind::UniformBuffer;
        const QJsonObject type = parameter.value(QStringLiteral("type")).toObject();
        if (ReadBindingIndex(parameter, isMetal, index) == false || ReadBindingKind(type, kind) == false)
        {
            outReason = QStringLiteral("resource '%1' is outside the viewport's binding profile").arg(name);
            return false;
        }
        const rhi::RasterBinding* builtInOrNull = FindBuiltInBindingOrNull(builtIn, index);
        if (builtInOrNull == nullptr)
        {
            outReason = QStringLiteral("resource '%1' uses binding %2, which the built-in %3 stage does not declare")
                            .arg(name)
                            .arg(index)
                            .arg(GetStageName(stage));
            return false;
        }
        if (builtInOrNull->Kind != kind)
        {
            outReason = QStringLiteral("binding %1 ('%2') changed from %3 to %4")
                            .arg(index)
                            .arg(name, GetBindingKindName(builtInOrNull->Kind), GetBindingKindName(kind));
            return false;
        }
        if (kind == rhi::RasterBindingKind::UniformBuffer)
        {
            const usize size = static_cast<usize>(type.value(QStringLiteral("elementVarLayout"))
                                                      .toObject()
                                                      .value(QStringLiteral("binding"))
                                                      .toObject()
                                                      .value(QStringLiteral("size"))
                                                      .toInt());
            if (size != builtInOrNull->MinSize)
            {
                outReason = QStringLiteral("uniform buffer '%1' changed size from %2 to %3 bytes")
                                .arg(name)
                                .arg(builtInOrNull->MinSize)
                                .arg(size);
                return false;
            }
            if (AreUniformMembersCompatible(type, index, builtIn, name, outReason) == false)
            {
                return false;
            }
        }
    }
    if (stage == PreviewShaderStage::Vertex)
    {
        QList<rhi::RasterShaderInput> inputs;
        for (const QJsonValue& point : root.value(QStringLiteral("entryPoints")).toArray())
        {
            if (point.toObject().value(QStringLiteral("stage")).toString() != GetStageName(stage))
            {
                continue;
            }
            for (const QJsonValue& parameter : point.toObject().value(QStringLiteral("parameters")).toArray())
            {
                if (ReadVertexInputs(parameter.toObject(), inputs, outReason) == false)
                {
                    return false;
                }
            }
        }
        bool isSame = inputs.size() == static_cast<qsizetype>(builtIn.Inputs.size());
        for (qsizetype i = 0; i < inputs.size() && isSame == true; ++i)
        {
            isSame = inputs[i].Location == builtIn.Inputs[static_cast<usize>(i)].Location &&
                     inputs[i].Format == builtIn.Inputs[static_cast<usize>(i)].Format;
        }
        if (isSame == false)
        {
            outReason = QStringLiteral("vertex inputs changed");
            return false;
        }
    }
    return true;
}

PreviewShaderWorkerCompiler::PreviewShaderWorkerCompiler(const QString& endpoint)
    : mEndpoint(QFile::encodeName(endpoint))
{
}

compiler::Status PreviewShaderWorkerCompiler::Compile(const compiler::Request& request,
                                                      compiler::Result& outResult,
                                                      const QAtomicInt& cancel) noexcept
{
    QElapsedTimer clock;
    clock.start();
    compiler::Client client;
    compiler::Status status = client.Submit(ToView(mEndpoint), request);
    while (status == compiler::Status::Unavailable && clock.elapsed() < WORKER_START_MILLISECONDS &&
           cancel.loadRelaxed() == 0)
    {
        QThread::msleep(WORKER_RETRY_MILLISECONDS);
        status = client.Submit(ToView(mEndpoint), request);
    }
    compiler::Result result;
    while (status == compiler::Status::Pending)
    {
        if (cancel.loadRelaxed() != 0 || clock.elapsed() > COMPILE_TIMEOUT_MILLISECONDS)
        {
            client.Cancel();
            status = cancel.loadRelaxed() != 0 ? compiler::Status::InvalidState : compiler::Status::TransportFailed;
            break;
        }
        status = client.Poll(result);
        if (status == compiler::Status::Pending)
        {
            QThread::msleep(WORKER_POLL_MILLISECONDS);
        }
    }
    if (status != compiler::Status::Ready && status != compiler::Status::CompileFailed)
    {
        result = compiler::Result{};
        result.Identity = request.Identity;
    }
    result.Outcome = status;
    outResult = Move(result);
    return status;
}

PreviewShaderBuilder::PreviewShaderBuilder(PreviewShaderSources sources, compiler::Target target, QString validatorPath)
    : mSources(Move(sources)), mTarget(target), mValidatorPath(Move(validatorPath))
{
}

qsizetype PreviewShaderBuilder::findStage(const QString& source, const QByteArray& entry, PreviewShaderStage kind)
{
    for (qsizetype i = 0; i < mStages.size(); ++i)
    {
        if (mStages[i].Source == source && mStages[i].Entry == entry && mStages[i].Kind == kind)
        {
            return i;
        }
    }
    Stage stage;
    stage.Source = source;
    stage.Entry = entry;
    stage.Kind = kind;
    mStages.append(stage);
    return mStages.size() - 1;
}

bool PreviewShaderBuilder::needsCompile(const Stage& stage, const QHash<QString, QByteArray>& hashes) const
{
    if (stage.HasOutput == false)
    {
        return true;
    }
    for (QHash<QString, QByteArray>::const_iterator it = stage.Reads.constBegin(); it != stage.Reads.constEnd(); ++it)
    {
        if (hashes.value(it.key()) != it.value())
        {
            return true;
        }
    }
    return false;
}

bool PreviewShaderBuilder::validateSpirv(const QList<uint32>& words, QString& outError) const
{
    if (mValidatorPath.isEmpty() == true)
    {
        return true;
    }
    QTemporaryFile file;
    if (file.open() == false || file.write(reinterpret_cast<const char*>(words.constData()),
                                           words.size() * static_cast<qsizetype>(sizeof(uint32))) !=
                                    words.size() * static_cast<qsizetype>(sizeof(uint32)))
    {
        outError = QStringLiteral("Cannot stage SPIR-V for validation.");
        return false;
    }
    file.close();
    QProcess validator;
    validator.setProcessChannelMode(QProcess::MergedChannels);
    validator.start(mValidatorPath, {QStringLiteral("--target-env"), QStringLiteral("vulkan1.1"), file.fileName()});
    if (validator.waitForFinished(VALIDATOR_TIMEOUT_MILLISECONDS) == false)
    {
        validator.kill();
        static_cast<void>(validator.waitForFinished());
        outError = QStringLiteral("spirv-val did not finish: %1").arg(validator.errorString());
        return false;
    }
    if (validator.exitStatus() != QProcess::NormalExit || validator.exitCode() != 0)
    {
        outError = QStringLiteral("spirv-val rejected the compiled shader: %1")
                       .arg(QString::fromUtf8(validator.readAll()).trimmed());
        return false;
    }
    return true;
}

bool PreviewShaderBuilder::compileStage(Stage& inoutStage,
                                        const PreviewShaderSnapshot& snapshot,
                                        PreviewShaderCompiler& inoutCompiler,
                                        const QAtomicInt& cancel,
                                        PreviewShaderBuildResult& inoutResult)
{
    QList<QByteArray> names;
    names.reserve(snapshot.Names.size());
    for (const QString& name : snapshot.Names)
    {
        names.append(name.toUtf8());
    }
    Array<compiler::SourceFile> files;
    if (files.TryResize(static_cast<usize>(names.size())) == false)
    {
        inoutResult.Status = PreviewShaderBuildStatus::Unavailable;
        inoutResult.Message = QStringLiteral("Out of memory while staging shader sources.");
        return false;
    }
    for (qsizetype i = 0; i < names.size(); ++i)
    {
        files[static_cast<usize>(i)] = {ToView(names[i]), ToView(snapshot.Texts[i])};
    }
    const QByteArray source = inoutStage.Source.toUtf8();
    compiler::Request request;
    request.Identity = static_cast<uint64>(inoutResult.CompiledStages) + 1;
    request.Files = {files.GetData(), files.GetSize()};
    request.Source = ToView(source);
    request.Entry = ToView(inoutStage.Entry);
    request.OutputTarget = mTarget;
    request.EntryStage = ToCompilerStage(inoutStage.Kind);
    compiler::Result result;
    const compiler::Status status = inoutCompiler.Compile(request, result, cancel);
    ++inoutResult.CompiledStages;
    if (cancel.loadRelaxed() != 0)
    {
        inoutResult.Status = PreviewShaderBuildStatus::Cancelled;
        return false;
    }
    if (status == compiler::Status::CompileFailed)
    {
        inoutResult.Status = PreviewShaderBuildStatus::CompileFailed;
        inoutResult.Diagnostic =
            ParsePreviewShaderDiagnostic(QByteArray(reinterpret_cast<const char*>(result.Diagnostics.GetData()),
                                                    static_cast<qsizetype>(result.Diagnostics.GetSize())),
                                         snapshot);
        return false;
    }
    if (status != compiler::Status::Ready)
    {
        inoutResult.Status = PreviewShaderBuildStatus::Unavailable;
        inoutResult.Message = QStringLiteral("The shader compiler worker %1.").arg(DescribeStatus(status));
        return false;
    }
    PreviewShaderArtifact artifact;
    QString error;
    if (mTarget == compiler::Target::Spirv)
    {
        if (result.Artifact.GetSize() % sizeof(uint32) != 0)
        {
            error = QStringLiteral("The compiler returned a truncated SPIR-V module.");
        }
        else
        {
            artifact.Spirv.resize(static_cast<qsizetype>(result.Artifact.GetSize() / sizeof(uint32)));
            std::memcpy(artifact.Spirv.data(), result.Artifact.GetData(), result.Artifact.GetSize());
            if (FindSpirvEntry(artifact.Spirv, artifact.Entry) == false)
            {
                error = QStringLiteral("The compiled SPIR-V module has no single entry point.");
            }
            else
            {
                static_cast<void>(validateSpirv(artifact.Spirv, error));
            }
        }
    }
    else
    {
        artifact.Msl = QByteArray(reinterpret_cast<const char*>(result.Artifact.GetData()),
                                  static_cast<qsizetype>(result.Artifact.GetSize()));
        if (FindMetalEntry(artifact.Msl, inoutStage.Kind, artifact.Entry) == false)
        {
            error =
                QStringLiteral("The compiled Metal source has no single %1 entry.").arg(GetStageName(inoutStage.Kind));
        }
    }
    if (error.isEmpty() == false)
    {
        inoutResult.Status = PreviewShaderBuildStatus::CompileFailed;
        inoutResult.Diagnostic = {};
        inoutResult.Diagnostic.Path = QDir(mSources.RecipeDirectory).filePath(inoutStage.Source);
        inoutResult.Diagnostic.Message = error;
        return false;
    }
    QHash<QString, QByteArray> reads;
    const bool hasMask = result.SourceReadMask.GetSize() == static_cast<usize>(snapshot.Names.size());
    for (qsizetype i = 0; i < snapshot.Names.size(); ++i)
    {
        if (hasMask == false || result.SourceReadMask[static_cast<usize>(i)] != 0)
        {
            reads.insert(snapshot.Names[i], HashText(snapshot.Texts[i]));
        }
    }
    inoutStage.HasOutput = true;
    inoutStage.Artifact = artifact;
    inoutStage.Reflection = QByteArray(reinterpret_cast<const char*>(result.Reflection.GetData()),
                                       static_cast<qsizetype>(result.Reflection.GetSize()));
    inoutStage.Reads = reads;
    return true;
}

PreviewShaderBuildResult PreviewShaderBuilder::Build(PreviewShaderCompiler& inoutCompiler, const QAtomicInt& cancel)
{
    PreviewShaderBuildResult result;
    const QString manifest = QDir(mSources.RecipeDirectory).filePath(RECIPE_MANIFEST);
    PreviewShaderRecipes recipes{};
    PreviewShaderSnapshot snapshot;
    if (ReadPreviewShaderRecipes(mSources, recipes, result.Message) == false ||
        CapturePreviewShaderSnapshot(mSources, snapshot, result.Message) == false)
    {
        result.Status = PreviewShaderBuildStatus::Unavailable;
        result.WatchedPaths = {manifest};
        return result;
    }
    result.SourceFiles = static_cast<int32>(snapshot.Names.size());
    result.WatchedPaths = snapshot.Paths;
    result.WatchedPaths.append(manifest);
    QHash<QString, QByteArray> hashes;
    for (qsizetype i = 0; i < snapshot.Names.size(); ++i)
    {
        hashes.insert(snapshot.Names[i], HashText(snapshot.Texts[i]));
    }
    StaticArray<qsizetype, PREVIEW_SHADER_SLOT_COUNT> slots{};
    for (usize program = 0; program < PREVIEW_SHADER_PROGRAM_COUNT; ++program)
    {
        const PreviewShaderRecipe& recipe = recipes[program];
        slots[program * PREVIEW_SHADER_STAGE_COUNT] =
            findStage(recipe.Source, recipe.VertexEntry, PreviewShaderStage::Vertex);
        slots[program * PREVIEW_SHADER_STAGE_COUNT + 1] =
            findStage(recipe.Source, recipe.FragmentEntry, PreviewShaderStage::Fragment);
    }
    QList<bool> isCompiled(mStages.size(), false);
    for (const qsizetype index : slots)
    {
        if (cancel.loadRelaxed() != 0)
        {
            result.Status = PreviewShaderBuildStatus::Cancelled;
            return result;
        }
        if (isCompiled[index] == true || needsCompile(mStages[index], hashes) == false)
        {
            continue;
        }
        isCompiled[index] = true;
        if (compileStage(mStages[index], snapshot, inoutCompiler, cancel, result) == false)
        {
            return result;
        }
    }
    const bool isMetal = mTarget == compiler::Target::Metal;
    PreviewShaderSet shaders;
    for (usize slot = 0; slot < PREVIEW_SHADER_SLOT_COUNT; ++slot)
    {
        const PreviewShaderProgram program = static_cast<PreviewShaderProgram>(slot / PREVIEW_SHADER_STAGE_COUNT);
        const PreviewShaderStage kind = static_cast<PreviewShaderStage>(slot % PREVIEW_SHADER_STAGE_COUNT);
        const Stage& stage = mStages[slots[slot]];
        const rhi::RasterShaderDescription builtIn = GetBuiltInPreviewShader(program, kind);
        const bool isBuiltIn =
            isMetal == true
                ? QByteArrayView(builtIn.Artifact.Msl.data(), static_cast<qsizetype>(builtIn.Artifact.Msl.size())) ==
                      QByteArrayView(stage.Artifact.Msl)
                : builtIn.Artifact.Spirv.size() == static_cast<usize>(stage.Artifact.Spirv.size()) &&
                      std::memcmp(builtIn.Artifact.Spirv.data(),
                                  stage.Artifact.Spirv.constData(),
                                  builtIn.Artifact.Spirv.size_bytes()) == 0;
        if (isBuiltIn == true)
        {
            continue;
        }
        QString reason;
        if (IsPreviewShaderInterfaceCompatible(stage.Reflection, kind, isMetal, builtIn, reason) == false)
        {
            result.Status = PreviewShaderBuildStatus::RestartRequired;
            result.Message = QStringLiteral("%1 (%2 %3): %4")
                                 .arg(stage.Source,
                                      QString::fromLatin1(GetPreviewShaderRecipeName(program)),
                                      GetStageName(kind),
                                      reason);
            return result;
        }
        shaders.SetArtifact(program, kind, stage.Artifact);
    }
    result.Status = PreviewShaderBuildStatus::Ready;
    result.Shaders = shaders;
    return result;
}
} // namespace ludus::editor
