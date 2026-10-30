#include "pbrt_parser.h"

#include "../../native_files.h"
#include "../byte_source.h"
#include "../transform_utilities.h"
#include "pbrt_tokenizer.h"
#include "ply_reader.h"
#include "shape_tessellation.h"
#include "spectrum_tables.h"

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::content::scene_cook::importers::pbrt
{
namespace
{
using foundation::StringStatus;

// ---------------------------------------------------------------------------
// Directive arguments.

/// One positional value or parameter value: a scalar or a bracketed list.
struct ArgumentItem final
{
    bool IsList = false;
    Array<float64> Numbers;
    Array<String> Strings;
    /// Bare `true`/`false` words and quoted "true"/"false" strings.
    Array<uint8> Booleans;
};

struct Parameter final
{
    String Type;
    String Name;
    ArgumentItem Value;
    bool IsUsed = false;
};

struct Directive final
{
    String Name;
    uint32 Line = 0;
    Array<String> Positional;
    Array<float64> Numbers;
    Array<Parameter> Parameters;
};

/// Number of leading string arguments a directive takes before its parameter list.
[[nodiscard]] usize GetPositionalCount(std::string_view name) noexcept
{
    if (name == "Texture")
    {
        return 3;
    }
    if (name == "MediumInterface")
    {
        return 2;
    }
    static constexpr std::string_view ONE[] = {
        "MakeNamedMaterial",
        "NamedMaterial",
        "Material",
        "Shape",
        "LightSource",
        "AreaLightSource",
        "Camera",
        "Film",
        "Sampler",
        "Integrator",
        "PixelFilter",
        "Accelerator",
        "MakeNamedMedium",
        "ObjectBegin",
        "ObjectInstance",
        "Include",
        "Import",
        "CoordinateSystem",
        "CoordSysTransform",
        "ColorSpace",
        "Attribute",
    };
    for (const std::string_view entry : ONE)
    {
        if (entry == name)
        {
            return 1;
        }
    }
    return 0;
}

[[nodiscard]] bool IsKnownDirective(std::string_view name) noexcept
{
    static constexpr std::string_view KNOWN[] = {
        "Texture",
        "MediumInterface",
        "MakeNamedMaterial",
        "NamedMaterial",
        "Material",
        "Shape",
        "LightSource",
        "AreaLightSource",
        "Camera",
        "Film",
        "Sampler",
        "Integrator",
        "PixelFilter",
        "Accelerator",
        "MakeNamedMedium",
        "ObjectBegin",
        "ObjectEnd",
        "ObjectInstance",
        "Include",
        "Import",
        "CoordinateSystem",
        "CoordSysTransform",
        "ColorSpace",
        "Attribute",
        "Option",
        "Identity",
        "Translate",
        "Scale",
        "Rotate",
        "LookAt",
        "Transform",
        "ConcatTransform",
        "TransformTimes",
        "ActiveTransform",
        "ReverseOrientation",
        "AttributeBegin",
        "AttributeEnd",
        "TransformBegin",
        "TransformEnd",
        "WorldBegin",
        "WorldEnd",
    };
    for (const std::string_view entry : KNOWN)
    {
        if (entry == name)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool IsBooleanWord(std::string_view text) noexcept
{
    return text == "true" || text == "false";
}

// ---------------------------------------------------------------------------
// Graphics state.

enum class BlockKind : uint8
{
    Attribute,
    Transform,
};

struct GraphicsState final
{
    math::Matrix4d Transform = math::Matrix4d::Identity();
    uint32 Material = IR_NONE;
    bool HasAreaLight = false;
    uint32 AreaLight = IR_NONE;
    bool IsEmissionTwoSided = false;
    bool HasEmissionPower = false;
    float64 EmissionPowerLumens = 0.0;
    float64 EmissionScale = 1.0;
    float64 Emission[3] = {0.0, 0.0, 0.0};
    bool IsReversed = false;
};

struct SavedState final
{
    BlockKind Kind = BlockKind::Attribute;
    GraphicsState State;
    uint32 Line = 0;
};

struct NamedTransform final
{
    String Name;
    math::Matrix4d Transform;
};

/// A declared scalar texture: an evaluated constant or an image in SceneIr.
struct FloatTexture final
{
    String Name;
    bool IsConstant = false;
    float64 Value = .5;
    uint32 Image = IR_NONE;
};

struct AlphaMaterial final
{
    uint32 Source = IR_NONE;
    uint32 Texture = IR_NONE;
    float64 Opacity = 1.0;
    uint32 Material = IR_NONE;
};

struct PlyCacheEntry final
{
    String File;
    uint32 Mesh = IR_NONE;
};

/// ByteSource over an open SequentialFile.
class FileByteSource final : public ByteSource
{
public:
    explicit FileByteSource(detail::SequentialFile& inoutFile) noexcept : mFile(inoutFile) {}
    [[nodiscard]] Status Read(std::span<uint8> outBuffer, usize& outCount) noexcept override
    {
        return mFile.Read(outBuffer, outCount);
    }

private:
    detail::SequentialFile& mFile;
};

/// A source path inside the import root: root-relative for reports, native for I/O.
struct ResolvedSourcePath final
{
    String Relative;
    String Native;
};

class SceneReader final
{
public:
    SceneReader(std::string_view root,
                SceneIr& outScene,
                DiagnosticLog& inoutLog,
                const Cancellation* cancelOrNull) noexcept
        : mRoot(root), mScene(outScene), mLog(inoutLog), mCancelOrNull(cancelOrNull)
    {
    }

    [[nodiscard]] Status ReadEntry(ByteSource& inoutSource, std::string_view relativeName) noexcept;
    [[nodiscard]] Status Finish() noexcept;

private:
    void report(DiagnosticSeverity severity, std::string_view code, uint32 line, std::string_view message) noexcept;
    void warn(std::string_view code, uint32 line, std::string_view message) noexcept
    {
        report(DiagnosticSeverity::Warning, code, line, message);
    }
    void inform(std::string_view code, uint32 line, std::string_view message) noexcept
    {
        report(DiagnosticSeverity::Info, code, line, message);
    }
    void reportUnusedParameters(const Directive& directive) noexcept;
    void markAllUsed(Directive& inoutDirective) noexcept
    {
        for (Parameter& parameter : inoutDirective.Parameters)
        {
            parameter.IsUsed = true;
        }
    }

    [[nodiscard]] Status readFile(ByteSource& inoutSource, uint32 fileIndex) noexcept;
    [[nodiscard]] Status readItem(Tokenizer& inoutTokens, ArgumentItem& outItem, bool& outIsDone) noexcept;
    [[nodiscard]] Status addTokenToItem(const Token& token, ArgumentItem& inoutItem) noexcept;
    [[nodiscard]] Status readDirective(Tokenizer& inoutTokens, Directive& outDirective) noexcept;
    [[nodiscard]] Status applyDirective(Directive& inoutDirective) noexcept;
    [[nodiscard]] Status includeFile(Directive& inoutDirective) noexcept;

    [[nodiscard]] Parameter* findParameter(Directive& inoutDirective, std::string_view name) noexcept;
    [[nodiscard]] bool findFloat(Directive& inoutDirective, std::string_view name, float64& outValue) noexcept;
    [[nodiscard]] bool
    findString(Directive& inoutDirective, std::string_view name, std::string_view& outValue) noexcept;
    [[nodiscard]] bool findBoolean(Directive& inoutDirective, std::string_view name, bool& outValue) noexcept;
    [[nodiscard]] bool findColour(Directive& inoutDirective, std::string_view name, float64 (&outColour)[3]) noexcept;

    [[nodiscard]] bool applyTransformDirective(Directive& inoutDirective) noexcept;

    [[nodiscard]] Status defineTexture(Directive& inoutDirective) noexcept;
    [[nodiscard]] Status
    defineMaterial(Directive& inoutDirective, std::string_view type, std::string_view name) noexcept;
    [[nodiscard]] Status applyNamedMaterial(Directive& inoutDirective) noexcept;
    [[nodiscard]] Status addShape(Directive& inoutDirective) noexcept;
    [[nodiscard]] Status readShapeAlpha(Directive& inoutDirective, uint32& inoutMaterial) noexcept;
    [[nodiscard]] Status readNormalMap(Directive& inoutDirective, IrMaterial& inoutMaterial) noexcept;
    [[nodiscard]] Status addLight(Directive& inoutDirective) noexcept;
    [[nodiscard]] Status applyAreaLight(Directive& inoutDirective) noexcept;
    [[nodiscard]] Status applyCamera(Directive& inoutDirective) noexcept;
    [[nodiscard]] Status beginObject(Directive& inoutDirective) noexcept;
    [[nodiscard]] Status addInstance(Directive& inoutDirective) noexcept;
    [[nodiscard]] Status loadPly(Directive& inoutDirective, uint32& outMesh) noexcept;
    [[nodiscard]] bool resolveSourcePath(std::string_view name, ResolvedSourcePath& outPath) noexcept;
    [[nodiscard]] uint32 findSpectrumTexture(std::string_view name) const noexcept;
    [[nodiscard]] const FloatTexture* findFloatTexture(std::string_view name) const noexcept;
    void readRoughness(Directive& inoutDirective, float64 defaultValue, float64& outRoughness) noexcept;
    void readBaseColour(Directive& inoutDirective,
                        std::string_view parameterName,
                        float64 defaultGrey,
                        IrMaterial& inoutMaterial) noexcept;

    std::string_view mRoot;
    SceneIr& mScene;
    DiagnosticLog& mLog;
    const Cancellation* mCancelOrNull;
    GraphicsState mState;
    Array<SavedState> mStack;
    Array<NamedTransform> mCoordinateSystems;
    Array<FloatTexture> mFloatTextures;
    Array<AlphaMaterial> mAlphaMaterials;
    Array<PlyCacheEntry> mPlyCache;
    /// Resolved native paths of the files currently being read (cycle detection).
    Array<String> mIncludeStack;
    uint32 mCurrentFile = IR_NONE;
    uint32 mCurrentLine = 0;
    uint32 mOpenObject = IR_NONE;
    bool mHasOutOfMemory = false;
    bool mHasReportedInstanceTruncation = false;
    math::Matrix4d mCameraTransform = math::Matrix4d::Identity();
    float64 mFieldOfViewDegrees = 90.0;
};

void SceneReader::report(DiagnosticSeverity severity,
                         std::string_view code,
                         uint32 line,
                         std::string_view message) noexcept
{
    std::string_view file;
    if (mCurrentFile != IR_NONE && mCurrentFile < mScene.SourceFiles.GetSize())
    {
        file = mScene.SourceFiles[mCurrentFile].GetView();
    }
    if (mLog.Add(severity, code, file, line, message) == false)
    {
        mHasOutOfMemory = true;
    }
}

void SceneReader::reportUnusedParameters(const Directive& directive) noexcept
{
    for (const Parameter& parameter : directive.Parameters)
    {
        if (parameter.IsUsed == true)
        {
            continue;
        }
        char text[160]{};
        usize length = 0;
        for (const std::string_view part : {directive.Name.GetView(),
                                            std::string_view(": "),
                                            parameter.Type.GetView(),
                                            std::string_view(" "),
                                            parameter.Name.GetView()})
        {
            for (usize i = 0; i < part.size() && length < sizeof(text); ++i)
            {
                text[length++] = part[i];
            }
        }
        inform(codes::UNKNOWN_PARAMETER, directive.Line, std::string_view(text, length));
    }
}

// ---------------------------------------------------------------------------
// Token stream to directives.

Status SceneReader::addTokenToItem(const Token& token, ArgumentItem& inoutItem) noexcept
{
    if (token.Kind == TokenKind::Number)
    {
        float64 value = 0.0;
        if (ParseNumber(token.Text.GetView(), value) == false)
        {
            warn(codes::SYNTAX_ERROR, token.Line, "malformed number");
            value = 0.0;
        }
        return inoutItem.Numbers.TryAdd(value) == true ? Status::Ok : Status::OutOfMemory;
    }
    if (IsBooleanWord(token.Text.GetView()) == true)
    {
        if (inoutItem.Booleans.TryAdd(token.Text.GetView() == "true" ? uint8{1} : uint8{0}) == false)
        {
            return Status::OutOfMemory;
        }
        if (token.Kind == TokenKind::Word)
        {
            return Status::Ok;
        }
    }
    String text;
    if (text.TryAssign(token.Text.GetView()) != StringStatus::Ok ||
        inoutItem.Strings.TryAdd(static_cast<String&&>(text)) == false)
    {
        return Status::OutOfMemory;
    }
    return Status::Ok;
}

Status SceneReader::readItem(Tokenizer& inoutTokens, ArgumentItem& outItem, bool& outIsDone) noexcept
{
    outIsDone = false;
    outItem = ArgumentItem{};
    const Token& token = inoutTokens.GetCurrent();
    if (token.Kind == TokenKind::End || (token.Kind == TokenKind::Word && IsBooleanWord(token.Text.GetView()) == false))
    {
        outIsDone = true;
        return Status::Ok;
    }
    if (token.Kind == TokenKind::CloseBracket)
    {
        warn(codes::SYNTAX_ERROR, token.Line, "unexpected ']'");
        return inoutTokens.Advance() == TokenizerStatus::Ok ? Status::Ok : Status::Invalid;
    }
    if (token.Kind != TokenKind::OpenBracket)
    {
        const Status status = addTokenToItem(token, outItem);
        if (status != Status::Ok)
        {
            return status;
        }
        return inoutTokens.Advance() == TokenizerStatus::Ok ? Status::Ok : Status::Invalid;
    }
    outItem.IsList = true;
    while (true)
    {
        if (inoutTokens.Advance() != TokenizerStatus::Ok)
        {
            return Status::Invalid;
        }
        const Token& element = inoutTokens.GetCurrent();
        if (element.Kind == TokenKind::CloseBracket)
        {
            return inoutTokens.Advance() == TokenizerStatus::Ok ? Status::Ok : Status::Invalid;
        }
        if (element.Kind == TokenKind::End || element.Kind == TokenKind::OpenBracket ||
            (element.Kind == TokenKind::Word && IsBooleanWord(element.Text.GetView()) == false))
        {
            warn(codes::SYNTAX_ERROR, element.Line, "unterminated '[' list");
            return element.Kind == TokenKind::OpenBracket ? Status::Invalid : Status::Ok;
        }
        const Status status = addTokenToItem(element, outItem);
        if (status != Status::Ok)
        {
            return status;
        }
    }
}

Status SceneReader::readDirective(Tokenizer& inoutTokens, Directive& outDirective) noexcept
{
    const Token& head = inoutTokens.GetCurrent();
    outDirective.Positional.Clear();
    outDirective.Numbers.Clear();
    outDirective.Parameters.Clear();
    outDirective.Line = head.Line;
    if (outDirective.Name.TryAssign(head.Text.GetView()) != StringStatus::Ok)
    {
        return Status::OutOfMemory;
    }
    if (inoutTokens.Advance() != TokenizerStatus::Ok)
    {
        return Status::Invalid;
    }
    const usize positionalCount = GetPositionalCount(outDirective.Name.GetView());
    bool isAwaitingValue = false;
    while (true)
    {
        ArgumentItem item;
        bool isDone = false;
        const Status status = readItem(inoutTokens, item, isDone);
        if (status != Status::Ok || isDone == true)
        {
            if (isAwaitingValue == true)
            {
                warn(codes::SYNTAX_ERROR, outDirective.Line, "parameter without a value");
                outDirective.Parameters.RemoveLast();
            }
            return status;
        }
        if (isAwaitingValue == true)
        {
            outDirective.Parameters.GetLast().Value = static_cast<ArgumentItem&&>(item);
            isAwaitingValue = false;
            continue;
        }
        const bool isSingleString = item.IsList == false && item.Strings.GetSize() == 1;
        if (isSingleString == true && outDirective.Positional.GetSize() < positionalCount)
        {
            if (outDirective.Positional.TryAdd(static_cast<String&&>(item.Strings[0])) == false)
            {
                return Status::OutOfMemory;
            }
            continue;
        }
        if (isSingleString == false)
        {
            if (outDirective.Parameters.IsEmpty() == false)
            {
                warn(codes::SYNTAX_ERROR, outDirective.Line, "value without a parameter declaration");
                continue;
            }
            if (outDirective.Numbers.TryAddRange(item.Numbers.AsSpan()) == false)
            {
                return Status::OutOfMemory;
            }
            continue;
        }
        // A "type name" declaration; its value is the next item.
        const std::string_view declaration = item.Strings[0].GetView();
        usize space = declaration.find(' ');
        if (space == std::string_view::npos)
        {
            warn(codes::SYNTAX_ERROR, outDirective.Line, "malformed parameter declaration");
            space = 0;
        }
        std::string_view name = declaration.substr(space);
        while (name.empty() == false && name.front() == ' ')
        {
            name.remove_prefix(1);
        }
        Parameter parameter;
        if (parameter.Type.TryAssign(declaration.substr(0, space)) != StringStatus::Ok ||
            parameter.Name.TryAssign(name) != StringStatus::Ok ||
            outDirective.Parameters.TryAdd(static_cast<Parameter&&>(parameter)) == false)
        {
            return Status::OutOfMemory;
        }
        isAwaitingValue = true;
    }
}

Status SceneReader::readFile(ByteSource& inoutSource, uint32 fileIndex) noexcept
{
    const uint32 previousFile = mCurrentFile;
    mCurrentFile = fileIndex;
    Tokenizer tokens(inoutSource);
    Status result = Status::Ok;
    const auto reportTokenizerError = [&]() noexcept {
        const std::string_view message =
            tokens.GetErrorMessage().empty() == false ? tokens.GetErrorMessage() : std::string_view("syntax error");
        const uint32 line = tokens.GetErrorLine() != 0 ? tokens.GetErrorLine() : tokens.GetCurrent().Line;
        report(DiagnosticSeverity::Error, codes::SYNTAX_ERROR, line, message);
    };
    if (tokens.Advance() != TokenizerStatus::Ok)
    {
        reportTokenizerError();
        mCurrentFile = previousFile;
        return Status::Invalid;
    }
    Directive directive;
    while (tokens.GetCurrent().Kind != TokenKind::End)
    {
        if (mCancelOrNull != nullptr && mCancelOrNull->Requested != nullptr &&
            mCancelOrNull->Requested(mCancelOrNull->ContextOrNull) == true)
        {
            result = Status::Cancelled;
            break;
        }
        const Token& head = tokens.GetCurrent();
        if (head.Kind != TokenKind::Word)
        {
            warn(codes::SYNTAX_ERROR, head.Line, "expected a directive");
            ArgumentItem ignored;
            bool isDone = false;
            if (readItem(tokens, ignored, isDone) != Status::Ok)
            {
                reportTokenizerError();
                result = Status::Invalid;
                break;
            }
            continue;
        }
        const Status read = readDirective(tokens, directive);
        if (read == Status::OutOfMemory)
        {
            result = read;
            break;
        }
        if (read != Status::Ok)
        {
            reportTokenizerError();
            result = Status::Invalid;
            break;
        }
        mCurrentLine = directive.Line;
        const Status applied = applyDirective(directive);
        if (applied != Status::Ok)
        {
            result = applied;
            break;
        }
        if (mHasOutOfMemory == true)
        {
            result = Status::OutOfMemory;
            break;
        }
    }
    mCurrentFile = previousFile;
    return result;
}

// ---------------------------------------------------------------------------
// Parameter access.

Parameter* SceneReader::findParameter(Directive& inoutDirective, std::string_view name) noexcept
{
    for (Parameter& parameter : inoutDirective.Parameters)
    {
        if (parameter.Name.GetView() == name)
        {
            parameter.IsUsed = true;
            return &parameter;
        }
    }
    return nullptr;
}

bool SceneReader::findFloat(Directive& inoutDirective, std::string_view name, float64& outValue) noexcept
{
    const Parameter* parameter = findParameter(inoutDirective, name);
    if (parameter == nullptr || parameter->Value.Numbers.IsEmpty() == true ||
        math::IsFinite(parameter->Value.Numbers[0]) == false)
    {
        return false;
    }
    outValue = parameter->Value.Numbers[0];
    return true;
}

bool SceneReader::findString(Directive& inoutDirective, std::string_view name, std::string_view& outValue) noexcept
{
    const Parameter* parameter = findParameter(inoutDirective, name);
    if (parameter == nullptr || parameter->Value.Strings.IsEmpty() == true)
    {
        return false;
    }
    outValue = parameter->Value.Strings[0].GetView();
    return true;
}

bool SceneReader::findBoolean(Directive& inoutDirective, std::string_view name, bool& outValue) noexcept
{
    const Parameter* parameter = findParameter(inoutDirective, name);
    if (parameter == nullptr || parameter->Value.Booleans.IsEmpty() == true)
    {
        return false;
    }
    outValue = parameter->Value.Booleans[0] != 0;
    return true;
}

bool SceneReader::findColour(Directive& inoutDirective, std::string_view name, float64 (&outColour)[3]) noexcept
{
    Parameter* parameter = findParameter(inoutDirective, name);
    if (parameter == nullptr)
    {
        return false;
    }
    const ArgumentItem& value = parameter->Value;
    const std::string_view type = parameter->Type.GetView();
    if ((type == "rgb" || type == "color") && value.Numbers.GetSize() >= 3)
    {
        for (usize k = 0; k < 3; ++k)
        {
            outColour[k] = math::IsFinite(value.Numbers[k]) == true ? math::Max(value.Numbers[k], 0.0) : 0.0;
        }
        return true;
    }
    if (type == "blackbody" && value.Numbers.IsEmpty() == false)
    {
        ConvertBlackbodyToRgb(value.Numbers[0], outColour);
        return true;
    }
    if (type == "float" && value.Numbers.IsEmpty() == false)
    {
        outColour[0] = outColour[1] = outColour[2] = math::Max(value.Numbers[0], 0.0);
        return true;
    }
    if (type != "spectrum")
    {
        return false;
    }
    if (value.Strings.IsEmpty() == false && LookUpNamedSpectrum(value.Strings[0].GetView(), outColour) == true)
    {
        inform(codes::SPECTRUM_APPROXIMATED, inoutDirective.Line, value.Strings[0].GetView());
        return true;
    }
    if (value.Numbers.GetSize() >= 2)
    {
        // (wavelength, value) pairs: the mean value becomes grey.
        float64 sum = 0.0;
        usize count = 0;
        for (usize i = 1; i < value.Numbers.GetSize(); i += 2)
        {
            sum += value.Numbers[i];
            ++count;
        }
        outColour[0] = outColour[1] = outColour[2] = math::Max(sum / static_cast<float64>(count), 0.0);
        warn(codes::SPECTRUM_APPROXIMATED, inoutDirective.Line, "sampled spectrum averaged to grey");
        return true;
    }
    outColour[0] = outColour[1] = outColour[2] = 1.0;
    warn(codes::SPECTRUM_APPROXIMATED, inoutDirective.Line, "unknown spectrum replaced with white");
    return true;
}

// ---------------------------------------------------------------------------
// Transforms.

bool SceneReader::applyTransformDirective(Directive& inoutDirective) noexcept
{
    const std::string_view name = inoutDirective.Name.GetView();
    const Array<float64>& values = inoutDirective.Numbers;
    math::Matrix4d& transform = mState.Transform;
    const auto hasFiniteValues = [&](usize count) noexcept -> bool {
        bool isFinite = values.GetSize() >= count;
        for (usize i = 0; isFinite == true && i < count; ++i)
        {
            isFinite = math::IsFinite(values[i]);
        }
        if (isFinite == false)
        {
            warn(codes::SYNTAX_ERROR, inoutDirective.Line, "transform needs finite numeric arguments");
        }
        return isFinite;
    };
    if (name == "Identity")
    {
        transform = math::Matrix4d::Identity();
    }
    else if (name == "Translate")
    {
        if (hasFiniteValues(3) == true)
        {
            transform = MultiplyMatrices(transform, MakeTranslation(math::Vector3d{values[0], values[1], values[2]}));
        }
    }
    else if (name == "Scale")
    {
        if (hasFiniteValues(3) == true)
        {
            transform = MultiplyMatrices(transform, MakeScale(math::Vector3d{values[0], values[1], values[2]}));
        }
    }
    else if (name == "Rotate")
    {
        math::Matrix4d rotation;
        if (hasFiniteValues(4) == true)
        {
            if (MakeRotation(values[0], math::Vector3d{values[1], values[2], values[3]}, rotation) == true)
            {
                transform = MultiplyMatrices(transform, rotation);
            }
            else
            {
                warn(codes::SYNTAX_ERROR, inoutDirective.Line, "Rotate axis is zero");
            }
        }
    }
    else if (name == "LookAt")
    {
        math::Matrix4d look;
        if (hasFiniteValues(9) == true)
        {
            if (MakeLookAt(values.AsSpan().first(9), look) == true)
            {
                transform = MultiplyMatrices(transform, look);
            }
            else
            {
                warn(codes::SYNTAX_ERROR, inoutDirective.Line, "degenerate LookAt");
            }
        }
    }
    else if (name == "Transform" || name == "ConcatTransform")
    {
        if (hasFiniteValues(16) == true)
        {
            math::Matrix4d matrix;
            for (usize k = 0; k < 16; ++k)
            {
                matrix.Elements[k] = values[k];
            }
            transform = name == "Transform" ? matrix : MultiplyMatrices(transform, matrix);
        }
    }
    else if (name == "CoordinateSystem")
    {
        NamedTransform named;
        named.Transform = transform;
        if (inoutDirective.Positional.IsEmpty() == false &&
            (named.Name.TryAssign(inoutDirective.Positional[0].GetView()) != StringStatus::Ok ||
             mCoordinateSystems.TryAdd(static_cast<NamedTransform&&>(named)) == false))
        {
            mHasOutOfMemory = true;
        }
    }
    else if (name == "CoordSysTransform")
    {
        const std::string_view system =
            inoutDirective.Positional.IsEmpty() == false ? inoutDirective.Positional[0].GetView() : std::string_view();
        bool isFound = false;
        for (usize i = mCoordinateSystems.GetSize(); i > 0 && isFound == false; --i)
        {
            if (mCoordinateSystems[i - 1].Name.GetView() == system)
            {
                transform = mCoordinateSystems[i - 1].Transform;
                isFound = true;
            }
        }
        if (isFound == false)
        {
            warn(codes::NAME_UNDEFINED, inoutDirective.Line, "unknown coordinate system");
        }
    }
    else
    {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Directive dispatch.

Status SceneReader::applyDirective(Directive& inoutDirective) noexcept
{
    const std::string_view name = inoutDirective.Name.GetView();
    const std::string_view first =
        inoutDirective.Positional.IsEmpty() == false ? inoutDirective.Positional[0].GetView() : std::string_view();
    if (IsKnownDirective(name) == false)
    {
        warn(codes::UNKNOWN_DIRECTIVE, inoutDirective.Line, name);
        return Status::Ok;
    }
    if (applyTransformDirective(inoutDirective) == true)
    {
        return Status::Ok;
    }
    if (name == "AttributeBegin" || name == "TransformBegin")
    {
        if (name == "TransformBegin")
        {
            inform(codes::DEPRECATED_DIRECTIVE, inoutDirective.Line, "TransformBegin");
        }
        SavedState saved;
        saved.Kind = name == "AttributeBegin" ? BlockKind::Attribute : BlockKind::Transform;
        saved.State = mState;
        saved.Line = inoutDirective.Line;
        return mStack.TryAdd(saved) == true ? Status::Ok : Status::OutOfMemory;
    }
    if (name == "AttributeEnd" || name == "TransformEnd")
    {
        const BlockKind kind = name == "AttributeEnd" ? BlockKind::Attribute : BlockKind::Transform;
        if (mStack.IsEmpty() == true)
        {
            warn(codes::UNBALANCED_BLOCK, inoutDirective.Line, name);
            return Status::Ok;
        }
        const SavedState& saved = mStack.GetLast();
        if (saved.Kind != kind)
        {
            warn(codes::UNBALANCED_BLOCK, inoutDirective.Line, "mismatched block end");
        }
        if (kind == BlockKind::Transform)
        {
            mState.Transform = saved.State.Transform;
        }
        else
        {
            mState = saved.State;
        }
        mStack.RemoveLast();
        return Status::Ok;
    }
    if (name == "ReverseOrientation")
    {
        mState.IsReversed = mState.IsReversed == false;
        return Status::Ok;
    }
    if (name == "WorldBegin")
    {
        mState.Transform = math::Matrix4d::Identity();
        return Status::Ok;
    }
    if (name == "Film")
    {
        float64 width = 0.0;
        float64 height = 0.0;
        if (findFloat(inoutDirective, "xresolution", width) == true && width >= 1.0 && width < 1e6)
        {
            mScene.FilmWidth = static_cast<uint32>(width);
        }
        if (findFloat(inoutDirective, "yresolution", height) == true && height >= 1.0 && height < 1e6)
        {
            mScene.FilmHeight = static_cast<uint32>(height);
        }
        inform(codes::DIRECTIVE_IGNORED, inoutDirective.Line, "Film (resolution kept for the camera aspect)");
        return Status::Ok;
    }
    if (name == "Sampler" || name == "Integrator" || name == "PixelFilter" || name == "Accelerator" ||
        name == "ColorSpace" || name == "Option" || name == "Attribute" || name == "WorldEnd" ||
        name == "TransformTimes" || name == "ActiveTransform" || name == "MediumInterface")
    {
        inform(codes::DIRECTIVE_IGNORED, inoutDirective.Line, name);
        return Status::Ok;
    }
    if (name == "MakeNamedMedium")
    {
        warn(codes::MEDIUM_SKIPPED, inoutDirective.Line, first);
        return Status::Ok;
    }
    if (name == "Include" || name == "Import")
    {
        return includeFile(inoutDirective);
    }
    if (name == "ObjectEnd")
    {
        if (mOpenObject == IR_NONE)
        {
            warn(codes::UNBALANCED_BLOCK, inoutDirective.Line, "ObjectEnd without ObjectBegin");
            return Status::Ok;
        }
        mOpenObject = IR_NONE;
        if (mStack.IsEmpty() == false)
        {
            mState = mStack.GetLast().State;
            mStack.RemoveLast();
        }
        return Status::Ok;
    }
    Status status = Status::Ok;
    if (name == "Camera")
    {
        status = applyCamera(inoutDirective);
    }
    else if (name == "Texture")
    {
        status = defineTexture(inoutDirective);
    }
    else if (name == "Material")
    {
        status = defineMaterial(inoutDirective, first, std::string_view());
    }
    else if (name == "MakeNamedMaterial")
    {
        std::string_view type;
        if (findString(inoutDirective, "type", type) == false)
        {
            warn(codes::MATERIAL_DEFAULTED, inoutDirective.Line, "named material without a type");
        }
        status = defineMaterial(inoutDirective, type, first);
    }
    else if (name == "NamedMaterial")
    {
        status = applyNamedMaterial(inoutDirective);
    }
    else if (name == "Shape")
    {
        status = addShape(inoutDirective);
    }
    else if (name == "AreaLightSource")
    {
        status = applyAreaLight(inoutDirective);
    }
    else if (name == "LightSource")
    {
        status = addLight(inoutDirective);
    }
    else if (name == "ObjectBegin")
    {
        status = beginObject(inoutDirective);
    }
    else if (name == "ObjectInstance")
    {
        status = addInstance(inoutDirective);
    }
    if (status == Status::Ok)
    {
        reportUnusedParameters(inoutDirective);
    }
    return status;
}

// ---------------------------------------------------------------------------
// Files.

bool SceneReader::resolveSourcePath(std::string_view name, ResolvedSourcePath& outPath) noexcept
{
    if (mRoot.empty() == true || name.empty() == true || name.front() == '/' ||
        name.find('\\') != std::string_view::npos || name.find('\0') != std::string_view::npos)
    {
        return false;
    }
    String joined;
    const std::string_view parts[] = {mRoot, std::string_view("/"), name};
    if (joined.TryAppendMany(parts) != StringStatus::Ok ||
        detail::ResolvePath(joined.GetView(), outPath.Native) != Status::Ok)
    {
        return false;
    }
    const std::string_view native = outPath.Native.GetView();
    if (native.size() <= mRoot.size() + 1 || native.substr(0, mRoot.size()) != mRoot || native[mRoot.size()] != '/')
    {
        return false;
    }
    return outPath.Relative.TryAssign(native.substr(mRoot.size() + 1)) == StringStatus::Ok;
}

Status SceneReader::includeFile(Directive& inoutDirective) noexcept
{
    const std::string_view name =
        inoutDirective.Positional.IsEmpty() == false ? inoutDirective.Positional[0].GetView() : std::string_view();
    ResolvedSourcePath resolved;
    if (resolveSourcePath(name, resolved) == false)
    {
        warn(codes::INCLUDE_FAILED, inoutDirective.Line, "path is missing or outside the import root");
        return Status::Ok;
    }
    if (mIncludeStack.GetSize() >= MAX_INCLUDE_DEPTH)
    {
        warn(codes::INCLUDE_FAILED, inoutDirective.Line, "include depth limit reached");
        return Status::Ok;
    }
    for (const String& open : mIncludeStack)
    {
        if (open.GetView() == resolved.Native.GetView())
        {
            warn(codes::INCLUDE_FAILED, inoutDirective.Line, "include cycle");
            return Status::Ok;
        }
    }
    detail::SequentialFile file;
    if (file.Open(resolved.Native.GetView()) != Status::Ok)
    {
        warn(codes::INCLUDE_FAILED, inoutDirective.Line, "cannot open the included file");
        return Status::Ok;
    }
    FileByteSource fileSource(file);
    HashingByteSource source(fileSource);
    const uint32 fileIndex = static_cast<uint32>(mScene.SourceFiles.GetSize());
    if (mScene.SourceFiles.TryAdd(static_cast<String&&>(resolved.Relative)) == false ||
        mScene.SourceDigests.TryAdd(Digest{}) == false ||
        mIncludeStack.TryAdd(static_cast<String&&>(resolved.Native)) == false)
    {
        return Status::OutOfMemory;
    }
    const Status status = readFile(source, fileIndex);
    mIncludeStack.RemoveLast();
    if (source.Drain() != Status::Ok)
    {
        return Status::IoError;
    }
    mScene.SourceDigests[fileIndex] = source.GetDigest();
    // A syntax error inside an included file stops that file only.
    return status == Status::Invalid ? Status::Ok : status;
}

Status SceneReader::ReadEntry(ByteSource& inoutSource, std::string_view relativeName) noexcept
{
    String name;
    if (name.TryAssign(relativeName) != StringStatus::Ok ||
        mScene.SourceFiles.TryAdd(static_cast<String&&>(name)) == false ||
        mScene.SourceDigests.TryAdd(Digest{}) == false)
    {
        return Status::OutOfMemory;
    }
    if (mRoot.empty() == false)
    {
        String native;
        const std::string_view parts[] = {mRoot, std::string_view("/"), relativeName};
        if (native.TryAppendMany(parts) != StringStatus::Ok ||
            mIncludeStack.TryAdd(static_cast<String&&>(native)) == false)
        {
            return Status::OutOfMemory;
        }
    }
    HashingByteSource source(inoutSource);
    const Status status = readFile(source, 0);
    if (source.Drain() != Status::Ok)
    {
        return Status::IoError;
    }
    mScene.SourceDigests[0] = source.GetDigest();
    return status;
}

// ---------------------------------------------------------------------------
// Textures.

uint32 SceneReader::findSpectrumTexture(std::string_view name) const noexcept
{
    for (usize i = mScene.Textures.GetSize(); i > 0; --i)
    {
        if (mScene.Textures[i - 1].Name.GetView() == name && mScene.Textures[i - 1].IsScalar == false)
        {
            return static_cast<uint32>(i - 1);
        }
    }
    return IR_NONE;
}

const FloatTexture* SceneReader::findFloatTexture(std::string_view name) const noexcept
{
    for (usize i = mFloatTextures.GetSize(); i > 0; --i)
    {
        if (mFloatTextures[i - 1].Name.GetView() == name)
        {
            return &mFloatTextures[i - 1];
        }
    }
    return nullptr;
}

Status SceneReader::defineTexture(Directive& inoutDirective) noexcept
{
    if (inoutDirective.Positional.GetSize() < 3)
    {
        warn(codes::SYNTAX_ERROR, inoutDirective.Line, "Texture needs a name, type and class");
        markAllUsed(inoutDirective);
        return Status::Ok;
    }
    const std::string_view name = inoutDirective.Positional[0].GetView();
    const std::string_view type = inoutDirective.Positional[1].GetView();
    const std::string_view textureClass = inoutDirective.Positional[2].GetView();
    float64 scale = 1.0;
    (void)findFloat(inoutDirective, "scale", scale);
    if (type == "float" && textureClass != "imagemap")
    {
        if (findFloatTexture(name) != nullptr)
        {
            inform(codes::NAME_REDEFINED, inoutDirective.Line, name);
        }
        FloatTexture texture;
        texture.IsConstant = textureClass == "constant";
        if (texture.IsConstant == true)
        {
            texture.Value = 1.0;
            (void)findFloat(inoutDirective, "value", texture.Value);
        }
        else
        {
            inform(codes::TEXTURE_APPROXIMATED, inoutDirective.Line, "float texture evaluated as a constant");
        }
        texture.Value *= scale;
        markAllUsed(inoutDirective);
        if (texture.Name.TryAssign(name) != StringStatus::Ok ||
            mFloatTextures.TryAdd(static_cast<FloatTexture&&>(texture)) == false)
        {
            return Status::OutOfMemory;
        }
        return Status::Ok;
    }
    if (type != "spectrum" && type != "color" && type != "float")
    {
        warn(codes::TEXTURE_APPROXIMATED, inoutDirective.Line, "unknown texture type");
        markAllUsed(inoutDirective);
        return Status::Ok;
    }
    if ((type == "float" && findFloatTexture(name) != nullptr) ||
        (type != "float" && findSpectrumTexture(name) != IR_NONE))
    {
        inform(codes::NAME_REDEFINED, inoutDirective.Line, name);
    }
    IrTexture texture;
    texture.IsScalar = type == "float";
    texture.SourceFile = mCurrentFile;
    texture.Line = inoutDirective.Line;
    if (texture.Name.TryAssign(name) != StringStatus::Ok)
    {
        return Status::OutOfMemory;
    }
    if (textureClass == "imagemap")
    {
        std::string_view file;
        ResolvedSourcePath resolved;
        if (findString(inoutDirective, "filename", file) == false || resolveSourcePath(file, resolved) == false)
        {
            warn(codes::TEXTURE_UNREADABLE, inoutDirective.Line, "image path is missing or outside the import root");
            texture.File.Clear();
            texture.Scale[0] = texture.Scale[1] = texture.Scale[2] = .5;
        }
        else
        {
            texture.File = static_cast<String&&>(resolved.Relative);
        }
        // PBRT float imagemaps default to sRGB for PNG, linear otherwise.
        if (type == "float" && file.ends_with(".png") == false && file.ends_with(".PNG") == false)
        {
            texture.Encoding = TextureEncoding::Linear;
        }
        std::string_view text;
        if (findString(inoutDirective, "encoding", text) == true)
        {
            if (text == "linear")
            {
                texture.Encoding = TextureEncoding::Linear;
            }
            else if (text == "sRGB" || text == "srgb")
            {
                texture.Encoding = TextureEncoding::Srgb;
            }
            else if (text.substr(0, 6) == "gamma ")
            {
                float64 gamma = 2.2;
                texture.Encoding = TextureEncoding::Gamma;
                if (ParseNumber(text.substr(6), gamma) == true && gamma > 0.0)
                {
                    texture.Gamma = gamma;
                }
            }
        }
        if (findString(inoutDirective, "wrap", text) == true)
        {
            texture.IsRepeated = text == "repeat";
        }
        bool isInverted = false;
        if (findBoolean(inoutDirective, "invert", isInverted) == true)
        {
            texture.IsInverted = isInverted;
        }
        (void)findParameter(inoutDirective, "filter");
        (void)findParameter(inoutDirective, "maxanisotropy");
        for (float64& channel : texture.Scale)
        {
            channel *= scale;
        }
    }
    else if (textureClass == "constant")
    {
        float64 value[3] = {1.0, 1.0, 1.0};
        (void)findColour(inoutDirective, "value", value);
        for (usize k = 0; k < 3; ++k)
        {
            texture.Scale[k] = value[k] * scale;
        }
    }
    else if (textureClass == "scale")
    {
        // tex x scale: reuse the referenced image with a multiplier.
        const Parameter* referenced = findParameter(inoutDirective, "tex");
        const uint32 source = referenced != nullptr && referenced->Type.GetView() == "texture" &&
                                      referenced->Value.Strings.IsEmpty() == false
                                  ? findSpectrumTexture(referenced->Value.Strings[0].GetView())
                                  : IR_NONE;
        if (source != IR_NONE)
        {
            const IrTexture& base = mScene.Textures[source];
            if (texture.File.TryAssign(base.File.GetView()) != StringStatus::Ok)
            {
                return Status::OutOfMemory;
            }
            texture.Encoding = base.Encoding;
            texture.Gamma = base.Gamma;
            texture.IsInverted = base.IsInverted;
            texture.IsRepeated = base.IsRepeated;
            for (usize k = 0; k < 3; ++k)
            {
                texture.Scale[k] = base.Scale[k] * scale;
            }
        }
        else
        {
            float64 colour[3] = {1.0, 1.0, 1.0};
            if (referenced != nullptr)
            {
                Parameter* writable = findParameter(inoutDirective, "tex");
                writable->IsUsed = true;
                (void)findColour(inoutDirective, "tex", colour);
            }
            for (usize k = 0; k < 3; ++k)
            {
                texture.Scale[k] = colour[k] * scale;
            }
        }
    }
    else
    {
        warn(codes::TEXTURE_APPROXIMATED, inoutDirective.Line, textureClass);
        texture.Scale[0] = texture.Scale[1] = texture.Scale[2] = .5;
        markAllUsed(inoutDirective);
    }
    if (type == "float")
    {
        FloatTexture scalar;
        scalar.Image = static_cast<uint32>(mScene.Textures.GetSize());
        if (scalar.Name.TryAssign(name) != StringStatus::Ok ||
            mFloatTextures.TryAdd(static_cast<FloatTexture&&>(scalar)) == false)
        {
            return Status::OutOfMemory;
        }
    }
    return mScene.Textures.TryAdd(static_cast<IrTexture&&>(texture)) == true ? Status::Ok : Status::OutOfMemory;
}

// ---------------------------------------------------------------------------
// Materials.

void SceneReader::readRoughness(Directive& inoutDirective, float64 defaultValue, float64& outRoughness) noexcept
{
    // pbrt maps roughness r to the microfacet alpha as sqrt(r) when remaproughness
    // is true (the default) and as r otherwise. The engine's perceptual roughness
    // is sqrt(alpha).
    float64 raw = defaultValue;
    bool isFound = false;
    for (const std::string_view name :
         {std::string_view("roughness"), std::string_view("uroughness"), std::string_view("vroughness")})
    {
        const Parameter* parameter = findParameter(inoutDirective, name);
        if (parameter == nullptr || isFound == true)
        {
            continue;
        }
        if (parameter->Value.Numbers.IsEmpty() == false && math::IsFinite(parameter->Value.Numbers[0]) == true)
        {
            raw = parameter->Value.Numbers[0];
            isFound = true;
        }
        else if (parameter->Value.Strings.IsEmpty() == false)
        {
            const FloatTexture* texture = findFloatTexture(parameter->Value.Strings[0].GetView());
            raw = texture != nullptr ? texture->Value : .5;
            inform(codes::TEXTURE_APPROXIMATED, inoutDirective.Line, "roughness texture evaluated as a constant");
            isFound = true;
        }
    }
    bool isRemapped = true;
    (void)findBoolean(inoutDirective, "remaproughness", isRemapped);
    raw = math::Clamp(raw, 0.0, 1.0);
    const float64 alpha = isRemapped == true ? math::Sqrt(raw) : raw;
    outRoughness = math::Clamp(math::Sqrt(alpha), 0.045, 1.0);
}

void SceneReader::readBaseColour(Directive& inoutDirective,
                                 std::string_view parameterName,
                                 float64 defaultGrey,
                                 IrMaterial& inoutMaterial) noexcept
{
    inoutMaterial.BaseColour[0] = inoutMaterial.BaseColour[1] = inoutMaterial.BaseColour[2] = defaultGrey;
    Parameter* parameter = findParameter(inoutDirective, parameterName);
    if (parameter == nullptr)
    {
        return;
    }
    if (parameter->Type.GetView() != "texture")
    {
        (void)findColour(inoutDirective, parameterName, inoutMaterial.BaseColour);
        return;
    }
    const std::string_view textureName =
        parameter->Value.Strings.IsEmpty() == false ? parameter->Value.Strings[0].GetView() : std::string_view();
    inoutMaterial.BaseColourTexture = findSpectrumTexture(textureName);
    if (inoutMaterial.BaseColourTexture != IR_NONE)
    {
        inoutMaterial.BaseColour[0] = inoutMaterial.BaseColour[1] = inoutMaterial.BaseColour[2] = 1.0;
        return;
    }
    if (const FloatTexture* grey = findFloatTexture(textureName); grey != nullptr)
    {
        inoutMaterial.BaseColour[0] = inoutMaterial.BaseColour[1] = inoutMaterial.BaseColour[2] = grey->Value;
        inform(codes::TEXTURE_APPROXIMATED, inoutDirective.Line, "float texture used as a grey base colour");
        return;
    }
    warn(codes::NAME_UNDEFINED, inoutDirective.Line, textureName);
}

Status SceneReader::defineMaterial(Directive& inoutDirective, std::string_view type, std::string_view name) noexcept
{
    IrMaterial material;
    material.SourceFile = mCurrentFile;
    material.Line = inoutDirective.Line;
    if (material.Name.TryAssign(name) != StringStatus::Ok || material.SourceType.TryAssign(type) != StringStatus::Ok)
    {
        return Status::OutOfMemory;
    }
    const std::string_view label = name.empty() == false ? name : type;
    if (findParameter(inoutDirective, "displacement") != nullptr)
    {
        warn(codes::DISPLACEMENT_IGNORED, inoutDirective.Line, label);
    }
    const Status normalStatus = readNormalMap(inoutDirective, material);
    if (normalStatus != Status::Ok)
    {
        return normalStatus;
    }
    if (type == "diffuse" || type == "diffusetransmission")
    {
        readBaseColour(inoutDirective, "reflectance", .5, material);
        material.Roughness = 1.0;
        if (type == "diffusetransmission")
        {
            material.IsApproximated = true;
            warn(codes::TRANSMISSION_DROPPED, inoutDirective.Line, label);
        }
    }
    else if (type == "coateddiffuse")
    {
        readBaseColour(inoutDirective, "reflectance", .5, material);
        readRoughness(inoutDirective, 0.0, material.Roughness);
    }
    else if (type == "conductor" || type == "coatedconductor" || type == "metal")
    {
        const bool isCoated = type == "coatedconductor";
        const std::string_view reflectanceName = isCoated == true ? "conductor.reflectance" : "reflectance";
        const std::string_view etaName = isCoated == true ? "conductor.eta" : "eta";
        material.Metallic = 1.0;
        if (findParameter(inoutDirective, reflectanceName) != nullptr)
        {
            readBaseColour(inoutDirective, reflectanceName, 1.0, material);
        }
        else
        {
            const Parameter* eta = findParameter(inoutDirective, etaName);
            const std::string_view spectrum = eta != nullptr && eta->Value.Strings.IsEmpty() == false
                                                  ? eta->Value.Strings[0].GetView()
                                                  : std::string_view("metal-Cu-eta");
            if (LookUpConductorColour(spectrum, material.BaseColour) == false)
            {
                material.BaseColour[0] = material.BaseColour[1] = material.BaseColour[2] = .9;
                material.IsApproximated = true;
            }
            inform(codes::SPECTRUM_APPROXIMATED, inoutDirective.Line, "conductor colour from a fixed eta/k table");
        }
        if (isCoated == true)
        {
            Parameter* conductorRoughness = findParameter(inoutDirective, "conductor.roughness");
            if (conductorRoughness != nullptr && conductorRoughness->Value.Numbers.IsEmpty() == false)
            {
                bool isRemapped = true;
                (void)findBoolean(inoutDirective, "remaproughness", isRemapped);
                const float64 raw = math::Clamp(conductorRoughness->Value.Numbers[0], 0.0, 1.0);
                material.Roughness = math::Clamp(math::Sqrt(isRemapped == true ? math::Sqrt(raw) : raw), 0.045, 1.0);
            }
            else
            {
                material.Roughness = .045;
            }
        }
        else
        {
            readRoughness(inoutDirective, 0.0, material.Roughness);
        }
    }
    else if (type == "dielectric" || type == "thindielectric")
    {
        material.BaseColour[0] = material.BaseColour[1] = material.BaseColour[2] = .04;
        readRoughness(inoutDirective, 0.0, material.Roughness);
        material.IsApproximated = true;
        warn(codes::TRANSMISSION_DROPPED, inoutDirective.Line, label);
    }
    else if (type == "mix")
    {
        material.IsApproximated = true;
        const Parameter* materials = findParameter(inoutDirective, "materials");
        const std::string_view firstName = materials != nullptr && materials->Value.Strings.IsEmpty() == false
                                               ? materials->Value.Strings[0].GetView()
                                               : std::string_view();
        bool isFound = false;
        for (usize i = mScene.Materials.GetSize(); i > 0 && firstName.empty() == false && isFound == false; --i)
        {
            const IrMaterial& candidate = mScene.Materials[i - 1];
            if (candidate.Name.GetView() == firstName)
            {
                for (usize k = 0; k < 3; ++k)
                {
                    material.BaseColour[k] = candidate.BaseColour[k];
                }
                material.BaseColourTexture = candidate.BaseColourTexture;
                material.Metallic = candidate.Metallic;
                material.Roughness = candidate.Roughness;
                isFound = true;
            }
        }
        warn(codes::MIX_APPROXIMATED, inoutDirective.Line, isFound == true ? "first material used" : "default used");
    }
    else
    {
        material.IsApproximated = true;
        warn(codes::MATERIAL_DEFAULTED, inoutDirective.Line, type.empty() == false ? type : "missing type");
        markAllUsed(inoutDirective);
    }
    // Recognised parameters with no PBR slot; read so the unknown-parameter report stays focused.
    for (const std::string_view known : {std::string_view("type"),
                                         std::string_view("remaproughness"),
                                         std::string_view("thickness"),
                                         std::string_view("albedo"),
                                         std::string_view("g"),
                                         std::string_view("maxdepth"),
                                         std::string_view("nsamples"),
                                         std::string_view("transmittance"),
                                         std::string_view("interface.eta"),
                                         std::string_view("interface.roughness"),
                                         std::string_view("interface.uroughness"),
                                         std::string_view("interface.vroughness"),
                                         std::string_view("conductor.k"),
                                         std::string_view("k"),
                                         std::string_view("eta"),
                                         std::string_view("conductor.eta"),
                                         std::string_view("amount")})
    {
        (void)findParameter(inoutDirective, known);
    }
    if (findParameter(inoutDirective, "alpha") != nullptr)
    {
        warn(codes::ALPHA_APPROXIMATED, inoutDirective.Line, "alpha cut-out cooked as opaque");
    }
    if (name.empty() == false)
    {
        for (const IrMaterial& existing : mScene.Materials)
        {
            if (existing.Name.GetView() == name)
            {
                inform(codes::NAME_REDEFINED, inoutDirective.Line, name);
                break;
            }
        }
    }
    const uint32 index = static_cast<uint32>(mScene.Materials.GetSize());
    if (mScene.Materials.TryAdd(static_cast<IrMaterial&&>(material)) == false)
    {
        return Status::OutOfMemory;
    }
    if (name.empty() == true)
    {
        mState.Material = index;
    }
    return Status::Ok;
}

Status SceneReader::applyNamedMaterial(Directive& inoutDirective) noexcept
{
    const std::string_view name =
        inoutDirective.Positional.IsEmpty() == false ? inoutDirective.Positional[0].GetView() : std::string_view();
    for (usize i = mScene.Materials.GetSize(); i > 0; --i)
    {
        if (mScene.Materials[i - 1].Name.GetView() == name)
        {
            mState.Material = static_cast<uint32>(i - 1);
            return Status::Ok;
        }
    }
    warn(codes::NAME_UNDEFINED, inoutDirective.Line, name);
    mState.Material = IR_NONE;
    return Status::Ok;
}

// ---------------------------------------------------------------------------
// Lights and camera.

Status SceneReader::applyAreaLight(Directive& inoutDirective) noexcept
{
    const std::string_view type =
        inoutDirective.Positional.IsEmpty() == false ? inoutDirective.Positional[0].GetView() : std::string_view();
    ++mScene.Lights.Area;
    if (type != "diffuse")
    {
        warn(codes::LIGHT_SKIPPED, inoutDirective.Line, "unsupported area light type");
        markAllUsed(inoutDirective);
        mState.HasAreaLight = false;
        return Status::Ok;
    }
    float64 radiance[3] = {1.0, 1.0, 1.0};
    (void)findColour(inoutDirective, "L", radiance);
    float64 scale = 1.0;
    (void)findFloat(inoutDirective, "scale", scale);
    mState.EmissionScale = scale;
    mState.AreaLight = mScene.Lights.Area - 1;
    mState.IsEmissionTwoSided = false;
    (void)findBoolean(inoutDirective, "twosided", mState.IsEmissionTwoSided);
    mState.HasEmissionPower = findFloat(inoutDirective, "power", mState.EmissionPowerLumens);
    if (scale < 0.0 || (mState.HasEmissionPower == true && mState.EmissionPowerLumens < 0.0))
    {
        warn(codes::LIGHT_SKIPPED, inoutDirective.Line, "negative area-light scale or power rejected");
        mState.HasAreaLight = false;
        markAllUsed(inoutDirective);
        return Status::Ok;
    }
    (void)findParameter(inoutDirective, "filename");
    mState.HasAreaLight = true;
    for (usize k = 0; k < 3; ++k)
    {
        mState.Emission[k] = math::Max(radiance[k] * scale, 0.0);
    }
    return Status::Ok;
}

Status SceneReader::addLight(Directive& inoutDirective) noexcept
{
    const std::string_view type =
        inoutDirective.Positional.IsEmpty() == false ? inoutDirective.Positional[0].GetView() : std::string_view();
    IrLightCounts& counts = mScene.Lights;
    if (type == "infinite")
    {
        ++counts.Infinite;
        if (counts.Infinite > 1)
        {
            warn(codes::LIMIT_EXCEEDED,
                 inoutDirective.Line,
                 "multiple infinite sources require separate scene imports");
            return Status::Invalid;
        }
        IrEnvironment& environment = mScene.Environment;
        float64 radiance[3] = {1.0, 1.0, 1.0};
        (void)findColour(inoutDirective, "L", radiance);
        float64 scale = 1.0;
        (void)findFloat(inoutDirective, "scale", scale);
        environment.Scale = scale;
        environment.LightToWorld = mState.Transform;
        environment.HasIlluminance = findFloat(inoutDirective, "illuminance", environment.IlluminanceLux);
        if (scale < 0 || (environment.HasIlluminance == true && environment.IlluminanceLux < 0))
        {
            warn(codes::LIGHT_SKIPPED, inoutDirective.Line, "negative environment scale or illuminance");
            environment.IsPresent = false;
            return Status::Ok;
        }
        (void)findParameter(inoutDirective, "portal");
        std::string_view file;
        ResolvedSourcePath resolved;
        environment.File.Clear();
        if (findString(inoutDirective, "filename", file) == true && resolveSourcePath(file, resolved) == true)
        {
            environment.File = static_cast<String&&>(resolved.Relative);
        }
        else if (file.empty() == false)
        {
            warn(codes::TEXTURE_UNREADABLE, inoutDirective.Line, "environment map is missing or outside the root");
            return Status::Invalid;
        }
        environment.IsPresent = true;
        for (usize k = 0; k < 3; ++k)
        {
            environment.Radiance[k] = radiance[k] * scale;
        }
        warn(codes::ENVIRONMENT_FALLBACK,
             inoutDirective.Line,
             "infinite source uses the frozen Rec709 photometric/environment-map conversion");
        return Status::Ok;
    }
    IrAnalyticLight light;
    if (type == "point")
    {
        ++counts.Point;
        light.Kind = IrLightKind::Point;
    }
    else if (type == "spot")
    {
        ++counts.Spot;
        light.Kind = IrLightKind::Spot;
    }
    else if (type == "distant")
    {
        ++counts.Distant;
        light.Kind = IrLightKind::Directional;
    }
    else
    {
        ++counts.Other;
        markAllUsed(inoutDirective);
        warn(codes::LIGHT_SKIPPED, inoutDirective.Line, type);
        return Status::Ok;
    }
    // Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, Physically Based
    // Rendering, fourth edition, sections 12.2/12.3 (point/spot and distant
    // lights): preserve source intensity/irradiance and transformed from/to.
    // https://pbr-book.org/4ed/Light_Sources/Point_Lights
    // https://pbr-book.org/4ed/Light_Sources/Distant_Lights
    float64 strength[3] = {1.0, 1.0, 1.0};
    (void)findColour(inoutDirective, light.Kind == IrLightKind::Directional ? "L" : "I", strength);
    float64 scale = 1.0;
    (void)findFloat(inoutDirective, "scale", scale);
    light.Strength = {strength[0] * scale, strength[1] * scale, strength[2] * scale};
    light.Scale = scale;
    math::Vector3d from{};
    math::Vector3d to{0.0, 0.0, 1.0};
    const auto readPoint = [this, &inoutDirective](std::string_view name, math::Vector3d& outPoint) noexcept {
        const Parameter* parameterOrNull = findParameter(inoutDirective, name);
        if (parameterOrNull != nullptr && parameterOrNull->Value.Numbers.GetSize() == 3)
        {
            outPoint = {parameterOrNull->Value.Numbers[0],
                        parameterOrNull->Value.Numbers[1],
                        parameterOrNull->Value.Numbers[2]};
        }
    };
    readPoint("from", from);
    readPoint("to", to);
    light.PositionWorld = TransformPoint(mState.Transform, from);
    const math::Vector3d direction = light.Kind == IrLightKind::Point ? math::Vector3d{0.0, 0.0, 1.0}
                                                                      : TransformDirection(mState.Transform, to - from);
    const float64 length = math::Length(direction);
    float64 cone = 30.0;
    float64 delta = 5.0;
    if (light.Kind == IrLightKind::Spot)
    {
        (void)findFloat(inoutDirective, "coneangle", cone);
        (void)findFloat(inoutDirective, "conedeltaangle", delta);
        light.OuterAngleDegrees = cone;
        light.InnerAngleDegrees = cone - delta;
    }
    light.HasPhotometricNormalization = findFloat(inoutDirective,
                                                  light.Kind == IrLightKind::Directional ? "illuminance" : "power",
                                                  light.PhotometricNormalization);
    light.SourceFile = mCurrentFile;
    light.Line = inoutDirective.Line;
    if (scale < 0.0 || math::IsFinite(light.Strength.X) == false || math::IsFinite(light.Strength.Y) == false ||
        math::IsFinite(light.Strength.Z) == false || light.Strength.X < 0.0 || light.Strength.Y < 0.0 ||
        light.Strength.Z < 0.0 || math::IsFinite(light.PositionWorld.X) == false ||
        math::IsFinite(light.PositionWorld.Y) == false || math::IsFinite(light.PositionWorld.Z) == false ||
        math::IsFinite(length) == false || length <= 0.0 || cone <= 0.0 || cone >= 90.0 || delta <= 0.0 ||
        delta > cone || (light.HasPhotometricNormalization == true && light.PhotometricNormalization < 0.0))
    {
        warn(codes::LIGHT_SKIPPED, inoutDirective.Line, "invalid authored analytic light");
        return Status::Ok;
    }
    light.DirectionWorld = direction * (1.0 / length);
    return mScene.AnalyticLights.TryAdd(light) == true ? Status::Ok : Status::OutOfMemory;
}

Status SceneReader::applyCamera(Directive& inoutDirective) noexcept
{
    const std::string_view type =
        inoutDirective.Positional.IsEmpty() == false ? inoutDirective.Positional[0].GetView() : std::string_view();
    if (type != "perspective")
    {
        warn(codes::CAMERA_APPROXIMATED, inoutDirective.Line, "non-perspective camera replaced with perspective");
    }
    float64 fov = 90.0;
    if (findFloat(inoutDirective, "fov", fov) == false || fov <= 0.0 || fov >= 180.0)
    {
        fov = 90.0;
    }
    markAllUsed(inoutDirective);
    mFieldOfViewDegrees = fov;
    mCameraTransform = mState.Transform;
    math::Matrix4d worldFromCamera;
    if (TryInvertAffine(mState.Transform, worldFromCamera) == false)
    {
        warn(codes::CAMERA_DEFAULTED, inoutDirective.Line, "camera transform is not invertible");
        return Status::Ok;
    }
    IrCamera& camera = mScene.Camera;
    camera.IsPresent = true;
    camera.Eye = TransformPoint(worldFromCamera, math::Vector3d{0.0, 0.0, 0.0});
    camera.Target = camera.Eye + TransformDirection(worldFromCamera, math::Vector3d{0.0, 0.0, 1.0});
    camera.Up = TransformDirection(worldFromCamera, math::Vector3d{0.0, 1.0, 0.0});
    NamedTransform named;
    named.Transform = worldFromCamera;
    if (named.Name.TryAssign("camera") != StringStatus::Ok ||
        mCoordinateSystems.TryAdd(static_cast<NamedTransform&&>(named)) == false)
    {
        return Status::OutOfMemory;
    }
    return Status::Ok;
}

// ---------------------------------------------------------------------------
// Shapes and instancing.

Status SceneReader::loadPly(Directive& inoutDirective, uint32& outMesh) noexcept
{
    outMesh = IR_NONE;
    std::string_view file;
    ResolvedSourcePath resolved;
    if (findString(inoutDirective, "filename", file) == false || resolveSourcePath(file, resolved) == false)
    {
        warn(codes::SHAPE_FAILED, inoutDirective.Line, "PLY path is missing or outside the import root");
        return Status::Ok;
    }
    for (const PlyCacheEntry& entry : mPlyCache)
    {
        if (entry.File.GetView() == resolved.Relative.GetView())
        {
            outMesh = entry.Mesh;
            return Status::Ok;
        }
    }
    Array<uint8> bytes;
    const Status read = detail::ReadFileBounded(resolved.Native.GetView(), MAX_PLY_BYTES, bytes);
    if (read == Status::OutOfMemory)
    {
        return read;
    }
    IrMesh mesh;
    PlyHeaderSummary summary;
    std::string_view message = "cannot read the PLY file";
    if (read != Status::Ok || ReadPly(bytes.AsSpan(), mesh, summary, message) == false)
    {
        warn(codes::SHAPE_FAILED, inoutDirective.Line, message);
        return Status::Ok;
    }
    if (summary.HasExtraFaceProperties == true)
    {
        inform(codes::FACE_INDICES_IGNORED, inoutDirective.Line, "extra per-face PLY properties ignored");
    }
    String dataFile;
    if (dataFile.TryAssign(resolved.Relative.GetView()) != StringStatus::Ok ||
        mScene.DataFiles.TryAdd(static_cast<String&&>(dataFile)) == false ||
        mScene.DataDigests.TryAdd(Hash(bytes.AsSpan())) == false)
    {
        return Status::OutOfMemory;
    }
    PlyCacheEntry entry;
    entry.Mesh = static_cast<uint32>(mScene.Meshes.GetSize());
    entry.File = static_cast<String&&>(resolved.Relative);
    outMesh = entry.Mesh;
    if (mScene.Meshes.TryAdd(static_cast<IrMesh&&>(mesh)) == false ||
        mPlyCache.TryAdd(static_cast<PlyCacheEntry&&>(entry)) == false)
    {
        return Status::OutOfMemory;
    }
    return Status::Ok;
}

// Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, pbrt-v4 Input File
// Format, Materials and Shapes (https://www.pbrt.org/fileformat-v4): normalmap
// names a linear tangent-space image; alpha belongs to each shape. We use the
// existing MikkTSpace surface-map path and approximate fractional alpha at 0.5.
Status SceneReader::readNormalMap(Directive& inoutDirective, IrMaterial& inoutMaterial) noexcept
{
    std::string_view file;
    if (findString(inoutDirective, "normalmap", file) == false)
    {
        return Status::Ok;
    }
    ResolvedSourcePath resolved;
    if (resolveSourcePath(file, resolved) == false)
    {
        warn(codes::NORMALMAP_DROPPED, inoutDirective.Line, "normal image path is outside the import root");
        return Status::Ok;
    }
    for (usize i = 0; i < mScene.Textures.GetSize(); ++i)
    {
        const IrTexture& existing = mScene.Textures[i];
        if (existing.File.GetView() == resolved.Relative.GetView() && existing.Name.IsEmpty() == true &&
            existing.Encoding == TextureEncoding::Linear)
        {
            inoutMaterial.NormalTexture = static_cast<uint32>(i);
            return Status::Ok;
        }
    }
    IrTexture texture;
    texture.File = static_cast<String&&>(resolved.Relative);
    texture.Encoding = TextureEncoding::Linear;
    texture.SourceFile = mCurrentFile;
    texture.Line = inoutDirective.Line;
    inoutMaterial.NormalTexture = static_cast<uint32>(mScene.Textures.GetSize());
    return mScene.Textures.TryAdd(static_cast<IrTexture&&>(texture)) == true ? Status::Ok : Status::OutOfMemory;
}

Status SceneReader::readShapeAlpha(Directive& inoutDirective, uint32& inoutMaterial) noexcept
{
    const Parameter* parameterOrNull = findParameter(inoutDirective, "alpha");
    if (parameterOrNull == nullptr)
    {
        return Status::Ok;
    }
    uint32 textureIndex = IR_NONE;
    float64 opacity = 1.0;
    if (parameterOrNull->Type.GetView() == "texture" && parameterOrNull->Value.Strings.IsEmpty() == false)
    {
        const FloatTexture* textureOrNull = findFloatTexture(parameterOrNull->Value.Strings[0].GetView());
        if (textureOrNull == nullptr)
        {
            warn(codes::NAME_UNDEFINED, inoutDirective.Line, parameterOrNull->Value.Strings[0].GetView());
            warn(codes::ALPHA_APPROXIMATED, inoutDirective.Line, "undefined alpha texture cooked as opaque");
            return Status::Ok;
        }
        textureIndex = textureOrNull->Image;
        if (textureIndex != IR_NONE && mScene.Textures[textureIndex].File.IsEmpty() == true)
        {
            warn(codes::ALPHA_APPROXIMATED, inoutDirective.Line, "invalid alpha image path cooked as opaque");
            return Status::Ok;
        }
        opacity = textureIndex != IR_NONE ? 1.0 : textureOrNull->Value;
    }
    else if (findFloat(inoutDirective, "alpha", opacity) == false)
    {
        warn(codes::ALPHA_APPROXIMATED, inoutDirective.Line, "invalid alpha cooked as opaque");
        return Status::Ok;
    }
    opacity = math::Clamp(opacity, 0.0, 1.0);
    if (textureIndex == IR_NONE && opacity == 1.0)
    {
        return Status::Ok;
    }
    inform(codes::ALPHA_APPROXIMATED, inoutDirective.Line, "shape alpha retained as a mask at cutoff 0.5");
    for (const AlphaMaterial& existing : mAlphaMaterials)
    {
        if (existing.Source == inoutMaterial && existing.Texture == textureIndex && existing.Opacity == opacity)
        {
            inoutMaterial = existing.Material;
            return Status::Ok;
        }
    }
    IrMaterial material;
    if (inoutMaterial != IR_NONE)
    {
        // Copy the surface terms this PBRT front-end produces; the variant has no
        // declared name, so subsequent NamedMaterial lookups still find the original.
        const IrMaterial& source = mScene.Materials[inoutMaterial];
        if (material.SourceType.TryAssign(source.SourceType.GetView()) != StringStatus::Ok)
        {
            return Status::OutOfMemory;
        }
        for (usize k = 0; k < IR_RGB_CHANNEL_COUNT; ++k)
        {
            material.BaseColour[k] = source.BaseColour[k];
        }
        material.BaseColourTexture = source.BaseColourTexture;
        material.Metallic = source.Metallic;
        material.Roughness = source.Roughness;
        material.NormalTexture = source.NormalTexture;
        material.IsApproximated = source.IsApproximated;
    }
    material.SourceFile = mCurrentFile;
    material.Line = inoutDirective.Line;
    material.IsShapeVariant = true;
    material.VariantSource = inoutMaterial;
    material.Opacity = opacity;
    material.OpacityTexture = textureIndex;
    material.IsOpacityFloatImage = textureIndex != IR_NONE;
    material.IsBaseAlphaIgnored = true;
    material.AlphaMode = IrAlphaMode::Mask;
    const uint32 variant = static_cast<uint32>(mScene.Materials.GetSize());
    if (mAlphaMaterials.TryAdd(
            { .Source = inoutMaterial, .Texture = textureIndex, .Opacity = opacity, .Material = variant }) == false ||
        mScene.Materials.TryAdd(static_cast<IrMaterial&&>(material)) == false)
    {
        return Status::OutOfMemory;
    }
    inoutMaterial = variant;
    return Status::Ok;
}

Status SceneReader::addShape(Directive& inoutDirective) noexcept
{
    const std::string_view type =
        inoutDirective.Positional.IsEmpty() == false ? inoutDirective.Positional[0].GetView() : std::string_view();
    if (findParameter(inoutDirective, "displacement") != nullptr)
    {
        inform(codes::DISPLACEMENT_IGNORED, inoutDirective.Line, type);
    }
    (void)findParameter(inoutDirective, "edgelength");
    uint32 materialIndex = mState.Material;
    const Status alphaStatus = readShapeAlpha(inoutDirective, materialIndex);
    if (alphaStatus != Status::Ok)
    {
        return alphaStatus;
    }
    uint32 meshIndex = IR_NONE;
    if (type == "plymesh")
    {
        const Status status = loadPly(inoutDirective, meshIndex);
        if (status != Status::Ok)
        {
            return status;
        }
    }
    else
    {
        ShapeArguments arguments;
        const auto copyNumbers = [&](std::string_view name, Array<float64>& outValues) noexcept -> bool {
            const Parameter* parameter = findParameter(inoutDirective, name);
            return parameter == nullptr || outValues.TryAddRange(parameter->Value.Numbers.AsSpan()) == true;
        };
        bool isCopied = copyNumbers("P", arguments.Positions) == true && copyNumbers("N", arguments.Normals) == true &&
                        copyNumbers("indices", arguments.Indices) == true && copyNumbers("uv", arguments.Uvs) == true;
        if (arguments.Uvs.IsEmpty() == true)
        {
            isCopied = isCopied == true && copyNumbers("st", arguments.Uvs) == true;
        }
        if (isCopied == false)
        {
            return Status::OutOfMemory;
        }
        (void)findFloat(inoutDirective, "radius", arguments.Radius);
        (void)findFloat(inoutDirective, "innerradius", arguments.InnerRadius);
        (void)findFloat(inoutDirective, "height", arguments.Height);
        (void)findFloat(inoutDirective, "zmin", arguments.ZMin);
        (void)findFloat(inoutDirective, "zmax", arguments.ZMax);
        (void)findFloat(inoutDirective, "phimax", arguments.PhiMaxDegrees);
        for (const std::string_view known :
             {std::string_view("S"), std::string_view("faceIndices"), std::string_view("levels")})
        {
            (void)findParameter(inoutDirective, known);
        }
        IrMesh mesh;
        std::string_view message;
        const ShapeResult result = TessellateShape(type, arguments, mesh, message);
        if (result == ShapeResult::OutOfMemory)
        {
            return Status::OutOfMemory;
        }
        if (result == ShapeResult::Unsupported)
        {
            markAllUsed(inoutDirective);
            warn(codes::SHAPE_SKIPPED, inoutDirective.Line, type);
            return Status::Ok;
        }
        if (result == ShapeResult::Invalid)
        {
            warn(codes::SHAPE_FAILED, inoutDirective.Line, message);
            return Status::Ok;
        }
        if (result == ShapeResult::Approximated)
        {
            warn(codes::SUBDIV_SKIPPED, inoutDirective.Line, "loopsubdiv cooked as its base cage");
        }
        meshIndex = static_cast<uint32>(mScene.Meshes.GetSize());
        if (mScene.Meshes.TryAdd(static_cast<IrMesh&&>(mesh)) == false)
        {
            return Status::OutOfMemory;
        }
    }
    if (meshIndex == IR_NONE)
    {
        return Status::Ok;
    }
    if (materialIndex != IR_NONE && (type == "plymesh" || type == "trianglemesh" || type == "loopsubdiv"))
    {
        const IrMaterial& material = mScene.Materials[materialIndex];
        const bool hasImageMapping = material.NormalTexture != IR_NONE || material.OpacityTexture != IR_NONE ||
                                     (material.BaseColourTexture != IR_NONE &&
                                      mScene.Textures[material.BaseColourTexture].File.IsEmpty() == false);
        if (hasImageMapping == true)
        {
            const Status mapped = ApplyDefaultTriangleUvs(mScene.Meshes[meshIndex]);
            if (mapped != Status::Ok)
            {
                return mapped;
            }
        }
    }
    IrShape shape;
    shape.Mesh = meshIndex;
    shape.Material = materialIndex;
    shape.Transform = mState.Transform;
    shape.IsOrientationReversed = mState.IsReversed;
    shape.SourceFile = mCurrentFile;
    shape.Line = inoutDirective.Line;
    if (mState.HasAreaLight == true)
    {
        shape.AreaLight = mState.AreaLight;
        shape.IsEmissionTwoSided = mState.IsEmissionTwoSided;
        shape.HasEmissionPower = mState.HasEmissionPower;
        shape.EmissionPowerLumens = mState.EmissionPowerLumens;
        shape.EmissionScale = mState.EmissionScale;
        for (usize k = 0; k < 3; ++k)
        {
            shape.Emission[k] = mState.Emission[k];
        }
    }
    const uint32 shapeIndex = static_cast<uint32>(mScene.Shapes.GetSize());
    if (mScene.Shapes.TryAdd(shape) == false)
    {
        return Status::OutOfMemory;
    }
    Array<uint32>& owner = mOpenObject != IR_NONE ? mScene.Objects[mOpenObject].Shapes : mScene.WorldShapes;
    return owner.TryAdd(shapeIndex) == true ? Status::Ok : Status::OutOfMemory;
}

Status SceneReader::beginObject(Directive& inoutDirective) noexcept
{
    const std::string_view name =
        inoutDirective.Positional.IsEmpty() == false ? inoutDirective.Positional[0].GetView() : std::string_view();
    if (mOpenObject != IR_NONE)
    {
        warn(codes::UNBALANCED_BLOCK, inoutDirective.Line, "nested ObjectBegin");
    }
    for (const IrObject& existing : mScene.Objects)
    {
        if (existing.Name.GetView() == name)
        {
            inform(codes::NAME_REDEFINED, inoutDirective.Line, name);
            break;
        }
    }
    SavedState saved;
    saved.State = mState;
    saved.Line = inoutDirective.Line;
    IrObject object;
    if (object.Name.TryAssign(name) != StringStatus::Ok || mStack.TryAdd(saved) == false)
    {
        return Status::OutOfMemory;
    }
    mOpenObject = static_cast<uint32>(mScene.Objects.GetSize());
    return mScene.Objects.TryAdd(static_cast<IrObject&&>(object)) == true ? Status::Ok : Status::OutOfMemory;
}

Status SceneReader::addInstance(Directive& inoutDirective) noexcept
{
    const std::string_view name =
        inoutDirective.Positional.IsEmpty() == false ? inoutDirective.Positional[0].GetView() : std::string_view();
    if (mOpenObject != IR_NONE)
    {
        warn(codes::SYNTAX_ERROR, inoutDirective.Line, "ObjectInstance inside ObjectBegin");
        return Status::Ok;
    }
    for (usize i = mScene.Objects.GetSize(); i > 0; --i)
    {
        if (mScene.Objects[i - 1].Name.GetView() != name)
        {
            continue;
        }
        if (mScene.Instances.GetSize() >= MAX_INSTANCES)
        {
            if (mHasReportedInstanceTruncation == false)
            {
                warn(codes::INSTANCES_TRUNCATED, inoutDirective.Line, "instance limit reached");
                mHasReportedInstanceTruncation = true;
            }
            return Status::Ok;
        }
        IrInstance instance;
        instance.Object = static_cast<uint32>(i - 1);
        instance.Transform = mState.Transform;
        return mScene.Instances.TryAdd(instance) == true ? Status::Ok : Status::OutOfMemory;
    }
    warn(codes::NAME_UNDEFINED, inoutDirective.Line, name);
    return Status::Ok;
}

Status SceneReader::Finish() noexcept
{
    mCurrentFile = 0;
    if (mStack.IsEmpty() == false)
    {
        warn(codes::UNBALANCED_BLOCK, mStack.GetLast().Line, "block left open at end of input");
    }
    if (mOpenObject != IR_NONE)
    {
        warn(codes::UNBALANCED_BLOCK, mCurrentLine, "ObjectBegin left open at end of input");
    }
    // pbrt's fov spans the shorter image axis; LUSCN stores the vertical angle.
    const float64 halfAngle = math::DegreesToRadians(mFieldOfViewDegrees) * .5;
    float64 halfTangent = math::Sin(halfAngle) / math::Cos(halfAngle);
    if (mScene.FilmWidth != 0 && mScene.FilmHeight > mScene.FilmWidth)
    {
        halfTangent *= static_cast<float64>(mScene.FilmHeight) / static_cast<float64>(mScene.FilmWidth);
    }
    mScene.Camera.VerticalFov = 2.0 * math::Atan2(halfTangent, 1.0);
    if (mScene.Camera.IsPresent == false)
    {
        warn(codes::CAMERA_DEFAULTED, 0, "no camera; the planner frames the scene bounds");
    }
    return mHasOutOfMemory == true ? Status::OutOfMemory : Status::Ok;
}

[[nodiscard]] Status RunReader(SceneReader& inoutReader,
                               ByteSource& inoutSource,
                               std::string_view entryName,
                               DiagnosticLog& inoutLog) noexcept
{
    Status status = inoutReader.ReadEntry(inoutSource, entryName);
    if (status == Status::Ok || status == Status::Invalid)
    {
        const Status finished = inoutReader.Finish();
        if (finished != Status::Ok)
        {
            status = finished;
        }
    }
    if (status == Status::Ok && inoutLog.HasErrors() == true)
    {
        status = Status::Invalid;
    }
    return status;
}
} // namespace

Status ReadSceneFromMemory(const MemoryEntryFile& entry,
                           SceneIr& outScene,
                           DiagnosticLog& inoutLog,
                           const Cancellation* cancelOrNull) noexcept
{
    SceneIr freshScene;
    outScene = static_cast<SceneIr&&>(freshScene);
    SceneReader reader(entry.Root, outScene, inoutLog, cancelOrNull);
    MemoryByteSource source(entry.Text);
    return RunReader(reader, source, entry.EntryName, inoutLog);
}

Status ReadScene(std::string_view entryFile,
                 SceneIr& outScene,
                 DiagnosticLog& inoutLog,
                 foundation::String& outRoot,
                 const Cancellation* cancelOrNull) noexcept
{
    SceneIr freshScene;
    outScene = static_cast<SceneIr&&>(freshScene);
    String native;
    const Status resolved = detail::ResolvePath(entryFile, native);
    if (resolved != Status::Ok)
    {
        return resolved;
    }
    const std::string_view path = native.GetView();
    const usize slash = path.rfind('/');
    if (slash == std::string_view::npos || slash == 0)
    {
        return Status::Invalid;
    }
    if (outRoot.TryAssign(path.substr(0, slash)) != StringStatus::Ok)
    {
        return Status::OutOfMemory;
    }
    detail::SequentialFile file;
    const Status opened = file.Open(path);
    if (opened != Status::Ok)
    {
        return opened;
    }
    FileByteSource source(file);
    SceneReader reader(outRoot.GetView(), outScene, inoutLog, cancelOrNull);
    return RunReader(reader, source, path.substr(slash + 1), inoutLog);
}
} // namespace ludus::content::scene_cook::importers::pbrt
