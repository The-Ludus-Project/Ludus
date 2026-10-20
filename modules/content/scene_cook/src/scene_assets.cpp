#include <ludus/content/scene_cook/best_effort_import.h>
#include <ludus/content/scene_cook/scene_assets.h>

#include "native_files.h"
#include "python_compat.h"
#include "python_json.h"
#include "uuid.h"

#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/strings/string.hpp>

#include <bit>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <numbers>
#include <utility>

namespace ludus::content::scene_cook
{
namespace
{
namespace math = ludus::foundation::math;
using namespace detail;
using foundation::Array;
using foundation::String;
namespace format = scene_format;

// The reference recorded SHA-256 digests of its own source files in every
// artifact's build record. They are frozen so artifact keys stay identical
// to the Phase 0 goldens (tests/goldens/scene_pipeline/expected/goldens.json,
// "python_files").
constexpr std::string_view IMPORTER_SHA256 = "d0d1e78b7e206b3034c7897cf27794cf311a628050621deadece39596b55f3eb";
constexpr std::string_view PUBLISHER_SHA256 = "ba9bd32092878c5ba7e06d1c8472ddabb805e51e6a72c5a460cfb5a87f3c96e1";
constexpr std::string_view IMPORTER_NAME = "pbrt-reference-v1";
constexpr std::string_view BUILD_TARGET = "portable-rgba8-f32-le";
constexpr std::string_view CATALOG_PATH = "assets/catalog.json";
constexpr std::string_view LOCK_PATH = "assets/.scene-writer.lock";
constexpr std::string_view OBJECTS_PATH = "assets/objects/";

constexpr usize MAX_CATALOG_RESOURCES = 4096;
// A level places at most the version 5 import profile's objects.
constexpr usize MAX_LEVEL_OBJECTS = format::IMPORT_LIMITS.Objects;
constexpr usize MAX_LEVEL_IMPORTS = 128;
constexpr usize MAX_NAME_CODE_POINTS = 128;
constexpr usize MAX_DEPENDENCY_DEPTH = 8;
// A scene manifest depends on every texture, material and mesh part of its version 5
// bundle and on its lighting part.
constexpr usize MAX_ARTIFACT_DEPENDENCIES =
    usize{format::IMPORT_LIMITS.Meshes} + format::IMPORT_LIMITS.Materials + format::IMPORT_LIMITS.Textures + 1;
// Every mesh, material and texture an assembled version 5 bundle admits, plus a scene
// manifest and lighting part per level import and the level itself.
constexpr usize MAX_CLOSURE_RESOURCES = usize{format::IMPORT_LIMITS.Meshes} + format::IMPORT_LIMITS.Materials +
                                        format::IMPORT_LIMITS.Textures + 2 * MAX_LEVEL_IMPORTS + 1;
static_assert(MAX_ASSET_BYTES >= format::IMPORT_LIMITS.BundleBytes, "an artifact holds any admitted bundle");
constexpr usize MAX_LOCK_IDENTITY_BYTES = 4096;
constexpr usize HEX_DIGEST_BYTES = 64;
constexpr usize MATERIAL_RECORD_BYTES = 24;
constexpr usize MATERIAL_RECORD_V2_BYTES = 28;
constexpr usize MATERIAL_RECORD_V3_BYTES = sizeof(format::MaterialRecordV3);
constexpr usize MATERIAL_RECORD_V5_BYTES = sizeof(format::MaterialRecordV5);
constexpr usize MATERIAL_RECORD_V6_BYTES = sizeof(format::MaterialRecordV6);
/// Offsets of the version 6 map fields after the version 3 fields.
constexpr usize MATERIAL_V6_SURFACE_OFFSET =
    offsetof(format::MaterialRecordV6, SurfaceTexture) - MATERIAL_RECORD_V3_BYTES;
constexpr usize MATERIAL_V6_EMISSION_OFFSET =
    offsetof(format::MaterialRecordV6, EmissionTexture) - MATERIAL_RECORD_V3_BYTES;
constexpr usize MATERIAL_V6_NORMAL_SCALE_OFFSET =
    offsetof(format::MaterialRecordV6, NormalScale) - MATERIAL_RECORD_V3_BYTES;
/// Offset of the version 3 reserved word (the version 5 alpha mode) after the version 2 fields.
constexpr usize MATERIAL_V3_RESERVED_OFFSET = offsetof(format::MaterialRecordV3, Reserved) - MATERIAL_RECORD_V2_BYTES;
constexpr std::string_view LIGHTING_BUILD_TARGET = "portable-rgba8-f32-le-v3";
/// Parts split from a LUSCN version 4 bundle: version 3 lighting records with
/// indexed VertexRecord meshes whose bake charts are implicit.
constexpr std::string_view INDEXED_LIGHTING_BUILD_TARGET = "portable-rgba8-f32-le-v4";
/// Parts split from a LUSCN version 5 bundle: version 4 records with alpha-masked
/// materials, within the import profile's limits.
constexpr std::string_view IMPORT_PROFILE_BUILD_TARGET = "portable-rgba8-f32-le-v5";
/// Parts split from a LUSCN version 6 bundle: tangent-framed meshes and materials with
/// surface and emission maps, within the import profile.
constexpr std::string_view SURFACE_MAP_BUILD_TARGET = "portable-rgba8-f32-le-v6";
constexpr float64 MAX_AUTHORED_MAGNITUDE = 1e12;
constexpr float64 MAX_WORLD_MAGNITUDE = 3e38;
constexpr float64 MIN_DIRECTION_SQUARED = 1e-12;
// The reference compared widened float32 values with the float64 literal .045,
// not with the float32 format constant.
constexpr float64 MIN_ROUGHNESS_REFERENCE = .045;
constexpr float64 MAX_EMISSION_REFERENCE = 10000.0;
constexpr usize CAMERA_FLOATS = 10;

// ---------------------------------------------------------------------------
// Failure plumbing.

class Failure final
{
public:
    explicit Failure(AssetError& outError) noexcept : mError(outError) {}

    bool Fail(AssetErrorCode code, std::initializer_list<std::string_view> parts) noexcept
    {
        if (mHasFailed == false)
        {
            Replace(code, parts);
        }
        return false;
    }

    bool Fail(AssetErrorCode code, std::string_view message) noexcept
    {
        return Fail(code, {message});
    }

    /// Overwrite an earlier failure (a failed rollback supersedes its cause).
    bool Replace(AssetErrorCode code, std::initializer_list<std::string_view> parts) noexcept
    {
        mHasFailed = true;
        mError.Code = code;
        char text[MAX_MESSAGE_BYTES + 1]{};
        usize at = 0;
        for (const std::string_view part : parts)
        {
            for (usize i = 0; i < part.size() && at < MAX_MESSAGE_BYTES; ++i)
            {
                text[at++] = part[i];
            }
        }
        (void)mError.Message.Set(std::string_view(text, at));
        return false;
    }

    bool FailInvalid(std::string_view message) noexcept
    {
        return Fail(AssetErrorCode::InvalidScene, message);
    }

    bool FailOutOfMemory() noexcept
    {
        return Fail(AssetErrorCode::OutOfMemory, "out of memory");
    }

