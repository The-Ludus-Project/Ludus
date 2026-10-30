// Native port of the retired scripts/python/ludus_tools/scene_cook.py.
//
// Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, pbrt-v4 Input File
// Format (https://pbrt.org/fileformat-v4), Transformations, Cameras, Materials
// and Shapes, for camera-from-world and scoped graphics-state semantics.
// This is an original offline converter, not a PBRT renderer or copied parser.
//
// Every arithmetic step mirrors the CPython 3.12 reference in float64 order,
// including Neumaier-compensated sum(), so outputs are byte-identical.

#include <ludus/content/scene_cook/limits.h>
#include <ludus/content/scene_cook/scene_cook.h>
#include <ludus/content/scene_format/limits.h>
#include <ludus/content/scene_format/luscn.h>

#include "native_files.h"
#include "python_compat.h"

#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/summation.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/parsing/parsing.hpp>
#include <ludus/foundation/strings/string.hpp>

#include <bit>
#include <iterator>

namespace ludus::content::scene_cook
{
namespace
{
using foundation::Array;
using foundation::String;
using foundation::StringStatus;
using namespace detail;
namespace format = content::scene_format;

constexpr usize LEGACY_QUAD_MESH = 4;
constexpr usize LEGACY_DISK_MESH = 5;
constexpr int64 LEGACY_REFERENCE_MESH_COUNT = 6;
constexpr usize AFFINE_SECOND_DIAGONAL = 4;
constexpr usize AFFINE_THIRD_DIAGONAL = 8;
constexpr std::string_view COMMANDS[] = {
    "Integrator",
    "Transform",
    "Sampler",
    "PixelFilter",
    "Film",
    "Camera",
    "WorldBegin",
    "Texture",
    "MakeNamedMaterial",
    "NamedMaterial",
    "Material",
    "Shape",
    "AreaLightSource",
    "AttributeBegin",
    "AttributeEnd",
    "TransformBegin",
    "TransformEnd",
    "WorldEnd",
};

[[nodiscard]] bool IsCommand(std::string_view token) noexcept
{
    for (const std::string_view command : COMMANDS)
    {
        if (command == token)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::string_view ViewAsText(const Array<uint8>& bytes) noexcept
{
    return {reinterpret_cast<const char*>(bytes.GetData()), bytes.GetSize()};
}

[[nodiscard]] bool EndsWith(std::string_view text, std::string_view suffix) noexcept
{
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// ---------------------------------------------------------------------------
// Failure plumbing.

struct Failure final
{
    CookError& Error;
    bool HasFailed = false;

    bool Fail(Status status, std::string_view message, std::string_view detail = {}) noexcept
    {
        if (HasFailed == false)
        {
            HasFailed = true;
            Error.Result = status;
            // Truncate message + detail to the shared MessageText capacity.
            char text[MAX_MESSAGE_BYTES + 1]{};
            usize at = 0;
            for (const std::string_view part : {message, detail})
            {
                for (usize i = 0; i < part.size() && at < MAX_MESSAGE_BYTES; ++i)
                {
                    text[at++] = part[i];
                }
            }
            (void)Error.Message.Set(std::string_view(text, at));
        }
        return false;
    }
    bool FailInvalid(std::string_view message, std::string_view detail = {}) noexcept
    {
        return Fail(Status::Invalid, message, detail);
    }
    bool FailOutOfMemory() noexcept
    {
        return Fail(Status::OutOfMemory, "out of memory");
    }
};

// ---------------------------------------------------------------------------
// Parsed scene description.

struct Argument final
{
    bool IsList = false;
    String Text;
    Array<String> Items;
};

struct Parameter final
{
    String Key;
    Array<String> Values;
    bool IsAssigned = false;
};

struct Command final
{
    std::string_view Name;
    Array<Argument> Names;
    Array<Parameter> Params;
};

struct MaterialReference final
{
    usize CommandIndex = 0;
    bool IsInline = false; // Material directive: "string type" comes from the names.
};

struct State final
{
    math::Matrix4d Transform = math::Matrix4d::Identity();
    int64 Material = -1;
    bool HasEmission = false;
    float64 Emission[3]{};
    std::string_view Name;
    uint32 SourceIdentity = 0;
    uint64 EmissionSourceIdentity = 0;
    math::Vector3d Reflectance{1, 1, 1};
    format::MaterialScattering Scattering{};
};

struct Shape final
{
    State Graphics;
    usize CommandIndex = 0;
};

struct TextureDeclaration final
{
    std::string_view Name;
    std::string_view File;
};

struct NamedMaterialSlot final
{
    std::string_view Name;
    usize Slot = 0; // Index into Scene::Materials.
};

struct Scene final
{
    Array<Command> Commands;
    Array<TextureDeclaration> Textures;
    Array<NamedMaterialSlot> NamedMaterials;
    Array<MaterialReference> Materials; // Named and inline materials in directive order.
    Array<Shape> Shapes;
    math::Matrix4d Camera;
    float64 FieldOfView = 0.0;
};

[[nodiscard]] const Parameter* FindParameter(const Command& command, std::string_view key) noexcept
{
    for (const Parameter& parameter : command.Params)
    {
        if (parameter.Key.GetView() == key)
        {
            return &parameter;
        }
    }
    return nullptr;
}

[[nodiscard]] bool
AreNamesEqual(const Array<Argument>& names, usize first, std::initializer_list<std::string_view> expected) noexcept
{
    if (names.GetSize() < first || names.GetSize() - first != expected.size())
    {
        return false;
    }
    usize index = first;
    for (const std::string_view value : expected)
    {
        if (names[index].IsList == true || names[index].Text.GetView() != value)
        {
            return false;
        }
        ++index;
    }
    return true;
}

[[nodiscard]] bool HasSingleValue(const Parameter* parameterOrNull, std::string_view value) noexcept
{
    return parameterOrNull != nullptr && parameterOrNull->Values.GetSize() == 1 &&
           parameterOrNull->Values[0].GetView() == value;
}

// ---------------------------------------------------------------------------
// Cooking context.

struct ReadRecord final
{
    String Path;
    Digest Hash;
};

struct CookContext final
{
    CookContext(Failure& inoutErrors, const Cancellation* cancelOrNull) noexcept
        : Errors(inoutErrors), CancelOrNull(cancelOrNull)
    {
    }

    Failure& Errors;
    const Cancellation* CancelOrNull = nullptr;
    String Root;
    Array<ReadRecord> Reads;

    [[nodiscard]] bool CheckCancellation() noexcept
    {
        if (CancelOrNull != nullptr && CancelOrNull->Requested != nullptr &&
            CancelOrNull->Requested(CancelOrNull->ContextOrNull) == true)
        {
            return Errors.Fail(Status::Cancelled, "Scene conversion cancelled");
        }
        return true;
    }

    // asset_path(): reject absolute and parent segments; normalise like pathlib.
    [[nodiscard]] bool ResolveAssetPath(std::string_view name, String& outPath) noexcept
    {
        if (name.empty() == false && name.front() == '/')
        {
            return Errors.FailInvalid("asset path escapes source directory");
        }
        if (outPath.TryAssign("") != StringStatus::Ok)
        {
            return Errors.FailOutOfMemory();
        }
        usize begin = 0;
        for (usize i = 0; i <= name.size(); ++i)
        {
            if (i != name.size() && name[i] != '/')
            {
                continue;
            }
            const std::string_view part = name.substr(begin, i - begin);
            begin = i + 1;
            if (part.empty() == true || part == ".")
            {
                continue;
            }
            if (part == "..")
            {
                return Errors.FailInvalid("asset path escapes source directory");
            }
            if ((outPath.GetSize() != 0 && outPath.TryAppend("/") != StringStatus::Ok) ||
                outPath.TryAppend(part) != StringStatus::Ok)
            {
                return Errors.FailOutOfMemory();
            }
        }
        if (outPath.GetSize() == 0)
        {
            return Errors.Fail(Status::IoError, "Is a directory: ", name);
        }
        return true;
    }

    // bounded_bytes(): bounded read plus repeated-read conflict detection.
    [[nodiscard]] bool ReadBounded(std::string_view relative, usize maxBytes, Bytes& outBytes) noexcept
    {
        if (CheckCancellation() == false)
        {
            return false;
        }
        Bytes data;
        const Status status = ReadFile(Root.GetView(), relative, maxBytes, data);
        switch (status)
        {
            case Status::Ok:
                break;
            case Status::Limit:
                return Errors.Fail(Status::Limit, "source file byte limit");
            case Status::NotFound:
                return Errors.Fail(Status::NotFound, "No such file or directory: ", relative);
            case Status::Invalid:
                return Errors.FailInvalid("unsupported source path (symlinks and reserved characters are rejected): ",
                                          relative);
            case Status::OutOfMemory:
                return Errors.FailOutOfMemory();
            case Status::Conflict:
                return Errors.Fail(Status::Conflict, "Source changed during conversion: ", GetBaseName(relative));
            default:
                return Errors.Fail(Status::IoError, "cannot read source file: ", relative);
        }
        const Digest digest = Hash(data.Data());
        for (const ReadRecord& record : Reads)
        {
            if (record.Path.GetView() == relative)
            {
                if ((record.Hash == digest) == false)
                {
                    return Errors.Fail(Status::Conflict, "Source changed during conversion: ", GetBaseName(relative));
                }
                outBytes = static_cast<Bytes&&>(data);
                return true;
            }
        }
        ReadRecord record;
        record.Hash = digest;
        if (record.Path.TryAssign(relative) != StringStatus::Ok ||
            Reads.TryAdd(static_cast<ReadRecord&&>(record)) == false)
        {
            return Errors.FailOutOfMemory();
        }
        outBytes = static_cast<Bytes&&>(data);
        return true;
    }

    // finite(): float() every token and reject NaN/infinity.
    [[nodiscard]] bool ParseFiniteValues(std::span<const std::string_view> tokens, Array<float64>& outValues) noexcept
    {
        switch (ParseFloats(tokens, outValues))
        {
            case NumberResult::Ok:
                return true;
            case NumberResult::Nonfinite:
                return Errors.FailInvalid("nonfinite source number");
            case NumberResult::OutOfMemory:
                return Errors.FailOutOfMemory();
            default:
                return Errors.FailInvalid("could not convert string to float");
        }
    }

    [[nodiscard]] bool ParseFiniteValues(const Array<String>& values, Array<float64>& outValues) noexcept
    {
        Array<std::string_view> views;
        if (views.TryResize(values.GetSize()) == false)
        {
            return Errors.FailOutOfMemory();
        }
        for (usize i = 0; i < values.GetSize(); ++i)
        {
            views[i] = values[i].GetView();
        }
        return ParseFiniteValues(views.AsSpan(), outValues);
    }

    [[nodiscard]] bool
    ParseFiniteParameter(const Parameter* parameterOrNull, Array<float64>& outValues, std::string_view key) noexcept
    {
        if (parameterOrNull == nullptr)
        {
            return Errors.FailInvalid("missing parameter ", key);
        }
        return ParseFiniteValues(parameterOrNull->Values, outValues);
    }
};

// ---------------------------------------------------------------------------
// parse_scene().

[[nodiscard]] uint32 DecodeCodePointAt(std::string_view text, usize at, usize& outLength) noexcept
{
    const auto lead = static_cast<uint8>(text[at]);
    if (lead < 0x80 || at + 1 >= text.size())
    {
        outLength = 1;
        return lead;
    }
    if (lead >= 0xF0 && at + 3 < text.size())
    {
        outLength = 4;
        return ((lead & 0x07U) << 18) | ((static_cast<uint8>(text[at + 1]) & 0x3FU) << 12) |
               ((static_cast<uint8>(text[at + 2]) & 0x3FU) << 6) | (static_cast<uint8>(text[at + 3]) & 0x3FU);
    }
    if (lead >= 0xE0 && at + 2 < text.size())
    {
        outLength = 3;
        return ((lead & 0x0FU) << 12) | ((static_cast<uint8>(text[at + 1]) & 0x3FU) << 6) |
               (static_cast<uint8>(text[at + 2]) & 0x3FU);
    }
    outLength = 2;
    return ((lead & 0x1FU) << 6) | (static_cast<uint8>(text[at + 1]) & 0x3FU);
}

// re.findall(r'"(?:[^"\\]|\\.)*"|\[|\]|[^\s\[\]"]+', text) over decoded code points.
[[nodiscard]] bool Tokenize(std::string_view text, Array<std::string_view>& outTokens) noexcept
{
    usize at = 0;
    while (at < text.size())
    {
        const char value = text[at];
        if (value == '"')
        {
            usize scan = at + 1;
            bool isClosed = false;
            while (scan < text.size())
            {
                if (text[scan] == '"')
                {
                    isClosed = true;
                    break;
                }
                if (text[scan] == '\\')
                {
                    if (scan + 1 >= text.size() || text[scan + 1] == '\n')
                    {
                        break;
                    }
                    scan += 2;
                    continue;
                }
                ++scan;
            }
            if (isClosed == true)
            {
                if (outTokens.TryAdd(text.substr(at, scan + 1 - at)) == false)
                {
                    return false;
                }
                at = scan + 1;
            }
            else
            {
                ++at; // No alternative matches an unterminated quote; findall skips it.
            }
            continue;
        }
        if (value == '[' || value == ']')
        {
            if (outTokens.TryAdd(text.substr(at, 1)) == false)
            {
                return false;
            }
            ++at;
            continue;
        }
        usize length = 1;
        if (IsPythonSpace(DecodeCodePointAt(text, at, length)) == true)
        {
            at += length;
            continue;
        }
        const usize begin = at;
        while (at < text.size() && text[at] != '"' && text[at] != '[' && text[at] != ']')
        {
            if (IsPythonSpace(DecodeCodePointAt(text, at, length)) == true)
            {
                break;
            }
            at += length;
        }
        if (outTokens.TryAdd(text.substr(begin, at - begin)) == false)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool DecodeValue(CookContext& inoutContext, std::string_view token, String& outValue) noexcept
{
    if (token.empty() == false && token.front() == '"')
    {
        if (DecodeJsonString(token, outValue) == false)
        {
            return inoutContext.Errors.FailInvalid("invalid string literal");
        }
        return true;
    }
    return outValue.TryAssign(token) == StringStatus::Ok || inoutContext.Errors.FailOutOfMemory() == true;
}

struct AllowedParams final
{
    std::string_view Command;
    std::initializer_list<std::string_view> Keys;
};

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] bool IsParameterAllowed(std::string_view command, std::string_view key) noexcept
{
    static const AllowedParams TABLE[] = {
        {"Integrator", {"integer maxdepth", "string lightsampler", "float trainingBudgetRatio"}},
        {"Sampler", {"integer pixelsamples"}},
        {"Film", {"string filename", "integer xresolution", "integer yresolution"}},
        {"Camera", {"float fov"}},
        {"Texture", {"string filename", "string filter"}},
        {"MakeNamedMaterial",
         {"string type",
          "rgb reflectance",
          "texture reflectance",
          "float uroughness",
          "float vroughness",
          "bool remaproughness",
          "rgb eta",
          "rgb k"}},
        {"Material", {"rgb reflectance"}},
        {"AreaLightSource", {"rgb L"}},
        {"Shape", {"string filename", "float radius", "point3 P", "normal N", "point2 uv", "integer indices"}},
    };
    for (const AllowedParams& entry : TABLE)
    {
        if (entry.Command == command)
        {
            for (const std::string_view allowed : entry.Keys)
            {
                if (allowed == key)
                {
                    return true;
                }
            }
            return false;
        }
    }
    return false;
}

[[nodiscard]] bool AppendDecoded(CookContext& inoutContext, std::string_view token, Array<String>& inoutValues) noexcept
{
    String value;
    if (DecodeValue(inoutContext, token, value) == false)
    {
        return false;
    }
    return inoutValues.TryAdd(static_cast<String&&>(value)) == true || inoutContext.Errors.FailOutOfMemory() == true;
}

[[nodiscard]] bool
ParseCommands(CookContext& inoutContext, std::span<const std::string_view> tokens, Scene& inoutScene) noexcept
{
    Failure& errors = inoutContext.Errors;
    usize i = 0;
    while (i < tokens.size())
    {
        const std::string_view name = tokens[i++];
        if (IsCommand(name) == false)
        {
            return errors.FailInvalid("unsupported directive ", name);
        }
        Command command;
        command.Name = name;
        Array<Argument> arguments;
        while (i < tokens.size() && IsCommand(tokens[i]) == false)
        {
            Argument argument;
            if (tokens[i] == "[")
            {
                ++i;
                argument.IsList = true;
                while (i < tokens.size() && tokens[i] != "]")
                {
                    String item;
                    if (item.TryAssign(tokens[i]) != StringStatus::Ok ||
                        argument.Items.TryAdd(static_cast<String&&>(item)) == false)
                    {
                        return errors.FailOutOfMemory();
                    }
                    ++i;
                }
                if (i == tokens.size())
                {
                    return errors.FailInvalid("unterminated array");
                }
                ++i;
            }
            else
            {
                if (argument.Text.TryAssign(tokens[i]) != StringStatus::Ok)
                {
                    return errors.FailOutOfMemory();
                }
                ++i;
            }
            if (arguments.TryAdd(static_cast<Argument&&>(argument)) == false)
            {
                return errors.FailOutOfMemory();
            }
        }
        for (Argument& argument : arguments)
        {
            const usize parameterCount = command.Params.GetSize();
            if (parameterCount != 0 && command.Params[parameterCount - 1].IsAssigned == false)
            {
                Parameter& parameter = command.Params[parameterCount - 1];
                parameter.IsAssigned = true;
                if (argument.IsList == true)
                {
                    for (const String& item : argument.Items)
                    {
                        if (AppendDecoded(inoutContext, item.GetView(), parameter.Values) == false)
                        {
                            return false;
                        }
                    }
                }
                else if (AppendDecoded(inoutContext, argument.Text.GetView(), parameter.Values) == false)
                {
                    return false;
                }
            }
            else if (argument.IsList == false && argument.Text.GetView().empty() == false &&
                     argument.Text.GetView().front() == '"')
            {
                String value;
                if (DecodeValue(inoutContext, argument.Text.GetView(), value) == false)
                {
                    return false;
                }
                if (value.GetView().find(' ') == std::string_view::npos && parameterCount == 0)
                {
                    Argument decoded;
                    decoded.Text = static_cast<String&&>(value);
                    if (command.Names.TryAdd(static_cast<Argument&&>(decoded)) == false)
                    {
                        return errors.FailOutOfMemory();
                    }
                }
                else
                {
                    if (FindParameter(command, value.GetView()) != nullptr)
                    {
                        return errors.FailInvalid("duplicate parameter");
                    }
                    Parameter parameter;
                    parameter.Key = static_cast<String&&>(value);
                    if (command.Params.TryAdd(static_cast<Parameter&&>(parameter)) == false)
                    {
                        return errors.FailOutOfMemory();
                    }
                }
            }
            else if (command.Names.TryAdd(static_cast<Argument&&>(argument)) == false)
            {
                return errors.FailOutOfMemory();
            }
        }
        for (const Parameter& parameter : command.Params)
        {
            if (parameter.IsAssigned == false || IsParameterAllowed(name, parameter.Key.GetView()) == false)
            {
                return errors.FailInvalid("unsupported or missing parameter for ", name);
            }
        }
        if (inoutScene.Commands.TryAdd(static_cast<Command&&>(command)) == false)
        {
            return errors.FailOutOfMemory();
        }
    }
    return true;
}

[[nodiscard]] bool GetFirstName(CookContext& inoutContext, const Command& command, std::string_view& outName) noexcept
{
    if (command.Names.GetSize() == 0)
    {
        return inoutContext.Errors.FailInvalid("list index out of range");
    }
    if (command.Names[0].IsList == true)
    {
        return inoutContext.Errors.FailInvalid("unhashable type: 'list'");
    }
    outName = command.Names[0].Text.GetView();
    return true;
}

[[nodiscard]] bool StripComments(std::string_view text, Array<uint8>& outText) noexcept
{
    // re.sub(r"#[^\n]*", "", text) applies everywhere, including inside strings.
    ByteWriter writer(outText);
    usize at = 0;
    while (at < text.size())
    {
        const usize hash = text.find('#', at);
        if (hash == std::string_view::npos)
        {
            writer.Append(text.substr(at));
            break;
        }
        writer.Append(text.substr(at, hash - at));
        const usize newline = text.find('\n', hash);
        at = newline == std::string_view::npos ? text.size() : newline;
    }
    return writer.IsOk();
}

struct SavedState final
{
    std::string_view Start;
    State Graphics;
};

struct SceneState final
{
    State Current;
    Array<SavedState> Stack;
    bool HasCamera = false;
    bool IsInWorld = false;
    Array<float64> Values;
};

[[nodiscard]] bool ApplyTransform(CookContext& inoutContext, const Command& command, SceneState& inoutState) noexcept
{
    Failure& errors = inoutContext.Errors;
    if (command.Names.GetSize() == 0)
    {
        return errors.FailInvalid("list index out of range");
    }
    if (command.Names[0].IsList == false)
    {
        return errors.FailInvalid("non-affine transform");
    }
    Array<float64>& values = inoutState.Values;
    if (inoutContext.ParseFiniteValues(command.Names[0].Items, values) == false)
    {
        return false;
    }
    if (values.GetSize() != 16 || values[3] != 0.0 || values[7] != 0.0 || values[11] != 0.0 || values[15] != 1.0)
    {
        return errors.FailInvalid("non-affine transform");
    }
    for (usize k = 0; k < 16; ++k)
    {
        inoutState.Current.Transform.Elements[k] = values[k];
    }
    return true;
}

[[nodiscard]] bool
ApplyCamera(CookContext& inoutContext, const Command& command, SceneState& inoutState, Scene& inoutScene) noexcept
{
    Failure& errors = inoutContext.Errors;
    if (AreNamesEqual(command.Names, 0, {"perspective"}) == false)
    {
        return errors.FailInvalid("only perspective camera is supported");
    }
    if (inoutState.HasCamera == true)
    {
        return errors.FailInvalid("duplicate camera");
    }
    inoutScene.Camera = inoutState.Current.Transform;
    if (inoutContext.ParseFiniteParameter(FindParameter(command, "float fov"), inoutState.Values, "float fov") == false)
    {
        return false;
    }
    if (inoutState.Values.GetSize() == 0)
    {
        return errors.FailInvalid("list index out of range");
    }
    inoutScene.FieldOfView = inoutState.Values[0];
    inoutState.HasCamera = true;
    if (inoutScene.FieldOfView <= 0.0 || inoutScene.FieldOfView >= 180.0)
    {
        return errors.FailInvalid("camera field of view");
    }
    return true;
}

[[nodiscard]] bool ApplyTexture(CookContext& inoutContext, const Command& command, Scene& inoutScene) noexcept
{
    Failure& errors = inoutContext.Errors;
    if (AreNamesEqual(command.Names, 1, {"spectrum", "imagemap"}) == false)
    {
        return errors.FailInvalid("unsupported texture");
    }
    std::string_view textureName;
    if (GetFirstName(inoutContext, command, textureName) == false)
    {
        return false;
    }
    bool isDuplicate = false;
    for (const TextureDeclaration& texture : inoutScene.Textures)
    {
        isDuplicate = isDuplicate == true || texture.Name == textureName;
    }
    const Parameter* filter = FindParameter(command, "string filter");
    if (isDuplicate == true || (filter != nullptr && HasSingleValue(filter, "trilinear") == false))
    {
        return errors.FailInvalid("unsupported texture profile");
    }
    const Parameter* file = FindParameter(command, "string filename");
    if (file == nullptr || file->Values.GetSize() == 0)
    {
        return errors.FailInvalid("missing parameter string filename");
    }
    return inoutScene.Textures.TryAdd(TextureDeclaration{textureName, file->Values[0].GetView()}) == true ||
           errors.FailOutOfMemory() == true;
}

[[nodiscard]] bool ApplyMaterial(CookContext& inoutContext,
                                 const Command& command,
                                 usize index,
                                 SceneState& inoutState,
                                 Scene& inoutScene) noexcept
{
    Failure& errors = inoutContext.Errors;
    const std::string_view name = command.Name;
    if (name == "Material")
    {
        if (inoutScene.Materials.TryAdd(MaterialReference{index, true}) == false)
        {
            return errors.FailOutOfMemory();
        }
        inoutState.Current.Material = static_cast<int64>(inoutScene.Materials.GetSize() - 1);
        return true;
    }
    std::string_view materialName;
    if (GetFirstName(inoutContext, command, materialName) == false)
    {
        return false;
    }
    if (name == "MakeNamedMaterial")
    {
        for (const NamedMaterialSlot& existing : inoutScene.NamedMaterials)
        {
            if (existing.Name == materialName)
            {
                return errors.FailInvalid("duplicate material");
            }
        }
        const usize slot = inoutScene.Materials.GetSize();
        return (inoutScene.NamedMaterials.TryAdd(NamedMaterialSlot{materialName, slot}) == true &&
                inoutScene.Materials.TryAdd(MaterialReference{index, false}) == true) ||
               errors.FailOutOfMemory() == true;
    }
    for (const NamedMaterialSlot& named : inoutScene.NamedMaterials)
    {
        if (named.Name == materialName)
        {
            inoutState.Current.Material = static_cast<int64>(named.Slot);
            return true;
        }
    }
    return errors.FailInvalid("unknown material ", materialName);
}

[[nodiscard]] bool ApplyEmitter(CookContext& inoutContext, const Command& command, SceneState& inoutState) noexcept
{
    Failure& errors = inoutContext.Errors;
    if (AreNamesEqual(command.Names, 0, {"diffuse"}) == false)
    {
        return errors.FailInvalid("unsupported emitter");
    }
    if (inoutContext.ParseFiniteParameter(FindParameter(command, "rgb L"), inoutState.Values, "rgb L") == false)
    {
        return false;
    }
    if (inoutState.Values.GetSize() != 3)
    {
        return errors.FailInvalid("emission profile");
    }
    for (usize k = 0; k < 3; ++k)
    {
        if (inoutState.Values[k] < 0.0 || inoutState.Values[k] > 10000.0)
        {
            return errors.FailInvalid("emission profile");
        }
        inoutState.Current.Emission[k] = inoutState.Values[k];
    }
    inoutState.Current.HasEmission = true;
    return true;
}

[[nodiscard]] bool ApplyStack(CookContext& inoutContext, std::string_view name, SceneState& inoutState) noexcept
{
    Failure& errors = inoutContext.Errors;
    if (name == "AttributeBegin" || name == "TransformBegin")
    {
        return inoutState.Stack.TryAdd(SavedState{name, inoutState.Current}) == true ||
               errors.FailOutOfMemory() == true;
    }
    if (inoutState.Stack.GetSize() == 0)
    {
        return errors.FailInvalid("unbalanced state stack");
    }
    const SavedState previous = inoutState.Stack[inoutState.Stack.GetSize() - 1];
    inoutState.Stack.RemoveLast();
    const bool isAttribute = previous.Start == "AttributeBegin";
    if (isAttribute != (name == "AttributeEnd"))
    {
        return errors.FailInvalid("mismatched state stack");
    }
    if (isAttribute == true)
    {
        inoutState.Current = previous.Graphics;
        return true;
    }
    inoutState.Current.Transform = previous.Graphics.Transform;
    return true;
}

[[nodiscard]] bool ParseScene(CookContext& inoutContext, Scene& inoutScene) noexcept
{
    Failure& errors = inoutContext.Errors;
    Bytes raw;
    if (inoutContext.ReadBounded("scene-v4.pbrt", MAX_REFERENCE_DOCUMENT_BYTES, raw) == false)
    {
        return false;
    }
    foundation::parsing::ParseError utf8{};
    if (foundation::parsing::ValidateUtf8(raw.String(), utf8) != foundation::parsing::ParseStatus::Ok)
    {
        return errors.FailInvalid("'utf-8' codec can't decode scene-v4.pbrt");
    }
    Array<uint8> stripped;
    Array<std::string_view> tokens;
    if (StripComments(raw.String(), stripped) == false || Tokenize(ViewAsText(stripped), tokens) == false)
    {
        return errors.FailOutOfMemory();
    }
    if (ParseCommands(inoutContext, tokens.AsSpan(), inoutScene) == false)
    {
        return false;
    }
    SceneState state;
    for (usize index = 0; index < inoutScene.Commands.GetSize(); ++index)
    {
        const Command& command = inoutScene.Commands[index];
        const std::string_view name = command.Name;
        bool isOk = true;
        if (name == "Transform")
        {
            isOk = ApplyTransform(inoutContext, command, state);
        }
        else if (name == "Camera")
        {
            isOk = ApplyCamera(inoutContext, command, state, inoutScene);
        }
        else if (name == "WorldBegin")
        {
            state.IsInWorld = true;
            state.Current.Transform = math::Matrix4d::Identity();
        }
        else if (name == "Texture")
        {
            isOk = ApplyTexture(inoutContext, command, inoutScene);
        }
        else if (name == "MakeNamedMaterial" || name == "NamedMaterial" || name == "Material")
        {
            isOk = ApplyMaterial(inoutContext, command, index, state, inoutScene);
        }
        else if (name == "AreaLightSource")
        {
            isOk = ApplyEmitter(inoutContext, command, state);
        }
        else if (name == "AttributeBegin" || name == "TransformBegin" || name == "AttributeEnd" ||
                 name == "TransformEnd")
        {
            isOk = ApplyStack(inoutContext, name, state);
        }
        else if (name == "Shape")
        {
            if (state.IsInWorld == false || state.Current.Material < 0)
            {
                return errors.FailInvalid("shape outside world or without material");
            }
            isOk = inoutScene.Shapes.TryAdd(Shape{state.Current, index}) == true || errors.FailOutOfMemory() == true;
        }
        else if (name == "WorldEnd")
        {
            state.IsInWorld = false;
        }
        // Integrator, Sampler, PixelFilter and Film are replaced by the documented L6 capture profile.
        if (isOk == false)
        {
            return false;
        }
    }
    if (state.Stack.GetSize() != 0 || state.HasCamera == false || inoutScene.Shapes.GetSize() == 0)
    {
        return errors.FailInvalid("incomplete scene");
    }
    return true;
}

// ---------------------------------------------------------------------------
// Geometry helpers mirroring the reference tuple arithmetic.

[[nodiscard]] float64 GetAxis(const math::Vector3d& vector, usize axis) noexcept
{
    return axis == 0 ? vector.X : axis == 1 ? vector.Y : vector.Z;
}
void SetAxis(math::Vector3d& inoutVector, usize axis, float64 value) noexcept
{
    (axis == 0 ? inoutVector.X : axis == 1 ? inoutVector.Y : inoutVector.Z) = value;
}
[[nodiscard]] bool NormaliseVector(Failure& inoutErrors, const math::Vector3d& a, math::Vector3d& outUnit) noexcept
{
    const float64 length = math::Sqrt(math::CompensatedDot(a, a));
    if (length < 1e-12)
    {
        return inoutErrors.FailInvalid("degenerate geometry");
    }
    outUnit = a * (1.0 / length);
    return true;
}
[[nodiscard]] math::Vector3d TransformVector(const math::Matrix4d& matrix, const math::Vector3d& p) noexcept
{
    math::Vector3d output;
    for (usize r = 0; r < 3; ++r)
    {
        math::CompensatedSum sum;
        for (usize c = 0; c < 3; ++c)
        {
            sum.Add(matrix.Elements[4 * c + r] * GetAxis(p, c));
        }
        SetAxis(output, r, sum.GetResult());
    }
    return output;
}
[[nodiscard]] math::Vector3d TransformPoint(const math::Matrix4d& matrix, const math::Vector3d& p) noexcept
{
    math::Vector3d output = TransformVector(matrix, p);
    for (usize r = 0; r < 3; ++r)
    {
        SetAxis(output, r, GetAxis(output, r) + matrix.Elements[12 + r]);
    }
    return output;
}

struct Point final
{
    math::Vector3d Position;
    math::Vector3d Normal;
    uint8 NormalCount = 0; // 3 for a normal; 0 when absent (Python None or an empty tuple).
    float64 Uv[2]{};
};

struct Triangle final
{
    int64 Indices[3]{};
};

struct Vertex final
{
    math::Vector3d Position;
    math::Vector3d Normal;
    float64 Uv[2]{};
};

struct Mesh final
{
    Array<Vertex> Vertices;
    Array<uint32> Indices;
};

struct Image final
{
    uint32 Width = 0;
    uint32 Height = 0;
    Array<uint8> Rgba;
};

struct Material final
{
    uint32 Texture = 0;
    float64 Metallic = 0.0;
    float64 Roughness = 0.0;
    float64 Emission[3]{};
    std::string_view Name{};
    uint32 SourceIdentity = 0;
    uint64 EmissionSourceIdentity = 0;
    math::Vector3d Reflectance{1, 1, 1};
    format::MaterialScattering Scattering{};
};

struct SceneItem final
{
    uint32 Mesh = 0;
    uint32 Material = 0;
    float64 Transform[format::OBJECT_TRANSFORM_COMPONENTS]{};
};

// ---------------------------------------------------------------------------
// read_ply() and read_tga().

[[nodiscard]] uint32 LoadUint32(const uint8* data) noexcept
{
    return static_cast<uint32>(data[0]) | (static_cast<uint32>(data[1]) << 8) | (static_cast<uint32>(data[2]) << 16) |
           (static_cast<uint32>(data[3]) << 24);
}

[[nodiscard]] bool SplitLines(std::string_view header, Array<std::string_view>& outLines) noexcept
{
    // str.splitlines() boundaries for ASCII text.
    usize begin = 0;
    usize at = 0;
    while (at < header.size())
    {
        const char value = header[at];
        if (value == '\n' || value == '\r' || value == '\x0b' || value == '\x0c' || value == '\x1c' ||
            value == '\x1d' || value == '\x1e')
        {
            if (outLines.TryAdd(header.substr(begin, at - begin)) == false)
            {
                return false;
            }
            at += value == '\r' && at + 1 < header.size() && header[at + 1] == '\n' ? 2 : 1;
            begin = at;
        }
        else
        {
            ++at;
        }
    }
    return begin >= header.size() || outLines.TryAdd(header.substr(begin)) == true;
}

[[nodiscard]] bool SplitFields(std::string_view text, Array<std::string_view>& outFields) noexcept
{
    // str.split() with no separator.
    outFields.Clear();
    usize at = 0;
    while (at < text.size())
    {
        while (at < text.size() && IsPythonSpace(static_cast<uint8>(text[at])) == true)
        {
            ++at;
        }
        const usize begin = at;
        while (at < text.size() && IsPythonSpace(static_cast<uint8>(text[at])) == false)
        {
            ++at;
        }
        if (at > begin && outFields.TryAdd(text.substr(begin, at - begin)) == false)
        {
            return false;
        }
    }
    return true;
}

struct PlyHeader final
{
    Array<std::string_view> Properties;
    int64 Vertices = -1;
    int64 Faces = -1;
    char IndexFormat = 0;
};

[[nodiscard]] bool ParsePlyHeader(Failure& inoutErrors, std::string_view header, PlyHeader& outPly) noexcept
{
    for (const char value : header)
    {
        if (static_cast<uint8>(value) >= 0x80)
        {
            return inoutErrors.FailInvalid("'ascii' codec can't decode PLY header");
        }
    }
    Array<std::string_view> lines;
    if (SplitLines(header, lines) == false)
    {
        return inoutErrors.FailOutOfMemory();
    }
    if (lines.GetSize() < 2 || lines[0] != "ply" || lines[1] != "format binary_little_endian 1.0")
    {
        return inoutErrors.FailInvalid("unsupported PLY encoding");
    }
    std::string_view element;
    bool hasElement = false;
    Array<std::string_view> fields;
    for (usize line = 2; line < lines.GetSize(); ++line)
    {
        if (SplitFields(lines[line], fields) == false)
        {
            return inoutErrors.FailOutOfMemory();
        }
        if (fields.GetSize() == 0 || fields[0] == "comment")
        {
            continue;
        }
        if (fields[0] == "element")
        {
            if (fields.GetSize() < 2)
            {
                return inoutErrors.FailInvalid("list index out of range");
            }
            element = fields[1];
            hasElement = true;
            if (element != "vertex" && element != "face")
            {
                return inoutErrors.FailInvalid("unsupported PLY element");
            }
            int64 count = 0;
            if (fields.GetSize() < 3 || ParseInt(fields[2], count) == false)
            {
                return inoutErrors.FailInvalid("invalid PLY element count");
            }
            (element == "vertex" ? outPly.Vertices : outPly.Faces) = count;
        }
        else if (fields[0] == "property" && hasElement == true && element == "vertex")
        {
            if (fields.GetSize() < 3)
            {
                return inoutErrors.FailInvalid("list index out of range");
            }
            if (fields[1] != "float")
            {
                return inoutErrors.FailInvalid("unsupported vertex property");
            }
            if (outPly.Properties.TryAdd(fields[2]) == false)
            {
                return inoutErrors.FailOutOfMemory();
            }
        }
        else if (fields[0] == "property" && hasElement == true && element == "face")
        {
            if (fields.GetSize() < 3 || fields[1] != "list" || (fields[2] != "uint8" && fields[2] != "uchar"))
            {
                return inoutErrors.FailInvalid("unsupported face list");
            }
            if (fields.GetSize() < 4)
            {
                return inoutErrors.FailInvalid("list index out of range");
            }
            if (fields[3] != "int" && fields[3] != "uint")
            {
                return inoutErrors.FailInvalid("unsupported index type");
            }
            outPly.IndexFormat = fields[3] == "int" ? 'i' : 'I';
        }
    }
    return true;
}

[[nodiscard]] bool ReadPly(CookContext& inoutContext,
                           std::string_view path,
                           Array<Point>& inoutPoints,
                           Array<Triangle>& inoutTriangles) noexcept
{
    Failure& errors = inoutContext.Errors;
    Bytes raw;
    if (inoutContext.ReadBounded(path, MAX_SOURCE_BYTES, raw) == false)
    {
        return false;
    }
    const std::string_view data = raw.String();
    const usize boundary = data.find("end_header\n");
    if (boundary == std::string_view::npos || boundary > 4096)
    {
        return errors.FailInvalid("invalid PLY header");
    }
    PlyHeader ply;
    if (ParsePlyHeader(errors, data.substr(0, boundary), ply) == false)
    {
        return false;
    }
    const std::span<const uint8> payload = raw.Data().subspan(boundary + 11);
    constexpr std::string_view KNOWN[] = {"x", "y", "z", "nx", "ny", "nz", "u", "v", "s", "t"};
    usize slot[10]{};
    bool isPresent[10]{};
    bool isSchemaOk = true;
    for (usize p = 0; p < ply.Properties.GetSize(); ++p)
    {
        bool isKnown = false;
        for (usize k = 0; k < 10; ++k)
        {
            if (ply.Properties[p] == KNOWN[k])
            {
                isKnown = true;
                isSchemaOk = isSchemaOk == true && isPresent[k] == false;
                isPresent[k] = true;
                slot[k] = p;
            }
        }
        isSchemaOk = isSchemaOk == true && isKnown == true;
    }
    if (isSchemaOk == false || isPresent[0] == false || isPresent[1] == false || isPresent[2] == false)
    {
        return errors.FailInvalid("unsupported PLY vertex schema");
    }
    const bool hasAnyNormal = isPresent[3] == true || isPresent[4] == true || isPresent[5] == true;
    const bool hasAllNormals = isPresent[3] == true && isPresent[4] == true && isPresent[5] == true;
    if (hasAnyNormal == true && hasAllNormals == false)
    {
        return errors.FailInvalid("incomplete PLY normals");
    }
    if (ply.Vertices < 0 || ply.Faces < 0 || ply.IndexFormat == 0 || ply.Vertices <= 0 || ply.Vertices > 1000000 ||
        ply.Faces <= 0 || ply.Faces > 1000000)
    {
        return errors.FailInvalid("PLY limits");
    }
    const usize rowSize = 4 * ply.Properties.GetSize();
    usize offset = 0;
    if (inoutPoints.TryResize(static_cast<usize>(ply.Vertices)) == false)
    {
        return errors.FailOutOfMemory();
    }
    float64 row[10]{};
    for (usize index = 0; index < static_cast<usize>(ply.Vertices); ++index)
    {
        if (index % 256 == 0 && inoutContext.CheckCancellation() == false)
        {
            return false;
        }
        if (payload.size() < offset + rowSize)
        {
            return errors.FailInvalid("unpack_from requires a buffer of more bytes");
        }
        for (usize p = 0; p < ply.Properties.GetSize(); ++p)
        {
            row[p] = static_cast<float64>(std::bit_cast<float32>(LoadUint32(payload.data() + offset + 4 * p)));
            if (math::IsFinite(row[p]) == false)
            {
                return errors.FailInvalid("nonfinite source number");
            }
        }
        offset += rowSize;
        Point& point = inoutPoints[index];
        for (usize k = 0; k < 3; ++k)
        {
            SetAxis(point.Position, k, row[slot[k]]);
        }
        point.NormalCount = 0;
        if (hasAllNormals == true)
        {
            point.NormalCount = 3;
            for (usize k = 0; k < 3; ++k)
            {
                SetAxis(point.Normal, k, row[slot[3 + k]]);
            }
        }
        point.Uv[0] = isPresent[6] == true ? row[slot[6]] : isPresent[8] == true ? row[slot[8]] : 0.0;
        point.Uv[1] = 1.0 - (isPresent[7] == true ? row[slot[7]] : isPresent[9] == true ? row[slot[9]] : 0.0);
    }
    for (int64 index = 0; index < ply.Faces; ++index)
    {
        if (index % 256 == 0 && inoutContext.CheckCancellation() == false)
        {
            return false;
        }
        if (offset >= payload.size())
        {
            return errors.FailInvalid("index out of range");
        }
        const uint8 count = payload[offset];
        offset += 1;
        if (count != 3 && count != 4)
        {
            return errors.FailInvalid("only triangle/convex quad faces are supported");
        }
        if (payload.size() < offset + 4 * usize{count})
        {
            return errors.FailInvalid("unpack_from requires a buffer of more bytes");
        }
        int64 indices[4]{};
        for (usize k = 0; k < count; ++k)
        {
            const uint32 bits = LoadUint32(payload.data() + offset + 4 * k);
            indices[k] =
                ply.IndexFormat == 'i' ? static_cast<int64>(static_cast<int32>(bits)) : static_cast<int64>(bits);
        }
        offset += 4 * usize{count};
        for (usize k = 0; k < count; ++k)
        {
            if (indices[k] < 0 || indices[k] >= ply.Vertices)
            {
                return errors.FailInvalid("PLY index range");
            }
        }
        for (usize k = 1; k + 1 < count; ++k)
        {
            if (inoutTriangles.TryAdd(Triangle{{indices[0], indices[k], indices[k + 1]}}) == false)
            {
                return errors.FailOutOfMemory();
            }
        }
    }
    if (offset != payload.size())
    {
        return errors.FailInvalid("unexpected PLY trailing bytes");
    }
    return true;
}

[[nodiscard]] bool ReadTga(CookContext& inoutContext, std::string_view path, Image& outImage) noexcept
{
    Failure& errors = inoutContext.Errors;
    Bytes raw;
    if (inoutContext.ReadBounded(path, MAX_SOURCE_BYTES, raw) == false)
    {
        return false;
    }
    const std::span<const uint8> data = raw.Data();
    if (data.size() < 18)
    {
        return errors.FailInvalid("unpack_from requires a buffer of at least 18 bytes");
    }
    const usize identifierLength = data[0];
    const uint8 colorMapType = data[1];
    const uint8 kind = data[2];
    const usize width = static_cast<usize>(data[12]) | (static_cast<usize>(data[13]) << 8);
    const usize height = static_cast<usize>(data[14]) | (static_cast<usize>(data[15]) << 8);
    const uint8 bits = data[16];
    const uint8 flags = data[17];
    if (colorMapType != 0 || kind != 2 || (bits != 24 && bits != 32) || (flags & 0xC0) != 0 || width == 0 ||
        width > format::MAX_TEXTURE_DIMENSION || height == 0 || height > format::MAX_TEXTURE_DIMENSION)
    {
        return errors.FailInvalid("only bounded uncompressed true-color TGA is supported");
    }
    if (width * height * format::TEXEL_BYTES > format::MAX_TEXTURE_BYTES)
    {
        return errors.FailInvalid("decoded texture byte limit");
    }
    const usize step = bits / 8;
    const usize offset = 18 + identifierLength;
    if (data.size() < offset + width * height * step)
    {
        return errors.FailInvalid("truncated TGA");
    }
    if (outImage.Rgba.TryResize(width * height * format::TEXEL_BYTES) == false)
    {
        return errors.FailOutOfMemory();
    }
    for (usize y = 0; y < height; ++y)
    {
        for (usize x = 0; x < width; ++x)
        {
            const usize sourceX = (flags & 0x10) != 0 ? width - 1 - x : x;
            const usize sourceY = (flags & 0x20) != 0 ? y : height - 1 - y;
            const usize index = offset + (sourceY * width + sourceX) * step;
            uint8* out = outImage.Rgba.GetData() + (y * width + x) * 4;
            out[0] = data[index + 2];
            out[1] = data[index + 1];
            out[2] = data[index];
            out[3] = step == 4 ? data[index + 3] : 255;
        }
    }
    for (usize i = 3; i < outImage.Rgba.GetSize(); i += 4)
    {
        if (outImage.Rgba[i] != 255)
        {
            return errors.FailInvalid("alpha requires a separate conversion profile");
        }
    }
    outImage.Width = static_cast<uint32>(width);
    outImage.Height = static_cast<uint32>(height);
    return true;
}

// ---------------------------------------------------------------------------
// rectangle(), clip() and atlas_triangles().

[[nodiscard]] bool ComputeRectangle(Failure& inoutErrors,
                                    const Array<Point>& points,
                                    const Array<Triangle>& triangles,
                                    float64 (&outCorners)[12]) noexcept
{
    // min()/max() keep the first of equal values, which preserves the sign of zeroes.
    math::Vector3d low = points[0].Position;
    math::Vector3d high = points[0].Position;
    for (usize i = 0; i < 3; ++i)
    {
        for (const Point& point : points)
        {
            SetAxis(low,
                    i,
                    GetAxis(point.Position, i) < GetAxis(low, i) ? GetAxis(point.Position, i) : GetAxis(low, i));
            SetAxis(high,
                    i,
                    GetAxis(point.Position, i) > GetAxis(high, i) ? GetAxis(point.Position, i) : GetAxis(high, i));
        }
    }
    usize axes[3]{};
    usize axisCount = 0;
    for (usize i = 0; i < 3; ++i)
    {
        if (GetAxis(high, i) - GetAxis(low, i) > 1e-5)
        {
            axes[axisCount++] = i;
        }
    }
    if (axisCount != 2)
    {
        return inoutErrors.FailInvalid("emissive PLY is not an axis-aligned planar rectangle");
    }
    const Triangle& first = triangles[0];
    const math::Vector3d& origin = points[static_cast<usize>(first.Indices[0])].Position;
    math::Vector3d normal;
    if (NormaliseVector(inoutErrors,
                        math::Cross((points[static_cast<usize>(first.Indices[1])].Position - origin),
                                    (points[static_cast<usize>(first.Indices[2])].Position - origin)),
                        normal) == false)
    {
        return false;
    }
    math::Vector3d u;
    math::Vector3d v;
    SetAxis(u, axes[0], (GetAxis(high, axes[0]) - GetAxis(low, axes[0])) / 2.0);
    SetAxis(v, axes[1], (GetAxis(high, axes[1]) - GetAxis(low, axes[1])) / 2.0);
    if (math::CompensatedDot(math::Cross(u, v), normal) < 0.0)
    {
        v = (v * -1.0);
    }
    math::CompensatedSum area;
    for (const Triangle& triangle : triangles)
    {
        const math::Vector3d& a = points[static_cast<usize>(triangle.Indices[0])].Position;
        const math::Vector3d faceNormal = math::Cross((points[static_cast<usize>(triangle.Indices[1])].Position - a),
                                                      (points[static_cast<usize>(triangle.Indices[2])].Position - a));
        area.Add(math::Sqrt(math::CompensatedDot(faceNormal, faceNormal)) / 2.0);
    }
    const float64 total = area.GetResult();
    const float64 tolerance = 1e-5 >= total * 1e-4 ? 1e-5 : total * 1e-4;
    if (math::Abs(total - 4.0 * math::Sqrt(math::CompensatedDot(u, u) * math::CompensatedDot(v, v))) > tolerance)
    {
        return inoutErrors.FailInvalid("emitter is not a complete rectangle");
    }
    for (usize i = 0; i < 3; ++i)
    {
        outCorners[i] = GetAxis(u, i);
        outCorners[3 + i] = GetAxis(v, i);
        outCorners[6 + i] = GetAxis(normal, i);
        outCorners[9 + i] = (GetAxis(low, i) + GetAxis(high, i)) / 2.0;
    }
    return true;
}

[[nodiscard]] float64 GetComponent(const Vertex& vertex, usize index) noexcept
{
    return index < 3   ? GetAxis(vertex.Position, index)
           : index < 6 ? GetAxis(vertex.Normal, index - 3)
                       : vertex.Uv[index - 6];
}

void SetComponent(Vertex& inoutVertex, usize index, float64 value) noexcept
{
    if (index < 3)
    {
        SetAxis(inoutVertex.Position, index, value);
    }
    else if (index < 6)
    {
        SetAxis(inoutVertex.Normal, index - 3, value);
    }
    else
    {
        inoutVertex.Uv[index - 6] = value;
    }
}

struct ClipPlane final
{
    usize Axis = 0;
    float64 Limit = 0.0;
    bool IsGreater = false;
};

[[nodiscard]] bool Clip(const Array<Vertex>& polygon, const ClipPlane& plane, Array<Vertex>& outPolygon) noexcept
{
    outPolygon.Clear();
    const usize count = polygon.GetSize();
    for (usize i = 0; i < count; ++i)
    {
        const Vertex& previous = polygon[(i + count - 1) % count];
        const Vertex& current = polygon[i];
        const float64 a = previous.Uv[plane.Axis];
        const float64 b = current.Uv[plane.Axis];
        const bool isPreviousInside = plane.IsGreater == true ? a >= plane.Limit : a <= plane.Limit;
        const bool isCurrentInside = plane.IsGreater == true ? b >= plane.Limit : b <= plane.Limit;
        if (isPreviousInside != isCurrentInside)
        {
            const float64 t = (plane.Limit - a) / (b - a);
            Vertex mixed;
            for (usize k = 0; k < format::VERTEX_COMPONENTS; ++k)
            {
                const float64 x = GetComponent(previous, k);
                const float64 y = GetComponent(current, k);
                SetComponent(mixed, k, x + t * (y - x));
            }
            if (outPolygon.TryAdd(mixed) == false)
            {
                return false;
            }
        }
        if (isCurrentInside == true && outPolygon.TryAdd(current) == false)
        {
            return false;
        }
    }
    return true;
}

struct AtlasPiece final
{
    Vertex Vertices[3];
};

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] bool BuildAtlasCell(Failure& inoutErrors,
                                  const Vertex (&vertices)[3],
                                  float64 x,
                                  float64 y,
                                  float64 atlasWidth,
                                  float64 atlasHeight,
                                  Array<AtlasPiece>& inoutPieces) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    Array<Vertex> polygon;
    Array<Vertex> next;
    if (polygon.TryAddRange(std::span<const Vertex>(vertices, 3)) == false)
    {
        return inoutErrors.FailOutOfMemory();
    }
    const ClipPlane planes[4] = {{0, x, true}, {0, x + 1.0, false}, {1, y, true}, {1, y + 1.0, false}};
    for (const ClipPlane& plane : planes)
    {
        if (polygon.GetSize() == 0)
        {
            break;
        }
        if (Clip(polygon, plane, next) == false)
        {
            return inoutErrors.FailOutOfMemory();
        }
        Array<Vertex> swap = static_cast<Array<Vertex>&&>(polygon);
        polygon = static_cast<Array<Vertex>&&>(next);
        next = static_cast<Array<Vertex>&&>(swap);
    }
    for (usize i = 1; i + 1 < polygon.GetSize(); ++i)
    {
        const Vertex triangle[3] = {polygon[0], polygon[i], polygon[i + 1]};
        const math::Vector3d geometric =
            math::Cross((triangle[1].Position - triangle[0].Position), (triangle[2].Position - triangle[0].Position));
        if (math::CompensatedDot(geometric, geometric) < 1e-20)
        {
            continue;
        }
        AtlasPiece piece;
        for (usize k = 0; k < 3; ++k)
        {
            piece.Vertices[k] = triangle[k];
            piece.Vertices[k].Uv[0] = (2.0 + (triangle[k].Uv[0] - x) * 512.0) / atlasWidth;
            piece.Vertices[k].Uv[1] = (2.0 + (triangle[k].Uv[1] - y) * 512.0) / atlasHeight;
        }
        if (inoutPieces.TryAdd(piece) == false)
        {
            return inoutErrors.FailOutOfMemory();
        }
    }
    return true;
}

[[nodiscard]] bool BuildAtlasTriangles(Failure& inoutErrors,
                                       const Vertex (&vertices)[3],
                                       float64 atlasWidth,
                                       float64 atlasHeight,
                                       Array<AtlasPiece>& outPieces) noexcept
{
    float64 minimum[2] = {vertices[0].Uv[0], vertices[0].Uv[1]};
    float64 maximum[2] = {vertices[0].Uv[0], vertices[0].Uv[1]};
    for (usize axis = 0; axis < 2; ++axis)
    {
        for (const Vertex& vertex : vertices)
        {
            minimum[axis] = vertex.Uv[axis] < minimum[axis] ? vertex.Uv[axis] : minimum[axis];
            maximum[axis] = vertex.Uv[axis] > maximum[axis] ? vertex.Uv[axis] : maximum[axis];
        }
    }
    const float64 lowX = math::Floor(minimum[0]);
    const float64 lowY = math::Floor(minimum[1]);
    const float64 cellsX = math::Floor(maximum[0]) - lowX + 1.0;
    const float64 cellsY = math::Floor(maximum[1]) - lowY + 1.0;
    if (cellsX * cellsY > 256.0)
    {
        return inoutErrors.FailInvalid("wallpaper repeat clipping limit");
    }
    // The limit above bounds both whole-cell counts to [1, 256]; a NaN count
    // fails every comparison and, as before, admits no cells.
    const usize countX = cellsX >= 1.0 && cellsX <= 256.0 ? static_cast<usize>(cellsX) : 0;
    const usize countY = cellsY >= 1.0 && cellsY <= 256.0 ? static_cast<usize>(cellsY) : 0;
    for (usize cellX = 0; cellX < countX; ++cellX)
    {
        for (usize cellY = 0; cellY < countY; ++cellY)
        {
            const float64 originX = lowX + static_cast<float64>(cellX);
            const float64 originY = lowY + static_cast<float64>(cellY);
            if (BuildAtlasCell(inoutErrors, vertices, originX, originY, atlasWidth, atlasHeight, outPieces) == false)
            {
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// _cook().

struct Color final
{
    float64 Components[3]{};
};

[[nodiscard]] bool IsSameColor(const Color& a, const Color& b) noexcept
{
    return a.Components[0] == b.Components[0] && a.Components[1] == b.Components[1] &&
           a.Components[2] == b.Components[2];
}

struct Cooker final
{
    static constexpr usize ATLAS_WIDTH = 544;
    static constexpr usize ATLAS_HEIGHT = 516;

    Cooker(CookContext& inoutContext, Scene& inoutSource) noexcept : Context(inoutContext), Source(inoutSource) {}

    CookContext& Context;
    Scene& Source;
    Array<Image> Images;
    usize Wallpaper = 0;
    Array<uint8> Atlas;
    Array<Color> Colors;
    Mesh Meshes[6];
    Array<Material> Materials;
    Array<Color> Emissions;
    Array<SceneItem> EmitterItems;
    Array<std::string_view> Inputs;
    int64 SourceTriangles = 0;
    int64 SourceMeshes = 0;
    int64 DegenerateTriangles = 0;
    int64 CookedVertices = 0;

    [[nodiscard]] bool AddInput(std::string_view path) noexcept
    {
        for (const std::string_view existing : Inputs)
        {
            if (existing == path)
            {
                return true;
            }
        }
        return Inputs.TryAdd(path) == true || Context.Errors.FailOutOfMemory() == true;
    }

    // solid(): one 16x16 sRGB block per distinct base colour.
    [[nodiscard]] bool FindOrAddSolidBlock(const Array<float64>& color, float64 (&outUv)[2]) noexcept
    {
        Failure& errors = Context.Errors;
        if (color.GetSize() != 3)
        {
            return errors.FailInvalid("base color profile");
        }
        Color value;
        for (usize i = 0; i < 3; ++i)
        {
            if (color[i] < 0.0 || color[i] > 1.0)
            {
                return errors.FailInvalid("base color profile");
            }
            value.Components[i] = color[i];
        }
        usize index = Colors.GetSize();
        for (usize i = 0; i < Colors.GetSize() && index == Colors.GetSize(); ++i)
        {
            index = IsSameColor(Colors[i], value) == true ? i : index;
        }
        if (index == Colors.GetSize() && Colors.TryAdd(value) == false)
        {
            return errors.FailOutOfMemory();
        }
        if (index >= 16)
        {
            return errors.FailInvalid("solid atlas limit");
        }
        uint8 rgba[4] = {0, 0, 0, 255};
        for (usize i = 0; i < 3; ++i)
        {
            const float64 v = value.Components[i];
            const float64 encoded = v <= 0.0031308 ? 12.92 * v : 1.055 * math::Pow(v, 1.0 / 2.4) - 0.055;
            const float64 rounded = RoundHalfEven(255.0 * encoded);
            rgba[i] = static_cast<uint8>(rounded < 0.0 ? 0.0 : rounded > 255.0 ? 255.0 : rounded);
        }
        for (usize y = index * 16; y < (index + 1) * 16; ++y)
        {
            for (usize x = 520; x < 536; ++x)
            {
                uint8* out = Atlas.GetData() + (y * ATLAS_WIDTH + x) * 4;
                for (usize k = 0; k < 4; ++k)
                {
                    out[k] = rgba[k];
                }
            }
        }
        outUv[0] = 528.5 / static_cast<float64>(ATLAS_WIDTH);
        outUv[1] = (static_cast<float64>(index * 16) + 8.5) / static_cast<float64>(ATLAS_HEIGHT);
        return true;
    }

    [[nodiscard]] const Parameter* FindMaterialParameter(const MaterialReference& material,
                                                         std::string_view key) const noexcept
    {
        return FindParameter(Source.Commands[material.CommandIndex], key);
    }

    [[nodiscard]] bool GetMaterialType(const MaterialReference& material, std::string_view& outType) noexcept
    {
        const Command& command = Source.Commands[material.CommandIndex];
        if (material.IsInline == true)
        {
            if (command.Names.GetSize() == 0)
            {
                return Context.Errors.FailInvalid("list index out of range");
            }
            outType = command.Names[0].IsList == true ? std::string_view("<list>") : command.Names[0].Text.GetView();
            return true;
        }
        const Parameter* parameter = FindParameter(command, "string type");
        if (parameter == nullptr)
        {
            return Context.Errors.FailInvalid("missing parameter string type");
        }
        if (parameter->Values.GetSize() == 0)
        {
            return Context.Errors.FailInvalid("list index out of range");
        }
        outType = parameter->Values[0].GetView();
        return true;
    }

    // finite(material.get(key, [])) == [expected]
    [[nodiscard]] bool IsRoughnessEqual(const MaterialReference& material,
                                        std::string_view key,
                                        float64 expected,
                                        bool& outIsEqual) noexcept
    {
        const Parameter* parameter = FindMaterialParameter(material, key);
        Array<float64> values;
        if (parameter != nullptr && Context.ParseFiniteValues(parameter->Values, values) == false)
        {
            return false;
        }
        outIsEqual = values.GetSize() == 1 && values[0] == expected;
        return true;
    }

    [[nodiscard]] bool
    CheckRoughnessProfile(const MaterialReference& material, float64 expected, std::string_view message) noexcept
    {
        if (HasSingleValue(FindMaterialParameter(material, "bool remaproughness"), "false") == false)
        {
            return Context.Errors.FailInvalid(message);
        }
        bool isURoughnessOk = false;
        bool isVRoughnessOk = false;
        if (IsRoughnessEqual(material, "float uroughness", expected, isURoughnessOk) == false ||
            (isURoughnessOk == true &&
             IsRoughnessEqual(material, "float vroughness", expected, isVRoughnessOk) == false))
        {
            return false;
        }
        return (isURoughnessOk == true && isVRoughnessOk == true) || Context.Errors.FailInvalid(message) == true;
    }

    [[nodiscard]] bool
    LoadTriangleMesh(const Command& command, Array<Point>& inoutPoints, Array<Triangle>& inoutTriangles) noexcept
    {
        Failure& errors = Context.Errors;
        Array<float64> positions;
        Array<float64> normals;
        Array<float64> uv;
        if (Context.ParseFiniteParameter(FindParameter(command, "point3 P"), positions, "point3 P") == false ||
            Context.ParseFiniteParameter(FindParameter(command, "normal N"), normals, "normal N") == false ||
            Context.ParseFiniteParameter(FindParameter(command, "point2 uv"), uv, "point2 uv") == false)
        {
            return false;
        }
        if (positions.GetSize() % 3 != 0)
        {
            return errors.FailInvalid("trianglemesh positions are not whole points");
        }
        const usize count = positions.GetSize() / 3;
        if (inoutPoints.TryResize(count) == false)
        {
            return errors.FailOutOfMemory();
        }
        for (usize p = 0; p < count; ++p)
        {
            Point& point = inoutPoints[p];
            for (usize k = 0; k < 3; ++k)
            {
                SetAxis(point.Position, k, positions[3 * p + k]);
            }
            const usize available = normals.GetSize() > 3 * p ? normals.GetSize() - 3 * p : 0;
            point.NormalCount = static_cast<uint8>(available >= 3 ? 3 : available);
            for (usize k = 0; k < point.NormalCount; ++k)
            {
                SetAxis(point.Normal, k, normals[3 * p + k]);
            }
            if (uv.GetSize() <= 2 * p + 1)
            {
                return errors.FailInvalid("list index out of range");
            }
            point.Uv[0] = uv[2 * p];
            point.Uv[1] = 1.0 - uv[2 * p + 1];
        }
        const Parameter* indices = FindParameter(command, "integer indices");
        if (indices == nullptr)
        {
            return errors.FailInvalid("missing parameter integer indices");
        }
        if (indices->Values.GetSize() % 3 != 0)
        {
            return errors.FailInvalid("list index out of range");
        }
        for (usize i = 0; i < indices->Values.GetSize(); i += 3)
        {
            Triangle triangle;
            for (usize k = 0; k < 3; ++k)
            {
                if (ParseInt(indices->Values[i + k].GetView(), triangle.Indices[k]) == false)
                {
                    return errors.FailInvalid("invalid literal for int() with base 10");
                }
            }
            if (inoutTriangles.TryAdd(triangle) == false)
            {
                return errors.FailOutOfMemory();
            }
        }
        SourceTriangles += static_cast<int64>(inoutTriangles.GetSize());
        return true;
    }

    [[nodiscard]] bool LoadPoints(const Shape& shape,
                                  std::string_view kind,
                                  Array<Point>& inoutPoints,
                                  Array<Triangle>& inoutTriangles) noexcept
    {
        Failure& errors = Context.Errors;
        const Command& command = Source.Commands[shape.CommandIndex];
        if (kind == "plymesh")
        {
            const Parameter* file = FindParameter(command, "string filename");
            if (file == nullptr || file->Values.GetSize() == 0)
            {
                return errors.FailInvalid("missing parameter string filename");
            }
            const std::string_view path = file->Values[0].GetView();
            String normalised;
            if (AddInput(path) == false || Context.ResolveAssetPath(path, normalised) == false ||
                ReadPly(Context, normalised.GetView(), inoutPoints, inoutTriangles) == false)
            {
                return false;
            }
            SourceMeshes += 1;
            SourceTriangles += static_cast<int64>(inoutTriangles.GetSize());
            return true;
        }
        if (kind == "trianglemesh")
        {
            return LoadTriangleMesh(command, inoutPoints, inoutTriangles);
        }
        if (kind == "disk")
        {
            return true;
        }
        return errors.FailInvalid("unsupported shape ", kind);
    }

    [[nodiscard]] bool
    MakeDiskItem(const Command& command, const math::Matrix4d& transform, SceneItem& outItem) noexcept
    {
        Failure& errors = Context.Errors;
        Array<float64> radius;
        if (Context.ParseFiniteParameter(FindParameter(command, "float radius"), radius, "float radius") == false)
        {
            return false;
        }
        if (radius.GetSize() == 0)
        {
            return errors.FailInvalid("list index out of range");
        }
        if (radius[0] <= 0.0 || radius[0] > 1.0)
        {
            return errors.FailInvalid("disk radius profile");
        }
        const math::Vector3d x = (TransformVector(transform, math::Vector3d{1.0, 0.0, 0.0}) * radius[0]);
        const math::Vector3d y = (TransformVector(transform, math::Vector3d{0.0, 1.0, 0.0}) * radius[0]);
        const math::Vector3d z = TransformVector(transform, math::Vector3d{0.0, 0.0, 1.0});
        const math::Vector3d origin = TransformPoint(transform, math::Vector3d{0.0, 0.0, 0.0});
        outItem.Mesh = 5;
        for (usize k = 0; k < 3; ++k)
        {
            outItem.Transform[k] = GetAxis(x, k);
            outItem.Transform[3 + k] = GetAxis(y, k);
            outItem.Transform[6 + k] = GetAxis(z, k);
            outItem.Transform[9 + k] = GetAxis(origin, k);
        }
        return true;
    }

    [[nodiscard]] bool MakeQuadItem(const Array<Point>& points,
                                    const Array<Triangle>& triangles,
                                    const math::Matrix4d& transform,
                                    SceneItem& outItem) noexcept
    {
        Failure& errors = Context.Errors;
        const bool isQuad = points.GetSize() == 4 && triangles.GetSize() == 2 && triangles[0].Indices[0] == 0 &&
                            triangles[0].Indices[1] == 1 && triangles[0].Indices[2] == 2 &&
                            triangles[1].Indices[0] == 0 && triangles[1].Indices[1] == 2 &&
                            triangles[1].Indices[2] == 3;
        if (isQuad == false)
        {
            return errors.FailInvalid("unsupported emissive quad");
        }
        math::Vector3d p[4];
        for (usize k = 0; k < 4; ++k)
        {
            p[k] = TransformPoint(transform, points[k].Position);
        }
        const math::Vector3d u = ((p[1] - p[0]) * 0.5);
        const math::Vector3d v = ((p[3] - p[0]) * 0.5);
        const math::Vector3d corner = (p[0] + ((u * 2.0) + (v * 2.0)));
        float64 deviation = math::Abs(corner.X - p[2].X);
        for (usize k = 1; k < 3; ++k)
        {
            const float64 value = math::Abs(GetAxis(corner, k) - GetAxis(p[2], k));
            deviation = value > deviation ? value : deviation;
        }
        if (deviation > 1e-4)
        {
            return errors.FailInvalid("non-parallelogram emitter");
        }
        math::Vector3d normal;
        if (NormaliseVector(errors, math::Cross(u, v), normal) == false)
        {
            return false;
        }
        const math::Vector3d centre = ((p[0] + p[2]) * 0.5);
        outItem.Mesh = 4;
        for (usize k = 0; k < 3; ++k)
        {
            outItem.Transform[k] = GetAxis(u, k);
            outItem.Transform[3 + k] = GetAxis(v, k);
            outItem.Transform[6 + k] = GetAxis(normal, k);
            outItem.Transform[9 + k] = GetAxis(centre, k);
        }
        return true;
    }

    [[nodiscard]] bool CookEmitter(const Shape& shape,
                                   std::string_view kind,
                                   const Array<Point>& points,
                                   const Array<Triangle>& triangles) noexcept
    {
        Failure& errors = Context.Errors;
        const Color emission{{shape.Graphics.Emission[0], shape.Graphics.Emission[1], shape.Graphics.Emission[2]}};
        usize index = Emissions.GetSize();
        for (usize i = 0; i < Emissions.GetSize() && index == Emissions.GetSize(); ++i)
        {
            index = IsSameColor(Emissions[i], emission) == true ? i : index;
        }
        if (index == Emissions.GetSize() && Emissions.TryAdd(emission) == false)
        {
            return errors.FailOutOfMemory();
        }
        SceneItem item;
        item.Material = static_cast<uint32>(4 + index);
        const math::Matrix4d& transform = shape.Graphics.Transform;
        bool isOk = false;
        if (kind == "disk")
        {
            isOk = MakeDiskItem(Source.Commands[shape.CommandIndex], transform, item);
        }
        else if (kind == "plymesh")
        {
            if (transform != math::Matrix4d::Identity())
            {
                return errors.FailInvalid("transformed rectangle profile not supported");
            }
            item.Mesh = 4;
            isOk = ComputeRectangle(errors, points, triangles, item.Transform);
        }
        else
        {
            isOk = MakeQuadItem(points, triangles, transform, item);
        }
        return isOk == true && (EmitterItems.TryAdd(item) == true || errors.FailOutOfMemory() == true);
    }

    // Material classification: group index plus an optional fixed solid-colour UV.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    [[nodiscard]] bool ClassifyMaterial(const MaterialReference& material,
                                        usize& outGroup,
                                        bool& outHasFixedUv,
                                        float64 (&outFixedUv)[2]) noexcept
    // NOLINTEND(bugprone-easily-swappable-parameters)
    {
        Failure& errors = Context.Errors;
        std::string_view type;
        if (GetMaterialType(material, type) == false)
        {
            return false;
        }
        outHasFixedUv = false;
        if (type == "conductor")
        {
            if (CheckRoughnessProfile(material, 0.1, "conductor roughness profile") == false)
            {
                return false;
            }
            Array<float64> eta;
            Array<float64> k;
            if (Context.ParseFiniteParameter(FindMaterialParameter(material, "rgb eta"), eta, "rgb eta") == false ||
                Context.ParseFiniteParameter(FindMaterialParameter(material, "rgb k"), k, "rgb k") == false)
            {
                return false;
            }
            if (eta.GetSize() != 3 || k.GetSize() != 3)
            {
                return errors.FailInvalid("conductor eta/k profile");
            }
            Array<float64> color;
            if (color.TryResize(3) == false)
            {
                return errors.FailOutOfMemory();
            }
            for (usize i = 0; i < 3; ++i)
            {
                if (eta[i] < 0.0 || k[i] < 0.0)
                {
                    return errors.FailInvalid("conductor eta/k profile");
                }
            }
            for (usize i = 0; i < 3; ++i)
            {
                const float64 etaValue = eta[i];
                const float64 a = k[i];
                color[i] = (math::Pow(etaValue - 1.0, 2.0) + a * a) / (math::Pow(etaValue + 1.0, 2.0) + a * a);
            }
            outGroup = 3;
            outHasFixedUv = true;
            return FindOrAddSolidBlock(color, outFixedUv);
        }
        if (type != "diffuse" && type != "coateddiffuse")
        {
            return errors.FailInvalid("unsupported material ", type);
        }
        const Parameter* reflectance = FindMaterialParameter(material, "texture reflectance");
        bool hasTexture = false;
        std::string_view texture;
        if (reflectance != nullptr)
        {
            if (reflectance->Values.GetSize() == 0)
            {
                return errors.FailInvalid("list index out of range");
            }
            hasTexture = true;
            texture = reflectance->Values[0].GetView();
        }
        std::string_view textureFile;
        bool isKnown = false;
        usize textureIndex = 0;
        for (usize i = 0; hasTexture == true && i < Source.Textures.GetSize(); ++i)
        {
            if (Source.Textures[i].Name == texture)
            {
                textureFile = Source.Textures[i].File;
                textureIndex = i;
                isKnown = true;
            }
        }
        outGroup = EndsWith(textureFile, "wood5.tga") == true ? 1 : EndsWith(textureFile, "Tiles.tga") == true ? 2 : 0;
        if (hasTexture == true && isKnown == false)
        {
            return errors.FailInvalid("unknown reflectance texture");
        }
        if (hasTexture == true && outGroup == 0 && textureIndex != Wallpaper)
        {
            return errors.FailInvalid("diffuse texture profile");
        }
        if (type == "coateddiffuse")
        {
            if (outGroup != 1 && outGroup != 2)
            {
                return errors.FailInvalid("coated diffuse roughness profile");
            }
            if (CheckRoughnessProfile(material, outGroup == 1 ? 0.1 : 0.01, "coated diffuse roughness profile") ==
                false)
            {
                return false;
            }
        }
        else if (outGroup != 0)
        {
            return errors.FailInvalid("diffuse material profile");
        }
        if (hasTexture == true && texture.empty() == false)
        {
            return true;
        }
        Array<float64> color;
        const Parameter* rgb = FindMaterialParameter(material, "rgb reflectance");
        if (rgb != nullptr)
        {
            if (Context.ParseFiniteValues(rgb->Values, color) == false)
            {
                return false;
            }
        }
        else if (color.TryResize(3, 0.5) == false)
        {
            return errors.FailOutOfMemory();
        }
        outHasFixedUv = true;
        return FindOrAddSolidBlock(color, outFixedUv);
    }

    [[nodiscard]] bool GetTriangleVertices(const Array<Point>& points,
                                           const Triangle& triangle,
                                           Vertex (&outVertices)[3],
                                           bool& outIsDegenerate) noexcept
    {
        Failure& errors = Context.Errors;
        const auto count = static_cast<int64>(points.GetSize());
        const Point* source[3]{};
        for (usize k = 0; k < 3; ++k)
        {
            int64 index = triangle.Indices[k];
            index = index < 0 ? index + count : index; // Python negative indexing.
            if (index < 0 || index >= count)
            {
                return errors.FailInvalid("list index out of range");
            }
            source[k] = &points[static_cast<usize>(index)];
        }
        const math::Vector3d geometric =
            math::Cross((source[1]->Position - source[0]->Position), (source[2]->Position - source[0]->Position));
        outIsDegenerate = math::CompensatedDot(geometric, geometric) < 1e-24;
        if (outIsDegenerate == true)
        {
            return true;
        }
        math::Vector3d normal;
        if (NormaliseVector(errors, geometric, normal) == false)
        {
            return false;
        }
        for (usize k = 0; k < 3; ++k)
        {
            outVertices[k].Position = source[k]->Position;
            outVertices[k].Uv[0] = source[k]->Uv[0];
            outVertices[k].Uv[1] = source[k]->Uv[1];
            if (source[k]->NormalCount == 3)
            {
                if (NormaliseVector(errors, source[k]->Normal, outVertices[k].Normal) == false)
                {
                    return false;
                }
            }
            else if (source[k]->NormalCount == 0)
            {
                outVertices[k].Normal = normal;
            }
            else
            {
                return errors.FailInvalid("incomplete trianglemesh normal");
            }
        }
        return true;
    }

    [[nodiscard]] bool
    CookSurface(const Shape& shape, const Array<Point>& points, const Array<Triangle>& triangles) noexcept
    {
        Failure& errors = Context.Errors;
        usize group = 0;
        bool hasFixedUv = false;
        float64 fixedUv[2]{};
        if (ClassifyMaterial(Source.Materials[static_cast<usize>(shape.Graphics.Material)],
                             group,
                             hasFixedUv,
                             fixedUv) == false)
        {
            return false;
        }
        if (shape.Graphics.Transform != math::Matrix4d::Identity())
        {
            return errors.FailInvalid("transformed room mesh profile not supported");
        }
        Mesh& mesh = Meshes[group];
        Array<AtlasPiece> pieces;
        for (usize triangleIndex = 0; triangleIndex < triangles.GetSize(); ++triangleIndex)
        {
            if (triangleIndex % 256 == 0 && Context.CheckCancellation() == false)
            {
                return false;
            }
            Vertex vertices[3];
            bool isDegenerate = false;
            if (GetTriangleVertices(points, triangles[triangleIndex], vertices, isDegenerate) == false)
            {
                return false;
            }
            if (isDegenerate == true)
            {
                DegenerateTriangles += 1;
                continue;
            }
            pieces.Clear();
            if (hasFixedUv == true)
            {
                AtlasPiece piece{{vertices[0], vertices[1], vertices[2]}};
                for (Vertex& vertex : piece.Vertices)
                {
                    vertex.Uv[0] = fixedUv[0];
                    vertex.Uv[1] = fixedUv[1];
                }
                if (pieces.TryAdd(piece) == false)
                {
                    return errors.FailOutOfMemory();
                }
            }
            else if (group == 0)
            {
                if (BuildAtlasTriangles(errors,
                                        vertices,
                                        static_cast<float64>(ATLAS_WIDTH),
                                        static_cast<float64>(ATLAS_HEIGHT),
                                        pieces) == false)
                {
                    return false;
                }
            }
            else if (pieces.TryAdd(AtlasPiece{{vertices[0], vertices[1], vertices[2]}}) == false)
            {
                return errors.FailOutOfMemory();
            }
            for (const AtlasPiece& piece : pieces)
            {
                CookedVertices += 3;
                if (CookedVertices > format::MAX_VERTICES)
                {
                    return errors.Fail(Status::Limit, "expanded vertex limit");
                }
                const auto base = static_cast<uint32>(mesh.Vertices.GetSize());
                for (usize k = 0; k < 3; ++k)
                {
                    if (mesh.Indices.TryAdd(base + static_cast<uint32>(k)) == false ||
                        mesh.Vertices.TryAdd(piece.Vertices[k]) == false)
                    {
                        return errors.FailOutOfMemory();
                    }
                }
            }
        }
        return true;
    }
};

[[nodiscard]] bool LoadTextures(Cooker& inoutCooker) noexcept
{
    CookContext& context = inoutCooker.Context;
    Failure& errors = context.Errors;
    usize decoded = 0;
    if (inoutCooker.Images.TryResize(inoutCooker.Source.Textures.GetSize()) == false)
    {
        return errors.FailOutOfMemory();
    }
    for (usize i = 0; i < inoutCooker.Source.Textures.GetSize(); ++i)
    {
        const std::string_view path = inoutCooker.Source.Textures[i].File;
        String normalised;
        if (inoutCooker.AddInput(path) == false || context.ResolveAssetPath(path, normalised) == false ||
            ReadTga(context, normalised.GetView(), inoutCooker.Images[i]) == false)
        {
            return false;
        }
        decoded += inoutCooker.Images[i].Rgba.GetSize();
        if (decoded > format::MAX_TOTAL_TEXTURE_BYTES)
        {
            return errors.FailInvalid("decoded texture byte limit");
        }
    }
    bool isFound = false;
    for (usize i = 0; i < inoutCooker.Source.Textures.GetSize() && isFound == false; ++i)
    {
        if (EndsWith(inoutCooker.Source.Textures[i].File, "Wallpaper.tga") == true)
        {
            inoutCooker.Wallpaper = i;
            isFound = true;
        }
    }
    if (isFound == false)
    {
        return errors.FailInvalid("supported PBRT reference profile requires Wallpaper.tga");
    }
    const Image& wallpaper = inoutCooker.Images[inoutCooker.Wallpaper];
    if (wallpaper.Width != 512 || wallpaper.Height != 512)
    {
        return errors.FailInvalid("staircase2 wallpaper profile requires 512x512");
    }
    if (inoutCooker.Atlas.TryResize(Cooker::ATLAS_WIDTH * Cooker::ATLAS_HEIGHT * 4, 0) == false)
    {
        return errors.FailOutOfMemory();
    }
    // 2-pixel wrapped border around the 512x512 wallpaper.
    for (usize y = 0; y < 516; ++y)
    {
        for (usize x = 0; x < 516; ++x)
        {
            const usize sourceOffset = (((y + 510) % 512) * 512 + (x + 510) % 512) * 4;
            const usize destinationOffset = (y * Cooker::ATLAS_WIDTH + x) * 4;
            for (usize k = 0; k < 4; ++k)
            {
                inoutCooker.Atlas[destinationOffset + k] = wallpaper.Rgba[sourceOffset + k];
            }
        }
    }
    return true;
}

[[nodiscard]] bool FindTexture(Cooker& inoutCooker, std::string_view suffix, usize& outIndex) noexcept
{
    for (usize i = 0; i < inoutCooker.Source.Textures.GetSize(); ++i)
    {
        if (EndsWith(inoutCooker.Source.Textures[i].File, suffix) == true)
        {
            outIndex = i;
            return true;
        }
    }
    return inoutCooker.Context.Errors.FailInvalid("supported PBRT reference profile requires ", suffix);
}

[[nodiscard]] bool AddFixedMeshes(Cooker& inoutCooker) noexcept
{
    // Unit quad (mesh 4) and 64-segment disk (mesh 5).
    Mesh& quad = inoutCooker.Meshes[4];
    const float64 corners[4][2] = {{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}};
    for (const auto& corner : corners)
    {
        if (quad.Vertices.TryAdd(
                Vertex{math::Vector3d{corner[0], corner[1], 0.0}, math::Vector3d{0.0, 0.0, 1.0}, {0.0, 0.0}}) == false)
        {
            return false;
        }
    }
    for (const uint32 index : {0U, 1U, 2U, 0U, 2U, 3U})
    {
        if (quad.Indices.TryAdd(index) == false)
        {
            return false;
        }
    }
    Mesh& disk = inoutCooker.Meshes[5];
    if (disk.Vertices.TryAdd(Vertex{math::Vector3d{0.0, 0.0, 0.0}, math::Vector3d{0.0, 0.0, 1.0}, {0.0, 0.0}}) == false)
    {
        return false;
    }
    constexpr float64 TAU = 6.283185307179586;
    for (uint32 i = 0; i < 64; ++i)
    {
        const float64 angle = static_cast<float64>(i) * TAU / 64.0;
        if (disk.Vertices.TryAdd(Vertex{math::Vector3d{math::Cos(angle), math::Sin(angle), 0.0},
                                        math::Vector3d{0.0, 0.0, 1.0},
                                        {0.0, 0.0}}) == false)
        {
            return false;
        }
    }
    for (uint32 i = 0; i < 64; ++i)
    {
        if (disk.Indices.TryAdd(0U) == false || disk.Indices.TryAdd(1U + i) == false ||
            disk.Indices.TryAdd(1U + (i + 1U) % 64U) == false)
        {
            return false;
        }
    }
    return true;
}

struct CameraFrame final
{
    math::Vector3d Eye;
    math::Vector3d Target;
    math::Vector3d Up;
};

[[nodiscard]] bool DeriveCamera(Failure& inoutErrors, const math::Matrix4d& camera, CameraFrame& outFrame) noexcept
{
    // PBRT's CTM is camera-from-world. This profile accepts rigid cameras only.
    math::Vector3d rows[3];
    for (usize r = 0; r < 3; ++r)
    {
        for (usize c = 0; c < 3; ++c)
        {
            SetAxis(rows[r], c, camera.Elements[c * 4 + r]);
        }
    }
    for (usize a = 0; a < 3; ++a)
    {
        for (usize b = 0; b < 3; ++b)
        {
            if (math::Abs(math::CompensatedDot(rows[a], rows[b]) - (a == b ? 1.0 : 0.0)) > 1e-5)
            {
                return inoutErrors.FailInvalid("camera must be rigid");
            }
        }
    }
    for (usize c = 0; c < 3; ++c)
    {
        math::CompensatedSum sum;
        for (usize r = 0; r < 3; ++r)
        {
            sum.Add(GetAxis(rows[r], c) * camera.Elements[12 + r]);
        }
        SetAxis(outFrame.Eye, c, -sum.GetResult());
    }
    outFrame.Up = rows[1];
    outFrame.Target = (outFrame.Eye + rows[2]);
    return true;
}

void PackMesh(ByteWriter& inoutBlob, const Mesh& mesh) noexcept
{
    inoutBlob.AppendUint32(static_cast<uint32>(mesh.Vertices.GetSize()));
    inoutBlob.AppendUint32(static_cast<uint32>(mesh.Indices.GetSize()));
    for (const Vertex& vertex : mesh.Vertices)
    {
        for (usize k = 0; k < format::VERTEX_COMPONENTS; ++k)
        {
            inoutBlob.AppendFloat32(GetComponent(vertex, k));
        }
    }
    for (const uint32 index : mesh.Indices)
    {
        inoutBlob.AppendUint32(index);
    }
}

[[nodiscard]] bool PackBundle(Cooker& inoutCooker,
                              const Array<SceneItem>& items,
                              const CameraFrame& frame,
                              usize wood,
                              usize tiles,
                              Array<uint8>& outBundle) noexcept
{
    Failure& errors = inoutCooker.Context.Errors;
    outBundle.Clear();
    ByteWriter blob(outBundle);
    blob.Append(format::MAGIC);
    blob.AppendUint32(format::VERSION_1);
    blob.AppendUint32(static_cast<uint32>(std::size(inoutCooker.Meshes)));
    blob.AppendUint32(static_cast<uint32>(inoutCooker.Materials.GetSize()));
    blob.AppendUint32(3); // The atlas plus the wood and tile textures.
    blob.AppendUint32(static_cast<uint32>(items.GetSize()));
    for (const math::Vector3d* vector : {&frame.Eye, &frame.Target, &frame.Up})
    {
        for (const float64 value : {vector->X, vector->Y, vector->Z})
        {
            blob.AppendFloat32(value);
        }
    }
    // math.radians(x) is x * (pi / 180).
    constexpr float64 DEGREES_TO_RADIANS = 3.141592653589793 / 180.0;
    blob.AppendFloat32(inoutCooker.Source.FieldOfView * DEGREES_TO_RADIANS);
    blob.AppendUint32(static_cast<uint32>(Cooker::ATLAS_WIDTH));
    blob.AppendUint32(static_cast<uint32>(Cooker::ATLAS_HEIGHT));
    blob.AppendUint32(static_cast<uint32>(inoutCooker.Atlas.GetSize()));
    blob.AppendBytes(inoutCooker.Atlas.AsSpan());
    for (const usize index : {wood, tiles})
    {
        const Image& image = inoutCooker.Images[index];
        blob.AppendUint32(image.Width);
        blob.AppendUint32(image.Height);
        blob.AppendUint32(static_cast<uint32>(image.Rgba.GetSize()));
        blob.AppendBytes(image.Rgba.AsSpan());
    }
    for (const Material& material : inoutCooker.Materials)
    {
        blob.AppendUint32(material.Texture);
        blob.AppendFloat32(material.Metallic);
        blob.AppendFloat32(material.Roughness);
        for (const float64 value : material.Emission)
        {
            blob.AppendFloat32(value);
        }
    }
    for (const Mesh& mesh : inoutCooker.Meshes)
    {
        if (inoutCooker.Context.CheckCancellation() == false)
        {
            return false;
        }
        PackMesh(blob, mesh);
    }
    for (const SceneItem& item : items)
    {
        blob.AppendUint32(item.Mesh);
        blob.AppendUint32(item.Material);
        for (const float64 value : item.Transform)
        {
            blob.AppendFloat32(value);
        }
    }
    if (blob.HasOverflowed() == true)
    {
        return errors.FailInvalid("float too large to pack with f format");
    }
    if (blob.IsOk() == false)
    {
        return errors.FailOutOfMemory();
    }
    if (outBundle.GetSize() > format::MAX_BUNDLE_BYTES)
    {
        return errors.Fail(Status::Limit, "bundle byte limit");
    }
    return true;
}

struct ManifestFacts final
{
    bool IsStaircase = false;
    std::string_view SourceName;
    const Array<std::string_view>* InputsOrNull = nullptr;
    const Array<Digest>* InputDigestsOrNull = nullptr;
    Digest Bundle;
    int64 SourceMeshes = 0;
    int64 SourceTriangles = 0;
    int64 Degenerate = 0;
    int64 MeshCount = LEGACY_REFERENCE_MESH_COUNT;
    bool HasTypedMaterials = false;
    int64 Materials = 0;
    int64 Items = 0;
    int64 CookedTriangles = 0;
    int64 RenderedTriangles = 0;
    int64 Bytes = 0;
    const CameraFrame* CameraOrNull = nullptr;
    float64 FieldOfView = 0.0;
};

void WriteHexString(PythonJsonWriter& inoutJson, const Digest& digest) noexcept
{
    char text[64]{};
    constexpr char HEX[] = "0123456789abcdef";
    for (usize i = 0; i < 32; ++i)
    {
        text[2 * i] = HEX[digest.Data[i] >> 4];
        text[2 * i + 1] = HEX[digest.Data[i] & 15];
    }
    inoutJson.WriteString(std::string_view(text, sizeof(text)));
}

[[nodiscard]] bool WriteManifest(const ManifestFacts& facts, Array<uint8>& outManifest) noexcept
{
    if (facts.InputsOrNull == nullptr || facts.InputDigestsOrNull == nullptr || facts.CameraOrNull == nullptr)
    {
        return false;
    }
    outManifest.Clear();
    ByteWriter writer(outManifest);
    PythonJsonWriter json(writer);
    json.BeginObject();
    json.WriteKey("version");
    json.WriteInteger(1);
    json.WriteKey("source");
    json.WriteString(facts.IsStaircase == true ? std::string_view("Modern Hall / staircase2") : facts.SourceName);
    json.WriteKey("author");
    json.WriteString(facts.IsStaircase == true ? std::string_view("NewSee2l035")
                                               : std::string_view("See retained LICENSE.txt"));
    json.WriteKey("license");
    json.WriteString(facts.IsStaircase == true ? std::string_view("https://creativecommons.org/licenses/by/3.0/")
                                               : std::string_view("See retained LICENSE.txt"));
    json.WriteKey("source_format");
    json.WriteString("PBRT v4 subset + binary PLY + uncompressed TGA");
    if (facts.HasTypedMaterials == true)
    {
        json.WriteKey("importer");
        json.WriteString(MATERIAL_PROFILE);
    }
    json.WriteKey("inputs");
    json.BeginObject();
    for (usize i = 0; i < facts.InputsOrNull->GetSize(); ++i)
    {
        json.WriteKey((*facts.InputsOrNull)[i]);
        WriteHexString(json, (*facts.InputDigestsOrNull)[i]);
    }
    json.EndObject();
    json.WriteKey("bundle_sha256");
    WriteHexString(json, facts.Bundle);
    const struct
    {
        std::string_view Key;
        int64 Value;
    } counts[] = {
        {"source_ply_meshes", facts.SourceMeshes},
        {"source_triangles", facts.SourceTriangles},
        {"discarded_zero_area_triangles", facts.Degenerate},
        {"meshes", facts.MeshCount},
        {"materials", facts.Materials},
        {"items", facts.Items},
        {"cooked_triangles", facts.CookedTriangles},
        {"rendered_triangles", facts.RenderedTriangles},
        {"bytes", facts.Bytes},
    };
    for (const auto& count : counts)
    {
        json.WriteKey(count.Key);
        json.WriteInteger(count.Value);
    }
    json.WriteKey("camera");
    json.BeginObject();
    const struct
    {
        std::string_view Key;
        const math::Vector3d* Value;
    } vectors[3] = {{"eye", &facts.CameraOrNull->Eye},
                    {"target", &facts.CameraOrNull->Target},
                    {"up", &facts.CameraOrNull->Up}};
    for (const auto& vector : vectors)
    {
        json.WriteKey(vector.Key);
        json.BeginArray();
        for (const float64 value : {vector.Value->X, vector.Value->Y, vector.Value->Z})
        {
            json.WriteFloat(value);
        }
        json.EndArray();
    }
    json.WriteKey("fov_degrees");
    json.WriteFloat(facts.FieldOfView);
    json.EndObject();
    json.WriteKey("approximations");
    json.BeginArray();
    if (facts.HasTypedMaterials == true)
    {
        for (const std::string_view text :
             {"PBRT coated diffuse parameters retained; shading remains RGB GGX/Lambert without layer multiple "
              "scattering",
              "conductor Fresnel uses retained RGB eta/k; spectral transport is not implemented",
              "single retained texture mip; no PBRT footprint/trilinear filtering",
              "disk tessellation: 64 segments; emissive planar grids represented by equivalent quads",
              "PBRT integrator/sampler options replaced by explicit reference capture settings"})
        {
            json.WriteString(text);
        }
    }
    else
        for (const std::string_view text : {
                 "PBRT coated diffuse mapped to frozen RGB GGX/Lambert; no layer multiple scattering",
                 "conductor eta/k reduced to RGB normal-incidence F0 and Schlick",
                 "diffuse surfaces use L6 dielectric F0 0.04",
                 "L6 surface emission is two-sided; PBRT diffuse area lights default to one-sided",
                 "single retained texture mip; no PBRT footprint/trilinear filtering",
                 "disk tessellation: 64 segments",
                 "emissive planar grids represented by equivalent quads",
                 "PBRT integrator/sampler options replaced by explicit L6 capture settings",
             })
        {
            json.WriteString(text);
        }
    json.EndArray();
    json.EndObject();
    writer.Append('\n');
    return writer.IsOk();
}

[[nodiscard]] bool CookShapes(Cooker& inoutCooker) noexcept
{
    CookContext& context = inoutCooker.Context;
    Array<Point> points;
    Array<Triangle> triangles;
    for (const Shape& shape : inoutCooker.Source.Shapes)
    {
        if (context.CheckCancellation() == false)
        {
            return false;
        }
        std::string_view kind;
        if (GetFirstName(context, inoutCooker.Source.Commands[shape.CommandIndex], kind) == false)
        {
            return false;
        }
        points.Clear();
        triangles.Clear();
        if (inoutCooker.LoadPoints(shape, kind, points, triangles) == false)
        {
            return false;
        }
        const bool isOk = shape.Graphics.HasEmission == true ? inoutCooker.CookEmitter(shape, kind, points, triangles)
                                                             : inoutCooker.CookSurface(shape, points, triangles);
        if (isOk == false)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool HashInputs(Cooker& inoutCooker, Array<Digest>& outDigests) noexcept
{
    // sorted(inputs) by code point, each re-read through asset_path().
    Array<std::string_view>& inputs = inoutCooker.Inputs;
    for (usize i = 1; i < inputs.GetSize(); ++i)
    {
        for (usize j = i; j > 0 && inputs[j] < inputs[j - 1]; --j)
        {
            const std::string_view swap = inputs[j];
            inputs[j] = inputs[j - 1];
            inputs[j - 1] = swap;
        }
    }
    for (const std::string_view input : inputs)
    {
        String normalised;
        Bytes data;
        if (inoutCooker.Context.ResolveAssetPath(input, normalised) == false ||
            inoutCooker.Context.ReadBounded(normalised.GetView(), MAX_SOURCE_BYTES, data) == false)
        {
            return false;
        }
        if (outDigests.TryAdd(Hash(data.Data())) == false)
        {
            return inoutCooker.Context.Errors.FailOutOfMemory();
        }
    }
    return true;
}

/// Resolve the source's explicit scalars without replacing them by texture-name recipes.
[[nodiscard]] bool ReadMaterialScalar(Cooker& inoutCooker,
                                      const MaterialReference& source,
                                      std::string_view key,
                                      float64 fallback,
                                      float64& outValue) noexcept
{
    const Parameter* parameterOrNull = inoutCooker.FindMaterialParameter(source, key);
    outValue = fallback;
    if (parameterOrNull == nullptr)
    {
        return true;
    }
    Array<float64> values;
    if (inoutCooker.Context.ParseFiniteParameter(parameterOrNull, values, key) == false || values.GetSize() != 1)
    {
        return inoutCooker.Context.Errors.FailInvalid("invalid material scalar ", key);
    }
    outValue = values[0];
    return true;
}
[[nodiscard]] bool PrepareTypedMaterials(Cooker& inoutCooker) noexcept
{
    if (inoutCooker.Source.Materials.GetSize() > format::IMPORT_LIMITS.Materials)
    {
        return inoutCooker.Context.Errors.Fail(Status::Limit, "source material capacity");
    }
    for (const MaterialReference& source : inoutCooker.Source.Materials)
    {
        Material material;
        material.Texture = format::NO_TEXTURE;
        material.Roughness = 1;
        material.SourceIdentity = static_cast<uint32>(source.CommandIndex + 1);
        std::string_view type;
        if (inoutCooker.GetMaterialType(source, type) == false)
        {
            return false;
        }
        if (source.IsInline == false &&
            GetFirstName(inoutCooker.Context, inoutCooker.Source.Commands[source.CommandIndex], material.Name) == false)
        {
            return false;
        }
        if (material.Name.size() > format::MAX_MATERIAL_NAME_BYTES)
        {
            return inoutCooker.Context.Errors.FailInvalid("material name exceeds the stored name capacity");
        }
        material.Scattering.Model = type == "diffuse"     ? format::MaterialModel::Diffuse
                                    : type == "conductor" ? format::MaterialModel::Conductor
                                                          : format::MaterialModel::CoatedDiffuse;
        if (type != "diffuse" && type != "conductor" && type != "coateddiffuse")
        {
            return inoutCooker.Context.Errors.FailInvalid("unsupported material ", type);
        }
        if (type != "diffuse")
        {
            const Parameter* remapOrNull = inoutCooker.FindMaterialParameter(source, "bool remaproughness");
            if (HasSingleValue(remapOrNull, "false") == false)
            {
                return inoutCooker.Context.Errors.FailInvalid("typed reference profile requires remaproughness=false");
            }
            float64 alphaU = 0;
            float64 alphaV = 0;
            if (ReadMaterialScalar(inoutCooker, source, "float uroughness", 0, alphaU) == false ||
                ReadMaterialScalar(inoutCooker, source, "float vroughness", 0, alphaV) == false)
            {
                return false;
            }
            material.Scattering.AlphaU = static_cast<float32>(alphaU);
            material.Scattering.AlphaV = static_cast<float32>(alphaV);
            material.Roughness = math::Max(math::Sqrt(alphaU), static_cast<float64>(format::MIN_ROUGHNESS));
            if (alphaU != alphaV || format::IsMaterialScatteringValid(material.Scattering) == false)
            {
                return inoutCooker.Context.Errors.FailInvalid(
                    "typed reference profile requires positive isotropic GGX alpha");
            }
        }
        const Parameter* textureOrNull = inoutCooker.FindMaterialParameter(source, "texture reflectance");
        const Parameter* reflectanceOrNull = inoutCooker.FindMaterialParameter(source, "rgb reflectance");
        if ((textureOrNull != nullptr && reflectanceOrNull != nullptr) ||
            (type == "conductor" && (textureOrNull != nullptr || reflectanceOrNull != nullptr)))
        {
            return inoutCooker.Context.Errors.FailInvalid(
                "unsupported conflicting or conductor reflectance parameters");
        }
        if (textureOrNull != nullptr)
        {
            if (textureOrNull->Values.GetSize() != 1)
            {
                return inoutCooker.Context.Errors.FailInvalid("invalid material texture reference");
            }
            for (usize texture = 0; texture < inoutCooker.Source.Textures.GetSize(); ++texture)
            {
                if (inoutCooker.Source.Textures[texture].Name == textureOrNull->Values[0].GetView())
                {
                    material.Texture = static_cast<uint32>(texture);
                }
            }
            if (material.Texture == format::NO_TEXTURE)
            {
                return inoutCooker.Context.Errors.FailInvalid("unknown reflectance texture");
            }
        }
        else if (type != "conductor")
        {
            Array<float64> reflectance;
            if (reflectanceOrNull != nullptr)
            {
                if (inoutCooker.Context.ParseFiniteParameter(reflectanceOrNull, reflectance, "rgb reflectance") ==
                        false ||
                    reflectance.GetSize() != format::MATERIAL_RGB_CHANNELS)
                {
                    return inoutCooker.Context.Errors.FailInvalid("invalid RGB reflectance parameters");
                }
                material.Reflectance = {reflectance[0], reflectance[1], reflectance[2]};
            }
            else
            {
                material.Reflectance = {0.5, 0.5, 0.5};
            }
        }
        if (material.Reflectance.X < 0 || material.Reflectance.X > 1 || material.Reflectance.Y < 0 ||
            material.Reflectance.Y > 1 || material.Reflectance.Z < 0 || material.Reflectance.Z > 1)
        {
            return inoutCooker.Context.Errors.FailInvalid("material reflectance outside [0,1]");
        }
        if (type == "conductor")
        {
            Array<float64> eta;
            Array<float64> extinction;
            if (inoutCooker.Context.ParseFiniteParameter(inoutCooker.FindMaterialParameter(source, "rgb eta"),
                                                         eta,
                                                         "rgb eta") == false ||
                inoutCooker.Context.ParseFiniteParameter(inoutCooker.FindMaterialParameter(source, "rgb k"),
                                                         extinction,
                                                         "rgb k") == false ||
                eta.GetSize() != format::MATERIAL_RGB_CHANNELS || extinction.GetSize() != format::MATERIAL_RGB_CHANNELS)
            {
                return inoutCooker.Context.Errors.FailInvalid("invalid RGB optical parameters");
            }
            for (usize channel = 0; channel < format::MATERIAL_RGB_CHANNELS; ++channel)
            {
                material.Scattering.Eta[channel] = static_cast<float32>(eta[channel]);
                material.Scattering.K[channel] = static_cast<float32>(extinction[channel]);
            }
            material.Metallic = 1;
        }
        if (format::IsMaterialScatteringValid(material.Scattering) == false)
        {
            return inoutCooker.Context.Errors.FailInvalid("invalid material optical constants");
        }
        if (inoutCooker.Materials.TryAdd(material) == false)
        {
            return inoutCooker.Context.Errors.FailOutOfMemory();
        }
    }
    return true;
}
void WriteMaterialExtension(ByteWriter& inoutWriter, const Material& material) noexcept
{
    inoutWriter.AppendUint32(material.SourceIdentity);
    inoutWriter.AppendUint32(static_cast<uint32>(material.Name.size()));
    inoutWriter.Append(material.Name);
    for (usize byte = material.Name.size(); byte < format::MAX_MATERIAL_NAME_BYTES; ++byte)
    {
        inoutWriter.Append(char{0});
    }
    inoutWriter.AppendUint32(static_cast<uint32>(material.Scattering.Model));
    for (const float64 value : {material.Reflectance.X, material.Reflectance.Y, material.Reflectance.Z})
    {
        inoutWriter.AppendFloat32(static_cast<float64>(value));
    }
    for (const float32 value : material.Scattering.Eta)
    {
        inoutWriter.AppendFloat32(static_cast<float64>(value));
    }
    for (const float32 value : material.Scattering.K)
    {
        inoutWriter.AppendFloat32(static_cast<float64>(value));
    }
    inoutWriter.AppendFloat32(static_cast<float64>(material.Scattering.AlphaU));
    inoutWriter.AppendFloat32(static_cast<float64>(material.Scattering.AlphaV));
    inoutWriter.AppendFloat32(static_cast<float64>(material.Scattering.CoatingEta));
    inoutWriter.AppendFloat32(static_cast<float64>(material.Scattering.CoatingThickness));
}
[[nodiscard]] bool CookTypedScene(CookContext& inoutContext, Scene& inoutSource, CookResult& outResult) noexcept
{
    Cooker cooker(inoutContext, inoutSource);
    if (cooker.AddInput("scene-v4.pbrt") == false || cooker.AddInput("LICENSE.txt") == false ||
        LoadTextures(cooker) == false || PrepareTypedMaterials(cooker) == false)
    {
        return false;
    }
    if (AddFixedMeshes(cooker) == false)
    {
        return inoutContext.Errors.FailOutOfMemory();
    }
    Array<Mesh> materialMeshes;
    Array<Mesh> meshes;
    Array<SceneItem> items;
    if (materialMeshes.TryResize(cooker.Materials.GetSize()) == false)
    {
        return inoutContext.Errors.FailOutOfMemory();
    }
    Array<Point> points;
    Array<Triangle> triangles;
    for (const Shape& shape : inoutSource.Shapes)
    {
        std::string_view kind;
        points.Clear();
        triangles.Clear();
        if (inoutContext.CheckCancellation() == false ||
            GetFirstName(inoutContext, inoutSource.Commands[shape.CommandIndex], kind) == false ||
            cooker.LoadPoints(shape, kind, points, triangles) == false)
        {
            return false;
        }
        const usize materialIndex = static_cast<usize>(shape.Graphics.Material);
        const Material sourceMaterial = cooker.Materials[materialIndex];
        if (shape.Graphics.HasEmission == true)
        {
            if (cooker.CookEmitter(shape, kind, points, triangles) == false)
            {
                return false;
            }
            SceneItem& emitter = cooker.EmitterItems[cooker.EmitterItems.GetSize() - 1];
            Material material = sourceMaterial;
            material.EmissionSourceIdentity = shape.CommandIndex + 1;
            emitter.Material = static_cast<uint32>(cooker.Materials.GetSize());
            for (usize channel = 0; channel < format::MATERIAL_RGB_CHANNELS; ++channel)
            {
                material.Emission[channel] = shape.Graphics.Emission[channel];
            }
            if (cooker.Materials.GetSize() >= format::IMPORT_LIMITS.Materials)
            {
                return inoutContext.Errors.Fail(Status::Limit, "emitter material variant capacity");
            }
            if (cooker.Materials.TryAdd(material) == false)
            {
                return inoutContext.Errors.FailOutOfMemory();
            }
            continue;
        }
        if (shape.Graphics.Transform != math::Matrix4d::Identity())
        {
            return inoutContext.Errors.FailInvalid("transformed room mesh profile not supported");
        }
        Mesh& mesh = materialMeshes[materialIndex];
        for (const Triangle& triangle : triangles)
        {
            if (inoutContext.CheckCancellation() == false)
            {
                return false;
            }
            Vertex vertices[format::TRIANGLE_CORNERS];
            bool isDegenerate = false;
            if (cooker.GetTriangleVertices(points, triangle, vertices, isDegenerate) == false)
            {
                return false;
            }
            if (isDegenerate == true)
            {
                ++cooker.DegenerateTriangles;
                continue;
            }
            const uint32 firstVertex = static_cast<uint32>(mesh.Vertices.GetSize());
            for (usize corner = 0; corner < format::TRIANGLE_CORNERS; ++corner)
            {
                if (mesh.Vertices.TryAdd(vertices[corner]) == false ||
                    mesh.Indices.TryAdd(firstVertex + static_cast<uint32>(corner)) == false)
                {
                    return inoutContext.Errors.FailOutOfMemory();
                }
            }
            cooker.CookedVertices += format::TRIANGLE_CORNERS;
            if (cooker.CookedVertices > format::IMPORT_LIMITS.Vertices)
            {
                return inoutContext.Errors.Fail(Status::Limit, "expanded vertex limit");
            }
        }
    }
    for (usize material = 0; material < materialMeshes.GetSize(); ++material)
    {
        if (materialMeshes[material].Indices.IsEmpty() == true)
        {
            continue;
        }
        SceneItem item;
        item.Mesh = static_cast<uint32>(meshes.GetSize());
        item.Material = static_cast<uint32>(material);
        item.Transform[0] = 1;
        item.Transform[AFFINE_SECOND_DIAGONAL] = 1;
        item.Transform[AFFINE_THIRD_DIAGONAL] = 1;
        if (items.TryAdd(item) == false || meshes.TryAdd(foundation::Move(materialMeshes[material])) == false)
        {
            return inoutContext.Errors.FailOutOfMemory();
        }
    }
    const uint32 quadMesh = static_cast<uint32>(meshes.GetSize());
    if (meshes.TryAdd(foundation::Move(cooker.Meshes[LEGACY_QUAD_MESH])) == false ||
        meshes.TryAdd(foundation::Move(cooker.Meshes[LEGACY_DISK_MESH])) == false)
    {
        return inoutContext.Errors.FailOutOfMemory();
    }
    for (SceneItem emitter : cooker.EmitterItems)
    {
        emitter.Mesh = quadMesh + (emitter.Mesh == LEGACY_DISK_MESH ? 1U : 0U);
        if (items.TryAdd(emitter) == false)
        {
            return inoutContext.Errors.FailOutOfMemory();
        }
    }
    if (items.IsEmpty() == true)
    {
        return inoutContext.Errors.FailInvalid("scene has no drawable geometry");
    }
    if (meshes.GetSize() > format::IMPORT_LIMITS.Meshes ||
        cooker.Materials.GetSize() > format::IMPORT_LIMITS.Materials || items.GetSize() > format::IMPORT_LIMITS.Objects)
    {
        return inoutContext.Errors.Fail(Status::Limit, "typed scene record capacity");
    }
    CameraFrame camera;
    if (DeriveCamera(inoutContext.Errors, inoutSource.Camera, camera) == false)
    {
        return false;
    }
    ByteWriter writer(outResult.Bundle);
    writer.Append(format::MAGIC);
    writer.AppendUint32(format::VERSION_7);
    writer.AppendUint32(static_cast<uint32>(meshes.GetSize()));
    writer.AppendUint32(static_cast<uint32>(cooker.Materials.GetSize()));
    writer.AppendUint32(static_cast<uint32>(cooker.Images.GetSize()));
    writer.AppendUint32(static_cast<uint32>(items.GetSize()));
    for (const math::Vector3d value : {camera.Eye, camera.Target, camera.Up})
    {
        writer.AppendFloat32(value.X);
        writer.AppendFloat32(value.Y);
        writer.AppendFloat32(value.Z);
    }
    writer.AppendFloat32(math::DegreesToRadians(inoutSource.FieldOfView));
    for (const Image& image : cooker.Images)
    {
        writer.AppendUint32(image.Width);
        writer.AppendUint32(image.Height);
        writer.AppendUint32(static_cast<uint32>(image.Rgba.GetSize()));
        writer.AppendBytes(image.Rgba.AsSpan());
    }
    for (const Material& material : cooker.Materials)
    {
        writer.AppendUint32(material.Texture);
        writer.AppendUint32(format::ADDRESS_REPEAT);
        writer.AppendFloat32(material.Metallic);
        writer.AppendFloat32(material.Roughness);
        for (const float64 value : material.Emission)
        {
            writer.AppendFloat32(value);
        }
        writer.AppendUint32(0); // PBRT area emission defaults to one geometric side.
        writer.AppendUint64(material.EmissionSourceIdentity);
        writer.AppendFloat32(0);
        writer.AppendUint32(format::ALPHA_OPAQUE);
        writer.AppendFloat32(0);
        writer.AppendUint32(format::NO_TEXTURE);
        writer.AppendUint32(format::NO_TEXTURE);
        writer.AppendFloat32(0);
        WriteMaterialExtension(writer, material);
    }
    for (const Mesh& mesh : meshes)
    {
        writer.AppendUint32(static_cast<uint32>(mesh.Vertices.GetSize()));
        writer.AppendUint32(static_cast<uint32>(mesh.Indices.GetSize()));
        for (const Vertex& vertex : mesh.Vertices)
        {
            for (usize component = 0; component < format::VERTEX_COMPONENTS; ++component)
            {
                writer.AppendFloat32(GetComponent(vertex, component));
            }
            for (usize component = 0; component < format::TANGENT_COMPONENTS; ++component)
            {
                writer.AppendFloat32(0);
            }
        }
        for (const uint32 index : mesh.Indices)
        {
            writer.AppendUint32(index);
        }
    }
    for (usize index = 0; index < items.GetSize(); ++index)
    {
        const SceneItem& item = items[index];
        writer.AppendUint32(item.Mesh);
        writer.AppendUint32(item.Material);
        for (const float64 value : item.Transform)
        {
            writer.AppendFloat32(value);
        }
        writer.AppendUint64(index + 1);
    }
    writer.AppendUint32(format::LIGHTING_PROFILE_REC709_PHOTOMETRIC);
    writer.AppendUint32(0);
    writer.AppendUint32(0);
    writer.AppendUint32(0);
    for (usize channel = 0; channel < format::MATERIAL_RGB_CHANNELS; ++channel)
    {
        writer.AppendFloat32(0);
    }
    if (writer.IsOk() == false)
    {
        return inoutContext.Errors.FailOutOfMemory();
    }
    if (writer.HasOverflowed() == true || outResult.Bundle.GetSize() > format::IMPORT_LIMITS.BundleBytes)
    {
        return inoutContext.Errors.Fail(Status::Limit, "typed bundle byte capacity");
    }
    Bytes license;
    Array<Digest> digests;
    if (inoutContext.ReadBounded("LICENSE.txt", MAX_LICENSE_BYTES, license) == false ||
        HashInputs(cooker, digests) == false)
    {
        return false;
    }
    foundation::parsing::ParseError utf8{};
    if (foundation::parsing::ValidateUtf8(license.String(), utf8) != foundation::parsing::ParseStatus::Ok)
    {
        return inoutContext.Errors.FailInvalid("'utf-8' codec can't decode LICENSE.txt");
    }
    ManifestFacts facts;
    facts.HasTypedMaterials = true;
    facts.IsStaircase = license.String().find("NewSee2l035") != std::string_view::npos;
    facts.SourceName = GetBaseName(inoutContext.Root.GetView());
    facts.InputsOrNull = &cooker.Inputs;
    facts.InputDigestsOrNull = &digests;
    facts.Bundle = Hash(outResult.Bundle.AsSpan());
    facts.SourceMeshes = cooker.SourceMeshes;
    facts.SourceTriangles = cooker.SourceTriangles;
    facts.Degenerate = cooker.DegenerateTriangles;
    facts.MeshCount = static_cast<int64>(meshes.GetSize());
    facts.Materials = static_cast<int64>(cooker.Materials.GetSize());
    facts.Items = static_cast<int64>(items.GetSize());
    facts.Bytes = static_cast<int64>(outResult.Bundle.GetSize());
    facts.CameraOrNull = &camera;
    facts.FieldOfView = inoutSource.FieldOfView;
    for (const Mesh& mesh : meshes)
    {
        facts.CookedTriangles += static_cast<int64>(mesh.Indices.GetSize() / format::TRIANGLE_CORNERS);
    }
    for (const SceneItem& item : items)
    {
        facts.RenderedTriangles += static_cast<int64>(meshes[item.Mesh].Indices.GetSize() / format::TRIANGLE_CORNERS);
    }
    if (WriteManifest(facts, outResult.Manifest) == false || outResult.License.TryAddRange(license.Data()) == false)
    {
        return inoutContext.Errors.FailOutOfMemory();
    }
    return true;
}

[[nodiscard]] bool CookScene(CookContext& inoutContext, CookResult& outResult) noexcept
{
    Failure& errors = inoutContext.Errors;
    Scene scene;
    if (ParseScene(inoutContext, scene) == false)
    {
        return false;
    }
    usize emitters = 0;
    for (const Shape& shape : scene.Shapes)
    {
        emitters += shape.Graphics.HasEmission == true ? 1 : 0;
    }
    if (4 + emitters > 128)
    {
        return errors.Fail(Status::Limit, "renderer profile capacity: at most 128 scene items");
    }
    Cooker cooker(inoutContext, scene);
    if (cooker.AddInput("scene-v4.pbrt") == false || cooker.AddInput("LICENSE.txt") == false ||
        LoadTextures(cooker) == false)
    {
        return false;
    }
    usize wood = 0;
    usize tiles = 0;
    if (FindTexture(cooker, "wood5.tga", wood) == false || FindTexture(cooker, "Tiles.tga", tiles) == false)
    {
        return false;
    }
    // Diffuse, wood-coated approximation, tile-coated approximation, conductor.
    const Material base[4] = {
        {0, 0.0, 1.0, {0.0, 0.0, 0.0}},
        {1, 0.0, math::Sqrt(0.1), {0.0, 0.0, 0.0}},
        {2, 0.0, math::Sqrt(0.01), {0.0, 0.0, 0.0}},
        {0, 1.0, math::Sqrt(0.1), {0.0, 0.0, 0.0}},
    };
    if (cooker.Materials.TryAddRange(std::span<const Material>(base, 4)) == false)
    {
        return errors.FailOutOfMemory();
    }
    if (CookShapes(cooker) == false)
    {
        return false;
    }
    for (const Color& emission : cooker.Emissions)
    {
        if (cooker.Materials.TryAdd(
                Material{0xFFFFFFFFU,
                         0.0,
                         1.0,
                         {emission.Components[0], emission.Components[1], emission.Components[2]}}) == false)
        {
            return errors.FailOutOfMemory();
        }
    }
    if (AddFixedMeshes(cooker) == false)
    {
        return errors.FailOutOfMemory();
    }
    Array<SceneItem> items;
    for (uint32 i = 0; i < 4; ++i)
    {
        if (items.TryAdd(SceneItem{i, i, {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0}}) == false)
        {
            return errors.FailOutOfMemory();
        }
    }
    if (items.TryAddRange(cooker.EmitterItems.AsSpan()) == false)
    {
        return errors.FailOutOfMemory();
    }
    if (cooker.Materials.GetSize() != 8 || items.GetSize() > format::MAX_OBJECTS)
    {
        return errors.FailInvalid("renderer profile capacity");
    }
    CameraFrame frame;
    if (DeriveCamera(errors, scene.Camera, frame) == false ||
        PackBundle(cooker, items, frame, wood, tiles, outResult.Bundle) == false)
    {
        return false;
    }
    Bytes license;
    if (inoutContext.ReadBounded("LICENSE.txt", MAX_LICENSE_BYTES, license) == false)
    {
        return false;
    }
    foundation::parsing::ParseError utf8{};
    if (foundation::parsing::ValidateUtf8(license.String(), utf8) != foundation::parsing::ParseStatus::Ok)
    {
        return errors.FailInvalid("'utf-8' codec can't decode LICENSE.txt");
    }
    Array<Digest> digests;
    if (HashInputs(cooker, digests) == false)
    {
        return false;
    }
    ManifestFacts facts;
    facts.IsStaircase = license.String().find("NewSee2l035") != std::string_view::npos;
    facts.SourceName = GetBaseName(inoutContext.Root.GetView());
    facts.InputsOrNull = &cooker.Inputs;
    facts.InputDigestsOrNull = &digests;
    facts.Bundle = Hash(outResult.Bundle.AsSpan());
    facts.SourceMeshes = cooker.SourceMeshes;
    facts.SourceTriangles = cooker.SourceTriangles;
    facts.Degenerate = cooker.DegenerateTriangles;
    facts.Materials = static_cast<int64>(cooker.Materials.GetSize());
    facts.Items = static_cast<int64>(items.GetSize());
    for (const Mesh& mesh : cooker.Meshes)
    {
        facts.CookedTriangles += static_cast<int64>(mesh.Indices.GetSize() / 3);
    }
    for (const SceneItem& item : items)
    {
        facts.RenderedTriangles += static_cast<int64>(cooker.Meshes[item.Mesh].Indices.GetSize() / 3);
    }
    facts.Bytes = static_cast<int64>(outResult.Bundle.GetSize());
    facts.CameraOrNull = &frame;
    facts.FieldOfView = scene.FieldOfView;
    outResult.License.Clear();
    if (WriteManifest(facts, outResult.Manifest) == false || outResult.License.TryAddRange(license.Data()) == false)
    {
        return errors.FailOutOfMemory();
    }
    return true;
}
} // namespace

bool Cook(std::string_view source,
          CookResult& outResult,
          CookError& outError,
          const Cancellation* cancelOrNull) noexcept
{
    outError = CookError{};
    Failure failure{outError};
    CookContext context(failure, cancelOrNull);
    const Status resolved = ResolveDirectory(source, context.Root);
    if (resolved != Status::Ok)
    {
        return failure.Fail(resolved, "cannot resolve source directory: ", source);
    }
    return CookScene(context, outResult);
}

bool CookMaterials(std::string_view source,
                   CookResult& outResult,
                   CookError& outError,
                   const Cancellation* cancelOrNull) noexcept
{
    outError = {};
    Failure failure{outError};
    CookContext context(failure, cancelOrNull);
    const Status resolved = ResolveDirectory(source, context.Root);
    if (resolved != Status::Ok)
    {
        return failure.Fail(resolved, "cannot resolve source directory: ", source);
    }
    Scene scene;
    CookResult candidate;
    if (ParseScene(context, scene) == false || CookTypedScene(context, scene, candidate) == false)
    {
        return false;
    }
    outResult = foundation::Move(candidate);
    return true;
}

bool WriteCook(std::string_view output, const CookResult& result, CookError& outError) noexcept
{
    outError = CookError{};
    Failure failure{outError};
    const Status made = MakeDirectories(output);
    if (made != Status::Ok)
    {
        return failure.Fail(made, "cannot create output directory: ", output);
    }
    const struct
    {
        std::string_view Name;
        std::span<const uint8> Data;
    } files[3] = {{"scene.luscn", result.Bundle.AsSpan()},
                  {"manifest.json", result.Manifest.AsSpan()},
                  {"LICENSE.txt", result.License.AsSpan()}};
    for (const auto& file : files)
    {
        const Status written = WriteFileAtomic(output, file.Name, file.Data);
        if (written != Status::Ok)
        {
            return failure.Fail(written, "cannot write ", file.Name);
        }
    }
    return true;
}
} // namespace ludus::content::scene_cook