    /// Map a native helper status onto the reference failure vocabulary.
    bool FailStatus(Status status, std::string_view action, std::string_view path) noexcept
    {
        switch (status)
        {
            case Status::OutOfMemory:
                return FailOutOfMemory();
            case Status::Unsupported:
                return Fail(AssetErrorCode::MissingTools, "This initial scene host requires macOS or Linux");
            default:
                return Fail(AssetErrorCode::StorageFailure, {action, " failed: ", path});
        }
    }

private:
    AssetError& mError;
    bool mHasFailed = false;
};

[[nodiscard]] bool IsCancelled(const AssetOptions& options) noexcept
{
    const Cancellation* cancelOrNull = options.CancelOrNull;
    return cancelOrNull != nullptr && cancelOrNull->Requested != nullptr &&
           cancelOrNull->Requested(cancelOrNull->ContextOrNull) == true;
}

// ---------------------------------------------------------------------------
// Strings, digests and documents.

[[nodiscard]] bool Concatenate(String& outText, std::initializer_list<std::string_view> parts) noexcept
{
    outText.Clear();
    for (const std::string_view part : parts)
    {
        if (outText.TryAppend(part) != foundation::StringStatus::Ok)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string_view GetView(const Array<uint8>& bytes) noexcept
{
    return std::string_view(reinterpret_cast<const char*>(bytes.GetData()), bytes.GetSize());
}

[[nodiscard]] std::span<const uint8> GetSpan(const Array<uint8>& bytes) noexcept
{
    return std::span<const uint8>(bytes.GetData(), bytes.GetSize());
}

[[nodiscard]] bool AreBytesEqual(std::span<const uint8> left, std::span<const uint8> right) noexcept
{
    return left.size() == right.size() &&
           (left.empty() == true || std::memcmp(left.data(), right.data(), left.size()) == 0);
}

/// Lowercase hexadecimal SHA-256 (hashlib.sha256(data).hexdigest()).
struct HexDigest final
{
    char Text[HEX_DIGEST_BYTES]{};
    [[nodiscard]] std::string_view View() const noexcept
    {
        return std::string_view(Text, HEX_DIGEST_BYTES);
    }
};

void FormatHexDigest(const Digest& digest, HexDigest& outHex) noexcept
{
    static constexpr char HEX[] = "0123456789abcdef";
    for (usize i = 0; i < sizeof(digest.Data); ++i)
    {
        outHex.Text[i * 2] = HEX[digest.Data[i] >> 4];
        outHex.Text[i * 2 + 1] = HEX[digest.Data[i] & 0xf];
    }
}

void ComputeHexDigest(std::span<const uint8> data, HexDigest& outHex) noexcept
{
    FormatHexDigest(Hash(data), outHex);
}

/// Stream-hash a source input in bounded memory; source files may exceed MAX_ASSET_BYTES.
[[nodiscard]] bool HashSourceInput(std::string_view path, HexDigest& outHex, Failure& inoutFailure) noexcept
{
    Digest digest;
    const Status status = HashFile(path, digest);
    switch (status)
    {
        case Status::Ok:
            FormatHexDigest(digest, outHex);
            return true;
        case Status::NotFound:
            return inoutFailure.Fail(AssetErrorCode::AssetCorrupt, {"No such file or directory: '", path, "'"});
        case Status::Unsupported:
            return inoutFailure.FailStatus(status, "Read", path);
        default:
            return inoutFailure.Fail(AssetErrorCode::AssetCorrupt, {"Cannot read '", path, "'"});
    }
}

struct Document final
{
    JsonTree Tree;
    JsonIndex Root = NO_JSON_NODE;
};

[[nodiscard]] bool
ParseDocument(std::span<const uint8> data, AssetErrorCode code, Document& outDocument, Failure& inoutFailure) noexcept
{
    MessageText message;
    if (ParsePythonJson(data, outDocument.Tree, outDocument.Root, message) == false)
    {
        if (outDocument.Tree.IsOk() == false)
        {
            return inoutFailure.FailOutOfMemory();
        }
        return inoutFailure.Fail(code, message.View());
    }
    return true;
}

/// canonical(value); allow_nan=False rejects nonfinite floats.
[[nodiscard]] bool
WriteCanonical(const JsonTree& tree, JsonIndex node, Array<uint8>& outBytes, Failure& inoutFailure) noexcept
{
    if (tree.IsOk() == false)
    {
        return inoutFailure.FailOutOfMemory();
    }
    if (WriteCanonicalJson(tree, node, outBytes) == false)
    {
        return inoutFailure.FailInvalid("Out of range float values are not JSON compliant");
    }
    return true;
}

/// re.fullmatch(kind + "/<uuid>", value).
[[nodiscard]] bool IsResourceIdText(std::string_view value, std::string_view kind) noexcept
{
    if (value.size() != kind.size() + 1 + UUID_TEXT_BYTES || value.substr(0, kind.size()) != kind ||
        value[kind.size()] != '/')
    {
        return false;
    }
    const std::string_view identity = value.substr(kind.size() + 1);
    for (usize i = 0; i < identity.size(); ++i)
    {
        const char item = identity[i];
        const bool isDashPosition = i == 8 || i == 13 || i == 18 || i == 23;
        const bool isLowerHex = (item >= '0' && item <= '9') || (item >= 'a' && item <= 'f');
        if ((isDashPosition == true && item != '-') || (isDashPosition == false && isLowerHex == false))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsResourceId(const JsonTree& tree, JsonIndex node, std::string_view kind) noexcept
{
    return node != NO_JSON_NODE && tree.GetKind(node) == JsonKind::String && IsResourceIdText(tree.GetText(node), kind);
}

[[nodiscard]] usize CountCodePoints(std::string_view text) noexcept
{
    usize count = 0;
    for (const char item : text)
    {
        count += (static_cast<uint8>(item) & 0xc0u) != 0x80u ? 1 : 0;
    }
    return count;
}

[[nodiscard]] PythonNumber MakeNumber(float64 value) noexcept
{
    PythonNumber number;
    number.IsFloat = true;
    number.Float = value;
    return number;
}

[[nodiscard]] PythonNumber MakeNumber(int64 value) noexcept
{
    PythonNumber number;
    number.Integer = value;
    return number;
}

/// math.isfinite(v) for an int or float.
[[nodiscard]] bool IsFinitePython(PythonNumber value) noexcept
{
    return math::IsFinite(value.ToFloat());
}

[[nodiscard]] PythonNumber GetAbsolute(PythonNumber value) noexcept
{
    if (value.IsFloat == true)
    {
        return MakeNumber(math::Abs(value.Float) + 0.0);
    }
    return value.Integer < 0 ? SubtractPython(MakeNumber(int64{0}), value) : value;
}

[[nodiscard]] bool IsAuthoredNumber(const JsonTree& tree, JsonIndex node, PythonNumber& outValue) noexcept
{
    const JsonKind kind = tree.GetKind(node);
    return (kind == JsonKind::Integer || kind == JsonKind::Float) && GetPythonNumber(tree, node, outValue) == true &&
           IsFinitePython(outValue) == true &&
           IsLessOrEqualPython(GetAbsolute(outValue), MakeNumber(MAX_AUTHORED_MAGNITUDE)) == true;
}

/// _numbers(values, count): count finite ints/floats (not bools) of magnitude <= 1e12.
[[nodiscard]] bool AreAuthoredNumbers(const JsonTree& tree, JsonIndex node, usize count) noexcept
{
    if (node == NO_JSON_NODE || tree.GetKind(node) != JsonKind::Array || tree.GetCount(node) != count)
    {
        return false;
    }
    for (usize i = 0; i < count; ++i)
    {
        PythonNumber value;
        if (IsAuthoredNumber(tree, tree.GetItem(node, i), value) == false)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] PythonNumber GetNumberItem(const JsonTree& tree, JsonIndex array, usize index) noexcept
{
    PythonNumber value;
    (void)GetPythonNumber(tree, tree.GetItem(array, index), value);
    return value;
}

[[nodiscard]] PythonNumber GetNumberMember(const JsonTree& tree, JsonIndex object, std::string_view key) noexcept
{
    PythonNumber value;
    (void)GetPythonNumber(tree, tree.GetMember(object, key), value);
    return value;
}

[[nodiscard]] bool IsNumberEqual(const JsonTree& tree, JsonIndex node, int64 expected) noexcept
{
    PythonNumber value;
    return node != NO_JSON_NODE && GetPythonNumber(tree, node, value) == true &&
           IsEqualPython(value, MakeNumber(expected)) == true;
}

// ---------------------------------------------------------------------------
// Paths, reads and atomic publication.

[[nodiscard]] bool ResolveNative(std::string_view path, String& outPath, Failure& inoutFailure) noexcept
{
    const Status status = ResolvePath(path, outPath);
    return status == Status::Ok ? true : inoutFailure.FailStatus(status, "Path resolution", path);
}

/// Path(relative).is_absolute() or ".." in Path(relative).parts.
[[nodiscard]] bool IsEscapingPath(std::string_view relative) noexcept
{
    if (relative.empty() == false && relative.front() == '/')
    {
        return true;
    }
    for (std::string_view rest = relative; rest.empty() == false;)
    {
        const usize slash = rest.find('/');
        if (rest.substr(0, slash) == "..")
        {
            return true;
        }
        rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
    }
    return false;
}

/// Path.is_relative_to(base) for resolved paths.
[[nodiscard]] bool IsBelow(std::string_view path, std::string_view base) noexcept
{
    return path == base || base == "/" ||
           (path.size() > base.size() && path.substr(0, base.size()) == base && path[base.size()] == '/');
}

/// inside(root, relative): a resolved path that must stay below the resolved root.
[[nodiscard]] bool
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ResolveInside(std::string_view root, std::string_view relative, String& outPath, Failure& inoutFailure) noexcept
{
    String resolvedRoot;
    if (ResolveNative(root, resolvedRoot, inoutFailure) == false)
    {
        return false;
    }
    if (IsEscapingPath(relative) == true)
    {
        return inoutFailure.FailInvalid("Project asset path must be relative and contained");
    }
    String joined;
    const std::string_view rootView = resolvedRoot.GetView();
    if (Concatenate(joined, {rootView, "/", relative}) == false)
    {
        return inoutFailure.FailOutOfMemory();
    }
    if (ResolveNative(joined.GetView(), outPath, inoutFailure) == false)
    {
        return false;
    }
    if (IsBelow(outPath.GetView(), rootView) == false)
    {
        return inoutFailure.FailInvalid("Project asset symlink escapes the project");
    }
    return true;
}

[[nodiscard]] std::string_view GetParent(std::string_view path) noexcept
{
    const usize slash = path.rfind('/');
    if (slash == std::string_view::npos)
    {
        return ".";
    }
    return slash == 0 ? std::string_view("/") : path.substr(0, slash);
}

/// Path.suffix.
[[nodiscard]] std::string_view GetSuffix(std::string_view path) noexcept
{
    const std::string_view name = GetBaseName(path);
    const usize dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size())
    {
        return {};
    }
    return name.substr(dot);
}

/// read(path, maximum): failures are AssetCorrupt.
[[nodiscard]] bool
ReadBytes(std::string_view path, usize maximum, Array<uint8>& outData, Failure& inoutFailure) noexcept
{
    const Status status = ReadFileBounded(path, maximum, outData);
    switch (status)
    {
        case Status::Ok:
            return true;
        case Status::Limit:
            return inoutFailure.Fail(AssetErrorCode::AssetCorrupt, "source file byte limit");
        case Status::NotFound:
            return inoutFailure.Fail(AssetErrorCode::AssetCorrupt, {"No such file or directory: '", path, "'"});
        case Status::OutOfMemory:
            return inoutFailure.FailOutOfMemory();
        case Status::Unsupported:
            return inoutFailure.FailStatus(status, "Read", path);
        default:
            return inoutFailure.Fail(AssetErrorCode::AssetCorrupt, {"Cannot read '", path, "'"});
    }
}

/// document(path): read at most maximumBytes and json.loads it with duplicate keys rejected.
[[nodiscard]] bool
LoadDocument(std::string_view path, usize maximumBytes, Document& outDocument, Failure& inoutFailure) noexcept
{
    Array<uint8> data;
    return ReadBytes(path, maximumBytes, data, inoutFailure) == true &&
           ParseDocument(GetSpan(data), AssetErrorCode::AssetCorrupt, outDocument, inoutFailure) == true;
}

/// atomic(path, data, immutable, cancelled).
[[nodiscard]] bool PublishBytes(std::string_view path,
                                std::span<const uint8> data,
                                PublishMode mode,
                                const Cancellation* cancelOrNull,
                                Failure& inoutFailure) noexcept
{
    switch (PublishFile(path, data, mode, cancelOrNull))
    {
        case PublishResult::Ok:
            return true;
        case PublishResult::Cancelled:
            return inoutFailure.Fail(AssetErrorCode::InstallCancelled, "Publication cancelled before commit");
        case PublishResult::Conflict:
            return inoutFailure.Fail(AssetErrorCode::Conflict, "Immutable artifact changed during publication");
        case PublishResult::ReadFailure:
            return inoutFailure.Fail(AssetErrorCode::AssetCorrupt, {"Cannot read '", path, "'"});
        default:
            return inoutFailure.FailStatus(Status::IoError, "Atomic write", path);
    }
}

// ---------------------------------------------------------------------------
// Project locks.

/// The innermost project lock this thread holds (the reference's ContextVar).
struct LockContext final
{
    char Identity[MAX_LOCK_IDENTITY_BYTES]{};
    usize Size = 0;
    bool IsActive = false;
    bool IsExclusive = false;
};

thread_local LockContext gLockContext;

/// _lock(root, exclusive) as a scope.
class LockScope final
{
public:
    LockScope() noexcept = default;
    ~LockScope() noexcept
    {
        if (mIsContextSet == true)
        {
            gLockContext = mPrevious;
        }
    }
    LockScope(const LockScope&) = delete;
    LockScope& operator=(const LockScope&) = delete;

    [[nodiscard]] bool Enter(std::string_view root, LockMode mode, Failure& inoutFailure) noexcept
    {
        String identity;
        if (ResolveNative(root, identity, inoutFailure) == false)
        {
            return false;
        }
        const bool isExclusive = mode == LockMode::Exclusive;
        if (gLockContext.IsActive == true &&
            std::string_view(gLockContext.Identity, gLockContext.Size) == identity.GetView())
        {
            if (isExclusive == true && gLockContext.IsExclusive == false)
            {
                return inoutFailure.Fail(AssetErrorCode::Busy, "Cannot upgrade a scene reader to a writer");
            }
            return true;
        }
        if (identity.GetSize() > MAX_LOCK_IDENTITY_BYTES)
        {
            return inoutFailure.FailStatus(Status::Limit, "Locking", root);
        }
        String path;
        if (ResolveInside(root, LOCK_PATH, path, inoutFailure) == false)
        {
            return false;
        }
        // Older immutable releases may have no coordination file. Readers do not
        // create files; every editable save/import and new release creates this lock.
        if (isExclusive == false && DoesPathExist(path.GetView()) == false)
        {
            return true;
        }
        if (isExclusive == true)
        {
            const Status status = MakeDirectories(GetParent(path.GetView()));
            if (status != Status::Ok)
            {
                return inoutFailure.FailStatus(status, "Creating", GetParent(path.GetView()));
            }
        }
        const Status status = mLock.Acquire(path.GetView(), mode);
        if (status == Status::Conflict)
        {
            return inoutFailure.Fail(AssetErrorCode::Busy,
                                     "Another scene publication is active; retry after it completes");
        }
        if (status != Status::Ok)
        {
            return inoutFailure.FailStatus(status, "Locking", path.GetView());
        }
        mPrevious = gLockContext;
        std::memcpy(gLockContext.Identity, identity.GetData(), identity.GetSize());
        gLockContext.Size = identity.GetSize();
        gLockContext.IsActive = true;
        gLockContext.IsExclusive = isExclusive;
        mIsContextSet = true;
        return true;
    }

private:
    FileLock mLock;
    LockContext mPrevious;
    bool mIsContextSet = false;
};

// ---------------------------------------------------------------------------
// Catalog and level validation.

/// catalog(root).
[[nodiscard]] bool ReadCatalogDocument(std::string_view root, Document& outCatalog, Failure& inoutFailure) noexcept
{
    LockScope lock;
    String path;
    if (lock.Enter(root, LockMode::Shared, inoutFailure) == false ||
        ResolveInside(root, CATALOG_PATH, path, inoutFailure) == false)
    {
        return false;
    }
    JsonTree& tree = outCatalog.Tree;
    if (DoesPathExist(path.GetView()) == true)
    {
        if (LoadDocument(path.GetView(), MAX_CATALOG_BYTES, outCatalog, inoutFailure) == false)
        {
            return false;
        }
    }
    else
    {
        outCatalog.Root = tree.AddObject();
        tree.SetMember(outCatalog.Root, "version", tree.AddInteger(1));
        tree.SetMember(outCatalog.Root, "resources", tree.AddObject());
        if (tree.IsOk() == false)
        {
            return inoutFailure.FailOutOfMemory();
        }
    }
    static constexpr std::string_view KEYS[] = {"version", "resources"};
    if (tree.HasExactKeys(outCatalog.Root, KEYS) == false ||
        IsNumberEqual(tree, tree.GetMember(outCatalog.Root, "version"), 1) == false)
    {
        return inoutFailure.FailInvalid("Unsupported scene catalog");
    }
    const JsonIndex resources = tree.GetMember(outCatalog.Root, "resources");
    if (tree.GetKind(resources) != JsonKind::Object || tree.GetCount(resources) > MAX_CATALOG_RESOURCES)
    {
        return inoutFailure.FailInvalid("Scene catalog resource limit");
    }
    return true;
}

[[nodiscard]] bool IsUuidString(const JsonTree& tree, JsonIndex node) noexcept
{
    Uuid value;
    return tree.GetKind(node) == JsonKind::String && ParseUuid(tree.GetText(node), value) == true;
}

[[nodiscard]] bool IsValidName(const JsonTree& tree, JsonIndex node) noexcept
{
    if (tree.GetKind(node) != JsonKind::String)
    {
        return false;
    }
    const usize length = CountCodePoints(tree.GetText(node));
    return length >= 1 && length <= MAX_NAME_CODE_POINTS;
}

[[nodiscard]] std::string_view GetObjectId(const JsonTree& tree, JsonIndex objects, usize index) noexcept
{
    return tree.GetText(tree.GetMember(tree.GetItem(objects, index), "id"));
}

/// validate_level(value).
[[nodiscard]] bool ValidateLevelNode(const JsonTree& tree, JsonIndex level, Failure& inoutFailure) noexcept
{
    static constexpr std::string_view LEVEL_KEYS[] =
        {"version", "id", "name", "profile", "objects", "camera", "imports"};
    if (tree.HasExactKeys(level, LEVEL_KEYS) == false)
    {
        return inoutFailure.FailInvalid("Invalid level fields");
    }
    if (IsNumberEqual(tree, tree.GetMember(level, "version"), 1) == false ||
        tree.IsString(tree.GetMember(level, "profile"), LEVEL_PROFILE) == false ||
        IsValidName(tree, tree.GetMember(level, "name")) == false)
    {
        return inoutFailure.FailInvalid("Unsupported level profile");
    }
    if (IsUuidString(tree, tree.GetMember(level, "id")) == false)
    {
        return inoutFailure.FailInvalid("Invalid persistent level ID");
    }
    const JsonIndex objects = tree.GetMember(level, "objects");
    if (tree.GetKind(objects) != JsonKind::Array || tree.GetCount(objects) > MAX_LEVEL_OBJECTS)
    {
        return inoutFailure.FailInvalid("Level object limit");
    }
    static constexpr std::string_view OBJECT_KEYS[] = {"id", "name", "parent", "mesh", "material", "transform"};
    const usize objectCount = tree.GetCount(objects);
    for (usize i = 0; i < objectCount; ++i)
    {
        const JsonIndex item = tree.GetItem(objects, i);
        if (tree.HasExactKeys(item, OBJECT_KEYS) == false)
        {
            return inoutFailure.FailInvalid("Invalid level object fields");
        }
        if (IsUuidString(tree, tree.GetMember(item, "id")) == false)
        {
            return inoutFailure.FailInvalid("Invalid persistent object ID");
        }
        bool isDuplicate = false;
        for (usize j = 0; j < i; ++j)
        {
            isDuplicate = isDuplicate || GetObjectId(tree, objects, j) == GetObjectId(tree, objects, i);
        }
        if (isDuplicate == true || IsValidName(tree, tree.GetMember(item, "name")) == false)
        {
            return inoutFailure.FailInvalid("Duplicate object ID or invalid name");
        }
        if (tree.GetKind(tree.GetMember(item, "parent")) != JsonKind::String ||
            AreAuthoredNumbers(tree, tree.GetMember(item, "transform"), format::OBJECT_TRANSFORM_COMPONENTS) == false)
        {
            return inoutFailure.FailInvalid("Invalid object hierarchy/transform");
        }
        if (IsResourceId(tree, tree.GetMember(item, "mesh"), "mesh") == false ||
            IsResourceId(tree, tree.GetMember(item, "material"), "material") == false)
        {
            return inoutFailure.FailInvalid("Invalid stable resource ID");
        }
    }
    // Every parent chain must end at "" without revisiting an object.
    for (usize i = 0; i < objectCount; ++i)
    {
        std::string_view identity = GetObjectId(tree, objects, i);
        usize steps = 0;
        while (identity.empty() == false)
        {
            JsonIndex found = NO_JSON_NODE;
            for (usize j = 0; j < objectCount; ++j)
            {
                if (GetObjectId(tree, objects, j) == identity)
                {
                    found = tree.GetItem(objects, j);
                }
            }
            if (found == NO_JSON_NODE || steps++ >= objectCount)
            {
                return inoutFailure.FailInvalid("Missing parent or hierarchy cycle");
            }
            identity = tree.GetText(tree.GetMember(found, "parent"));
        }
    }
    static constexpr std::string_view CAMERA_KEYS[] = {"eye", "target", "up", "fov"};
    const JsonIndex camera = tree.GetMember(level, "camera");
    if (tree.HasExactKeys(camera, CAMERA_KEYS) == false)
    {
        return inoutFailure.FailInvalid("Invalid camera fields");
    }
    const JsonIndex eye = tree.GetMember(camera, "eye");
    const JsonIndex target = tree.GetMember(camera, "target");
    const JsonIndex up = tree.GetMember(camera, "up");
    PythonNumber fov;
    if (AreAuthoredNumbers(tree, eye, 3) == false || AreAuthoredNumbers(tree, target, 3) == false ||
        AreAuthoredNumbers(tree, up, 3) == false ||
        IsAuthoredNumber(tree, tree.GetMember(camera, "fov"), fov) == false ||
        IsLessPython(MakeNumber(int64{0}), fov) == false || IsLessPython(fov, MakeNumber(std::numbers::pi)) == false)
    {
        return inoutFailure.FailInvalid("Invalid camera");
    }
    const JsonIndex imports = tree.GetMember(level, "imports");
    bool areImportsValid = tree.GetKind(imports) == JsonKind::Array && tree.GetCount(imports) <= MAX_LEVEL_IMPORTS;
    for (usize i = 0; areImportsValid == true && i < tree.GetCount(imports); ++i)
    {
        areImportsValid = IsResourceId(tree, tree.GetItem(imports, i), "scene");
    }
    if (areImportsValid == false)
    {
        return inoutFailure.FailInvalid("Invalid import provenance");
    }
    PythonNumber forward[3];
    PythonNumber upward[3];
    for (usize i = 0; i < 3; ++i)
    {
        forward[i] = SubtractPython(GetNumberItem(tree, target, i), GetNumberItem(tree, eye, i));
        upward[i] = GetNumberItem(tree, up, i);
    }
    const PythonNumber cross[3] = {
        SubtractPython(MultiplyPython(forward[1], upward[2]), MultiplyPython(forward[2], upward[1])),
        SubtractPython(MultiplyPython(forward[2], upward[0]), MultiplyPython(forward[0], upward[2])),
        SubtractPython(MultiplyPython(forward[0], upward[1]), MultiplyPython(forward[1], upward[0]))};
    PythonSum forwardLength;
    PythonSum crossLength;
    for (usize i = 0; i < 3; ++i)
    {
        forwardLength.Add(MultiplyPython(forward[i], forward[i]));
    }
    for (usize i = 0; i < 3; ++i)
    {
        crossLength.Add(MultiplyPython(cross[i], cross[i]));
    }
    if (IsLessOrEqualPython(forwardLength.GetResult(), MakeNumber(MIN_DIRECTION_SQUARED)) == true ||
        IsLessOrEqualPython(crossLength.GetResult(), MakeNumber(MIN_DIRECTION_SQUARED)) == true)
    {
        return inoutFailure.FailInvalid("Degenerate authored camera");
    }
    return true;
}

// ---------------------------------------------------------------------------
// Artifact keys and the resident closure.

/// _artifact_key(kind, content, dependencies, dependency_keys, build).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] bool ComputeArtifactKey(std::string_view kind,
                                      std::string_view contentHash,
                                      const JsonTree& source,
                                      JsonIndex dependencies,
                                      JsonIndex dependencyKeys,
                                      JsonIndex build,
                                      HexDigest& outKey,
                                      Failure& inoutFailure) noexcept
{
    JsonTree tree;
    const JsonIndex root = tree.AddObject();
    tree.SetMember(root, "schema", tree.AddInteger(1));
    tree.SetMember(root, "kind", tree.AddString(kind));
    tree.SetMember(root, "content", tree.AddString(contentHash));
    tree.SetMember(root, "dependencies", tree.CopyFrom(source, dependencies));
    tree.SetMember(root, "dependency_keys", tree.CopyFrom(source, dependencyKeys));
    tree.SetMember(root, "build", tree.CopyFrom(source, build));
    Array<uint8> bytes;
    if (WriteCanonical(tree, root, bytes, inoutFailure) == false)
    {
        return false;
    }
    ComputeHexDigest(GetSpan(bytes), outKey);
    return true;
}

[[nodiscard]] bool IsArtifactKind(const JsonTree& tree, JsonIndex node) noexcept
{
    for (const std::string_view kind : {"mesh", "material", "texture", "scene", "license", "lighting"})
    {
        if (tree.IsString(node, kind) == true)
        {
            return true;
        }
    }
    return false;
}

/// The visit() recursion of closure(root, level, current).
class ClosureBuilder final
{
public:
    ClosureBuilder(std::string_view root, const JsonTree& catalog, JsonIndex resources, Failure& inoutFailure) noexcept
        : mRoot(root), mCatalog(catalog), mResources(resources), mFailure(inoutFailure)
    {
    }

    [[nodiscard]] bool Visit(std::string_view identity, usize depth) noexcept
    {
        if (findSelected(identity) != NO_JSON_NODE)
        {
            return true;
        }
        bool isVisiting = false;
        for (const std::string_view item : mVisiting)
        {
            isVisiting = isVisiting || item == identity;
        }
        if (isVisiting == true || depth > MAX_DEPENDENCY_DEPTH)
        {
            return mFailure.FailInvalid("Cooked dependency cycle or depth limit");
        }
        const JsonIndex record = mCatalog.GetMember(mResources, identity);
        if (record == NO_JSON_NODE)
        {
            return mFailure.Fail(AssetErrorCode::AssetCorrupt, {"Missing cooked resource ", identity});
        }
        static constexpr std::string_view RECORD_KEYS[] =
            {"kind", "key", "sha256", "bytes", "path", "dependencies", "dependency_keys", "build"};
        if (mCatalog.HasExactKeys(record, RECORD_KEYS) == false)
        {
            return mFailure.FailInvalid("Invalid artifact record");
        }
        const JsonIndex kind = mCatalog.GetMember(record, "kind");
        const JsonIndex dependencies = mCatalog.GetMember(record, "dependencies");
        if (IsArtifactKind(mCatalog, kind) == false || IsResourceIdText(identity, mCatalog.GetText(kind)) == false ||
            mCatalog.GetKind(dependencies) != JsonKind::Array ||
            mCatalog.GetCount(dependencies) > MAX_ARTIFACT_DEPENDENCIES)
        {
            return mFailure.FailInvalid("Invalid artifact identity/dependencies");
        }
        const JsonIndex path = mCatalog.GetMember(record, "path");
        if (mCatalog.GetKind(path) != JsonKind::String)
        {
            return mFailure.FailInvalid("Invalid artifact path");
        }
        String native;
        Array<uint8> data;
        if (ResolveInside(mRoot, mCatalog.GetText(path), native, mFailure) == false ||
            ReadBytes(native.GetView(), MAX_ASSET_BYTES, data, mFailure) == false)
        {
            return false;
        }
        HexDigest digest;
        ComputeHexDigest(GetSpan(data), digest);
        if (IsNumberEqual(mCatalog, mCatalog.GetMember(record, "bytes"), static_cast<int64>(data.GetSize())) == false ||
            mCatalog.IsString(mCatalog.GetMember(record, "sha256"), digest.View()) == false)
        {
            return mFailure.Fail(AssetErrorCode::AssetCorrupt, {"Cooked artifact digest mismatch: ", identity});
        }
        const JsonIndex dependencyKeys = mCatalog.GetMember(record, "dependency_keys");
        HexDigest key;
        if (ComputeArtifactKey(mCatalog.GetText(kind),
                               digest.View(),
                               mCatalog,
                               dependencies,
                               dependencyKeys,
                               mCatalog.GetMember(record, "build"),
                               key,
                               mFailure) == false)
        {
            return false;
        }
        if (mCatalog.IsString(mCatalog.GetMember(record, "key"), key.View()) == false)
        {
            return mFailure.Fail(AssetErrorCode::AssetCorrupt, "Cooked artifact key mismatch");
        }
        if (mVisiting.TryAdd(identity) == false)
        {
            return mFailure.FailOutOfMemory();
        }
        if (areBindingsExact(dependencies, dependencyKeys) == false)
        {
            return mFailure.FailInvalid("Invalid exact dependency bindings");
        }
        for (usize i = 0; i < mCatalog.GetCount(dependencies); ++i)
        {
            const std::string_view child = mCatalog.GetText(mCatalog.GetItem(dependencies, i));
            if (Visit(child, depth + 1) == false)
            {
                return false;
            }
            const JsonIndex selected = findSelected(child);
            if (AreJsonEqual(mSelected,
                             mSelected.GetMember(selected, "key"),
                             mCatalog,
                             mCatalog.GetMember(dependencyKeys, child)) == false)
            {
                return mFailure.Fail(AssetErrorCode::Conflict, "Dependency artifact revision mismatch");
            }
        }
        mVisiting.RemoveLast();
        const JsonIndex copy = mSelected.CopyFrom(mCatalog, record);
        if (mSelected.IsOk() == false || mSelectedIds.TryAdd(identity) == false ||
            mSelectedRecords.TryAdd(copy) == false)
        {
            return mFailure.FailOutOfMemory();
        }
        if (mSelectedIds.GetSize() > MAX_CLOSURE_RESOURCES)
        {
            return mFailure.FailInvalid("Scene closure exceeds its resource limit");
        }
        return true;
    }

    /// dict(sorted(selected.items())) after the MAX_ASSET_BYTES resident check.
    [[nodiscard]] bool Finish(Document& outClosure) noexcept
    {
        PythonSum total;
        for (const JsonIndex record : mSelectedRecords)
        {
            PythonNumber bytes;
            if (GetPythonNumber(mSelected, mSelected.GetMember(record, "bytes"), bytes) == false)
            {
                return mFailure.FailInvalid("Invalid artifact byte count");
            }
            total.Add(bytes);
        }
        if (IsLessPython(MakeNumber(static_cast<int64>(MAX_ASSET_BYTES)), total.GetResult()) == true)
        {
            return mFailure.FailInvalid("Resident closure exceeds 384 MiB");
        }
        Array<usize> order;
        if (order.TryResize(mSelectedIds.GetSize()) == false)
        {
            return mFailure.FailOutOfMemory();
        }
        for (usize i = 0; i < order.GetSize(); ++i)
        {
            usize at = i;
            while (at > 0 && mSelectedIds[i] < mSelectedIds[order[at - 1]])
            {
                order[at] = order[at - 1];
                --at;
            }
            order[at] = i;
        }
        JsonTree& tree = outClosure.Tree;
        outClosure.Root = tree.AddObject();
        for (const usize index : order)
        {
            const JsonIndex record = tree.CopyFrom(mSelected, mSelectedRecords[index]);
            tree.SetMember(outClosure.Root, mSelectedIds[index], record);
        }
        return tree.IsOk() == true ? true : mFailure.FailOutOfMemory();
    }

private:
    [[nodiscard]] JsonIndex findSelected(std::string_view identity) const noexcept
    {
        for (usize i = 0; i < mSelectedIds.GetSize(); ++i)
        {
            if (mSelectedIds[i] == identity)
            {
                return mSelectedRecords[i];
            }
        }
        return NO_JSON_NODE;
    }

    /// isinstance(dependency_keys, dict) and set(dependency_keys) == set(dependencies).
    [[nodiscard]] bool areBindingsExact(JsonIndex dependencies, JsonIndex dependencyKeys) const noexcept
    {
        if (mCatalog.GetKind(dependencyKeys) != JsonKind::Object)
        {
            return false;
        }
        const usize count = mCatalog.GetCount(dependencies);
        for (usize i = 0; i < count; ++i)
        {
            const JsonIndex child = mCatalog.GetItem(dependencies, i);
            if (mCatalog.GetKind(child) != JsonKind::String ||
                mCatalog.GetMember(dependencyKeys, mCatalog.GetText(child)) == NO_JSON_NODE)
            {
                return false;
            }
        }
        for (usize i = 0; i < mCatalog.GetCount(dependencyKeys); ++i)
        {
            bool isListed = false;
            for (usize j = 0; j < count && isListed == false; ++j)
            {
                isListed = mCatalog.GetText(mCatalog.GetItem(dependencies, j)) == mCatalog.GetKey(dependencyKeys, i);
            }
            if (isListed == false)
            {
                return false;
            }
        }
        return true;
    }

    std::string_view mRoot;
    const JsonTree& mCatalog;
    JsonIndex mResources;
    Failure& mFailure;
    Array<std::string_view> mVisiting;
    Array<std::string_view> mSelectedIds;
    Array<JsonIndex> mSelectedRecords;
    JsonTree mSelected;
};

/// closure(root, level, current); currentOrNull selects a candidate catalog.
[[nodiscard]] bool ComputeClosure(std::string_view root,
                                  const JsonTree& levelTree,
                                  JsonIndex level,
                                  const Document* currentOrNull,
                                  Document& outClosure,
                                  Failure& inoutFailure) noexcept
{
    LockScope lock;
    if (lock.Enter(root, LockMode::Shared, inoutFailure) == false ||
        ValidateLevelNode(levelTree, level, inoutFailure) == false)
    {
        return false;
    }
    Document published;
    const Document* catalog = currentOrNull;
    if (catalog == nullptr)
    {
        if (ReadCatalogDocument(root, published, inoutFailure) == false)
        {
            return false;
        }
        catalog = &published;
    }
    const JsonIndex resources = catalog->Tree.GetMember(catalog->Root, "resources");
    if (resources == NO_JSON_NODE || catalog->Tree.GetKind(resources) != JsonKind::Object)
    {
        return inoutFailure.FailInvalid("Unsupported scene catalog");
    }
    ClosureBuilder builder(root, catalog->Tree, resources, inoutFailure);
    const JsonIndex objects = levelTree.GetMember(level, "objects");
    for (usize i = 0; i < levelTree.GetCount(objects); ++i)
    {
        const JsonIndex item = levelTree.GetItem(objects, i);
        if (builder.Visit(levelTree.GetText(levelTree.GetMember(item, "mesh")), 0) == false ||
            builder.Visit(levelTree.GetText(levelTree.GetMember(item, "material")), 0) == false)
        {
            return false;
        }
    }
    const JsonIndex imports = levelTree.GetMember(level, "imports");
    for (usize i = 0; i < levelTree.GetCount(imports); ++i)
    {
        if (builder.Visit(levelTree.GetText(levelTree.GetItem(imports, i)), 0) == false)
        {
            return false;
        }
    }
    return builder.Finish(outClosure);
}

// ---------------------------------------------------------------------------
// Bundle validation (split_bundle).

[[nodiscard]] uint32 DecodeUint32(const uint8* bytes) noexcept
{
    return static_cast<uint32>(bytes[0]) | (static_cast<uint32>(bytes[1]) << 8) |
           (static_cast<uint32>(bytes[2]) << 16) | (static_cast<uint32>(bytes[3]) << 24);
}

/// struct.unpack("<f") widened to float64.
[[nodiscard]] float64 DecodeFloat(const uint8* bytes) noexcept
{
    return static_cast<float64>(std::bit_cast<float32>(DecodeUint32(bytes)));
}

/// The reference's Reader: bounded takes and finite-checked fields.
class BundleReader final
{
public:
    explicit BundleReader(std::span<const uint8> data) noexcept : mData(data) {}

    [[nodiscard]] bool Take(usize count, std::span<const uint8>& outBytes, Failure& inoutFailure) noexcept
    {
        if (count > mData.size() - mOffset)
        {
            return inoutFailure.FailInvalid("Truncated cooked scene");
        }
        outBytes = mData.subspan(mOffset, count);
        mOffset += count;
        return true;
    }

    /// fields(): count little-endian uint32 values.
    [[nodiscard]] bool ReadIntegers(std::span<uint32> outValues, Failure& inoutFailure) noexcept
    {
        std::span<const uint8> bytes;
        if (Take(outValues.size() * 4, bytes, inoutFailure) == false)
        {
            return false;
        }
        for (usize i = 0; i < outValues.size(); ++i)
        {
            outValues[i] = DecodeUint32(bytes.data() + i * 4);
        }
        return true;
    }

    /// fields(): count float32 values; any nonfinite value fails.
    [[nodiscard]] bool ReadFloats(std::span<float64> outValues, Failure& inoutFailure) noexcept
    {
        std::span<const uint8> bytes;
        if (Take(outValues.size() * 4, bytes, inoutFailure) == false)
        {
            return false;
        }
        bool areFinite = true;
        for (usize i = 0; i < outValues.size(); ++i)
        {
            outValues[i] = DecodeFloat(bytes.data() + i * 4);
            areFinite = areFinite && math::IsFinite(outValues[i]);
        }
        return areFinite == true ? true : inoutFailure.FailInvalid("Nonfinite cooked scene value");
    }

    [[nodiscard]] usize GetOffset() const noexcept
    {
        return mOffset;
    }

private:
    std::span<const uint8> mData;
    usize mOffset = 0;
};

constexpr usize SOURCE_ID_WORDS = 2;
constexpr usize RGB_COMPONENTS = 3;

[[nodiscard]] bool ReadSourceId(const JsonTree& tree, JsonIndex node, uint64& outId) noexcept
{
    if (node == NO_JSON_NODE || tree.GetKind(node) != JsonKind::Array || tree.GetCount(node) != SOURCE_ID_WORDS)
    {
        return false;
    }
    foundation::StaticArray<uint32, SOURCE_ID_WORDS> words{};
    for (usize i = 0; i < words.GetSize(); ++i)
    {
        const JsonNode& value = tree.GetNode(tree.GetItem(node, i));
        if (value.Kind != JsonKind::Integer || value.Integer < 0 || static_cast<uint64>(value.Integer) > ~uint32{0})
        {
            return false;
        }
        words[i] = static_cast<uint32>(value.Integer);
    }
    outId = uint64{words[0]} | (uint64{words[1]} << 32);
    return true;
}

[[nodiscard]] JsonIndex AddSourceId(JsonTree& inoutTree, uint64 id) noexcept
{
    const JsonIndex result = inoutTree.AddArray();
    inoutTree.Append(result, inoutTree.AddInteger(static_cast<uint32>(id)));
    inoutTree.Append(result, inoutTree.AddInteger(static_cast<uint32>(id >> 32)));
    return result;
}

void AppendSourceId(ByteWriter& inoutWriter, uint64 id) noexcept
{
    inoutWriter.AppendUint32(static_cast<uint32>(id));
    inoutWriter.AppendUint32(static_cast<uint32>(id >> 32));
}

[[nodiscard]] bool MakeScopedSourceId(std::string_view scope, uint64 source, uint64& outId) noexcept
{
    Array<uint8> bytes;
    ByteWriter writer(bytes);
    writer.Append(scope);
    AppendSourceId(writer, source);
    if (writer.IsOk() == false)
    {
        return false;
    }
    const Digest digest = Hash(GetSpan(bytes));
    outId = uint64{DecodeUint32(digest.Data)} | (uint64{DecodeUint32(digest.Data + sizeof(uint32))} << 32);
    // Zero is reserved. Assembly separately rejects any collision in the complete lists.
    if (outId == 0)
    {
        outId = 1;
    }
    return true;
}

[[nodiscard]] bool ValidateLightingPayload(std::span<const uint8> bytes, Failure& inoutFailure) noexcept
{
    BundleReader reader(bytes);
    constexpr usize HEADER_INTEGERS = 4;
    constexpr usize LIGHT_INTEGERS = 4;
    constexpr usize LIGHT_FLOATS = 12;
    foundation::StaticArray<uint32, HEADER_INTEGERS> header{};
    foundation::StaticArray<float64, RGB_COMPONENTS> environment{};
    if (reader.ReadIntegers({header.GetData(), header.GetSize()}, inoutFailure) == false ||
        reader.ReadFloats({environment.GetData(), environment.GetSize()}, inoutFailure) == false ||
        header[0] != format::LIGHTING_PROFILE_REC709_PHOTOMETRIC || header[1] > format::MAX_LIGHTS ||
        header[2] > format::MAX_ENVIRONMENT_DIMENSION || header[3] > format::MAX_ENVIRONMENT_DIMENSION ||
        ((header[2] == 0) != (header[3] == 0)))
    {
        return inoutFailure.FailInvalid("Invalid authored lighting profile or dimensions");
    }
    const usize pixels = usize{header[2]} * header[3];
    if (reader.GetOffset() + usize{header[1]} * sizeof(format::LightRecord) +
            pixels * format::ENVIRONMENT_TEXEL_BYTES !=
        bytes.size())
    {
        return inoutFailure.FailInvalid("Invalid authored lighting payload length");
    }
    for (const float64 value : environment)
    {
        if (value < 0 || value > static_cast<float64>(format::MAX_EMISSION))
        {
            return inoutFailure.FailInvalid("Invalid authored environment energy");
        }
    }
    foundation::StaticArray<uint64, format::MAX_LIGHTS> ids{};
    for (usize i = 0; i < header[1]; ++i)
    {
        foundation::StaticArray<uint32, LIGHT_INTEGERS> integers{};
        foundation::StaticArray<float64, LIGHT_FLOATS> values{};
        if (reader.ReadIntegers({integers.GetData(), integers.GetSize()}, inoutFailure) == false ||
            reader.ReadFloats({values.GetData(), values.GetSize()}, inoutFailure) == false)
        {
            return false;
        }
        const uint64 id = uint64{integers[0]} | (uint64{integers[1]} << 32);
        if (id == 0 || integers[2] > format::LIGHT_SPOT || (integers[3] & ~format::LIGHT_ENABLED) != 0 ||
            values[0] < 0 || values[1] < 0 || values[2] < 0 ||
            values[0] > static_cast<float64>(format::MAX_LIGHT_INTENSITY) ||
            values[1] > static_cast<float64>(format::MAX_LIGHT_INTENSITY) ||
            values[2] > static_cast<float64>(format::MAX_LIGHT_INTENSITY) ||
            (values[6] == 0 && values[7] == 0 && values[8] == 0) || values[9] < .01 || values[9] > 1000000 ||
            values[10] <= -1 || values[10] > 1 || values[11] <= -1 || values[11] >= values[10])
        {
            return inoutFailure.FailInvalid("Invalid authored analytic light");
        }
        for (usize j = 0; j < i; ++j)
        {
            if (ids[j] == id)
            {
                return inoutFailure.FailInvalid("Duplicate authored light identity");
            }
        }
        ids[i] = id;
    }
    for (usize i = 0; i < pixels; ++i)
    {
        foundation::StaticArray<float64, RGB_COMPONENTS> pixel{};
        if (reader.ReadFloats({pixel.GetData(), pixel.GetSize()}, inoutFailure) == false)
        {
            return false;
        }
        for (const float64 value : pixel)
        {
            if (value < 0 || value > static_cast<float64>(format::MAX_EMISSION))
            {
                return inoutFailure.FailInvalid("Invalid HDR environment texel");
            }
        }
    }
    return true;
}

struct BundleMaterial final
{
    std::span<const uint8> Bytes;
    uint32 Texture = 0;
    /// Version 6 surface and emission map indices, or NO_TEXTURE.
    uint32 SurfaceTexture = format::NO_TEXTURE;
    uint32 EmissionTexture = format::NO_TEXTURE;
};

struct BundleObject final
{
    uint32 Mesh = 0;
    uint32 Material = 0;
    float64 Transform[format::OBJECT_TRANSFORM_COMPONENTS]{};
    uint64 SourceId = 0;
};

struct BundleParts final
{
    uint32 Version = format::VERSION_1;
    foundation::BorrowedView<const uint8> Lighting;
    Array<std::span<const uint8>> Textures;
    Array<BundleMaterial> Materials;
    Array<std::span<const uint8>> Meshes;
    Array<BundleObject> Objects;
    float64 Camera[CAMERA_FLOATS]{};
};

[[nodiscard]] bool ReadTextures(BundleReader& inoutReader,
                                std::span<const uint8> data,
                                uint32 textureCount,
                                BundleParts& outParts,
                                Failure& inoutFailure) noexcept
{
    usize textureBytes = 0;
    for (uint32 i = 0; i < textureCount; ++i)
    {
        const usize start = inoutReader.GetOffset();
        std::span<const uint8> header;
        if (inoutReader.Take(12, header, inoutFailure) == false)
        {
            return false;
        }
        const uint32 width = DecodeUint32(header.data());
        const uint32 height = DecodeUint32(header.data() + 4);
        const uint32 count = DecodeUint32(header.data() + 8);
        textureBytes += count;
        if (width == 0 || width > format::MAX_TEXTURE_DIMENSION || height == 0 ||
            height > format::MAX_TEXTURE_DIMENSION ||
            uint64{count} != uint64{width} * uint64{height} * format::TEXEL_BYTES ||
            count > format::MAX_TEXTURE_BYTES ||
            textureBytes > format::GetBundleLimits(outParts.Version).TotalTextureBytes)
        {
            return inoutFailure.FailInvalid("Cooked texture budget");
        }
        std::span<const uint8> texels;
        if (inoutReader.Take(count, texels, inoutFailure) == false)
        {
            return false;
        }
        if (outParts.Textures.TryAdd(data.subspan(start, inoutReader.GetOffset() - start)) == false)
        {
            return inoutFailure.FailOutOfMemory();
        }
    }
    return true;
}

[[nodiscard]] bool ReadMaterials(BundleReader& inoutReader,
                                 std::span<const uint8> data,
                                 // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                 uint32 version,
                                 uint32 materialCount,
                                 uint32 textureCount,
                                 BundleParts& outParts,
                                 Failure& inoutFailure) noexcept
{
    for (uint32 i = 0; i < materialCount; ++i)
    {
        const usize start = inoutReader.GetOffset();
        uint32 texture[1]{};
        if (inoutReader.ReadIntegers(texture, inoutFailure) == false)
        {
            return false;
        }
        uint32 address[1] = {(0 < texture[0] && texture[0] < textureCount) ? format::ADDRESS_REPEAT
                                                                           : format::ADDRESS_CLAMP};
        float64 values[5]{};
        if ((version != format::VERSION_1 && inoutReader.ReadIntegers(address, inoutFailure) == false) ||
            inoutReader.ReadFloats(values, inoutFailure) == false)
        {
            return false;
        }
        if (address[0] > format::ADDRESS_REPEAT)
        {
            return inoutFailure.FailInvalid("Invalid cooked sampler address");
        }
        const float64 metallic = values[0];
        const float64 roughness = values[1];
        const float64 emissionMinimum = math::Min(values[2], math::Min(values[3], values[4]));
        const float64 emissionMaximum = math::Max(values[2], math::Max(values[3], values[4]));
        if ((texture[0] != format::NO_TEXTURE && texture[0] >= textureCount) ||
            (0 <= metallic && metallic <= 1) == false ||
            (MIN_ROUGHNESS_REFERENCE <= roughness && roughness <= 1) == false || emissionMinimum < 0 ||
            emissionMaximum > MAX_EMISSION_REFERENCE || math::IsFinite(values[2]) == false ||
            math::IsFinite(values[3]) == false || math::IsFinite(values[4]) == false)
        {
            return inoutFailure.FailInvalid("Invalid cooked material");
        }
        uint32 surfaceTexture = format::NO_TEXTURE;
        uint32 emissionTexture = format::NO_TEXTURE;
        if (format::HasLightingRecords(version) == true)
        {
            std::span<const uint8> extra;
            if (inoutReader.Take(MATERIAL_RECORD_V3_BYTES - MATERIAL_RECORD_V2_BYTES, extra, inoutFailure) == false)
            {
                return false;
            }
            const uint32 flags = DecodeUint32(extra.data());
            const uint64 sourceId =
                uint64{DecodeUint32(extra.data() + 4)} | (uint64{DecodeUint32(extra.data() + 8)} << 32);
            const float64 power = DecodeFloat(extra.data() + 12);
            // Version 5 stores the alpha mode in the version 3 reserved word, then the
            // cutoff and a new reserved word; version 6 replaces that word by its maps.
            const bool hasSurfaceMaps = format::HasSurfaceMaps(version);
            std::span<const uint8> alpha;
            if (format::HasMaterialAlpha(version) == true &&
                inoutReader.Take((hasSurfaceMaps == true ? MATERIAL_RECORD_V6_BYTES : MATERIAL_RECORD_V5_BYTES) -
                                     MATERIAL_RECORD_V3_BYTES,
                                 alpha,
                                 inoutFailure) == false)
            {
                return false;
            }
            const uint8* reservedOrNull = alpha.empty() == true     ? extra.data() + MATERIAL_V3_RESERVED_OFFSET
                                          : hasSurfaceMaps == false ? alpha.data() + sizeof(float32)
                                                                    : nullptr;
            if ((flags & ~format::EMISSION_FLAGS) != 0 ||
                (reservedOrNull != nullptr && DecodeUint32(reservedOrNull) != 0) || math::IsFinite(power) == false ||
                power < 0 || power > static_cast<float64>(format::MAX_LIGHT_INTENSITY) ||
                ((flags & format::EMISSION_HAS_POWER) == 0 && power != 0) || (emissionMaximum > 0 && sourceId == 0))
            {
                return inoutFailure.FailInvalid("Invalid authored emitter flags, identity or power");
            }
            if (alpha.empty() == false)
            {
                const uint32 mode = DecodeUint32(extra.data() + MATERIAL_V3_RESERVED_OFFSET);
                const float64 cutoff = DecodeFloat(alpha.data());
                if (mode > format::ALPHA_MASKED || math::IsFinite(cutoff) == false || cutoff < 0 || cutoff > 1 ||
                    (mode == format::ALPHA_OPAQUE && cutoff != 0))
                {
                    return inoutFailure.FailInvalid("Invalid cooked material alpha");
                }
            }
            if (hasSurfaceMaps == true)
            {
                surfaceTexture = DecodeUint32(alpha.data() + MATERIAL_V6_SURFACE_OFFSET);
                emissionTexture = DecodeUint32(alpha.data() + MATERIAL_V6_EMISSION_OFFSET);
                const float64 normalScale = DecodeFloat(alpha.data() + MATERIAL_V6_NORMAL_SCALE_OFFSET);
                if ((surfaceTexture != format::NO_TEXTURE && surfaceTexture >= textureCount) ||
                    (emissionTexture != format::NO_TEXTURE && emissionTexture >= textureCount) ||
                    math::IsFinite(normalScale) == false || normalScale < 0 ||
                    normalScale > static_cast<float64>(format::MAX_NORMAL_SCALE) ||
                    (surfaceTexture == format::NO_TEXTURE && normalScale != 0))
                {
                    return inoutFailure.FailInvalid("Invalid cooked material maps");
                }
            }
        }
        BundleMaterial material;
        material.Bytes = data.subspan(start, inoutReader.GetOffset() - start);
        material.Texture = texture[0];
        material.SurfaceTexture = surfaceTexture;
        material.EmissionTexture = emissionTexture;
        if (outParts.Materials.TryAdd(material) == false)
        {
            return inoutFailure.FailOutOfMemory();
        }
    }
    return true;
}

/// A version 6 vertex tangent: handedness +1/-1 with a finite direction whose part
/// perpendicular to the vertex normal exceeds MIN_TANGENT_PERPENDICULAR, or all zero.
[[nodiscard]] bool IsValidTangent(const uint8* vertex) noexcept
{
    const auto decodeVector = [vertex](usize offset) {
        return math::Vector3d{DecodeFloat(vertex + offset),
                              DecodeFloat(vertex + offset + sizeof(float32)),
                              DecodeFloat(vertex + offset + 2 * sizeof(float32))};
    };
    const math::Vector3d normal = decodeVector(offsetof(format::VertexRecordV6, Normal));
    const math::Vector3d direction = decodeVector(offsetof(format::VertexRecordV6, Tangent));
    const float64 handedness = DecodeFloat(vertex + offsetof(format::VertexRecordV6, Tangent) +
                                           (format::TANGENT_COMPONENTS - 1) * sizeof(float32));
    if (math::IsFinite(direction) == false || math::IsFinite(handedness) == false)
    {
        return false;
    }
    if (handedness == 0)
    {
        return direction.X == 0 && direction.Y == 0 && direction.Z == 0;
    }
    const float64 normalLength = math::Length(normal);
    if ((handedness == 1 || handedness == -1) == false || normalLength <= 0)
    {
        return false;
    }
    const math::Vector3d unitNormal = normal * (1.0 / normalLength);
    return math::Length(direction - unitNormal * math::Dot(unitNormal, direction)) >
           static_cast<float64>(format::MIN_TANGENT_PERPENDICULAR);
}

[[nodiscard]] bool ReadMeshes(BundleReader& inoutReader,
                              std::span<const uint8> data,
                              uint32 meshCount,
                              BundleParts& outParts,
                              Failure& inoutFailure) noexcept
{
    const uint32 version = outParts.Version;
    const format::BundleLimits& limits = format::GetBundleLimits(version);
    const usize vertexBytes = format::GetVertexRecordBytes(version);
    usize vertices = 0;
    usize indices = 0;
    for (uint32 i = 0; i < meshCount; ++i)
    {
        const usize start = inoutReader.GetOffset();
        uint32 counts[2]{};
        if (inoutReader.ReadIntegers(counts, inoutFailure) == false)
        {
            return false;
        }
        const uint32 vertexCount = counts[0];
        const uint32 indexCount = counts[1];
        vertices += vertexCount;
        indices += indexCount;
        if (vertexCount == 0 || vertexCount > limits.Vertices || vertices > limits.Vertices || indexCount == 0 ||
            indexCount > limits.TotalIndices || indexCount % format::TRIANGLE_CORNERS != 0 ||
            (format::HasImplicitBakeCoordinates(version) == true && indices > limits.TotalIndices) ||
            (format::HasImplicitBakeCoordinates(version) == false && indexCount > format::MAX_INDICES))
        {
            return inoutFailure.FailInvalid("Cooked geometry budget");
        }
        std::span<const uint8> vertexData;
        if (inoutReader.Take(usize{vertexCount} * vertexBytes, vertexData, inoutFailure) == false)
        {
            return false;
        }
        for (uint32 vertex = 0; vertex < vertexCount; ++vertex)
        {
            const uint8* base = vertexData.data() + usize{vertex} * vertexBytes;
            bool isValid = true;
            PythonSum length;
            for (usize component = 0; component < format::VERTEX_COMPONENTS; ++component)
            {
                const float64 value = DecodeFloat(base + component * 4);
                isValid = isValid && math::IsFinite(value);
                if (component >= 3 && component < 6)
                {
                    length.Add(MakeNumber(value * value));
                }
            }
            if (format::HasStoredBakeCoordinates(version) == true)
            {
                const float64 u = DecodeFloat(base + sizeof(format::VertexRecord));
                const float64 v = DecodeFloat(base + sizeof(format::VertexRecord) + sizeof(float32));
                isValid = isValid == true && math::IsFinite(u) == true && math::IsFinite(v) == true && u >= 0 &&
                          u <= 1 && v >= 0 && v <= 1;
            }
            if (format::HasSurfaceMaps(version) == true)
            {
                isValid = isValid == true && IsValidTangent(base) == true;
            }
            if (isValid == false || length.GetResult().Float <= MIN_DIRECTION_SQUARED)
            {
                return inoutFailure.FailInvalid("Invalid cooked vertex");
            }
        }
        std::span<const uint8> indexData;
        if (inoutReader.Take(usize{indexCount} * 4, indexData, inoutFailure) == false)
        {
            return false;
        }
        for (uint32 index = 0; index < indexCount; ++index)
        {
            if (DecodeUint32(indexData.data() + usize{index} * 4) >= vertexCount)
            {
                return inoutFailure.FailInvalid("Cooked index outside mesh");
            }
        }
        if (outParts.Meshes.TryAdd(data.subspan(start, inoutReader.GetOffset() - start)) == false)
        {
            return inoutFailure.FailOutOfMemory();
        }
    }
    return true;
}

/// split_bundle(data): validate the complete binary profile before any publication.
[[nodiscard]] bool SplitBundle(std::span<const uint8> data, BundleParts& outParts, Failure& inoutFailure) noexcept
{
    if (data.size() > format::IMPORT_LIMITS.BundleBytes)
    {
        return inoutFailure.FailInvalid("Cooked scene byte limit");
    }
    BundleReader reader(data);
    std::span<const uint8> magic;
    if (reader.Take(format::MAGIC.size(), magic, inoutFailure) == false)
    {
        return false;
    }
    if (std::memcmp(magic.data(), format::MAGIC.data(), format::MAGIC.size()) != 0)
    {
        return inoutFailure.FailInvalid("Unsupported cooked scene magic");
    }
    uint32 header[5]{};
    if (reader.ReadIntegers(header, inoutFailure) == false || reader.ReadFloats(outParts.Camera, inoutFailure) == false)
    {
        return false;
    }
    const uint32 version = header[0];
    outParts.Version = version;
    const uint32 meshCount = header[1];
    const uint32 materialCount = header[2];
    const uint32 textureCount = header[3];
    const uint32 objectCount = header[4];
    if (format::IsSupportedVersion(version) == false)
    {
        return inoutFailure.FailInvalid("Unsupported cooked scene version");
    }
    const format::BundleLimits& limits = format::GetBundleLimits(version);
    if (data.size() > limits.BundleBytes || meshCount < 1 || meshCount > limits.Meshes || materialCount < 1 ||
        materialCount > limits.Materials || textureCount > limits.Textures || objectCount < 1 ||
        objectCount > limits.Objects)
    {
        return inoutFailure.FailInvalid(version == format::VERSION_5
                                            ? "Cooked scene exceeds the import profile"
                                            : "Cooked scene exceeds resident reference profile");
    }
    const float64 fov = outParts.Camera[CAMERA_FLOATS - 1];
    if ((0 < fov && fov < std::numbers::pi) == false)
    {
        return inoutFailure.FailInvalid("Invalid camera field of view");
    }
    if (ReadTextures(reader, data, textureCount, outParts, inoutFailure) == false ||
        ReadMaterials(reader, data, version, materialCount, textureCount, outParts, inoutFailure) == false ||
        ReadMeshes(reader, data, meshCount, outParts, inoutFailure) == false)
    {
        return false;
    }
    // A version 6 surface map is linear data; no material may sample it as colour.
    for (const BundleMaterial& surface : outParts.Materials)
    {
        for (const BundleMaterial& colour : outParts.Materials)
        {
            if (surface.SurfaceTexture != format::NO_TEXTURE &&
                (colour.Texture == surface.SurfaceTexture || colour.EmissionTexture == surface.SurfaceTexture))
            {
                return inoutFailure.FailInvalid("Cooked surface map also sampled as colour");
            }
        }
    }
    for (uint32 i = 0; i < objectCount; ++i)
    {
        BundleObject object;
        uint32 bindings[2]{};
        if (reader.ReadIntegers(bindings, inoutFailure) == false ||
            reader.ReadFloats(object.Transform, inoutFailure) == false)
        {
            return false;
        }
        if (format::HasLightingRecords(version) == true)
        {
            std::span<const uint8> source;
            if (reader.Take(sizeof(uint64), source, inoutFailure) == false ||
                (DecodeUint32(source.data()) == 0 && DecodeUint32(source.data() + 4) == 0))
            {
                return inoutFailure.FailInvalid("Invalid placed source identity");
            }
        }
        if (format::HasLightingRecords(version) == true)
        {
            const uint8* identity = data.data() + reader.GetOffset() - sizeof(uint64);
            object.SourceId = uint64{DecodeUint32(identity)} | (uint64{DecodeUint32(identity + sizeof(uint32))} << 32);
            for (const BundleObject& previous : outParts.Objects)
            {
                if (previous.SourceId == object.SourceId)
                {
                    return inoutFailure.FailInvalid("Duplicate placed source identity");
                }
            }
        }
        object.Mesh = bindings[0];
        object.Material = bindings[1];
        if (object.Mesh >= meshCount || object.Material >= materialCount)
        {
            return inoutFailure.FailInvalid("Cooked object binding");
        }
        if (outParts.Objects.TryAdd(object) == false)
        {
            return inoutFailure.FailOutOfMemory();
        }
    }
    if (format::HasLightingRecords(version) == true)
    {
        const usize start = reader.GetOffset();
        const std::span<const uint8> lighting = data.subspan(start);
        if (ValidateLightingPayload(lighting, inoutFailure) == false)
        {
            return false;
        }
        outParts.Lighting = {lighting.data(), lighting.size()};
        std::span<const uint8> consumed;
        if (reader.Take(lighting.size(), consumed, inoutFailure) == false)
        {
            return false;
        }
    }
    if (reader.GetOffset() != data.size())
    {
        return inoutFailure.FailInvalid("Trailing cooked scene bytes");
    }
    return true;
}

// ---------------------------------------------------------------------------
// Assembly and dumps.

/// Insert into a sorted list of unique strings (sorted({...})).
[[nodiscard]] bool AddSortedUnique(Array<std::string_view>& inoutValues, std::string_view value) noexcept
{
    usize at = 0;
    while (at < inoutValues.GetSize() && inoutValues[at] < value)
    {
        ++at;
    }
    if (at < inoutValues.GetSize() && inoutValues[at] == value)
    {
        return true;
    }
    if (inoutValues.TryAdd(value) == false)
    {
        return false;
    }
    for (usize i = inoutValues.GetSize() - 1; i > at; --i)
    {
        inoutValues[i] = inoutValues[i - 1];
    }
    inoutValues[at] = value;
    return true;
}

/// list.index(value) for a value known to be present.
[[nodiscard]] uint32 FindIndex(const Array<std::string_view>& values, std::string_view value) noexcept
{
    for (usize i = 0; i < values.GetSize(); ++i)
    {
        if (values[i] == value)
        {
            return static_cast<uint32>(i);
        }
    }
    return static_cast<uint32>(values.GetSize());
}

/// A twelve-number object transform (column-major basis, then position).
struct Transform final
{
    PythonNumber Values[format::OBJECT_TRANSFORM_COMPONENTS];
};

/// world(item): compose parent transforms with the reference's sum() order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void ComposeWorld(const JsonTree& tree, JsonIndex objects, JsonIndex item, Transform& outWorld, usize depth) noexcept
{
    const JsonIndex local = tree.GetMember(item, "transform");
    const std::string_view parent = tree.GetText(tree.GetMember(item, "parent"));
    if (parent.empty() == true || depth > MAX_LEVEL_OBJECTS)
    {
        for (usize i = 0; i < format::OBJECT_TRANSFORM_COMPONENTS; ++i)
        {
            outWorld.Values[i] = GetNumberItem(tree, local, i);
        }
        return;
    }
    // objects = {o["id"]: o}: the last object with an ID wins.
    JsonIndex parentItem = item;
    for (usize i = 0; i < tree.GetCount(objects); ++i)
    {
        if (GetObjectId(tree, objects, i) == parent)
        {
            parentItem = tree.GetItem(objects, i);
        }
    }
    Transform parentWorld;
    ComposeWorld(tree, objects, parentItem, parentWorld, depth + 1);
    for (usize column = 0; column < 3; ++column)
    {
        for (usize row = 0; row < 3; ++row)
        {
            PythonSum sum;
            for (usize k = 0; k < 3; ++k)
            {
                sum.Add(MultiplyPython(parentWorld.Values[k * 3 + row], GetNumberItem(tree, local, column * 3 + k)));
            }
            outWorld.Values[column * 3 + row] = sum.GetResult();
        }
    }
    for (usize row = 0; row < 3; ++row)
    {
        PythonSum sum;
        for (usize k = 0; k < 3; ++k)
        {
            sum.Add(MultiplyPython(parentWorld.Values[k * 3 + row], GetNumberItem(tree, local, 9 + k)));
        }
        outWorld.Values[9 + row] = AddPython(parentWorld.Values[9 + row], sum.GetResult());
    }
}

/// Append a texture a material description depends on: the base texture, then the
/// version 6 surface and emission maps, each once. Empty identities are skipped.
[[nodiscard]] bool AddMaterialDependency(Array<std::string_view>& inoutDependencies, std::string_view texture) noexcept
{
    if (texture.empty() == true)
    {
        return true;
    }
    for (const std::string_view dependency : inoutDependencies)
    {
        if (dependency == texture)
        {
            return true;
        }
    }
    return inoutDependencies.TryAdd(texture);
}

/// assemble()'s per-material description checks.
[[nodiscard]] bool IsValidMaterialDescription(const Document& description,
                                              const JsonTree& closureTree,
                                              JsonIndex record,
                                              Failure& inoutFailure) noexcept
{
    const JsonTree& tree = description.Tree;
    const JsonIndex root = description.Root;
    static constexpr std::string_view KEYS[] = {"texture", "metallic", "roughness", "emission", "address"};
    static constexpr std::string_view LIGHTING_KEYS[] = {"texture",
                                                         "metallic",
                                                         "roughness",
                                                         "emission",
                                                         "address",
                                                         "emission_flags",
                                                         "area_source_id",
                                                         "emission_power"};
    static constexpr std::string_view ALPHA_KEYS[] = {"texture",
                                                      "metallic",
                                                      "roughness",
                                                      "emission",
                                                      "address",
                                                      "emission_flags",
                                                      "area_source_id",
                                                      "emission_power",
                                                      "alpha_mode",
                                                      "alpha_cutoff"};
    static constexpr std::string_view SURFACE_KEYS[] = {"texture",
                                                        "metallic",
                                                        "roughness",
                                                        "emission",
                                                        "address",
                                                        "emission_flags",
                                                        "area_source_id",
                                                        "emission_power",
                                                        "alpha_mode",
                                                        "alpha_cutoff",
                                                        "surface_texture",
                                                        "emission_texture",
                                                        "normal_scale"};
    const bool hasLighting = tree.GetMember(root, "emission_flags") != NO_JSON_NODE;
    const bool hasAlpha = tree.GetMember(root, "alpha_mode") != NO_JSON_NODE;
    const bool hasSurfaceMaps = tree.GetMember(root, "surface_texture") != NO_JSON_NODE;
    const bool hasExactKeys = hasSurfaceMaps == true ? tree.HasExactKeys(root, SURFACE_KEYS)
                              : hasAlpha == true     ? tree.HasExactKeys(root, ALPHA_KEYS)
                              : hasLighting == true  ? tree.HasExactKeys(root, LIGHTING_KEYS)
                                                     : tree.HasExactKeys(root, KEYS);
    if (hasExactKeys == false)
    {
        return inoutFailure.FailInvalid("Invalid cooked material fields");
    }
    if (hasAlpha == true)
    {
        const JsonIndex mode = tree.GetMember(root, "alpha_mode");
        const bool isMasked = tree.IsString(mode, "masked");
        PythonNumber cutoff;
        if ((isMasked == false && tree.IsString(mode, "opaque") == false) ||
            IsAuthoredNumber(tree, tree.GetMember(root, "alpha_cutoff"), cutoff) == false ||
            IsLessPython(cutoff, MakeNumber(int64{0})) == true || IsLessPython(MakeNumber(int64{1}), cutoff) == true ||
            (isMasked == false && cutoff.ToFloat() != 0))
        {
            return inoutFailure.FailInvalid("Invalid cooked material alpha");
        }
    }
    if (hasLighting == true)
    {
        uint64 sourceId = 0;
        const JsonNode& flags = tree.GetNode(tree.GetMember(root, "emission_flags"));
        PythonNumber power;
        if (flags.Kind != JsonKind::Integer || flags.Integer < 0 ||
            (static_cast<uint64>(flags.Integer) & ~uint64{format::EMISSION_FLAGS}) != 0 ||
            ReadSourceId(tree, tree.GetMember(root, "area_source_id"), sourceId) == false ||
            IsAuthoredNumber(tree, tree.GetMember(root, "emission_power"), power) == false || power.ToFloat() < 0 ||
            power.ToFloat() > static_cast<float64>(format::MAX_LIGHT_INTENSITY) ||
            ((flags.Integer & format::EMISSION_HAS_POWER) == 0 && power.ToFloat() != 0))
        {
            return inoutFailure.FailInvalid("Invalid material emitter metadata");
        }
        const JsonIndex emissionValues = tree.GetMember(root, "emission");
        if (AreAuthoredNumbers(tree, emissionValues, RGB_COMPONENTS) == true && sourceId == 0)
        {
            for (usize channel = 0; channel < RGB_COMPONENTS; ++channel)
            {
                if (GetNumberItem(tree, emissionValues, channel).ToFloat() > 0)
                {
                    return inoutFailure.FailInvalid("Missing emitter source identity");
                }
            }
        }
    }
    const JsonIndex address = tree.GetMember(root, "address");
    if (tree.IsString(address, "clamp") == false && tree.IsString(address, "repeat") == false)
    {
        return inoutFailure.FailInvalid("Invalid cooked sampler address");
    }
    const JsonIndex texture = tree.GetMember(root, "texture");
    if (tree.GetKind(texture) != JsonKind::String ||
        (tree.GetText(texture).empty() == false && IsResourceId(tree, texture, "texture") == false))
    {
        return inoutFailure.FailInvalid("Cooked material dependency mismatch");
    }
    Array<std::string_view> dependencies;
    if (AddMaterialDependency(dependencies, tree.GetText(texture)) == false)
    {
        return inoutFailure.FailOutOfMemory();
    }
    if (hasSurfaceMaps == true)
    {
        PythonNumber normalScale;
        const JsonIndex surface = tree.GetMember(root, "surface_texture");
        const JsonIndex emissionMap = tree.GetMember(root, "emission_texture");
        for (const JsonIndex map : {surface, emissionMap})
        {
            if (tree.GetKind(map) != JsonKind::String ||
                (tree.GetText(map).empty() == false && IsResourceId(tree, map, "texture") == false))
            {
                return inoutFailure.FailInvalid("Cooked material dependency mismatch");
            }
        }
        if (IsAuthoredNumber(tree, tree.GetMember(root, "normal_scale"), normalScale) == false ||
            normalScale.ToFloat() < 0 || normalScale.ToFloat() > static_cast<float64>(format::MAX_NORMAL_SCALE) ||
            (tree.GetText(surface).empty() == true && normalScale.ToFloat() != 0) ||
            (tree.GetText(surface).empty() == false &&
             (tree.GetText(surface) == tree.GetText(texture) || tree.GetText(surface) == tree.GetText(emissionMap))))
        {
            return inoutFailure.FailInvalid("Invalid cooked material maps");
        }
        if (AddMaterialDependency(dependencies, tree.GetText(surface)) == false ||
            AddMaterialDependency(dependencies, tree.GetText(emissionMap)) == false)
        {
            return inoutFailure.FailOutOfMemory();
        }
    }
    JsonTree expected;
    const JsonIndex expectedList = expected.AddArray();
    for (const std::string_view dependency : dependencies)
    {
        expected.Append(expectedList, expected.AddString(dependency));
    }
    if (expected.IsOk() == false)
    {
        return inoutFailure.FailOutOfMemory();
    }
    if (AreJsonEqual(closureTree, closureTree.GetMember(record, "dependencies"), expected, expectedList) == false)
    {
        return inoutFailure.FailInvalid("Cooked material dependency mismatch");
    }
    PythonNumber metallic;
    PythonNumber roughness;
    const JsonIndex emission = tree.GetMember(root, "emission");
    bool isValid = IsAuthoredNumber(tree, tree.GetMember(root, "metallic"), metallic) == true &&
                   IsAuthoredNumber(tree, tree.GetMember(root, "roughness"), roughness) == true &&
                   AreAuthoredNumbers(tree, emission, 3) == true;
    if (isValid == true)
    {
        isValid = IsLessOrEqualPython(MakeNumber(int64{0}), metallic) == true &&
                  IsLessOrEqualPython(metallic, MakeNumber(int64{1})) == true &&
                  IsLessOrEqualPython(MakeNumber(MIN_ROUGHNESS_REFERENCE), roughness) == true &&
                  IsLessOrEqualPython(roughness, MakeNumber(int64{1})) == true;
        for (usize i = 0; i < 3 && isValid == true; ++i)
        {
            const PythonNumber value = GetNumberItem(tree, emission, i);
            isValid = IsLessPython(value, MakeNumber(int64{0})) == false &&
                      IsLessPython(MakeNumber(MAX_EMISSION_REFERENCE), value) == false;
        }
    }
    return isValid == true ? true : inoutFailure.FailInvalid("Invalid cooked material values");
}

/// Read one closure artifact's bytes.
[[nodiscard]] bool ReadArtifact(std::string_view root,
                                const Document& closure,
                                std::string_view identity,
                                usize maximum,
                                Array<uint8>& outData,
                                Failure& inoutFailure) noexcept
{
    const JsonIndex record = closure.Tree.GetMember(closure.Root, identity);
    String path;
    return ResolveInside(root, closure.Tree.GetText(closure.Tree.GetMember(record, "path")), path, inoutFailure) ==
               true &&
           ReadBytes(path.GetView(), maximum, outData, inoutFailure) == true;
}

struct LightingPart final
{
    Array<uint8> Bytes;
    std::string_view Identity;
};

/// Catalog part layout, recorded by its build target.
enum class PartLayout : uint8
{
    /// Version 1/2 records: VertexRecord meshes without lighting fields.
    Legacy,
    /// Version 3 records: VertexRecordV3 meshes with stored bake coordinates.
    StoredBakeCoordinates,
    /// Version 4 and 5 records: indexed VertexRecord meshes with implicit bake charts.
    Indexed,
    /// Version 6 records: indexed VertexRecordV6 meshes with tangents.
    Tangents,
};

[[nodiscard]] PartLayout GetPartLayout(const Document& closure, std::string_view identity) noexcept
{
    const JsonIndex record = closure.Tree.GetMember(closure.Root, identity);
    const JsonIndex build = closure.Tree.GetMember(record, "build");
    if (build == NO_JSON_NODE)
    {
        return PartLayout::Legacy;
    }
    const JsonIndex target = closure.Tree.GetMember(build, "target");
    if (closure.Tree.IsString(target, LIGHTING_BUILD_TARGET) == true)
    {
        return PartLayout::StoredBakeCoordinates;
    }
    if (closure.Tree.IsString(target, SURFACE_MAP_BUILD_TARGET) == true)
    {
        return PartLayout::Tangents;
    }
    return closure.Tree.IsString(target, INDEXED_LIGHTING_BUILD_TARGET) == true ||
                   closure.Tree.IsString(target, IMPORT_PROFILE_BUILD_TARGET) == true
               ? PartLayout::Indexed
               : PartLayout::Legacy;
}

/// True for parts split from a version 5 or 6 bundle, which assemble within the import profile.
[[nodiscard]] bool IsImportProfileBuild(const Document& closure, std::string_view identity) noexcept
{
    const JsonIndex record = closure.Tree.GetMember(closure.Root, identity);
    const JsonIndex build = closure.Tree.GetMember(record, "build");
    const JsonIndex target = build != NO_JSON_NODE ? closure.Tree.GetMember(build, "target") : NO_JSON_NODE;
    return build != NO_JSON_NODE && (closure.Tree.IsString(target, IMPORT_PROFILE_BUILD_TARGET) == true ||
                                     closure.Tree.IsString(target, SURFACE_MAP_BUILD_TARGET) == true);
}

/// True for parts that carry version 3 lighting fields (versions 3 to 5).
[[nodiscard]] bool IsLightingBuild(const Document& closure, std::string_view identity) noexcept
{
    return GetPartLayout(closure, identity) != PartLayout::Legacy;
}

// Scope equivalence groups by the owning imported scene, not by material index:
// multiple materials can belong to one authored area emitter.
[[nodiscard]] std::string_view GetMaterialScope(const Document& closure, std::string_view material) noexcept
{
    const JsonTree& tree = closure.Tree;
    for (usize i = 0; i < tree.GetCount(closure.Root); ++i)
    {
        const JsonIndex record = tree.GetValue(closure.Root, i);
        if (tree.IsString(tree.GetMember(record, "kind"), "scene") == true)
        {
            const JsonIndex dependencies = tree.GetMember(record, "dependencies");
            for (usize j = 0; j < tree.GetCount(dependencies); ++j)
            {
                if (tree.IsString(tree.GetItem(dependencies, j), material) == true)
                {
                    return tree.GetKey(closure.Root, i);
                }
            }
        }
    }
    return material;
}

[[nodiscard]] bool
AssembleLighting(std::string_view root, const Document& closure, Array<uint8>& outBytes, Failure& inoutFailure) noexcept
{
    Array<LightingPart> parts;
    uint32 lightCount = 0;
    uint32 width = 0;
    uint32 height = 0;
    for (usize i = 0; i < closure.Tree.GetCount(closure.Root); ++i)
    {
        const JsonIndex record = closure.Tree.GetValue(closure.Root, i);
        if (closure.Tree.IsString(closure.Tree.GetMember(record, "kind"), "lighting") == false)
        {
            continue;
        }
        LightingPart part;
        part.Identity = closure.Tree.GetKey(closure.Root, i);
        if (ReadArtifact(root, closure, part.Identity, MAX_ASSET_BYTES, part.Bytes, inoutFailure) == false ||
            ValidateLightingPayload(GetSpan(part.Bytes), inoutFailure) == false)
        {
            return false;
        }
        const uint8* bytes = part.Bytes.GetData();
        const uint32 count = DecodeUint32(bytes + sizeof(uint32));
        if (count > format::MAX_LIGHTS - lightCount)
        {
            return inoutFailure.FailInvalid("Combined authored analytic light capacity exceeded");
        }
        lightCount += count;
        const uint32 partWidth = DecodeUint32(bytes + SOURCE_ID_WORDS * sizeof(uint32));
        const uint32 partHeight = DecodeUint32(bytes + RGB_COMPONENTS * sizeof(uint32));
        if (partWidth != 0)
        {
            if (width != 0 && (width != partWidth || height != partHeight))
            {
                return inoutFailure.FailInvalid(
                    "Authored environment maps require matching dimensions for exact summation");
            }
            width = partWidth;
            height = partHeight;
        }
        if (parts.TryAdd(static_cast<LightingPart&&>(part)) == false)
        {
            return inoutFailure.FailOutOfMemory();
        }
    }
    if (parts.IsEmpty() == true)
    {
        return true;
    }
    const usize pixels = usize{width} * height;
    const usize total = sizeof(format::LightingRecord) + usize{lightCount} * sizeof(format::LightRecord) +
                        pixels * format::ENVIRONMENT_TEXEL_BYTES;
    if (total > MAX_ASSET_BYTES)
    {
        return inoutFailure.FailInvalid("Combined authored lighting byte capacity exceeded");
    }
    if (outBytes.TryEnsureCapacity(total) == false)
    {
        return inoutFailure.FailOutOfMemory();
    }
    foundation::StaticArray<float64, RGB_COMPONENTS> constant{};
    for (const LightingPart& part : parts)
    {
        for (usize channel = 0; channel < RGB_COMPONENTS; ++channel)
        {
            constant[channel] += DecodeFloat(part.Bytes.GetData() + sizeof(uint32) * 4 + channel * sizeof(float32));
            if (constant[channel] > static_cast<float64>(format::MAX_EMISSION))
            {
                return inoutFailure.FailInvalid("Combined authored environment energy exceeded");
            }
        }
    }
    ByteWriter writer(outBytes);
    writer.AppendUint32(format::LIGHTING_PROFILE_REC709_PHOTOMETRIC);
    writer.AppendUint32(lightCount);
    writer.AppendUint32(width);
    writer.AppendUint32(height);
    for (const float64 value : constant)
    {
        writer.AppendFloat32(value);
    }
    foundation::StaticArray<uint64, format::MAX_LIGHTS> ids{};
    usize idCount = 0;
    for (const LightingPart& part : parts)
    {
        const uint32 count = DecodeUint32(part.Bytes.GetData() + sizeof(uint32));
        for (usize i = 0; i < count; ++i)
        {
            const uint8* light =
                part.Bytes.GetData() + sizeof(format::LightingRecord) + i * sizeof(format::LightRecord);
            const uint64 source = uint64{DecodeUint32(light)} | (uint64{DecodeUint32(light + sizeof(uint32))} << 32);
            uint64 scoped = 0;
            if (MakeScopedSourceId(part.Identity, source, scoped) == false)
            {
                return inoutFailure.FailOutOfMemory();
            }
            for (usize j = 0; j < idCount; ++j)
            {
                if (ids[j] == scoped)
                {
                    return inoutFailure.FailInvalid("Scoped authored light identity collision");
                }
            }
            ids[idCount++] = scoped;
            AppendSourceId(writer, scoped);
            writer.AppendBytes({light + sizeof(uint64), sizeof(format::LightRecord) - sizeof(uint64)});
        }
    }
    if (parts.GetSize() == 1)
    {
        const LightingPart& part = parts[0];
        const usize offset = sizeof(format::LightingRecord) + usize{lightCount} * sizeof(format::LightRecord);
        writer.AppendBytes(GetSpan(part.Bytes).subspan(offset));
        return writer.IsOk() == true ? true : inoutFailure.FailOutOfMemory();
    }
    for (usize pixel = 0; pixel < pixels; ++pixel)
    {
        for (usize channel = 0; channel < RGB_COMPONENTS; ++channel)
        {
            float64 sum = 0;
            for (const LightingPart& part : parts)
            {
                const uint8* bytes = part.Bytes.GetData();
                const float64 scale = DecodeFloat(bytes + sizeof(uint32) * 4 + channel * sizeof(float32));
                if (DecodeUint32(bytes + SOURCE_ID_WORDS * sizeof(uint32)) == 0)
                {
                    sum += scale;
                }
                else
                {
                    const usize offset = sizeof(format::LightingRecord) +
                                         usize{DecodeUint32(bytes + sizeof(uint32))} * sizeof(format::LightRecord) +
                                         pixel * format::ENVIRONMENT_TEXEL_BYTES + channel * sizeof(float32);
                    sum += scale * DecodeFloat(bytes + offset);
                }
            }
            const float64 normalized = constant[channel] == 0 ? 0 : sum / constant[channel];
            if (math::IsFinite(normalized) == false || normalized > static_cast<float64>(format::MAX_EMISSION))
            {
                return inoutFailure.FailInvalid("Combined authored HDR environment energy exceeded");
            }
            writer.AppendFloat32(normalized);
        }
    }
    return writer.IsOk() == true ? true : inoutFailure.FailOutOfMemory();
}

/// assemble(root, level, current).
[[nodiscard]] bool AssembleBundle(std::string_view root,
                                  const JsonTree& levelTree,
                                  JsonIndex level,
                                  const Document* currentOrNull,
                                  Array<uint8>& outBundle,
                                  Failure& inoutFailure) noexcept
{
    LockScope lock;
    Document closure;
    if (lock.Enter(root, LockMode::Shared, inoutFailure) == false ||
        ComputeClosure(root, levelTree, level, currentOrNull, closure, inoutFailure) == false)
    {
        return false;
    }
    outBundle.Clear();
    const JsonIndex objects = levelTree.GetMember(level, "objects");
    const usize objectCount = levelTree.GetCount(objects);
    Array<std::string_view> meshes;
    Array<std::string_view> materials;
    for (usize i = 0; i < objectCount; ++i)
    {
        const JsonIndex item = levelTree.GetItem(objects, i);
        if (AddSortedUnique(meshes, levelTree.GetText(levelTree.GetMember(item, "mesh"))) == false ||
            AddSortedUnique(materials, levelTree.GetText(levelTree.GetMember(item, "material"))) == false)
        {
            return inoutFailure.FailOutOfMemory();
        }
    }
    if (meshes.IsEmpty() == true)
    {
        return true;
    }
    Array<Document> descriptions;
    if (descriptions.TryResize(materials.GetSize()) == false)
    {
        return inoutFailure.FailOutOfMemory();
    }
    for (usize i = 0; i < materials.GetSize(); ++i)
    {
        Array<uint8> data;
        if (ReadArtifact(root, closure, materials[i], MAX_METADATA_BYTES, data, inoutFailure) == false ||
            ParseDocument(GetSpan(data), AssetErrorCode::AssetCorrupt, descriptions[i], inoutFailure) == false)
        {
            return false;
        }
    }
    Array<std::string_view> textures;
    for (usize i = 0; i < materials.GetSize(); ++i)
    {
        const JsonIndex record = closure.Tree.GetMember(closure.Root, materials[i]);
        if (IsValidMaterialDescription(descriptions[i], closure.Tree, record, inoutFailure) == false)
        {
            return false;
        }
    }
    for (const Document& description : descriptions)
    {
        for (const std::string_view key : {"texture", "surface_texture", "emission_texture"})
        {
            const JsonIndex member = description.Tree.GetMember(description.Root, key);
            const std::string_view texture = member != NO_JSON_NODE ? description.Tree.GetText(member) : "";
            if (texture.empty() == false && AddSortedUnique(textures, texture) == false)
            {
                return inoutFailure.FailOutOfMemory();
            }
        }
    }
    // Version 6 assembles tangent-framed mesh parts or materials with surface/emission
    // maps; older mesh parts then gain absent (zero) tangents.
    bool isVersion6 = false;
    for (const std::string_view identity : meshes)
    {
        isVersion6 = isVersion6 == true || GetPartLayout(closure, identity) == PartLayout::Tangents;
    }
    for (const Document& description : descriptions)
    {
        isVersion6 =
            isVersion6 == true || description.Tree.GetMember(description.Root, "surface_texture") != NO_JSON_NODE;
    }
    // Version 5 assembles parts split from a version 5 bundle, alpha-masked materials,
    // or more meshes, materials, textures or objects than the resident reference
    // profile admits. It implies the version 4 indexed meshes and lighting records.
    bool isVersion5 = isVersion6 == true || meshes.GetSize() > format::RESIDENT_REFERENCE_LIMITS.Meshes ||
                      materials.GetSize() > format::RESIDENT_REFERENCE_LIMITS.Materials ||
                      textures.GetSize() > format::RESIDENT_REFERENCE_LIMITS.Textures ||
                      objectCount > format::RESIDENT_REFERENCE_LIMITS.Objects;
    for (const std::string_view identity : meshes)
    {
        isVersion5 = isVersion5 == true || IsImportProfileBuild(closure, identity) == true;
    }
    for (const Document& description : descriptions)
    {
        isVersion5 = isVersion5 == true || description.Tree.GetMember(description.Root, "alpha_mode") != NO_JSON_NODE;
    }
    const format::BundleLimits& limits = format::GetBundleLimits(isVersion6 == true   ? format::VERSION_6
                                                                 : isVersion5 == true ? format::VERSION_5
                                                                                      : format::VERSION_4);
    if (meshes.GetSize() > limits.Meshes || materials.GetSize() > limits.Materials ||
        textures.GetSize() > limits.Textures || objectCount > limits.Objects)
    {
        return inoutFailure.FailInvalid(
            "Scene exceeds 256 meshes/materials/textures or 1024 objects; no partial viewport publication");
    }
    Array<uint8> lighting;
    if (AssembleLighting(root, closure, lighting, inoutFailure) == false)
    {
        return false;
    }
    bool isVersion3 = isVersion5 == true || lighting.IsEmpty() == false;
    // Any indexed (version 4) mesh selects version 4: version 3 meshes then drop
    // their stored bake coordinates, which version 4 consumers derive instead.
    bool isVersion4 = isVersion5;
    for (const std::string_view identity : meshes)
    {
        isVersion3 = isVersion3 == true || IsLightingBuild(closure, identity) == true;
        isVersion4 = isVersion4 == true || GetPartLayout(closure, identity) == PartLayout::Indexed ||
                     GetPartLayout(closure, identity) == PartLayout::Tangents;
    }
    for (const Document& description : descriptions)
    {
        isVersion3 =
            isVersion3 == true || description.Tree.GetMember(description.Root, "emission_flags") != NO_JSON_NODE;
    }
    uint64 bundleBytes =
        sizeof(format::Header) +
        materials.GetSize() * (isVersion6 == true   ? sizeof(format::MaterialRecordV6)
                               : isVersion5 == true ? sizeof(format::MaterialRecordV5)
                               : isVersion3 == true ? sizeof(format::MaterialRecordV3)
                                                    : sizeof(format::MaterialRecordV2)) +
        objectCount * (isVersion3 == true ? sizeof(format::ObjectRecordV3) : sizeof(format::ObjectRecord)) +
        lighting.GetSize();
    if (isVersion3 == true && lighting.IsEmpty() == true)
    {
        bundleBytes += sizeof(format::LightingRecord);
    }
    for (const std::string_view identity : textures)
    {
        const JsonIndex record = closure.Tree.GetMember(closure.Root, identity);
        bundleBytes += static_cast<uint64>(GetNumberMember(closure.Tree, record, "bytes").ToFloat());
    }
    uint64 indexTotal = 0;
    for (const std::string_view identity : meshes)
    {
        const JsonIndex record = closure.Tree.GetMember(closure.Root, identity);
        bundleBytes += static_cast<uint64>(GetNumberMember(closure.Tree, record, "bytes").ToFloat());
        const PartLayout layout = GetPartLayout(closure, identity);
        const bool isAddingBakeCoordinates = isVersion4 == false && isVersion3 == true && layout == PartLayout::Legacy;
        const bool isDroppingBakeCoordinates = isVersion4 == true && layout == PartLayout::StoredBakeCoordinates;
        if (isAddingBakeCoordinates == true || isDroppingBakeCoordinates == true || isVersion4 == true)
        {
            Array<uint8> data;
            if (ReadArtifact(root, closure, identity, MAX_ASSET_BYTES, data, inoutFailure) == false)
            {
                return false;
            }
            const bool hasHeader = data.GetSize() >= sizeof(format::MeshRecord);
            const uint32 vertices = hasHeader == true ? DecodeUint32(data.GetData()) : 0;
            const uint32 indices = hasHeader == true ? DecodeUint32(data.GetData() + sizeof(uint32)) : 0;
            if (vertices == 0 || vertices > limits.Vertices)
            {
                return inoutFailure.FailInvalid("Invalid mesh conversion vertex count");
            }
            const uint64 bakeBytes = uint64{vertices} * (sizeof(format::VertexRecordV3) - sizeof(format::VertexRecord));
            if (isVersion6 == true && layout != PartLayout::Tangents)
            {
                bundleBytes += uint64{vertices} * (sizeof(format::VertexRecordV6) - sizeof(format::VertexRecord));
            }
            if (isAddingBakeCoordinates == true)
            {
                bundleBytes += bakeBytes;
            }
            if (isDroppingBakeCoordinates == true)
            {
                bundleBytes -= bakeBytes;
            }
            indexTotal += indices;
        }
    }
    if (isVersion4 == true && indexTotal > limits.TotalIndices)
    {
        return inoutFailure.FailInvalid("Assembled scene index capacity exceeded");
    }
    if (bundleBytes > limits.BundleBytes)
    {
        return inoutFailure.FailInvalid("Assembled scene byte capacity exceeded");
    }
    ByteWriter writer(outBundle);
    writer.Append(format::MAGIC);
    const uint32 version = isVersion6 == true   ? format::VERSION_6
                           : isVersion5 == true ? format::VERSION_5
                           : isVersion4 == true ? format::VERSION_4
                           : isVersion3 == true ? format::VERSION_3
                                                : format::VERSION_2;
    writer.AppendUint32(version);
    writer.AppendUint32(static_cast<uint32>(meshes.GetSize()));
    writer.AppendUint32(static_cast<uint32>(materials.GetSize()));
    writer.AppendUint32(static_cast<uint32>(textures.GetSize()));
    writer.AppendUint32(static_cast<uint32>(objectCount));
    const JsonIndex camera = levelTree.GetMember(level, "camera");
    for (const std::string_view field : {"eye", "target", "up"})
    {
        const JsonIndex values = levelTree.GetMember(camera, field);
        for (usize i = 0; i < 3; ++i)
        {
            writer.AppendFloat32(GetNumberItem(levelTree, values, i).ToFloat());
        }
    }
    writer.AppendFloat32(GetNumberMember(levelTree, camera, "fov").ToFloat());
    for (const std::string_view identity : textures)
    {
        Array<uint8> data;
        if (ReadArtifact(root, closure, identity, MAX_ASSET_BYTES, data, inoutFailure) == false)
        {
            return false;
        }
        writer.AppendBytes(GetSpan(data));
    }
    for (usize materialIndex = 0; materialIndex < descriptions.GetSize(); ++materialIndex)
    {
        const Document& description = descriptions[materialIndex];
        const JsonTree& tree = description.Tree;
        const std::string_view texture = tree.GetText(tree.GetMember(description.Root, "texture"));
        const bool isRepeat = tree.IsString(tree.GetMember(description.Root, "address"), "repeat");
        writer.AppendUint32(texture.empty() == true ? format::NO_TEXTURE : FindIndex(textures, texture));
        writer.AppendUint32(isRepeat == true ? format::ADDRESS_REPEAT : format::ADDRESS_CLAMP);
        writer.AppendFloat32(GetNumberMember(tree, description.Root, "metallic").ToFloat());
        writer.AppendFloat32(GetNumberMember(tree, description.Root, "roughness").ToFloat());
        const JsonIndex emission = tree.GetMember(description.Root, "emission");
        for (usize channel = 0; channel < 3; ++channel)
        {
            writer.AppendFloat32(GetNumberItem(tree, emission, channel).ToFloat());
        }
        if (isVersion3 == true)
        {
            const bool hasLighting = tree.GetMember(description.Root, "emission_flags") != NO_JSON_NODE;
            uint64 sourceId = 0;
            uint32 flags = format::EMISSION_TWO_SIDED;
            float64 power = 0;
            if (hasLighting == true)
            {
                flags = static_cast<uint32>(tree.GetNode(tree.GetMember(description.Root, "emission_flags")).Integer);
                power = GetNumberMember(tree, description.Root, "emission_power").ToFloat();
                if (ReadSourceId(tree, tree.GetMember(description.Root, "area_source_id"), sourceId) == false)
                {
                    return inoutFailure.FailInvalid("Invalid emitter identity");
                }
            }
            bool isEmissive = sourceId != 0;
            for (usize channel = 0; channel < RGB_COMPONENTS; ++channel)
            {
                isEmissive = isEmissive == true || GetNumberItem(tree, emission, channel).ToFloat() > 0;
            }
            if (isEmissive == true &&
                MakeScopedSourceId(hasLighting == true ? GetMaterialScope(closure, materials[materialIndex])
                                                       : materials[materialIndex],
                                   sourceId,
                                   sourceId) == false)
            {
                return inoutFailure.FailOutOfMemory();
            }
            writer.AppendUint32(flags);
            AppendSourceId(writer, sourceId);
            writer.AppendFloat32(power);
            if (isVersion5 == true)
            {
                const JsonIndex mode = tree.GetMember(description.Root, "alpha_mode");
                const bool isMasked = tree.IsString(mode, "masked");
                writer.AppendUint32(isMasked == true ? format::ALPHA_MASKED : format::ALPHA_OPAQUE);
                writer.AppendFloat32(
                    isMasked == true ? GetNumberMember(tree, description.Root, "alpha_cutoff").ToFloat() : 0.0);
            }
            if (isVersion6 == true)
            {
                const auto findMap = [&](std::string_view key) {
                    const JsonIndex member = tree.GetMember(description.Root, key);
                    const std::string_view map = member != NO_JSON_NODE ? tree.GetText(member) : "";
                    return map.empty() == true ? format::NO_TEXTURE : FindIndex(textures, map);
                };
                const bool hasScale = tree.GetMember(description.Root, "normal_scale") != NO_JSON_NODE;
                writer.AppendUint32(findMap("surface_texture"));
                writer.AppendUint32(findMap("emission_texture"));
                writer.AppendFloat32(
                    hasScale == true ? GetNumberMember(tree, description.Root, "normal_scale").ToFloat() : 0.0);
            }
            else
            {
                writer.AppendUint32(0);
            }
        }
    }
    for (const std::string_view identity : meshes)
    {
        Array<uint8> data;
        if (ReadArtifact(root, closure, identity, MAX_ASSET_BYTES, data, inoutFailure) == false)
        {
            return false;
        }
        const PartLayout layout = GetPartLayout(closure, identity);
        if (isVersion6 == true && layout != PartLayout::Tangents)
        {
            // Older parts gain absent tangents; version 3 parts drop their stored bake coordinates.
            const usize sourceBytes = layout == PartLayout::StoredBakeCoordinates ? sizeof(format::VertexRecordV3)
                                                                                  : sizeof(format::VertexRecord);
            const uint32 vertexCount = data.GetSize() >= sizeof(format::MeshRecord) ? DecodeUint32(data.GetData()) : 0;
            const uint32 indexCount =
                data.GetSize() >= sizeof(format::MeshRecord) ? DecodeUint32(data.GetData() + sizeof(uint32)) : 0;
            const usize expected =
                sizeof(format::MeshRecord) + usize{vertexCount} * sourceBytes + usize{indexCount} * sizeof(uint32);
            if (data.GetSize() != expected || vertexCount > limits.Vertices)
            {
                return inoutFailure.FailInvalid("Invalid tangent mesh conversion length");
            }
            constexpr uint8 ABSENT_TANGENT[sizeof(format::VertexRecordV6) - sizeof(format::VertexRecord)]{};
            writer.AppendBytes({data.GetData(), sizeof(format::MeshRecord)});
            for (usize vertex = 0; vertex < vertexCount; ++vertex)
            {
                writer.AppendBytes(
                    {data.GetData() + sizeof(format::MeshRecord) + vertex * sourceBytes, sizeof(format::VertexRecord)});
                writer.AppendBytes({ABSENT_TANGENT, sizeof(ABSENT_TANGENT)});
            }
            writer.AppendBytes({data.GetData() + sizeof(format::MeshRecord) + usize{vertexCount} * sourceBytes,
                                usize{indexCount} * sizeof(uint32)});
        }
        else if (format::HasImplicitBakeCoordinates(version) == true && layout == PartLayout::StoredBakeCoordinates)
        {
            const uint32 vertexCount = data.GetSize() >= sizeof(format::MeshRecord) ? DecodeUint32(data.GetData()) : 0;
            const uint32 indexCount =
                data.GetSize() >= sizeof(format::MeshRecord) ? DecodeUint32(data.GetData() + sizeof(uint32)) : 0;
            const usize expected = sizeof(format::MeshRecord) + usize{vertexCount} * sizeof(format::VertexRecordV3) +
                                   usize{indexCount} * sizeof(uint32);
            if (data.GetSize() != expected || vertexCount > limits.Vertices)
            {
                return inoutFailure.FailInvalid("Invalid version 3 mesh conversion length");
            }
            writer.AppendBytes({data.GetData(), sizeof(format::MeshRecord)});
            for (usize vertex = 0; vertex < vertexCount; ++vertex)
            {
                writer.AppendBytes(
                    {data.GetData() + sizeof(format::MeshRecord) + vertex * sizeof(format::VertexRecordV3),
                     sizeof(format::VertexRecord)});
            }
            writer.AppendBytes(
                {data.GetData() + sizeof(format::MeshRecord) + usize{vertexCount} * sizeof(format::VertexRecordV3),
                 usize{indexCount} * sizeof(uint32)});
        }
        else if (version == format::VERSION_3 && layout == PartLayout::Legacy)
        {
            const uint32 vertexCount = data.GetSize() >= sizeof(format::MeshRecord) ? DecodeUint32(data.GetData()) : 0;
            const uint32 indexCount =
                data.GetSize() >= sizeof(format::MeshRecord) ? DecodeUint32(data.GetData() + sizeof(uint32)) : 0;
            const usize expected = sizeof(format::MeshRecord) + usize{vertexCount} * sizeof(format::VertexRecord) +
                                   usize{indexCount} * sizeof(uint32);
            if (data.GetSize() != expected || vertexCount > limits.Vertices)
            {
                return inoutFailure.FailInvalid("Invalid legacy mesh conversion length");
            }
            writer.AppendBytes({data.GetData(), sizeof(format::MeshRecord)});
            for (usize vertex = 0; vertex < vertexCount; ++vertex)
            {
                writer.AppendBytes({data.GetData() + sizeof(format::MeshRecord) + vertex * sizeof(format::VertexRecord),
                                    sizeof(format::VertexRecord)});
                writer.AppendFloat32(0);
                writer.AppendFloat32(0);
            }
            writer.AppendBytes(
                {data.GetData() + sizeof(format::MeshRecord) + usize{vertexCount} * sizeof(format::VertexRecord),
                 usize{indexCount} * sizeof(uint32)});
        }
        else
        {
            writer.AppendBytes(GetSpan(data));
        }
    }
    Array<uint64> objectIds;
    if (objectIds.TryResize(objectCount) == false)
    {
        return inoutFailure.FailOutOfMemory();
    }
    for (usize i = 0; i < objectCount; ++i)
    {
        const JsonIndex item = levelTree.GetItem(objects, i);
        Transform world;
        ComposeWorld(levelTree, objects, item, world, 0);
        for (const PythonNumber& value : world.Values)
        {
            if (IsFinitePython(value) == false ||
                IsLessOrEqualPython(GetAbsolute(value), MakeNumber(MAX_WORLD_MAGNITUDE)) == false)
            {
                return inoutFailure.FailInvalid("World transform outside float32 range");
            }
        }
        writer.AppendUint32(FindIndex(meshes, levelTree.GetText(levelTree.GetMember(item, "mesh"))));
        writer.AppendUint32(FindIndex(materials, levelTree.GetText(levelTree.GetMember(item, "material"))));
        for (const PythonNumber& value : world.Values)
        {
            writer.AppendFloat32(value.ToFloat());
        }
        if (isVersion3 == true)
        {
            uint64 sourceId = 0;
            if (MakeScopedSourceId(levelTree.GetText(levelTree.GetMember(item, "id")), 0, sourceId) == false)
            {
                return inoutFailure.FailOutOfMemory();
            }
            for (usize j = 0; j < i; ++j)
            {
                if (objectIds[j] == sourceId)
                {
                    return inoutFailure.FailInvalid("Placed source identity collision");
                }
            }
            objectIds[i] = sourceId;
            AppendSourceId(writer, sourceId);
        }
    }
    if (isVersion3 == true)
    {
        if (lighting.IsEmpty() == true)
        {
            writer.AppendUint32(format::LIGHTING_PROFILE_REC709_PHOTOMETRIC);
            writer.AppendUint32(0);
            writer.AppendUint32(0);
            writer.AppendUint32(0);
            for (usize channel = 0; channel < RGB_COMPONENTS; ++channel)
            {
                writer.AppendFloat32(0);
            }
        }
        else
        {
            writer.AppendBytes(GetSpan(lighting));
        }
    }
    if (writer.HasOverflowed() == true)
    {
        return inoutFailure.FailInvalid("float too large to pack with f format");
    }
    if (writer.IsOk() == false)
    {
        return inoutFailure.FailOutOfMemory();
    }
    BundleParts parts;
    return SplitBundle(GetSpan(outBundle), parts, inoutFailure);
}

/// dump(root, level, current).
[[nodiscard]] bool DumpDocument(std::string_view root,
                                const JsonTree& levelTree,
                                JsonIndex level,
                                const Document* currentOrNull,
                                Document& outDump,
                                Failure& inoutFailure) noexcept
{
    LockScope lock;
    Document closure;
    if (lock.Enter(root, LockMode::Shared, inoutFailure) == false ||
        ComputeClosure(root, levelTree, level, currentOrNull, closure, inoutFailure) == false)
    {
        return false;
    }
    JsonTree& tree = outDump.Tree;
    outDump.Root = tree.AddObject();
    tree.SetMember(outDump.Root, "version", tree.AddInteger(1));
    tree.SetMember(outDump.Root, "profile", tree.AddString(LEVEL_PROFILE));
    tree.SetMember(outDump.Root, "level", tree.CopyFrom(levelTree, level));
    tree.SetMember(outDump.Root, "resources", tree.CopyFrom(closure.Tree, closure.Root));
    PythonSum total;
    for (usize i = 0; i < closure.Tree.GetCount(closure.Root); ++i)
    {
        total.Add(GetNumberMember(closure.Tree, closure.Tree.GetValue(closure.Root, i), "bytes"));
    }
    tree.SetMember(outDump.Root, "resident_bytes", AddPythonNumber(tree, total.GetResult()));
    return tree.IsOk() == true ? true : inoutFailure.FailOutOfMemory();
}

// ---------------------------------------------------------------------------
// Import.

/// A fresh logical identity: the caller's replay sequence verbatim, otherwise
/// FoundationIdentity's UUIDv4 from the caller's entropy. SceneCook acquires no
/// entropy itself; outValue is unchanged on failure.
[[nodiscard]] bool NextUuid(const AssetOptions& options, Uuid& outValue, Failure& inoutFailure) noexcept
{
    if (options.UuidsOrNull != nullptr && options.UuidsOrNull->Generate != nullptr)
    {
        options.UuidsOrNull->Generate(options.UuidsOrNull->ContextOrNull, outValue.Bytes);
        return true;
    }
    const foundation::identity::EntropySource unavailable;
    const foundation::identity::EntropySource& entropy =
        options.EntropyOrNull != nullptr ? *options.EntropyOrNull : unavailable;
    return foundation::identity::TryGenerateUuidV4(entropy, outValue) == foundation::identity::UuidStatus::Success
               ? true
               : inoutFailure.Fail(AssetErrorCode::MissingTools, "No entropy source for UUIDs");
}

/// prefix + str(uuid).
[[nodiscard]] bool FormatUuid(const Uuid& value, std::string_view prefix, String& outText) noexcept
{
    Array<uint8> bytes;
    ByteWriter writer(bytes);
    writer.Append(prefix);
    AppendUuid(writer, value);
    return writer.IsOk() == true && outText.TryAssign(GetView(bytes)) == foundation::StringStatus::Ok;
}

/// The reference profile's entry file; it keeps the directory as its origin.
constexpr std::string_view REFERENCE_ENTRY = "scene-v4.pbrt";

/// str(Path(source).resolve()), or the caller's provenance override. A best-effort
/// entry other than the reference scene-v4.pbrt appends "/<entry>", so two entry files
/// in one directory keep separate logical identities on reimport.
[[nodiscard]] bool
ResolveOrigin(std::string_view source, const AssetOptions& options, String& outOrigin, Failure& inoutFailure) noexcept
{
    if (options.SourceOrigin.empty() == false)
    {
        return outOrigin.TryAssign(options.SourceOrigin) == foundation::StringStatus::Ok
                   ? true
                   : inoutFailure.FailOutOfMemory();
    }
    if (ResolveNative(source, outOrigin, inoutFailure) == false)
    {
        return false;
    }
    const std::string_view entry = options.SourceEntry;
    if (entry.empty() == true || entry == REFERENCE_ENTRY)
    {
        return true;
    }
    const std::string_view parts[] = {std::string_view("/"), entry};
    return outOrigin.TryAppendMany(parts) == foundation::StringStatus::Ok ? true : inoutFailure.FailOutOfMemory();
}

/// scene_cook.asset_path(source, name).
[[nodiscard]] bool
ResolveSourceInput(std::string_view source, std::string_view name, String& outPath, Failure& inoutFailure) noexcept
{
    if (IsEscapingPath(name) == true)
    {
        return inoutFailure.Fail(AssetErrorCode::UnsupportedScene, "asset path escapes source directory");
    }
    String resolvedSource;
    String joined;
    if (ResolveNative(source, resolvedSource, inoutFailure) == false)
    {
        return false;
    }
    if (Concatenate(joined, {source, "/", name}) == false)
    {
        return inoutFailure.FailOutOfMemory();
    }
    if (ResolveNative(joined.GetView(), outPath, inoutFailure) == false)
    {
        return false;
    }
    if (IsBelow(outPath.GetView(), resolvedSource.GetView()) == false)
    {
        return inoutFailure.Fail(AssetErrorCode::UnsupportedScene, "asset symlink escapes source directory");
    }
    return true;
}

/// Recheck every manifest input digest against the source directory.
[[nodiscard]] bool AreSourceInputsUnchanged(std::string_view source,
                                            const JsonTree& tree,
                                            JsonIndex inputs,
                                            std::string_view message,
                                            Failure& inoutFailure) noexcept
{
    if (inputs == NO_JSON_NODE || tree.GetKind(inputs) != JsonKind::Object)
    {
        return inoutFailure.FailInvalid("Invalid manifest inputs");
    }
    for (usize i = 0; i < tree.GetCount(inputs); ++i)
    {
        String path;
        HexDigest digest;
        if (ResolveSourceInput(source, tree.GetKey(inputs, i), path, inoutFailure) == false ||
            HashSourceInput(path.GetView(), digest, inoutFailure) == false)
        {
            return false;
        }
        if (tree.IsString(tree.GetValue(inputs, i), digest.View()) == false)
        {
            return inoutFailure.Fail(AssetErrorCode::Conflict, message);
        }
    }
    return true;
}

/// The shared build record of one import.
struct BuildRecord final
{
    JsonTree Tree;
    JsonIndex Root = NO_JSON_NODE;
};

/// _put(root, resources, kind, data, dependencies, build, identity).
[[nodiscard]] bool StoreArtifact(std::string_view root,
                                 Document& inoutCatalog,
                                 std::string_view kind,
                                 std::span<const uint8> data,
                                 std::span<const std::string_view> dependencies,
                                 const BuildRecord& build,
                                 std::string_view identity,
                                 Failure& inoutFailure) noexcept
{
    JsonTree& tree = inoutCatalog.Tree;
    const JsonIndex resources = tree.GetMember(inoutCatalog.Root, "resources");
    HexDigest contentHash;
    ComputeHexDigest(data, contentHash);
    const JsonIndex dependencyList = tree.AddArray();
    const JsonIndex dependencyKeys = tree.AddObject();
    for (const std::string_view child : dependencies)
    {
        tree.Append(dependencyList, tree.AddString(child));
        const JsonIndex childKey = tree.CopyFrom(tree, tree.GetMember(tree.GetMember(resources, child), "key"));
        tree.SetMember(dependencyKeys, child, childKey);
    }
    const JsonIndex buildCopy = tree.CopyFrom(build.Tree, build.Root);
    if (tree.IsOk() == false)
    {
        return inoutFailure.FailOutOfMemory();
    }
    HexDigest key;
    String relative;
    String path;
    if (ComputeArtifactKey(kind,
                           contentHash.View(),
                           tree,
                           dependencyList,
                           dependencyKeys,
                           buildCopy,
                           key,
                           inoutFailure) == false)
    {
        return false;
    }
    if (Concatenate(relative, {OBJECTS_PATH, key.View(), ".bin"}) == false)
    {
        return inoutFailure.FailOutOfMemory();
    }
    if (ResolveInside(root, relative.GetView(), path, inoutFailure) == false)
    {
        return false;
    }
    if (DoesPathExist(path.GetView()) == true)
    {
        Array<uint8> existing;
        if (ReadBytes(path.GetView(), MAX_ASSET_BYTES, existing, inoutFailure) == false)
        {
            return false;
        }
        HexDigest existingHash;
        ComputeHexDigest(GetSpan(existing), existingHash);
        if (existingHash.View() != contentHash.View())
        {
            return inoutFailure.Fail(AssetErrorCode::AssetCorrupt, "An existing immutable artifact is corrupt");
        }
    }
    else if (PublishBytes(path.GetView(), data, PublishMode::Immutable, nullptr, inoutFailure) == false)
    {
        return false;
    }
    // A logical UUID remains stable across explicitly committed artifact revisions.
    const JsonIndex record = tree.AddObject();
    tree.SetMember(record, "kind", tree.AddString(kind));
    tree.SetMember(record, "key", tree.AddString(key.View()));
    tree.SetMember(record, "sha256", tree.AddString(contentHash.View()));
    tree.SetMember(record, "bytes", tree.AddInteger(static_cast<int64>(data.size())));
    tree.SetMember(record, "path", tree.AddString(relative.GetView()));
    tree.SetMember(record, "dependencies", dependencyList);
    tree.SetMember(record, "dependency_keys", dependencyKeys);
    tree.SetMember(record, "build", buildCopy);
    tree.SetMember(resources, identity, record);
    return tree.IsOk() == true ? true : inoutFailure.FailOutOfMemory();
}

/// {"eye", "target", "up", "fov"} from the bundle camera floats.
[[nodiscard]] JsonIndex AddCamera(JsonTree& inoutTree, const float64 (&camera)[CAMERA_FLOATS]) noexcept
{
    const JsonIndex result = inoutTree.AddObject();
    const std::string_view fields[3] = {"eye", "target", "up"};
    for (usize field = 0; field < 3; ++field)
    {
        const JsonIndex values = inoutTree.AddArray();
        for (usize i = 0; i < 3; ++i)
        {
            inoutTree.Append(values, inoutTree.AddFloat(camera[field * 3 + i]));
        }
        inoutTree.SetMember(result, fields[field], values);
    }
    inoutTree.SetMember(result, "fov", inoutTree.AddFloat(camera[CAMERA_FLOATS - 1]));
    return result;
}

[[nodiscard]] JsonIndex AddTransform(JsonTree& inoutTree,
                                     const float64 (&transform)[format::OBJECT_TRANSFORM_COMPONENTS]) noexcept
{
    const JsonIndex values = inoutTree.AddArray();
    for (const float64 value : transform)
    {
        inoutTree.Append(values, inoutTree.AddFloat(value));
    }
    return values;
}

[[nodiscard]] bool MapCookFailure(const CookError& error, Failure& inoutFailure) noexcept
{
    switch (error.Result)
    {
        case Status::Cancelled:
            return inoutFailure.Fail(AssetErrorCode::InstallCancelled, error.Message.View());
        case Status::Conflict:
            return inoutFailure.Fail(AssetErrorCode::Conflict, error.Message.View());
        case Status::OutOfMemory:
            return inoutFailure.FailOutOfMemory();
        default:
            return inoutFailure.Fail(AssetErrorCode::UnsupportedScene, error.Message.View());
    }
}

/// digest(read(catalog)) if the catalog exists.
[[nodiscard]] bool ReadPublishedCatalogDigest(std::string_view root,
                                              bool& outHasCatalog,
                                              Digest& outDigest,
                                              Failure& inoutFailure) noexcept
{
    String path;
    if (ResolveInside(root, CATALOG_PATH, path, inoutFailure) == false)
    {
        return false;
    }
    outHasCatalog = DoesPathExist(path.GetView());
    if (outHasCatalog == false)
    {
        return true;
    }
    Array<uint8> data;
    if (ReadBytes(path.GetView(), MAX_CATALOG_BYTES, data, inoutFailure) == false)
    {
        return false;
    }
    outDigest = Hash(GetSpan(data));
    return true;
}

/// Candidate produced by prepare_import, still in document form.
struct PreparedImport final
{
    Document Level;
    Document Catalog;
    bool HasPublishedCatalog = false;
    Digest PublishedCatalog;
};

/// The namespace of a previous import with the same source origin, if any.
[[nodiscard]] bool FindPreviousNamespace(std::string_view root,
                                         const Document& catalog,
                                         std::string_view origin,
                                         Uuid& inoutSpace,
                                         Failure& inoutFailure) noexcept
{
    const JsonTree& tree = catalog.Tree;
    const JsonIndex resources = tree.GetMember(catalog.Root, "resources");
    for (usize i = 0; i < tree.GetCount(resources); ++i)
    {
        const std::string_view identity = tree.GetKey(resources, i);
        const JsonIndex record = tree.GetValue(resources, i);
        if (tree.IsString(tree.GetMember(record, "kind"), "scene") == false ||
            IsResourceIdText(identity, "scene") == false)
        {
            continue;
        }
        const JsonIndex path = tree.GetMember(record, "path");
        String native;
        Document previous;
        if (tree.GetKind(path) != JsonKind::String)
        {
            return inoutFailure.FailInvalid("Invalid artifact path");
        }
        if (ResolveInside(root, tree.GetText(path), native, inoutFailure) == false ||
            LoadDocument(native.GetView(), MAX_METADATA_BYTES, previous, inoutFailure) == false)
        {
            return false;
        }
        if (previous.Tree.IsString(previous.Tree.GetMember(previous.Root, "source_origin"), origin) == true)
        {
            (void)ParseUuid(identity.substr(identity.find('/') + 1), inoutSpace);
            return true;
        }
    }
    return true;
}

/// kind + "/" + str(uuid5(namespace, kind + "/" + str(part))).
[[nodiscard]] bool MakeLogicalId(const Uuid& space, std::string_view kind, usize part, String& outIdentity) noexcept
{
    Array<uint8> name;
    ByteWriter writer(name);
    writer.Append(kind);
    writer.Append('/');
    const usize prefixSize = name.GetSize();
    AppendInteger(writer, static_cast<int64>(part));
    Uuid value;
    return writer.IsOk() == true && MakeNameUuid(space, GetView(name), value) == true &&
           FormatUuid(value, GetView(name).substr(0, prefixSize), outIdentity) == true;
}

/// The material description of one bundle material record.
[[nodiscard]] bool WriteMaterialDescription(const BundleMaterial& material,
                                            const Array<String>& textures,
                                            Array<uint8>& outBytes,
                                            Failure& inoutFailure) noexcept
{
    // Version 1 records (24 bytes) imply the sampler address; version 2 records
    // (pbrt-best-effort-v1) carry it explicitly after the texture index.
    const bool isVersion6 = material.Bytes.size() == MATERIAL_RECORD_V6_BYTES;
    const bool isVersion5 = material.Bytes.size() == MATERIAL_RECORD_V5_BYTES || isVersion6 == true;
    const bool isVersion3 = material.Bytes.size() == MATERIAL_RECORD_V3_BYTES || isVersion5 == true;
    const bool isVersion2 = material.Bytes.size() == MATERIAL_RECORD_V2_BYTES || isVersion3 == true;
    if (material.Bytes.size() != MATERIAL_RECORD_BYTES && isVersion2 == false)
    {
        return inoutFailure.Fail(AssetErrorCode::UnsupportedScene, "unpack requires a buffer of 24 bytes");
    }
    const uint32 texture = material.Texture;
    const bool hasTexture = texture != format::NO_TEXTURE;
    const bool isRepeat = isVersion2 == true ? DecodeUint32(material.Bytes.data() + 4) == format::ADDRESS_REPEAT
                                             : 0 < texture && texture < textures.GetSize();
    const uint8* values = material.Bytes.data() + (isVersion2 == true ? 8 : 4);
    JsonTree tree;
    const JsonIndex root = tree.AddObject();
    tree.SetMember(root, "texture", tree.AddString(hasTexture == true ? textures[texture].GetView() : ""));
    tree.SetMember(root, "metallic", tree.AddFloat(DecodeFloat(values)));
    tree.SetMember(root, "roughness", tree.AddFloat(DecodeFloat(values + 4)));
    const JsonIndex emission = tree.AddArray();
    for (usize channel = 0; channel < 3; ++channel)
    {
        tree.Append(emission, tree.AddFloat(DecodeFloat(values + 8 + channel * 4)));
    }
    tree.SetMember(root, "emission", emission);
    tree.SetMember(root, "address", tree.AddString(isRepeat == true ? "repeat" : "clamp"));
    if (isVersion3 == true)
    {
        const uint8* extra = material.Bytes.data() + MATERIAL_RECORD_V2_BYTES;
        tree.SetMember(root, "emission_flags", tree.AddInteger(DecodeUint32(extra)));
        const uint64 sourceId = uint64{DecodeUint32(extra + sizeof(uint32))} |
                                (uint64{DecodeUint32(extra + SOURCE_ID_WORDS * sizeof(uint32))} << 32);
        tree.SetMember(root, "area_source_id", AddSourceId(tree, sourceId));
        tree.SetMember(root, "emission_power", tree.AddFloat(DecodeFloat(extra + sizeof(uint32) + sizeof(uint64))));
    }
    if (isVersion5 == true)
    {
        const uint8* alpha = material.Bytes.data() + offsetof(format::MaterialRecordV5, AlphaMode);
        tree.SetMember(root,
                       "alpha_mode",
                       tree.AddString(DecodeUint32(alpha) == format::ALPHA_MASKED ? "masked" : "opaque"));
        tree.SetMember(root, "alpha_cutoff", tree.AddFloat(DecodeFloat(alpha + sizeof(uint32))));
    }
    if (isVersion6 == true)
    {
        const uint32 surface = material.SurfaceTexture;
        const uint32 emissionMap = material.EmissionTexture;
        tree.SetMember(root,
                       "surface_texture",
                       tree.AddString(surface != format::NO_TEXTURE ? textures[surface].GetView() : ""));
        tree.SetMember(root,
                       "emission_texture",
                       tree.AddString(emissionMap != format::NO_TEXTURE ? textures[emissionMap].GetView() : ""));
        tree.SetMember(
            root,
            "normal_scale",
            tree.AddFloat(DecodeFloat(material.Bytes.data() + offsetof(format::MaterialRecordV6, NormalScale))));
    }
    return WriteCanonical(tree, root, outBytes, inoutFailure);
}

/// Store every bundle part and the scene manifest; returns the logical IDs.
struct StoredParts final
{
    Array<String> Textures;
    Array<String> Materials;
    Array<String> Meshes;
    String License;
    String Scene;
    String Lighting;
};

[[nodiscard]] bool StoreParts(std::string_view root,
                              const BundleParts& parts,
                              const CookResult& cooked,
                              Document& inoutManifest,
                              std::string_view origin,
                              const Uuid& space,
                              const BuildRecord& build,
                              Document& inoutCatalog,
                              StoredParts& outStored,
                              Failure& inoutFailure) noexcept
{
    for (usize i = 0; i < parts.Textures.GetSize(); ++i)
    {
        String identity;
        if (MakeLogicalId(space, "texture", i, identity) == false)
        {
            return inoutFailure.FailOutOfMemory();
        }
        if (StoreArtifact(root,
                          inoutCatalog,
                          "texture",
                          parts.Textures[i],
                          {},
                          build,
                          identity.GetView(),
                          inoutFailure) == false)
        {
            return false;
        }
        if (outStored.Textures.TryAdd(std::move(identity)) == false)
        {
            return inoutFailure.FailOutOfMemory();
        }
    }
    for (usize i = 0; i < parts.Materials.GetSize(); ++i)
    {
        const uint32 texture = parts.Materials[i].Texture;
        Array<uint8> bytes;
        String identity;
        if (WriteMaterialDescription(parts.Materials[i], outStored.Textures, bytes, inoutFailure) == false)
        {
            return false;
        }
        if (MakeLogicalId(space, "material", i, identity) == false)
        {
            return inoutFailure.FailOutOfMemory();
        }
        const auto getTexture = [&outStored](uint32 index) {
            return index != format::NO_TEXTURE ? outStored.Textures[index].GetView() : std::string_view();
        };
        Array<std::string_view> dependencies;
        if (AddMaterialDependency(dependencies, getTexture(texture)) == false ||
            AddMaterialDependency(dependencies, getTexture(parts.Materials[i].SurfaceTexture)) == false ||
            AddMaterialDependency(dependencies, getTexture(parts.Materials[i].EmissionTexture)) == false)
        {
            return inoutFailure.FailOutOfMemory();
        }
        if (StoreArtifact(root,
                          inoutCatalog,
                          "material",
                          GetSpan(bytes),
                          {dependencies.GetData(), dependencies.GetSize()},
                          build,
                          identity.GetView(),
                          inoutFailure) == false)
        {
            return false;
        }
        if (outStored.Materials.TryAdd(std::move(identity)) == false)
        {
            return inoutFailure.FailOutOfMemory();
        }
    }
    for (usize i = 0; i < parts.Meshes.GetSize(); ++i)
    {
        String identity;
        if (MakeLogicalId(space, "mesh", i, identity) == false)
        {
            return inoutFailure.FailOutOfMemory();
        }
        if (StoreArtifact(root, inoutCatalog, "mesh", parts.Meshes[i], {}, build, identity.GetView(), inoutFailure) ==
            false)
        {
            return false;
        }
        if (outStored.Meshes.TryAdd(std::move(identity)) == false)
        {
            return inoutFailure.FailOutOfMemory();
        }
    }
    if (cooked.License.GetSize() > MAX_METADATA_BYTES)
    {
        return inoutFailure.Fail(AssetErrorCode::AssetCorrupt, "source file byte limit");
    }
    if (MakeLogicalId(space, "license", 0, outStored.License) == false)
    {
        return inoutFailure.FailOutOfMemory();
    }
    if (StoreArtifact(root,
                      inoutCatalog,
                      "license",
                      GetSpan(cooked.License),
                      {},
                      build,
                      outStored.License.GetView(),
                      inoutFailure) == false)
    {
        return false;
    }
    if (format::HasLightingRecords(parts.Version) == true)
    {
        if (MakeLogicalId(space, "lighting", 0, outStored.Lighting) == false)
        {
            return inoutFailure.FailOutOfMemory();
        }
        if (StoreArtifact(root,
                          inoutCatalog,
                          "lighting",
                          {parts.Lighting.GetDataOrNull(), parts.Lighting.GetSize()},
                          {},
                          build,
                          outStored.Lighting.GetView(),
                          inoutFailure) == false)
        {
            return false;
        }
    }
    JsonTree& manifest = inoutManifest.Tree;
    // Optional source provenance; geometry and lighting reopen from the closure.
    manifest.SetMember(inoutManifest.Root, "source_origin", manifest.AddString(origin));
    const JsonIndex templateRoot = manifest.AddObject();
    const JsonIndex templateObjects = manifest.AddArray();
    for (const BundleObject& object : parts.Objects)
    {
        const JsonIndex item = manifest.AddObject();
        manifest.SetMember(item, "mesh", manifest.AddString(outStored.Meshes[object.Mesh].GetView()));
        manifest.SetMember(item, "material", manifest.AddString(outStored.Materials[object.Material].GetView()));
        manifest.SetMember(item, "transform", AddTransform(manifest, object.Transform));
        manifest.Append(templateObjects, item);
    }
    manifest.SetMember(templateRoot, "objects", templateObjects);
    manifest.SetMember(templateRoot, "camera", AddCamera(manifest, parts.Camera));
    manifest.SetMember(inoutManifest.Root, "template", templateRoot);
    if (format::HasLightingRecords(parts.Version) == true)
    {
        manifest.SetMember(inoutManifest.Root, "lighting", manifest.AddString(outStored.Lighting.GetView()));
    }
    Array<uint8> manifestBytes;
    if (WriteCanonical(manifest, inoutManifest.Root, manifestBytes, inoutFailure) == false)
    {
        return false;
    }
    // sorted(set(meshes + materials + [license_id])).
    Array<std::string_view> sceneDependencies;
    bool isOk = AddSortedUnique(sceneDependencies, outStored.License.GetView());
    if (format::HasLightingRecords(parts.Version) == true)
    {
        isOk = isOk == true && AddSortedUnique(sceneDependencies, outStored.Lighting.GetView()) == true;
    }
    for (const String& identity : outStored.Meshes)
    {
        isOk = isOk && AddSortedUnique(sceneDependencies, identity.GetView());
    }
    for (const String& identity : outStored.Materials)
    {
        isOk = isOk && AddSortedUnique(sceneDependencies, identity.GetView());
    }
    if (isOk == false || FormatUuid(space, "scene/", outStored.Scene) == false)
    {
        return inoutFailure.FailOutOfMemory();
    }
    return StoreArtifact(root,
                         inoutCatalog,
                         "scene",
                         GetSpan(manifestBytes),
                         std::span<const std::string_view>(sceneDependencies.GetData(), sceneDependencies.GetSize()),
                         build,
                         outStored.Scene.GetView(),
                         inoutFailure);
}

/// Append the imported objects, camera and provenance to the candidate level.
[[nodiscard]] bool MergeCandidate(const BundleParts& parts,
                                  const StoredParts& stored,
                                  const AssetOptions& options,
                                  Document& inoutLevel,
                                  Failure& inoutFailure) noexcept
{
    JsonTree& tree = inoutLevel.Tree;
    const JsonIndex objects = tree.GetMember(inoutLevel.Root, "objects");
    for (usize i = 0; i < parts.Objects.GetSize(); ++i)
    {
        const BundleObject& object = parts.Objects[i];
        Uuid identity;
        String identityText;
        if (NextUuid(options, identity, inoutFailure) == false)
        {
            return false;
        }
        Array<uint8> name;
        ByteWriter nameWriter(name);
        nameWriter.Append("Imported object ");
        AppendInteger(nameWriter, static_cast<int64>(i) + 1);
        if (FormatUuid(identity, "", identityText) == false || nameWriter.IsOk() == false)
        {
            return inoutFailure.FailOutOfMemory();
        }
        const JsonIndex item = tree.AddObject();
        tree.SetMember(item, "id", tree.AddString(identityText.GetView()));
        tree.SetMember(item, "name", tree.AddString(GetView(name)));
        tree.SetMember(item, "parent", tree.AddString(""));
        tree.SetMember(item, "mesh", tree.AddString(stored.Meshes[object.Mesh].GetView()));
        tree.SetMember(item, "material", tree.AddString(stored.Materials[object.Material].GetView()));
        tree.SetMember(item, "transform", AddTransform(tree, object.Transform));
        tree.Append(objects, item);
    }
    tree.SetMember(inoutLevel.Root, "camera", AddCamera(tree, parts.Camera));
    const JsonIndex imports = tree.GetMember(inoutLevel.Root, "imports");
    bool isImported = false;
    for (usize i = 0; i < tree.GetCount(imports); ++i)
    {
        isImported = isImported || tree.IsString(tree.GetItem(imports, i), stored.Scene.GetView());
    }
    if (isImported == false)
    {
        tree.Append(imports, tree.AddString(stored.Scene.GetView()));
    }
    return tree.IsOk() == true ? true : inoutFailure.FailOutOfMemory();
}

/// Licence text recorded when a best-effort source directory has none.
constexpr std::string_view MISSING_LICENSE_TEXT = "No LICENSE.txt or LICENSE.md was found beside the imported scene.\n";

/// Cooked outputs of one import and the profile that produced them.
struct RoutedCook final
{
    CookResult Cooked;
    /// Importer profile: pbrt-reference-v1 or a best-effort profile (pbrt, glTF or OBJ).
    std::string_view Profile = IMPORTER_NAME;
    /// Best-effort import report; empty for the reference profile.
    Array<uint8> Report;
    /// Why the reference profile rejected scene-v4.pbrt before the fallback; empty otherwise.
    String ReferenceRejection;
};

/// Add "CODE: message" for every warning diagnostic of a best-effort report.
[[nodiscard]] JsonIndex AddReportApproximations(JsonTree& inoutTree, const Document& report) noexcept
{
    const JsonIndex approximations = inoutTree.AddArray();
    const JsonTree& source = report.Tree;
    const JsonIndex diagnostics = source.GetMember(report.Root, "diagnostics");
    for (usize i = 0; diagnostics != NO_JSON_NODE && i < source.GetCount(diagnostics); ++i)
    {
        const JsonIndex item = source.GetItem(diagnostics, i);
        const JsonIndex code = source.GetMember(item, "code");
        const JsonIndex message = source.GetMember(item, "message");
        if (source.IsString(source.GetMember(item, "severity"), "warning") == false || code == NO_JSON_NODE ||
            message == NO_JSON_NODE)
        {
            continue;
        }
        String text;
        if (Concatenate(text, {source.GetText(code), ": ", source.GetText(message)}) == false)
        {
            return NO_JSON_NODE;
        }
        inoutTree.Append(approximations, inoutTree.AddString(text.GetView()));
    }
    return approximations;
}

/// Human-readable source format recorded in a best-effort manifest.
[[nodiscard]] std::string_view DescribeSourceFormat(SceneSourceFormat format) noexcept
{
    switch (format)
    {
        case SceneSourceFormat::Gltf:
            return "glTF 2.0 (best effort)";
        case SceneSourceFormat::Obj:
            return "Wavefront OBJ (best effort)";
        case SceneSourceFormat::Pbrt:
        case SceneSourceFormat::Unknown:
            break;
    }
    return "PBRT v4 (best effort)";
}

/// Cook source/entry with the best-effort importer its content selects (pbrt-v4,
/// glTF/GLB or OBJ) into reference-shaped outputs: the manifest carries the input
/// digests, approximations and the full import report.
[[nodiscard]] bool CookBestEffortSource(std::string_view source,
                                        std::string_view entry,
                                        bool isDryRun,
                                        const AssetOptions& options,
                                        RoutedCook& inoutCook,
                                        Failure& inoutFailure) noexcept
{
    if (entry.empty() == true || entry.find('/') != std::string_view::npos ||
        entry.find('\\') != std::string_view::npos || entry == "." || entry == "..")
    {
        return inoutFailure.Fail(AssetErrorCode::UnsupportedScene,
                                 "scene entry must be a file name in the source directory");
    }
    String entryPath;
    if (Concatenate(entryPath, {source, "/", entry}) == false)
    {
        return inoutFailure.FailOutOfMemory();
    }
    BestEffortOptions importOptions;
    importOptions.IsDryRun = isDryRun;
    BestEffortResult imported;
    CookError cookError;
    if (ImportSceneBestEffort(entryPath.GetView(), importOptions, imported, cookError, options.CancelOrNull) == false)
    {
        return MapCookFailure(cookError, inoutFailure);
    }
    inoutCook.Profile = imported.Profile;
    Document report;
    if (ParseDocument(GetSpan(imported.Report), AssetErrorCode::UnsupportedScene, report, inoutFailure) == false)
    {
        return false;
    }
    JsonTree tree;
    const JsonIndex root = tree.AddObject();
    tree.SetMember(root, "version", tree.AddInteger(1));
    tree.SetMember(root, "source", tree.AddString(entry));
    tree.SetMember(root, "author", tree.AddString("See retained LICENSE.txt"));
    tree.SetMember(root, "license", tree.AddString("See retained LICENSE.txt"));
    tree.SetMember(root, "source_format", tree.AddString(DescribeSourceFormat(imported.Format)));
    tree.SetMember(root, "importer", tree.AddString(imported.Profile));
    const JsonIndex inputs = tree.AddObject();
    // Digests come from the importer's own reads, so they describe exactly the
    // bytes that produced the bundle even if a source changes mid-import.
    for (usize i = 0; i < imported.Inputs.GetSize(); ++i)
    {
        HexDigest digest;
        FormatHexDigest(imported.InputDigests[i], digest);
        tree.SetMember(inputs, imported.Inputs[i].GetView(), tree.AddString(digest.View()));
    }
    tree.SetMember(root, "inputs", inputs);
    HexDigest bundleDigest;
    ComputeHexDigest(GetSpan(imported.Bundle), bundleDigest);
    tree.SetMember(root, "bundle_sha256", tree.AddString(bundleDigest.View()));
    const JsonIndex approximations = AddReportApproximations(tree, report);
    if (approximations == NO_JSON_NODE)
    {
        return inoutFailure.FailOutOfMemory();
    }
    tree.SetMember(root, "approximations", approximations);
    tree.SetMember(root, "import_report", tree.CopyFrom(report.Tree, report.Root));
    if (tree.IsOk() == false || WriteCanonical(tree, root, inoutCook.Cooked.Manifest, inoutFailure) == false)
    {
        return tree.IsOk() == false ? inoutFailure.FailOutOfMemory() : false;
    }
    inoutCook.Cooked.Bundle = std::move(imported.Bundle);
    inoutCook.Cooked.License = std::move(imported.License);
    if (inoutCook.Cooked.License.IsEmpty() == true && inoutCook.Cooked.License.TryAddRange(std::span<const uint8>(
                                                          reinterpret_cast<const uint8*>(MISSING_LICENSE_TEXT.data()),
                                                          MISSING_LICENSE_TEXT.size())) == false)
    {
        return inoutFailure.FailOutOfMemory();
    }
    inoutCook.Report = std::move(imported.Report);
    return true;
}

/// Pick the importer for a source directory (see AssetOptions::SourceEntry) and cook it.
[[nodiscard]] bool CookRoutedSource(std::string_view source,
                                    const AssetOptions& options,
                                    bool isDryRun,
                                    RoutedCook& outCook,
                                    Failure& inoutFailure) noexcept
{
    const std::string_view entry = options.SourceEntry;
    if (options.CookOrNull != nullptr || entry.empty() == true || entry == REFERENCE_ENTRY)
    {
        CookError cookError;
        const bool isCooked = options.CookOrNull != nullptr
                                  ? options.CookOrNull(source, outCook.Cooked, cookError, options.CancelOrNull)
                                  : Cook(source, outCook.Cooked, cookError, options.CancelOrNull);
        if (isCooked == true)
        {
            return true;
        }
        // Only a profile rejection of scene-v4.pbrt falls back; cancellation, I/O
        // failures and an explicit reference request keep their error.
        if (options.CookOrNull != nullptr || entry.empty() == true || cookError.Result != Status::Invalid)
        {
            return MapCookFailure(cookError, inoutFailure);
        }
        if (outCook.ReferenceRejection.TryAssign(cookError.Message.View()) != foundation::StringStatus::Ok)
        {
            return inoutFailure.FailOutOfMemory();
        }
    }
    return CookBestEffortSource(source, entry, isDryRun, options, outCook, inoutFailure);
}

/// SHA-256 of the canonical `inputs` object of a cooked manifest: the token a
/// PreviewImport response carries as `source_digest` and a confirmed import checks.
[[nodiscard]] bool
ComputeSourceDigest(const JsonTree& manifest, JsonIndex root, HexDigest& outHex, Failure& inoutFailure) noexcept
{
    const JsonIndex inputs = manifest.GetMember(root, "inputs");
    Array<uint8> bytes;
    if (inputs == NO_JSON_NODE || manifest.GetKind(inputs) != JsonKind::Object)
    {
        return inoutFailure.FailInvalid("Invalid manifest inputs");
    }
    if (WriteCanonical(manifest, inputs, bytes, inoutFailure) == false)
    {
        return false;
    }
    ComputeHexDigest(GetSpan(bytes), outHex);
    return true;
}

[[nodiscard]] bool PrepareImportDocuments(std::string_view root,
                                          std::string_view source,
                                          const Document& level,
                                          const AssetOptions& options,
                                          PreparedImport& outPrepared,
                                          Failure& inoutFailure) noexcept
{
    if (ValidateLevelNode(level.Tree, level.Root, inoutFailure) == false)
    {
        return false;
    }
    if (IsCancelled(options) == true)
    {
        return inoutFailure.Fail(AssetErrorCode::InstallCancelled, "Scene import cancelled");
    }
    RoutedCook routed;
    if (CookRoutedSource(source, options, false, routed, inoutFailure) == false)
    {
        return false;
    }
    const CookResult& cooked = routed.Cooked;
    BundleParts parts;
    Document manifest;
    if (SplitBundle(GetSpan(cooked.Bundle), parts, inoutFailure) == false ||
        ParseDocument(GetSpan(cooked.Manifest), AssetErrorCode::UnsupportedScene, manifest, inoutFailure) == false)
    {
        return false;
    }
    if (options.ExpectedSourceDigest.empty() == false)
    {
        HexDigest current;
        if (ComputeSourceDigest(manifest.Tree, manifest.Root, current, inoutFailure) == false)
        {
            return false;
        }
        if (current.View() != options.ExpectedSourceDigest)
        {
            return inoutFailure.Fail(AssetErrorCode::Conflict,
                                     "Source changed since the import preview; preview it again");
        }
    }
    // Source read consistency is checked before the catalog commit point.
    if (AreSourceInputsUnchanged(source,
                                 manifest.Tree,
                                 manifest.Tree.GetMember(manifest.Root, "inputs"),
                                 "Source changed during conversion",
                                 inoutFailure) == false)
    {
        return false;
    }
    BuildRecord build;
    JsonTree& buildTree = build.Tree;
    build.Root = buildTree.AddObject();
    buildTree.SetMember(build.Root, "importer", buildTree.AddString(routed.Profile));
    if (routed.Profile == IMPORTER_NAME)
    {
        buildTree.SetMember(build.Root, "importer_sha256", buildTree.AddString(IMPORTER_SHA256));
    }
    buildTree.SetMember(build.Root, "publisher_sha256", buildTree.AddString(PUBLISHER_SHA256));
    buildTree.SetMember(build.Root,
                        "source_inputs",
                        buildTree.CopyFrom(manifest.Tree, manifest.Tree.GetMember(manifest.Root, "inputs")));
    const JsonIndex settings = buildTree.AddObject();
    buildTree.SetMember(settings, "profile", buildTree.AddString(LEVEL_PROFILE));
    buildTree.SetMember(build.Root, "settings", settings);
    buildTree.SetMember(build.Root, "schema", buildTree.AddInteger(1));
    buildTree.SetMember(build.Root,
                        "target",
                        buildTree.AddString(parts.Version == format::VERSION_6   ? SURFACE_MAP_BUILD_TARGET
                                            : parts.Version == format::VERSION_5 ? IMPORT_PROFILE_BUILD_TARGET
                                            : parts.Version == format::VERSION_4 ? INDEXED_LIGHTING_BUILD_TARGET
                                            : parts.Version == format::VERSION_3 ? LIGHTING_BUILD_TARGET
                                                                                 : BUILD_TARGET));
    if (buildTree.IsOk() == false)
    {
        return inoutFailure.FailOutOfMemory();
    }
    // candidate = json.loads(canonical(level)).
    Array<uint8> levelBytes;
    if (WriteCanonical(level.Tree, level.Root, levelBytes, inoutFailure) == false ||
        ParseDocument(GetSpan(levelBytes), AssetErrorCode::InvalidScene, outPrepared.Level, inoutFailure) == false)
    {
        return false;
    }
    LockScope writer;
    Document& current = outPrepared.Catalog;
    if (writer.Enter(root, LockMode::Exclusive, inoutFailure) == false ||
        ReadCatalogDocument(root, current, inoutFailure) == false ||
        ReadPublishedCatalogDigest(root, outPrepared.HasPublishedCatalog, outPrepared.PublishedCatalog, inoutFailure) ==
            false)
    {
        return false;
    }
    Uuid space;
    String origin;
    StoredParts stored;
    if (NextUuid(options, space, inoutFailure) == false ||
        ResolveOrigin(source, options, origin, inoutFailure) == false ||
        FindPreviousNamespace(root, current, origin.GetView(), space, inoutFailure) == false ||
        StoreParts(root, parts, cooked, manifest, origin.GetView(), space, build, current, stored, inoutFailure) ==
            false ||
        MergeCandidate(parts, stored, options, outPrepared.Level, inoutFailure) == false)
    {
        return false;
    }
    Document closure;
    Array<uint8> bundle;
    // Validate render admission as well as durable byte integrity.
    if (ComputeClosure(root, outPrepared.Level.Tree, outPrepared.Level.Root, &current, closure, inoutFailure) ==
            false ||
        AssembleBundle(root, outPrepared.Level.Tree, outPrepared.Level.Root, &current, bundle, inoutFailure) == false)
    {
        return false;
    }
    if (IsCancelled(options) == true)
    {
        return inoutFailure.Fail(AssetErrorCode::InstallCancelled, "Scene import cancelled");
    }
    if (current.Tree.GetCount(current.Tree.GetMember(current.Root, "resources")) > MAX_CATALOG_RESOURCES)
    {
        return inoutFailure.FailInvalid("Project catalog resource limit");
    }
    Array<uint8> catalogBytes;
    if (WriteCanonical(current.Tree, current.Root, catalogBytes, inoutFailure) == false)
    {
        return false;
    }
    if (catalogBytes.GetSize() > MAX_CATALOG_BYTES)
    {
        return inoutFailure.FailInvalid("Project catalog exceeds 64 MiB (MAX_CATALOG_BYTES)");
    }
    if (AreSourceInputsUnchanged(source,
                                 manifest.Tree,
                                 manifest.Tree.GetMember(manifest.Root, "inputs"),
                                 "Source changed before catalog publication",
                                 inoutFailure) == false)
    {
        return false;
    }
    if (IsCancelled(options) == true)
    {
        return inoutFailure.Fail(AssetErrorCode::InstallCancelled, "Scene import cancelled");
    }
    return true;
}

/// _validate_import, under an already held writer lock; returns canonical catalog bytes.
[[nodiscard]] bool ValidateImport(std::string_view root,
                                  const Document& level,
                                  const Document& catalog,
                                  const Digest* publishedOrNull,
                                  std::string_view source,
                                  const AssetOptions& options,
                                  Array<uint8>& outCatalog,
                                  Failure& inoutFailure) noexcept
{
    bool hasCatalog = false;
    Digest actual;
    if (ReadPublishedCatalogDigest(root, hasCatalog, actual, inoutFailure) == false)
    {
        return false;
    }
    const bool isUnchanged =
        hasCatalog == (publishedOrNull != nullptr) && (hasCatalog == false || (actual == *publishedOrNull) == true);
    if (isUnchanged == false)
    {
        return inoutFailure.Fail(AssetErrorCode::Conflict,
                                 "Project asset catalog changed while preparing the candidate");
    }
    Document closure;
    Array<uint8> bundle;
    String origin;
    if (ComputeClosure(root, level.Tree, level.Root, &catalog, closure, inoutFailure) == false ||
        AssembleBundle(root, level.Tree, level.Root, &catalog, bundle, inoutFailure) == false ||
        ResolveOrigin(source, options, origin, inoutFailure) == false)
    {
        return false;
    }
    const JsonTree& catalogTree = catalog.Tree;
    const JsonIndex resources = catalogTree.GetMember(catalog.Root, "resources");
    const JsonIndex imports = level.Tree.GetMember(level.Root, "imports");
    for (usize i = 0; i < level.Tree.GetCount(imports); ++i)
    {
        const JsonIndex record = catalogTree.GetMember(resources, level.Tree.GetText(level.Tree.GetItem(imports, i)));
        const JsonIndex path = catalogTree.GetMember(record, "path");
        String native;
        Document manifest;
        if (path == NO_JSON_NODE || catalogTree.GetKind(path) != JsonKind::String)
        {
            return inoutFailure.FailInvalid("Missing imported scene record");
        }
        if (ResolveInside(root, catalogTree.GetText(path), native, inoutFailure) == false ||
            LoadDocument(native.GetView(), MAX_METADATA_BYTES, manifest, inoutFailure) == false)
        {
            return false;
        }
        const bool isSameOrigin =
            manifest.Tree.IsString(manifest.Tree.GetMember(manifest.Root, "source_origin"), origin.GetView());
        if (isSameOrigin == true && AreSourceInputsUnchanged(source,
                                                             manifest.Tree,
                                                             manifest.Tree.GetMember(manifest.Root, "inputs"),
                                                             "Source changed before catalog publication",
                                                             inoutFailure) == false)
        {
            return false;
        }
    }
    if (WriteCanonical(catalogTree, catalog.Root, outCatalog, inoutFailure) == false)
    {
        return false;
    }
    if (outCatalog.GetSize() > MAX_CATALOG_BYTES)
    {
        return inoutFailure.FailInvalid("Project catalog exceeds 64 MiB (MAX_CATALOG_BYTES)");
    }
    if (IsCancelled(options) == true)
    {
        return inoutFailure.Fail(AssetErrorCode::InstallCancelled, "Scene import cancelled");
    }
    return true;
}

/// commit_import.
[[nodiscard]] bool CommitImportDocuments(std::string_view root,
                                         const Document& level,
                                         const Document& catalog,
                                         const Digest* publishedOrNull,
                                         std::string_view source,
                                         const AssetOptions& options,
                                         Failure& inoutFailure) noexcept
{
    LockScope writer;
    Array<uint8> data;
    String path;
    return writer.Enter(root, LockMode::Exclusive, inoutFailure) == true &&
           ValidateImport(root, level, catalog, publishedOrNull, source, options, data, inoutFailure) == true &&
           ResolveInside(root, CATALOG_PATH, path, inoutFailure) == true &&
           PublishBytes(path.GetView(), GetSpan(data), PublishMode::Replace, options.CancelOrNull, inoutFailure) ==
               true;
}

// ---------------------------------------------------------------------------
// Saved levels and releases.

/// open_level(root, relative).
[[nodiscard]] bool
OpenLevelDocument(std::string_view root, std::string_view relative, Document& outLevel, Failure& inoutFailure) noexcept
{
    LockScope lock;
    String path;
    Document closure;
    Array<uint8> bundle;
    return lock.Enter(root, LockMode::Shared, inoutFailure) == true &&
           ResolveInside(root, relative, path, inoutFailure) == true &&
           LoadDocument(path.GetView(), MAX_METADATA_BYTES, outLevel, inoutFailure) == true &&
           ValidateLevelNode(outLevel.Tree, outLevel.Root, inoutFailure) == true &&
           ComputeClosure(root, outLevel.Tree, outLevel.Root, nullptr, closure, inoutFailure) == true &&
           AssembleBundle(root, outLevel.Tree, outLevel.Root, nullptr, bundle, inoutFailure) == true;
}

/// save_level(root, relative, level, expected).
[[nodiscard]] bool SaveLevelDocument(std::string_view root,
                                     std::string_view relative,
                                     const Document& level,
                                     const Array<uint8>* expectedOrNull,
                                     Array<uint8>& outSaved,
                                     Failure& inoutFailure) noexcept
{
    String path;
    if (ResolveInside(root, relative, path, inoutFailure) == false)
    {
        return false;
    }
    if (GetSuffix(path.GetView()) != ".lulevel")
    {
        return inoutFailure.FailInvalid("Levels use the .lulevel extension");
    }
    LockScope writer;
    Document closure;
    Array<uint8> bundle;
    if (writer.Enter(root, LockMode::Exclusive, inoutFailure) == false ||
        ComputeClosure(root, level.Tree, level.Root, nullptr, closure, inoutFailure) == false ||
        AssembleBundle(root, level.Tree, level.Root, nullptr, bundle, inoutFailure) == false)
    {
        return false;
    }
    const bool hasPrevious = DoesPathExist(path.GetView());
    Array<uint8> previous;
    if (hasPrevious == true && ReadBytes(path.GetView(), MAX_METADATA_BYTES, previous, inoutFailure) == false)
    {
        return false;
    }
    const bool isExpected = hasPrevious == (expectedOrNull != nullptr) &&
                            (hasPrevious == false || AreBytesEqual(GetSpan(previous), GetSpan(*expectedOrNull)));
    if (isExpected == false)
    {
        return inoutFailure.Fail(AssetErrorCode::Conflict, "Saved level changed on disk");
    }
    if (WriteCanonical(level.Tree, level.Root, outSaved, inoutFailure) == false)
    {
        return false;
    }
    if (outSaved.GetSize() > MAX_METADATA_BYTES)
    {
        return inoutFailure.FailInvalid("Level document byte limit");
    }
    return PublishBytes(path.GetView(), GetSpan(outSaved), PublishMode::Replace, nullptr, inoutFailure);
}

/// Restore the catalog and level after a failed paired replacement.
[[nodiscard]] bool RollBackImport(std::string_view catalogPath,
                                  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                  std::string_view levelPath,
                                  std::string_view catalogStage,
                                  std::string_view levelStage,
                                  bool hasOldCatalog,
                                  bool isCatalogReplaced,
                                  bool isLevelReplaced) noexcept
{
    String backup;
    if (isLevelReplaced == true && (Concatenate(backup, {levelStage, "/previous-level"}) == false ||
                                    RenamePath(backup.GetView(), levelPath) != Status::Ok))
    {
        return false;
    }
    if (isCatalogReplaced == true)
    {
        if (hasOldCatalog == false)
        {
            if (RemoveFile(catalogPath) != Status::Ok)
            {
                return false;
            }
        }
        else if (Concatenate(backup, {catalogStage, "/previous-catalog"}) == false ||
                 RenamePath(backup.GetView(), catalogPath) != Status::Ok)
        {
            return false;
        }
    }
    return SyncDirectory(GetParent(catalogPath)) == Status::Ok && SyncDirectory(GetParent(levelPath)) == Status::Ok;
}

/// Bytes of a saved level that must not change during import_saved_level.
class LevelGuard final
{
public:
    LevelGuard(std::string_view path, const Array<uint8>& previous, Failure& inoutFailure) noexcept
        : mPath(path), mPrevious(previous), mFailure(inoutFailure)
    {
    }

    [[nodiscard]] bool IsUnchanged(std::string_view message) noexcept
    {
        Array<uint8> current;
        if (ReadBytes(mPath, MAX_METADATA_BYTES, current, mFailure) == false)
        {
            return false;
        }
        return AreBytesEqual(GetSpan(current), GetSpan(mPrevious)) == true
                   ? true
                   : mFailure.Fail(AssetErrorCode::Conflict, message);
    }

private:
    std::string_view mPath;
    const Array<uint8>& mPrevious;
    Failure& mFailure;
};

/// Stage the four files of a paired publication and swap them in, rolling back on failure.
[[nodiscard]] bool PublishPair(std::string_view catalogPath,
                               std::string_view levelPath,
                               const Array<uint8>& catalogData,
                               const Array<uint8>& levelData,
                               const Array<uint8>& previousLevel,
                               LevelGuard& inoutGuard,
                               Failure& inoutFailure) noexcept
{
    const bool hasOldCatalog = DoesPathExist(catalogPath);
    Array<uint8> oldCatalog;
    if (hasOldCatalog == true && ReadBytes(catalogPath, MAX_CATALOG_BYTES, oldCatalog, inoutFailure) == false)
    {
        return false;
    }
    String catalogStage;
    String levelStage;
    Status status = MakeTemporaryDirectory(GetParent(catalogPath), ".scene-import-", catalogStage);
    if (status != Status::Ok)
    {
        return inoutFailure.FailStatus(status, "Staging", GetParent(catalogPath));
    }
    String staged;
    const auto stage = [&](const String& directory, std::string_view name, const Array<uint8>& data) noexcept {
        return Concatenate(staged, {directory.GetView(), "/", name}) == true &&
               PublishBytes(staged.GetView(), GetSpan(data), PublishMode::Replace, nullptr, inoutFailure) == true;
    };
    // Each stage is on its destination filesystem; an unwritable level
    // directory fails before the published catalog is changed.
    status = MakeTemporaryDirectory(GetParent(levelPath), ".scene-import-", levelStage);
    bool isOk = status == Status::Ok ? true : inoutFailure.FailStatus(status, "Staging", GetParent(levelPath));
    isOk = isOk == true && stage(catalogStage, "next-catalog", catalogData) == true &&
           (hasOldCatalog == false || stage(catalogStage, "previous-catalog", oldCatalog) == true) &&
           stage(levelStage, "next-level", levelData) == true &&
           stage(levelStage, "previous-level", previousLevel) == true;
    bool isRetained = false;
    if (isOk == true)
    {
        bool isCatalogReplaced = false;
        bool isLevelReplaced = false;
        String next;
        isOk = inoutGuard.IsUnchanged("Saved level changed before import publication");
        if (isOk == true)
        {
            isCatalogReplaced = Concatenate(next, {catalogStage.GetView(), "/next-catalog"}) == true &&
                                RenamePath(next.GetView(), catalogPath) == Status::Ok;
            isOk =
                isCatalogReplaced == true ? true : inoutFailure.FailStatus(Status::IoError, "Replacing", catalogPath);
        }
        isOk = isOk == true && inoutGuard.IsUnchanged("Saved level changed before import publication") == true;
        if (isOk == true)
        {
            isLevelReplaced = Concatenate(next, {levelStage.GetView(), "/next-level"}) == true &&
                              RenamePath(next.GetView(), levelPath) == Status::Ok;
            isOk = isLevelReplaced == true ? true : inoutFailure.FailStatus(Status::IoError, "Replacing", levelPath);
        }
        if (isOk == true &&
            (SyncDirectory(GetParent(catalogPath)) != Status::Ok || SyncDirectory(GetParent(levelPath)) != Status::Ok))
        {
            isOk = inoutFailure.FailStatus(Status::IoError, "Synchronizing", GetParent(levelPath));
        }
        if (isOk == false && RollBackImport(catalogPath,
                                            levelPath,
                                            catalogStage.GetView(),
                                            levelStage.GetView(),
                                            hasOldCatalog,
                                            isCatalogReplaced,
                                            isLevelReplaced) == false)
        {
            isRetained = true;
            (void)inoutFailure.Replace(AssetErrorCode::RecoveryRequired,
                                       {"Import rollback failed; recovery files retained in ",
                                        catalogStage.GetView(),
                                        " and ",
                                        levelStage.GetView()});
        }
    }
    if (isRetained == false)
    {
        (void)RemoveTree(catalogStage.GetView());
        if (levelStage.IsEmpty() == false)
        {
            (void)RemoveTree(levelStage.GetView());
        }
    }
    return isOk;
}

/// import_saved_level(root, relative, source).
[[nodiscard]] bool ImportSavedLevelDocument(std::string_view root,
                                            // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                            std::string_view relative,
                                            std::string_view source,
                                            const AssetOptions& options,
                                            Array<uint8>& outLevel,
                                            Failure& inoutFailure) noexcept
{
    String path;
    if (ResolveInside(root, relative, path, inoutFailure) == false)
    {
        return false;
    }
    if (GetSuffix(path.GetView()) != ".lulevel")
    {
        return inoutFailure.FailInvalid("Levels use the .lulevel extension");
    }
    Array<uint8> previous;
    Document level;
    LevelGuard guard(path.GetView(), previous, inoutFailure);
    {
        LockScope reader;
        if (reader.Enter(root, LockMode::Shared, inoutFailure) == false ||
            ReadBytes(path.GetView(), MAX_METADATA_BYTES, previous, inoutFailure) == false ||
            OpenLevelDocument(root, relative, level, inoutFailure) == false ||
            guard.IsUnchanged("Saved level changed while reading") == false)
        {
            return false;
        }
    }
    PreparedImport prepared;
    if (PrepareImportDocuments(root, source, level, options, prepared, inoutFailure) == false)
    {
        return false;
    }
    // The paired publication itself is not cancellable.
    AssetOptions commitOptions = options;
    commitOptions.CancelOrNull = nullptr;
    LockScope writer;
    Array<uint8> catalogData;
    Array<uint8> levelData;
    String catalogPath;
    const Digest* publishedOrNull = prepared.HasPublishedCatalog == true ? &prepared.PublishedCatalog : nullptr;
    if (writer.Enter(root, LockMode::Exclusive, inoutFailure) == false ||
        ValidateImport(root,
                       prepared.Level,
                       prepared.Catalog,
                       publishedOrNull,
                       source,
                       commitOptions,
                       catalogData,
                       inoutFailure) == false ||
        guard.IsUnchanged("Saved level changed during import") == false ||
        WriteCanonical(prepared.Level.Tree, prepared.Level.Root, levelData, inoutFailure) == false)
    {
        return false;
    }
    if (levelData.GetSize() > MAX_METADATA_BYTES)
    {
        return inoutFailure.FailInvalid("Level document byte limit");
    }
    if (ResolveInside(root, CATALOG_PATH, catalogPath, inoutFailure) == false ||
        PublishPair(catalogPath.GetView(), path.GetView(), catalogData, levelData, previous, guard, inoutFailure) ==
            false)
    {
        return false;
    }
    outLevel = static_cast<Array<uint8>&&>(levelData);
    return true;
}

/// Copy one closure artifact into the release stage.
[[nodiscard]] bool StageRelease(std::string_view stage,
                                std::string_view relative,
                                std::span<const uint8> data,
                                Failure& inoutFailure) noexcept
{
    String path;
    return ResolveInside(stage, relative, path, inoutFailure) == true &&
           PublishBytes(path.GetView(), data, PublishMode::Replace, nullptr, inoutFailure) == true;
}

/// Write the release files below stage.
[[nodiscard]] bool WriteReleaseStage(std::string_view root,
                                     std::string_view stage,
                                     const Document& level,
                                     const Document& closure,
                                     Failure& inoutFailure) noexcept
{
    const JsonTree& resources = closure.Tree;
    for (usize i = 0; i < resources.GetCount(closure.Root); ++i)
    {
        const std::string_view relative =
            resources.GetText(resources.GetMember(resources.GetValue(closure.Root, i), "path"));
        String source;
        Array<uint8> data;
        if (ResolveInside(root, relative, source, inoutFailure) == false ||
            ReadBytes(source.GetView(), MAX_ASSET_BYTES, data, inoutFailure) == false ||
            StageRelease(stage, relative, GetSpan(data), inoutFailure) == false)
        {
            return false;
        }
    }
    JsonTree catalog;
    const JsonIndex catalogRoot = catalog.AddObject();
    catalog.SetMember(catalogRoot, "version", catalog.AddInteger(1));
    catalog.SetMember(catalogRoot, "resources", catalog.CopyFrom(resources, closure.Root));
    Array<uint8> bytes;
    if (WriteCanonical(catalog, catalogRoot, bytes, inoutFailure) == false ||
        StageRelease(stage, CATALOG_PATH, GetSpan(bytes), inoutFailure) == false ||
        StageRelease(stage, LOCK_PATH, {}, inoutFailure) == false ||
        WriteCanonical(level.Tree, level.Root, bytes, inoutFailure) == false ||
        StageRelease(stage, "scenes/main.lulevel", GetSpan(bytes), inoutFailure) == false ||
        AssembleBundle(root, level.Tree, level.Root, nullptr, bytes, inoutFailure) == false ||
        StageRelease(stage, "scene.luscn", GetSpan(bytes), inoutFailure) == false)
    {
        return false;
    }
    Document dump;
    return DumpDocument(stage, level.Tree, level.Root, nullptr, dump, inoutFailure) == true &&
           WriteCanonical(dump.Tree, dump.Root, bytes, inoutFailure) == true &&
           StageRelease(stage, "scene-release.json", GetSpan(bytes), inoutFailure) == true;
}

/// publish(root, level, destination).
[[nodiscard]] bool PublishRelease(std::string_view root,
                                  const Document& level,
                                  std::string_view requestedDestination,
                                  Document& outDump,
                                  Failure& inoutFailure) noexcept
{
    // Strip trailing separators so the staging parent is never the destination itself.
    std::string_view destination = requestedDestination;
    while (destination.size() > 1 && (destination.back() == '/' || destination.back() == '\\'))
    {
        destination.remove_suffix(1);
    }
    LockScope reader;
    if (reader.Enter(root, LockMode::Shared, inoutFailure) == false)
    {
        return false;
    }
    if (DoesPathExist(destination) == true)
    {
        return inoutFailure.Fail(AssetErrorCode::DestinationExists, "Release destination already exists");
    }
    Document closure;
    if (ComputeClosure(root, level.Tree, level.Root, nullptr, closure, inoutFailure) == false)
    {
        return false;
    }
    const std::string_view parent = GetParent(destination);
    Status status = MakeDirectories(parent);
    if (status != Status::Ok)
    {
        return inoutFailure.FailStatus(status, "Creating", parent);
    }
    String temporary;
    String stage;
    status = MakeTemporaryDirectory(parent, ".scene-release-", temporary);
    if (status != Status::Ok)
    {
        return inoutFailure.FailStatus(status, "Staging", parent);
    }
    bool isOk = Concatenate(stage, {temporary.GetView(), "/project"}) == true ? true : inoutFailure.FailOutOfMemory();
    if (isOk == true)
    {
        status = MakeDirectory(stage.GetView());
        isOk = status == Status::Ok ? true : inoutFailure.FailStatus(status, "Creating", stage.GetView());
    }
    isOk = isOk == true && WriteReleaseStage(root, stage.GetView(), level, closure, inoutFailure) == true;
    if (isOk == true)
    {
        status = RenamePath(stage.GetView(), destination);
        isOk = status == Status::Ok ? true : inoutFailure.FailStatus(status, "Renaming", destination);
    }
    (void)RemoveTree(temporary.GetView());
    return isOk == true && DumpDocument(destination, level.Tree, level.Root, nullptr, outDump, inoutFailure) == true;
}

// ---------------------------------------------------------------------------
// Public entry helpers.

[[nodiscard]] bool ParseInput(std::span<const uint8> bytes, Document& outDocument, Failure& inoutFailure) noexcept
{
    return ParseDocument(bytes, AssetErrorCode::InvalidScene, outDocument, inoutFailure);
}

/// Parse an optional candidate catalog; empty bytes select the published catalog.
[[nodiscard]] bool ParseOptionalCatalog(std::span<const uint8> bytes,
                                        Document& outCatalog,
                                        const Document*& outCatalogOrNull,
                                        Failure& inoutFailure) noexcept
{
    outCatalogOrNull = nullptr;
    if (bytes.empty() == true)
    {
        return true;
    }
    if (ParseInput(bytes, outCatalog, inoutFailure) == false)
    {
        return false;
    }
    const JsonTree& tree = outCatalog.Tree;
    if (tree.GetKind(outCatalog.Root) != JsonKind::Object ||
        tree.GetMember(outCatalog.Root, "resources") == NO_JSON_NODE ||
        tree.GetKind(tree.GetMember(outCatalog.Root, "resources")) != JsonKind::Object)
    {
        return inoutFailure.FailInvalid("Unsupported scene catalog");
    }
    outCatalogOrNull = &outCatalog;
    return true;
}

[[nodiscard]] bool
WriteLayout(const Document& document, DumpLayout layout, Array<uint8>& outBytes, Failure& inoutFailure) noexcept
{
    if (layout == DumpLayout::Canonical)
    {
        return WriteCanonical(document.Tree, document.Root, outBytes, inoutFailure);
    }
    outBytes.Clear();
    ByteWriter writer(outBytes);
    JsonStyle style;
    style.ItemSeparator = ", ";
    style.KeySeparator = ": ";
    style.Indent = layout == DumpLayout::Indented ? 2 : 0;
    if (WriteJson(document.Tree, document.Root, style, writer) == false)
    {
        return writer.IsOk() == true ? inoutFailure.FailInvalid("Out of range float values are not JSON compliant")
                                     : inoutFailure.FailOutOfMemory();
    }
    writer.Append('\n');
    return writer.IsOk() == true ? true : inoutFailure.FailOutOfMemory();
}
} // namespace

std::string_view GetAssetErrorCodeName(AssetErrorCode code) noexcept
{
    switch (code)
    {
        case AssetErrorCode::InvalidScene:
            return "InvalidScene";
        case AssetErrorCode::AssetCorrupt:
            return "AssetCorrupt";
        case AssetErrorCode::InstallCancelled:
            return "InstallCancelled";
        case AssetErrorCode::Conflict:
            return "Conflict";
        case AssetErrorCode::Busy:
            return "Busy";
        case AssetErrorCode::MissingTools:
            return "MissingTools";
        case AssetErrorCode::UnsupportedScene:
            return "UnsupportedScene";
        case AssetErrorCode::StorageFailure:
            return "StorageFailure";
        case AssetErrorCode::RecoveryRequired:
            return "RecoveryRequired";
        case AssetErrorCode::DestinationExists:
            return "DestinationExists";
        case AssetErrorCode::OutOfMemory:
            return "OutOfMemory";
    }
    return "InvalidScene";
}

bool CreateLevel(std::string_view name,
                 const AssetOptions& options,
                 foundation::Array<uint8>& outLevel,
                 AssetError& outError) noexcept
{
    Failure failure(outError);
    Uuid identity;
    String identityText;
    if (NextUuid(options, identity, failure) == false)
    {
        return false;
    }
    if (FormatUuid(identity, "", identityText) == false)
    {
        return failure.FailOutOfMemory();
    }
    JsonTree tree;
    const JsonIndex root = tree.AddObject();
    tree.SetMember(root, "version", tree.AddInteger(1));
    tree.SetMember(root, "id", tree.AddString(identityText.GetView()));
    tree.SetMember(root, "name", tree.AddString(name));
    tree.SetMember(root, "profile", tree.AddString(LEVEL_PROFILE));
    tree.SetMember(root, "objects", tree.AddArray());
    const JsonIndex camera = tree.AddObject();
    const int64 defaults[3][3] = {{0, 0, 3}, {0, 0, 0}, {0, 1, 0}};
    const std::string_view fields[3] = {"eye", "target", "up"};
    for (usize field = 0; field < 3; ++field)
    {
        const JsonIndex values = tree.AddArray();
        for (const int64 value : defaults[field])
        {
            tree.Append(values, tree.AddInteger(value));
        }
        tree.SetMember(camera, fields[field], values);
    }
    tree.SetMember(camera, "fov", tree.AddFloat(std::numbers::pi / 3));
    tree.SetMember(root, "camera", camera);
    tree.SetMember(root, "imports", tree.AddArray());
    return WriteCanonical(tree, root, outLevel, failure);
}

bool ValidateLevel(std::span<const uint8> level, AssetError& outError) noexcept
{
    Failure failure(outError);
    Document document;
    return ParseInput(level, document, failure) == true &&
           ValidateLevelNode(document.Tree, document.Root, failure) == true;
}

bool ReadCatalog(std::string_view root, foundation::Array<uint8>& outCatalog, AssetError& outError) noexcept
{
    Failure failure(outError);
    Document catalog;
    return ReadCatalogDocument(root, catalog, failure) == true &&
           WriteCanonical(catalog.Tree, catalog.Root, outCatalog, failure) == true;
}

bool PrepareImport(std::string_view root,
                   std::string_view source,
                   std::span<const uint8> level,
                   const AssetOptions& options,
                   ImportCandidate& outCandidate,
                   AssetError& outError) noexcept
{
    Failure failure(outError);
    Document document;
    PreparedImport prepared;
    if (ParseInput(level, document, failure) == false ||
        PrepareImportDocuments(root, source, document, options, prepared, failure) == false ||
        WriteCanonical(prepared.Level.Tree, prepared.Level.Root, outCandidate.Level, failure) == false ||
        WriteCanonical(prepared.Catalog.Tree, prepared.Catalog.Root, outCandidate.Catalog, failure) == false)
    {
        return false;
    }
    outCandidate.HasPublishedCatalog = prepared.HasPublishedCatalog;
    outCandidate.PublishedCatalog = prepared.PublishedCatalog;
    return true;
}

bool CommitImport(std::string_view root,
                  std::span<const uint8> level,
                  std::span<const uint8> catalog,
                  const Digest* publishedOrNull,
                  std::string_view source,
                  const AssetOptions& options,
                  AssetError& outError) noexcept
{
    Failure failure(outError);
    Document levelDocument;
    Document catalogDocument;
    const Document* catalogOrNull = nullptr;
    if (catalog.empty() == true)
    {
        return failure.FailInvalid("Unsupported scene catalog");
    }
    return ParseInput(level, levelDocument, failure) == true &&
           ParseOptionalCatalog(catalog, catalogDocument, catalogOrNull, failure) == true &&
           CommitImportDocuments(root, levelDocument, catalogDocument, publishedOrNull, source, options, failure) ==
               true;
}

bool ImportScene(std::string_view root,
                 std::string_view source,
                 std::span<const uint8> level,
                 const AssetOptions& options,
                 foundation::Array<uint8>& outLevel,
                 AssetError& outError) noexcept
{
    Failure failure(outError);
    Document document;
    PreparedImport prepared;
    const Digest* publishedOrNull = nullptr;
    if (ParseInput(level, document, failure) == false ||
        PrepareImportDocuments(root, source, document, options, prepared, failure) == false)
    {
        return false;
    }
    publishedOrNull = prepared.HasPublishedCatalog == true ? &prepared.PublishedCatalog : nullptr;
    return CommitImportDocuments(root, prepared.Level, prepared.Catalog, publishedOrNull, source, options, failure) ==
               true &&
           WriteCanonical(prepared.Level.Tree, prepared.Level.Root, outLevel, failure) == true;
}

bool ImportSavedLevel(std::string_view root,
                      std::string_view relative,
                      std::string_view source,
                      const AssetOptions& options,
                      foundation::Array<uint8>& outLevel,
                      AssetError& outError) noexcept
{
    Failure failure(outError);
    return ImportSavedLevelDocument(root, relative, source, options, outLevel, failure);
}

bool SaveLevel(std::string_view root,
               std::string_view relative,
               std::span<const uint8> level,
               const foundation::Array<uint8>* expectedOrNull,
               foundation::Array<uint8>& outSaved,
               AssetError& outError) noexcept
{
    Failure failure(outError);
    Document document;
    return ParseInput(level, document, failure) == true &&
           SaveLevelDocument(root, relative, document, expectedOrNull, outSaved, failure) == true;
}

bool OpenLevel(std::string_view root,
               std::string_view relative,
               foundation::Array<uint8>& outLevel,
               AssetError& outError) noexcept
{
    Failure failure(outError);
    Document document;
    return OpenLevelDocument(root, relative, document, failure) == true &&
           WriteCanonical(document.Tree, document.Root, outLevel, failure) == true;
}

bool AssembleLevel(std::string_view root,
                   std::span<const uint8> level,
                   std::span<const uint8> catalog,
                   foundation::Array<uint8>& outBundle,
                   AssetError& outError) noexcept
{
    Failure failure(outError);
    Document document;
    Document catalogDocument;
    const Document* catalogOrNull = nullptr;
    return ParseInput(level, document, failure) == true &&
           ParseOptionalCatalog(catalog, catalogDocument, catalogOrNull, failure) == true &&
           AssembleBundle(root, document.Tree, document.Root, catalogOrNull, outBundle, failure) == true;
}

bool DumpLevel(std::string_view root,
               std::span<const uint8> level,
               std::span<const uint8> catalog,
               DumpLayout layout,
               foundation::Array<uint8>& outDump,
               AssetError& outError) noexcept
{
    Failure failure(outError);
    Document document;
    Document catalogDocument;
    Document dump;
    const Document* catalogOrNull = nullptr;
    return ParseInput(level, document, failure) == true &&
           ParseOptionalCatalog(catalog, catalogDocument, catalogOrNull, failure) == true &&
           DumpDocument(root, document.Tree, document.Root, catalogOrNull, dump, failure) == true &&
           WriteLayout(dump, layout, outDump, failure) == true;
}

bool PublishLevel(std::string_view root,
                  std::span<const uint8> level,
                  std::string_view destination,
                  DumpLayout layout,
                  foundation::Array<uint8>& outDump,
                  AssetError& outError) noexcept
{
    Failure failure(outError);
    Document document;
    Document dump;
    return ParseInput(level, document, failure) == true &&
           PublishRelease(root, document, destination, dump, failure) == true &&
           WriteLayout(dump, layout, outDump, failure) == true;
}

bool WriteFileAtomically(std::string_view path, std::span<const uint8> data, AssetError& outError) noexcept
{
    Failure failure(outError);
    return PublishBytes(path, data, PublishMode::Replace, nullptr, failure);
}
namespace
{
void ReportStage(const EditorSceneRequest& request, std::string_view stage) noexcept
{
    const EditorSceneProgress* progressOrNull = request.ProgressOrNull;
    if (progressOrNull != nullptr && progressOrNull->Report != nullptr)
    {
        progressOrNull->Report(progressOrNull->ContextOrNull, stage);
    }
}

[[nodiscard]] bool ParseHexDigest(std::string_view text, Digest& outDigest) noexcept
{
    if (text.size() != HEX_DIGEST_BYTES)
    {
        return false;
    }
    for (usize i = 0; i < sizeof(outDigest.Data); ++i)
    {
        uint8 value = 0;
        for (usize half = 0; half < 2; ++half)
        {
            const char item = text[i * 2 + half];
            uint8 nibble = 0;
            if (item >= '0' && item <= '9')
            {
                nibble = static_cast<uint8>(item - '0');
            }
            else if (item >= 'a' && item <= 'f')
            {
                nibble = static_cast<uint8>(item - 'a' + 10);
            }
            else
            {
                return false;
            }
            value = static_cast<uint8>((value << 4) | nibble);
        }
        outDigest.Data[i] = value;
    }
    return true;
}

/// Sorted, de-duplicated manifest approximations of every imported scene.
[[nodiscard]] bool CollectWarnings(std::string_view root,
                                   const Document& level,
                                   const Document& catalog,
                                   JsonTree& inoutTree,
                                   JsonIndex& outWarnings,
                                   Failure& inoutFailure) noexcept
{
    Array<String> texts;
    const JsonIndex resources = catalog.Tree.GetMember(catalog.Root, "resources");
    const JsonIndex imports = level.Tree.GetMember(level.Root, "imports");
    for (usize i = 0; i < level.Tree.GetCount(imports); ++i)
    {
        const JsonIndex record = catalog.Tree.GetMember(resources, level.Tree.GetText(level.Tree.GetItem(imports, i)));
        const JsonIndex path = record == NO_JSON_NODE ? NO_JSON_NODE : catalog.Tree.GetMember(record, "path");
        if (path == NO_JSON_NODE || catalog.Tree.GetKind(path) != JsonKind::String)
        {
            return inoutFailure.FailInvalid("Missing imported scene record");
        }
        String native;
        Document manifest;
        if (ResolveInside(root, catalog.Tree.GetText(path), native, inoutFailure) == false ||
            LoadDocument(native.GetView(), MAX_METADATA_BYTES, manifest, inoutFailure) == false)
        {
            return false;
        }
        const JsonIndex approximations = manifest.Tree.GetKind(manifest.Root) == JsonKind::Object
                                             ? manifest.Tree.GetMember(manifest.Root, "approximations")
                                             : NO_JSON_NODE;
        if (approximations == NO_JSON_NODE)
        {
            continue;
        }
        if (manifest.Tree.GetKind(approximations) != JsonKind::Array)
        {
            return inoutFailure.FailInvalid("Unsupported scene manifest approximations");
        }
        for (usize item = 0; item < manifest.Tree.GetCount(approximations); ++item)
        {
            const JsonIndex warning = manifest.Tree.GetItem(approximations, item);
            if (manifest.Tree.GetKind(warning) != JsonKind::String)
            {
                return inoutFailure.FailInvalid("Unsupported scene manifest approximations");
            }
            String text;
            if (text.TryAppend(manifest.Tree.GetText(warning)) != foundation::StringStatus::Ok ||
                texts.TryAdd(std::move(text)) == false)
            {
                return inoutFailure.FailOutOfMemory();
            }
        }
    }
    // sorted(set(...)): byte order of UTF-8 equals code point order.
    for (usize i = 1; i < texts.GetSize(); ++i)
    {
        for (usize j = i; j > 0 && texts[j].GetView() < texts[j - 1].GetView(); --j)
        {
            String swap = std::move(texts[j]);
            texts[j] = std::move(texts[j - 1]);
            texts[j - 1] = std::move(swap);
        }
    }
    outWarnings = inoutTree.AddArray();
    for (usize i = 0; i < texts.GetSize(); ++i)
    {
        if (i == 0 || texts[i].GetView() != texts[i - 1].GetView())
        {
            inoutTree.Append(outWarnings, inoutTree.AddString(texts[i].GetView()));
        }
    }
    return inoutTree.IsOk() == true ? true : inoutFailure.FailOutOfMemory();
}

[[nodiscard]] bool ReadDiskDigest(std::string_view root,
                                  std::string_view relative,
                                  bool& outDoesExist,
                                  Array<uint8>& outBytes,
                                  Failure& inoutFailure) noexcept
{
    String path;
    if (ResolveInside(root, relative, path, inoutFailure) == false)
    {
        return false;
    }
    outDoesExist = DoesPathExist(path.GetView());
    outBytes.Clear();
    return outDoesExist == false || ReadBytes(path.GetView(), MAX_METADATA_BYTES, outBytes, inoutFailure) == true;
}

/// PreviewImport: cook as a dry run and describe the result without touching the project.
[[nodiscard]] bool PreviewImportDocuments(const EditorSceneRequest& request,
                                          const AssetOptions& options,
                                          EditorSceneResult& outResult,
                                          Failure& inoutFailure) noexcept
{
    ReportStage(request, "preview");
    RoutedCook routed;
    if (CookRoutedSource(request.Source, options, true, routed, inoutFailure) == false)
    {
        return false;
    }
    // The reference profile has no dry run; its manifest is its report.
    const Array<uint8>& reportBytes = routed.Report.IsEmpty() == true ? routed.Cooked.Manifest : routed.Report;
    Document report;
    Document manifest;
    HexDigest sourceDigest;
    if (ParseDocument(GetSpan(reportBytes), AssetErrorCode::UnsupportedScene, report, inoutFailure) == false ||
        ParseDocument(GetSpan(routed.Cooked.Manifest), AssetErrorCode::UnsupportedScene, manifest, inoutFailure) ==
            false ||
        ComputeSourceDigest(manifest.Tree, manifest.Root, sourceDigest, inoutFailure) == false)
    {
        return false;
    }
    JsonTree tree;
    const JsonIndex response = tree.AddObject();
    tree.SetMember(response, "schema", tree.AddInteger(1));
    tree.SetMember(response, "ok", tree.AddBoolean(true));
    tree.SetMember(response, "profile", tree.AddString(routed.Profile));
    tree.SetMember(response, "report", tree.CopyFrom(report.Tree, report.Root));
    tree.SetMember(response, "source_digest", tree.AddString(sourceDigest.View()));
    if (routed.ReferenceRejection.IsEmpty() == false)
    {
        tree.SetMember(response, "reference_rejection", tree.AddString(routed.ReferenceRejection.GetView()));
    }
    if (tree.IsOk() == false)
    {
        return inoutFailure.FailOutOfMemory();
    }
    ReportStage(request, "done");
    outResult.Scene.Clear();
    outResult.Catalog.Clear();
    return WriteCanonical(tree, response, outResult.Response, inoutFailure);
}

/// Stored best-effort import reports of the level's imported scenes, keyed by
/// import id; NO_JSON_NODE when none of them carries one (reference imports).
[[nodiscard]] bool CollectImportReports(std::string_view root,
                                        const Document& level,
                                        const Document& catalog,
                                        JsonTree& inoutTree,
                                        JsonIndex& outReports,
                                        Failure& inoutFailure) noexcept
{
    outReports = NO_JSON_NODE;
    const JsonIndex resources = catalog.Tree.GetMember(catalog.Root, "resources");
    const JsonIndex imports = level.Tree.GetMember(level.Root, "imports");
    for (usize i = 0; i < level.Tree.GetCount(imports); ++i)
    {
        const std::string_view identity = level.Tree.GetText(level.Tree.GetItem(imports, i));
        const JsonIndex record = catalog.Tree.GetMember(resources, identity);
        const JsonIndex path = record == NO_JSON_NODE ? NO_JSON_NODE : catalog.Tree.GetMember(record, "path");
        if (path == NO_JSON_NODE || catalog.Tree.GetKind(path) != JsonKind::String)
        {
            return inoutFailure.FailInvalid("Missing imported scene record");
        }
        String native;
        Document manifest;
        if (ResolveInside(root, catalog.Tree.GetText(path), native, inoutFailure) == false ||
            LoadDocument(native.GetView(), MAX_METADATA_BYTES, manifest, inoutFailure) == false)
        {
            return false;
        }
        const JsonIndex report = manifest.Tree.GetKind(manifest.Root) == JsonKind::Object
                                     ? manifest.Tree.GetMember(manifest.Root, "import_report")
                                     : NO_JSON_NODE;
        if (report == NO_JSON_NODE)
        {
            continue;
        }
        if (outReports == NO_JSON_NODE)
        {
            outReports = inoutTree.AddObject();
        }
        inoutTree.SetMember(outReports, identity, inoutTree.CopyFrom(manifest.Tree, report));
    }
    return inoutTree.IsOk() == true ? true : inoutFailure.FailOutOfMemory();
}

[[nodiscard]] bool OperateEditorSceneDocuments(const EditorSceneRequest& request,
                                               const AssetOptions& options,
                                               EditorSceneResult& outResult,
                                               Failure& inoutFailure) noexcept
{
    const std::string_view root = request.Root;
    const EditorSceneOperation operation = request.Operation;
    if (operation == EditorSceneOperation::PreviewImport)
    {
        return PreviewImportDocuments(request, options, outResult, inoutFailure);
    }
    Document level;
    Document current;
    const Document* currentOrNull = nullptr;
    bool isCommitted = false;
    bool isSaveCommitted = false;
    bool hasExpectedCatalog = false;
    Digest expectedCatalog;
    Array<uint8> disk;
    ReportStage(request, "validate");
    if (operation == EditorSceneOperation::Open)
    {
        bool doesExist = false;
        if (ReadDiskDigest(root, request.Path, doesExist, disk, inoutFailure) == false)
        {
            return false;
        }
        if (doesExist == false)
        {
            return inoutFailure.Fail(AssetErrorCode::AssetCorrupt, "No such file or directory");
        }
        if (OpenLevelDocument(root, request.Path, level, inoutFailure) == false)
        {
            return false;
        }
    }
    else
    {
        if (ParseInput(request.Level, level, inoutFailure) == false ||
            ValidateLevelNode(level.Tree, level.Root, inoutFailure) == false)
        {
            return false;
        }
        if (operation == EditorSceneOperation::Import)
        {
            ReportStage(request, "import");
            PreparedImport prepared;
            if (PrepareImportDocuments(root, request.Source, level, options, prepared, inoutFailure) == false ||
                WriteCanonical(prepared.Catalog.Tree, prepared.Catalog.Root, outResult.Catalog, inoutFailure) == false)
            {
                return false;
            }
            level = std::move(prepared.Level);
            current = std::move(prepared.Catalog);
            currentOrNull = &current;
            hasExpectedCatalog = prepared.HasPublishedCatalog;
            expectedCatalog = prepared.PublishedCatalog;
        }
        else if (operation == EditorSceneOperation::CommitImport)
        {
            ReportStage(request, "commit");
            Digest published;
            if (request.HasExpected == true && ParseHexDigest(request.Expected, published) == false)
            {
                return inoutFailure.FailInvalid("Unsupported editor scene protocol");
            }
            if (request.Catalog.empty() == true ||
                ParseOptionalCatalog(request.Catalog, current, currentOrNull, inoutFailure) == false)
            {
                return request.Catalog.empty() == true ? inoutFailure.FailInvalid("Unsupported scene catalog") : false;
            }
            if (CommitImportDocuments(root,
                                      level,
                                      current,
                                      request.HasExpected == true ? &published : nullptr,
                                      request.Source,
                                      options,
                                      inoutFailure) == false)
            {
                return false;
            }
            isCommitted = true;
        }
        else if (operation == EditorSceneOperation::Save)
        {
            ReportStage(request, "save");
            bool doesExist = false;
            Array<uint8> previous;
            if (ReadDiskDigest(root, request.Path, doesExist, previous, inoutFailure) == false)
            {
                return false;
            }
            HexDigest actual;
            ComputeHexDigest(GetSpan(previous), actual);
            const bool isExpected =
                doesExist == request.HasExpected && (doesExist == false || actual.View() == request.Expected);
            if (isExpected == false)
            {
                return inoutFailure.Fail(AssetErrorCode::Conflict, "Saved level changed on disk");
            }
            if (IsCancelled(options) == true)
            {
                return inoutFailure.Fail(AssetErrorCode::InstallCancelled, "Scene save cancelled");
            }
            if (SaveLevelDocument(root,
                                  request.Path,
                                  level,
                                  doesExist == true ? &previous : nullptr,
                                  disk,
                                  inoutFailure) == false)
            {
                return false;
            }
            isSaveCommitted = true;
        }
    }
    if (IsCancelled(options) == true && isCommitted == false && isSaveCommitted == false)
    {
        return inoutFailure.Fail(AssetErrorCode::InstallCancelled, "Scene import cancelled");
    }
    outResult.Scene.Clear();
    if (isCommitted == false)
    {
        ReportStage(request, "assemble");
        if (AssembleBundle(root, level.Tree, level.Root, currentOrNull, outResult.Scene, inoutFailure) == false)
        {
            return false;
        }
    }
    Document closure;
    if (ComputeClosure(root, level.Tree, level.Root, currentOrNull, closure, inoutFailure) == false)
    {
        return false;
    }
    JsonTree tree;
    const JsonIndex response = tree.AddObject();
    tree.SetMember(response, "schema", tree.AddInteger(1));
    tree.SetMember(response, "ok", tree.AddBoolean(true));
    tree.SetMember(response, "level", tree.CopyFrom(level.Tree, level.Root));
    tree.SetMember(response, "committed", tree.AddBoolean(isCommitted));
    const JsonIndex artifacts = tree.AddObject();
    PythonSum total;
    for (usize i = 0; i < closure.Tree.GetCount(closure.Root); ++i)
    {
        const JsonIndex record = closure.Tree.GetValue(closure.Root, i);
        tree.SetMember(artifacts,
                       closure.Tree.GetKey(closure.Root, i),
                       tree.CopyFrom(closure.Tree, closure.Tree.GetMember(record, "key")));
        total.Add(GetNumberMember(closure.Tree, record, "bytes"));
    }
    tree.SetMember(response, "artifacts", artifacts);
    if (hasExpectedCatalog == true)
    {
        HexDigest text;
        static constexpr char HEX[] = "0123456789abcdef";
        for (usize i = 0; i < sizeof(expectedCatalog.Data); ++i)
        {
            text.Text[i * 2] = HEX[expectedCatalog.Data[i] >> 4];
            text.Text[i * 2 + 1] = HEX[expectedCatalog.Data[i] & 0xf];
        }
        tree.SetMember(response, "catalog_expected", tree.AddString(text.View()));
    }
    else
    {
        tree.SetMember(response, "catalog_expected", tree.AddNull());
    }
    tree.SetMember(response, "resident_bytes", AddPythonNumber(tree, total.GetResult()));
    Document published;
    const Document* catalog = currentOrNull;
    if (catalog == nullptr)
    {
        if (ReadCatalogDocument(root, published, inoutFailure) == false)
        {
            return false;
        }
        catalog = &published;
    }
    JsonIndex warnings = NO_JSON_NODE;
    if (CollectWarnings(root, level, *catalog, tree, warnings, inoutFailure) == false)
    {
        return false;
    }
    tree.SetMember(response, "warnings", warnings);
    JsonIndex importReports = NO_JSON_NODE;
    if (CollectImportReports(root, level, *catalog, tree, importReports, inoutFailure) == false)
    {
        return false;
    }
    if (importReports != NO_JSON_NODE)
    {
        tree.SetMember(response, "import_reports", importReports);
    }
    if (operation == EditorSceneOperation::Open || operation == EditorSceneOperation::Save)
    {
        bool doesExist = false;
        Array<uint8> now;
        if (ReadDiskDigest(root, request.Path, doesExist, now, inoutFailure) == false)
        {
            return false;
        }
        if (doesExist == false || AreBytesEqual(GetSpan(now), GetSpan(disk)) == false)
        {
            return inoutFailure.Fail(AssetErrorCode::Conflict, "Saved level changed while preparing the response");
        }
        HexDigest digest;
        ComputeHexDigest(GetSpan(disk), digest);
        tree.SetMember(response, "disk", tree.AddString(digest.View()));
    }
    ReportStage(request, "done");
    return WriteCanonical(tree, response, outResult.Response, inoutFailure);
}
} // namespace

bool OperateEditorScene(const EditorSceneRequest& request,
                        const AssetOptions& options,
                        EditorSceneResult& outResult,
                        AssetError& outError) noexcept
{
    Failure failure(outError);
    // Stage into a local result so a failure leaves the caller's previous result untouched.
    EditorSceneResult staged;
    if (OperateEditorSceneDocuments(request, options, staged, failure) == false)
    {
        return false;
    }
    outResult = std::move(staged);
    return true;
}
} // namespace ludus::content::scene_cook
