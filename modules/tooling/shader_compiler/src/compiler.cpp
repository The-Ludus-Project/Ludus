#include <ludus/tooling/shader_compiler/compiler.hpp>

#include "codec.hpp"
#include "language_source.hpp"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/config.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/parsing/json.hpp>
#include <ludus/foundation/strings/static_string.hpp>

#include <cstring>
#include <new>
#include <span>
#include <string_view>

#include <slang-com-ptr.h>
#include <slang.h>

#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
#    include <windows.h>
#endif

// Thanks to the Slang authors, "Using the Slang Compilation API", sections
// "About Sessions" and "Load Modules", https://docs.shader-slang.org/en/latest/compilation-api.html.
// Reuse the global compiler and an unchanged snapshot, but replace the session on
// any source edit. A closed in-memory filesystem prevents uncaptured disk inputs.
// Each request compiles its source as one translation unit with one entry point, the
// route slangc uses. Loading the same source with ISession::loadModule made Slang
// 2026.1.2 spin forever in its entry-point parameter transform for render-graph
// wrappers (TransformParamsToConstRefContext), while slangc finished in seconds.
//
// Thanks to the Slang team, "Supported Compilation Targets" (DXIL through DXC),
// https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/09-targets.html:
// Slang emits HLSL and has the pinned DXC compile it; dxil.dll beside dxcompiler.dll signs it.
// The DXIL session mirrors cmake/shaders/compile_shader.py (sm_6_0, LUDUS_DXIL=1).
namespace ludus::tooling::shader_compiler
{
using namespace foundation;
namespace
{
// Portable sessions hold SPIR-V 1.3, WGSL and Metal; Target values index them directly.
constexpr usize PORTABLE_TARGET_COUNT = 3;
// DXBC container header: four-byte code, 16-byte hash, version, total size and part count.
constexpr usize DXIL_CONTAINER_HEADER_BYTES = 32;
constexpr usize DXIL_HASH_OFFSET = 4;
constexpr usize DXIL_HASH_BYTES = 16;
constexpr usize DXIL_SIZE_OFFSET = 24;
constexpr std::string_view DXIL_CONTAINER_CODE = "DXBC";
constexpr std::string_view UNSIGNED_DXIL_DIAGNOSTIC =
    "DXC produced an unsigned DXIL container; place the pinned dxil.dll beside dxcompiler.dll";
// One session per profile: a request for another profile replaces the cached session.
enum class SessionProfile : uint8
{
    Portable, ///< SPIR-V 1.3, WGSL and Metal from one snapshot.
    RayQuery, ///< SPIR-V 1.5 compute with spvRayQueryKHR.
    Dxil,     ///< Direct3D 12 DXIL shader model 6.0 through DXC.
};
SessionProfile GetSessionProfile(Target target) noexcept
{
    if (target == Target::SpirvRayQuery)
    {
        return SessionProfile::RayQuery;
    }
    return target == Target::Dxil ? SessionProfile::Dxil : SessionProfile::Portable;
}
// Unsigned output carries a zero or constant "bypass" hash, which retail Direct3D 12
// runtimes reject; the same check guards offline builds (compile_shader.py dxil_container).
bool IsSignedDxilContainer(const void* bytes, usize size) noexcept
{
    const std::span<const uint8> data{static_cast<const uint8*>(bytes), size};
    uint32 containerSize = 0;
    if (size < DXIL_CONTAINER_HEADER_BYTES ||
        std::memcmp(data.data(), DXIL_CONTAINER_CODE.data(), DXIL_CONTAINER_CODE.size()) != 0 ||
        TryReadLittleEndian<uint32>(data.subspan(DXIL_SIZE_OFFSET), containerSize) == false || containerSize != size)
    {
        return false;
    }
    for (usize i = 1; i < DXIL_HASH_BYTES; ++i)
    {
        if (data[DXIL_HASH_OFFSET + i] != data[DXIL_HASH_OFFSET])
        {
            return true;
        }
    }
    return false;
}
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
// Windows extended-length paths are limited to 32767 UTF-16 code units.
constexpr usize MAX_EXECUTABLE_PATH_CHARACTERS = 32768;
// The worker deploys the pinned dxcompiler.dll and dxil.dll beside its executable. Naming that
// directory keeps an unrelated DXC on PATH or in the system directory out of the compiler.
bool GetExecutableDirectory(StaticString<filesystem::MAX_PATH_BYTES>& outDirectory) noexcept
{
    Array<wchar_t> path;
    if (path.TryResize(MAX_EXECUTABLE_PATH_CHARACTERS) == false)
    {
        return false;
    }
    const DWORD length = ::GetModuleFileNameW(nullptr, path.GetData(), static_cast<DWORD>(path.GetSize()));
    if (length == 0 || length >= path.GetSize())
    {
        return false;
    }
    DWORD directoryLength = length;
    while (directoryLength != 0 && path[directoryLength - 1] != L'\\' && path[directoryLength - 1] != L'/')
    {
        --directoryLength;
    }
    if (directoryLength < 2)
    {
        return false;
    }
    // Drop the trailing separator; Slang joins the library name itself.
    --directoryLength;
    StaticArray<char, filesystem::MAX_PATH_BYTES + 1> bytes{};
    const int32 count = ::WideCharToMultiByte(CP_UTF8,
                                              WC_ERR_INVALID_CHARS,
                                              path.GetData(),
                                              static_cast<int32>(directoryLength),
                                              bytes.GetData(),
                                              static_cast<int32>(filesystem::MAX_PATH_BYTES),
                                              nullptr,
                                              nullptr);
    return count > 0 && outDirectory.TryAssign({bytes.GetData(), static_cast<usize>(count)}) == StringStatus::Ok;
}
#endif
using PathText = StaticString<filesystem::MAX_PATH_BYTES>;
using EntryText = StaticString<MAX_ENTRY_BYTES>;
std::string_view GetText(BorrowedView<const char> value) noexcept
{
    return {value.GetDataOrNull(), value.GetSize()};
}
bool CopyBytes(const void* bytes, usize count, Array<uint8>& outBytes) noexcept
{
    if (outBytes.TryResize(count) == false)
    {
        return false;
    }
    if (count != 0)
    {
        std::memcpy(outBytes.GetData(), bytes, count);
    }
    return true;
}
// Slang's API JSON omits the CLI emitter's per-entry binding usage. Query
// generated-code metadata explicitly: absent usage must never erase live uniforms.
bool WriteReflection(slang::ProgramLayout& inoutLayout,
                     slang::IMetadata& inoutMetadata,
                     ISlangBlob& inoutJson,
                     Array<uint8>& outBytes) noexcept
{
    const std::string_view json{static_cast<const char*>(inoutJson.getBufferPointer()), inoutJson.getBufferSize()};
    const usize closing = json.find_last_not_of(" \t\r\n");
    if (closing == std::string_view::npos || json[closing] != '}' || outBytes.TryResize(MAX_ARTIFACT_BYTES) == false)
    {
        return false;
    }
    parsing::JsonWriter writer{{outBytes.GetData(), outBytes.GetSize()}};
    writer.Raw(json.substr(0, closing));
    writer.Raw(",\"nativeEntryPointResourceUsage\":{");
    for (uint32 i = 0; i < inoutLayout.getParameterCount(); ++i)
    {
        slang::VariableLayoutReflection* parameter = inoutLayout.getParameterByIndex(i);
        bool isUsed = false;
        for (uint32 categoryIndex = 0; categoryIndex < parameter->getCategoryCount(); ++categoryIndex)
        {
            const SlangParameterCategory category =
                static_cast<SlangParameterCategory>(parameter->getCategoryByIndex(categoryIndex));
            bool isLocationUsed = false;
            if (SLANG_FAILED(
                    inoutMetadata.isParameterLocationUsed(category,
                                                          static_cast<SlangUInt>(parameter->getBindingSpace(category)),
                                                          static_cast<SlangUInt>(parameter->getOffset(category)),
                                                          isLocationUsed)))
            {
                return false;
            }
            isUsed = isUsed == true || isLocationUsed == true;
        }
        if (i != 0)
        {
            writer.Raw(",");
        }
        writer.String(parameter->getName());
        writer.Raw(isUsed == true ? ":true" : ":false");
    }
    writer.Raw("}}");
    std::span<const uint8> bytes;
    return writer.Finish(bytes) == parsing::ParseStatus::Ok && outBytes.TryResize(bytes.size()) == true;
}
bool AreIdsEqual(const SlangUUID& first, const SlangUUID& second) noexcept
{
    return std::memcmp(&first, &second, sizeof(first)) == 0;
}
class SourceBlob final : public ISlangBlob
{
public:
    Array<char> Bytes;
    SlangResult SLANG_MCALL queryInterface(const SlangUUID& identity, void** outObject) noexcept override
    {
        if (AreIdsEqual(identity, ISlangBlob::getTypeGuid()) == false &&
            AreIdsEqual(identity, ISlangUnknown::getTypeGuid()) == false)
        {
            *outObject = nullptr;
            return SLANG_E_NO_INTERFACE;
        }
        *outObject = static_cast<ISlangBlob*>(this);
        addRef();
        return SLANG_OK;
    }
    LUDUS_INLINE uint32 SLANG_MCALL addRef() noexcept override
    {
        return ++mReferences;
    }
    uint32 SLANG_MCALL release() noexcept override
    {
        const uint32 remaining = --mReferences;
        if (remaining == 0)
        {
            delete this;
        }
        return remaining;
    }
    LUDUS_INLINE const void* SLANG_MCALL getBufferPointer() noexcept override
    {
        return Bytes.GetData();
    }
    LUDUS_INLINE usize SLANG_MCALL getBufferSize() noexcept override
    {
        return Bytes.GetSize() - 1;
    }

private:
    uint32 mReferences = 1;
};
struct OwnedFile final
{
    PathText Path;
    Slang::ComPtr<SourceBlob> Blob;
    bool HasBeenRead = false;
};
class SnapshotFilesystem final : public ISlangFileSystem
{
public:
    Array<OwnedFile> Files;
    SlangResult SLANG_MCALL queryInterface(const SlangUUID& identity, void** outObject) noexcept override
    {
        *outObject = castAs(identity);
        if (*outObject == nullptr)
        {
            return SLANG_E_NO_INTERFACE;
        }
        addRef();
        return SLANG_OK;
    }
    void* SLANG_MCALL castAs(const SlangUUID& identity) noexcept override
    {
        if (AreIdsEqual(identity, ISlangFileSystem::getTypeGuid()) == true ||
            AreIdsEqual(identity, ISlangCastable::getTypeGuid()) == true ||
            AreIdsEqual(identity, ISlangUnknown::getTypeGuid()) == true)
        {
            return static_cast<ISlangFileSystem*>(this);
        }
        return nullptr;
    }
    LUDUS_INLINE uint32 SLANG_MCALL addRef() noexcept override
    {
        return ++mReferences;
    }
    uint32 SLANG_MCALL release() noexcept override
    {
        const uint32 remaining = --mReferences;
        if (remaining == 0)
        {
            delete this;
        }
        return remaining;
    }
    SlangResult SLANG_MCALL loadFile(const char* path, ISlangBlob** outBlob) noexcept override
    {
        *outBlob = nullptr;
        std::string_view remaining = path;
        PathText normalized;
        if (remaining.empty() == true || remaining.front() == '/' || remaining.size() > filesystem::MAX_PATH_BYTES)
        {
            return SLANG_E_NOT_FOUND;
        }
        while (remaining.empty() == false)
        {
            const usize separator = remaining.find('/');
            const std::string_view component = remaining.substr(0, separator);
            remaining = separator == std::string_view::npos ? std::string_view{} : remaining.substr(separator + 1);
            if (component.empty() == true || component == ".")
            {
                continue;
            }
            if (component == "..")
            {
                if (normalized.IsEmpty() == true)
                {
                    return SLANG_E_NOT_FOUND;
                }
                const usize parent = normalized.GetView().rfind('/');
                static_cast<void>(normalized.TryAssign(
                    parent == std::string_view::npos ? std::string_view{} : normalized.GetView().substr(0, parent)));
            }
            else
            {
                if ((normalized.IsEmpty() == false && normalized.TryAppend("/") != StringStatus::Ok) ||
                    normalized.TryAppend(component) != StringStatus::Ok)
                {
                    return SLANG_E_NOT_FOUND;
                }
            }
        }
        if (filesystem::ValidPath(normalized.GetView()) == false)
        {
            return SLANG_E_NOT_FOUND;
        }
        const std::string_view relative = normalized.GetView();
        for (OwnedFile& file : Files)
        {
            if (file.Path.GetView() == relative)
            {
                file.HasBeenRead = true;
                *outBlob = file.Blob.get();
                (*outBlob)->addRef();
                return SLANG_OK;
            }
        }
        return SLANG_E_NOT_FOUND;
    }
    bool Matches(const Request& request) const noexcept
    {
        if (Files.GetSize() != request.Files.GetSize())
        {
            return false;
        }
        for (usize i = 0; i < Files.GetSize(); ++i)
        {
            const OwnedFile& file = Files[i];
            if (file.Path.GetView() != GetText(request.Files[i].Path) ||
                file.Blob->getBufferSize() != request.Files[i].Text.GetSize() ||
                (request.Files[i].Text.GetSize() != 0 && std::memcmp(file.Blob->getBufferPointer(),
                                                                     request.Files[i].Text.GetDataOrNull(),
                                                                     request.Files[i].Text.GetSize()) != 0))
            {
                return false;
            }
        }
        return true;
    }

private:
    uint32 mReferences = 1;
};
} // namespace
struct Compiler::State final
{
    Slang::ComPtr<slang::IGlobalSession> Global;
    Slang::ComPtr<SnapshotFilesystem> Filesystem;
    Slang::ComPtr<slang::ISession> Session;
    PathText Source;
    SessionProfile Profile = SessionProfile::Portable;
    /// The session emits debug information. Its loaded modules keep the level they were compiled
    /// with, so a request with the other level needs a session of its own.
    bool HasDebugInformation = false;

    Status Capture(const Request& request) noexcept
    {
        const SessionProfile profile = GetSessionProfile(request.OutputTarget);
        if (Session.get() != nullptr && Profile == profile &&
            HasDebugInformation == request.ShouldEmitDebugInformation && Source.GetView() == GetText(request.Source) &&
            Filesystem->Matches(request) == true)
        {
            return Status::Ready;
        }
        Session.setNull();
        Filesystem.setNull();
        Filesystem.attach(new (std::nothrow) SnapshotFilesystem);
        if (Filesystem.get() == nullptr || Filesystem->Files.TryResize(request.Files.GetSize()) == false)
        {
            return Status::OutOfMemory;
        }
        for (usize i = 0; i < request.Files.GetSize(); ++i)
        {
            OwnedFile& file = Filesystem->Files[i];
            if (file.Path.TryAssign(GetText(request.Files[i].Path)) != StringStatus::Ok)
            {
                return Status::InvalidRequest;
            }
            file.Blob.attach(new (std::nothrow) SourceBlob);
            if (file.Blob.get() == nullptr || file.Blob->Bytes.TryResize(request.Files[i].Text.GetSize() + 1) == false)
            {
                return Status::OutOfMemory;
            }
            if (request.Files[i].Text.GetSize() != 0)
            {
                std::memcpy(file.Blob->Bytes.GetData(),
                            request.Files[i].Text.GetDataOrNull(),
                            request.Files[i].Text.GetSize());
            }
            file.Blob->Bytes[request.Files[i].Text.GetSize()] = '\0';
        }
        static_cast<void>(Source.TryAssign(GetText(request.Source)));
        const bool hasNativeRayQueries = profile == SessionProfile::RayQuery;
        StaticArray<slang::TargetDesc, PORTABLE_TARGET_COUNT> targets{};
        targets[0].format = SLANG_SPIRV;
        targets[0].profile = Global->findProfile(hasNativeRayQueries == true ? "spirv_1_5" : "spirv_1_3");
        slang::CompilerOptionEntry capability{};
        capability.name = slang::CompilerOptionName::Capability;
        capability.value.kind = slang::CompilerOptionValueKind::Int;
        capability.value.intValue0 = Global->findCapability("spvRayQueryKHR");
        targets[0].compilerOptionEntries = hasNativeRayQueries == true ? &capability : nullptr;
        targets[0].compilerOptionEntryCount = hasNativeRayQueries == true ? 1 : 0;
        targets[1].format = SLANG_WGSL;
        targets[1].profile = Global->findProfile("sm_6_0");
        targets[2].format = SLANG_METAL;
        targets[2].profile = Global->findProfile("sm_6_0");
        const slang::PreprocessorMacroDesc dxilMacro{"LUDUS_DXIL", "1"};
        slang::SessionDesc description;
        if (profile == SessionProfile::Dxil)
        {
            targets[0] = slang::TargetDesc{};
            targets[0].format = SLANG_DXIL;
            targets[0].profile = Global->findProfile("sm_6_0");
            description.preprocessorMacros = &dxilMacro;
            description.preprocessorMacroCount = 1;
        }
        description.targets = targets.GetData();
        description.targetCount =
            profile == SessionProfile::Portable ? static_cast<SlangInt>(PORTABLE_TARGET_COUNT) : 1;
        description.fileSystem = Filesystem.get();
        slang::CompilerOptionEntry debugInformation{};
        debugInformation.name = slang::CompilerOptionName::DebugInformation;
        debugInformation.value.kind = slang::CompilerOptionValueKind::Int;
        debugInformation.value.intValue0 = SLANG_DEBUG_INFO_LEVEL_STANDARD;
        if (request.ShouldEmitDebugInformation == true)
        {
            description.compilerOptionEntries = &debugInformation;
            description.compilerOptionEntryCount = 1;
        }
        Profile = profile;
        HasDebugInformation = request.ShouldEmitDebugInformation;
        return SLANG_SUCCEEDED(Global->createSession(description, Session.writeRef())) ? Status::Ready
                                                                                       : Status::Unavailable;
    }
};
namespace
{
/// @brief Builds DeclarationResult records from Slang's declaration reflection.
///
/// Thanks to the Slang team, "Reflection API" (Slang User Guide, chapter 9,
/// https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/09-reflection.html),
/// whose DeclReflection walk this follows.
class DeclarationReflector final
{
public:
    explicit DeclarationReflector(DeclarationResult& outResult) noexcept : mResult(outResult) {}

    /// Add the module-level declarations below `parent`; false when a limit or allocation fails.
    [[nodiscard]] bool AddChildren(slang::DeclReflection* parent) noexcept
    {
        const uint32 count = parent->getChildrenCount();
        for (uint32 i = 0; i < count; ++i)
        {
            slang::DeclReflection* childOrNull = parent->getChild(i);
            if (childOrNull != nullptr && addDeclaration(childOrNull) == false)
            {
                return false;
            }
        }
        return true;
    }

private:
    [[nodiscard]] bool addDeclaration(slang::DeclReflection* declaration) noexcept
    {
        switch (declaration->getKind())
        {
            case slang::DeclReflection::Kind::Func: {
                slang::FunctionReflection* functionOrNull = declaration->asFunction();
                if (functionOrNull == nullptr)
                {
                    return true;
                }
                ReflectedDeclaration record;
                record.Kind = DeclarationKind::Function;
                if (appendText(functionOrNull->getName(), record.Name) == false ||
                    reflectType(functionOrNull->getReturnType(), record.Type) == false)
                {
                    return false;
                }
                record.FirstMember = static_cast<uint32>(mResult.Members.GetSize());
                for (uint32 i = 0; i < functionOrNull->getParameterCount(); ++i)
                {
                    slang::VariableReflection* parameterOrNull = functionOrNull->getParameterByIndex(i);
                    if (parameterOrNull == nullptr)
                    {
                        continue;
                    }
                    ReflectedMember member;
                    member.Direction =
                        parameterOrNull->findModifier(slang::Modifier::InOut) != nullptr ? ReflectedDirection::Inout
                        : parameterOrNull->findModifier(slang::Modifier::Out) != nullptr ? ReflectedDirection::Out
                                                                                         : ReflectedDirection::In;
                    if (addMember(parameterOrNull, member) == false)
                    {
                        return false;
                    }
                    ++record.MemberCount;
                }
                return addRecord(record);
            }
            case slang::DeclReflection::Kind::Struct: {
                slang::TypeReflection* typeOrNull = declaration->getType();
                if (typeOrNull == nullptr)
                {
                    return true;
                }
                ReflectedDeclaration record;
                record.Kind = DeclarationKind::Struct;
                if (appendText(typeOrNull->getName(), record.Name) == false)
                {
                    return false;
                }
                record.Type.Shape = ReflectedShape::Struct;
                record.Type.Name = record.Name;
                record.FirstMember = static_cast<uint32>(mResult.Members.GetSize());
                for (uint32 i = 0; i < typeOrNull->getFieldCount(); ++i)
                {
                    slang::VariableReflection* fieldOrNull = typeOrNull->getFieldByIndex(i);
                    if (fieldOrNull == nullptr || fieldOrNull->findModifier(slang::Modifier::Static) != nullptr)
                    {
                        continue;
                    }
                    ReflectedMember member;
                    if (addMember(fieldOrNull, member) == false)
                    {
                        return false;
                    }
                    ++record.MemberCount;
                }
                return addRecord(record);
            }
            case slang::DeclReflection::Kind::Variable: {
                slang::VariableReflection* variableOrNull = declaration->asVariable();
                if (variableOrNull == nullptr || variableOrNull->findModifier(slang::Modifier::Static) == nullptr ||
                    variableOrNull->findModifier(slang::Modifier::Const) == nullptr)
                {
                    return true;
                }
                ReflectedDeclaration record;
                record.Kind = DeclarationKind::Constant;
                if (appendText(variableOrNull->getName(), record.Name) == false ||
                    reflectType(variableOrNull->getType(), record.Type) == false)
                {
                    return false;
                }
                record.FirstMember = static_cast<uint32>(mResult.Members.GetSize());
                int64_t value = 0;
                record.HasValue = variableOrNull->hasDefaultValue() == true &&
                                  SLANG_SUCCEEDED(variableOrNull->getDefaultValueInt(&value));
                record.Value = value;
                return addRecord(record);
            }
            case slang::DeclReflection::Kind::Enum: {
                slang::TypeReflection* typeOrNull = declaration->getType();
                if (typeOrNull == nullptr)
                {
                    return true;
                }
                ReflectedDeclaration record;
                record.Kind = DeclarationKind::Enum;
                if (appendText(typeOrNull->getName(), record.Name) == false)
                {
                    return false;
                }
                // Slang enums are 32-bit unless declared otherwise; the reflection API reports no tag type.
                record.Type.Shape = ReflectedShape::Scalar;
                record.Type.Scalar = ReflectedScalar::Uint32;
                record.FirstMember = static_cast<uint32>(mResult.Members.GetSize());
                // Slang reflects an enum's cases as the fields of its type.
                for (uint32 i = 0; i < typeOrNull->getFieldCount(); ++i)
                {
                    slang::VariableReflection* enumeratorOrNull = typeOrNull->getFieldByIndex(i);
                    if (enumeratorOrNull == nullptr)
                    {
                        continue;
                    }
                    ReflectedMember member;
                    int64_t value = 0;
                    member.HasValue = enumeratorOrNull->hasDefaultValue() == true &&
                                      SLANG_SUCCEEDED(enumeratorOrNull->getDefaultValueInt(&value));
                    member.Value = value;
                    if (appendText(enumeratorOrNull->getName(), member.Name) == false ||
                        mResult.Members.GetSize() >= MAX_REFLECTED_MEMBERS || mResult.Members.TryAdd(member) == false)
                    {
                        return false;
                    }
                    ++record.MemberCount;
                }
                return addRecord(record);
            }
            case slang::DeclReflection::Kind::Namespace:
                return AddChildren(declaration);
            default:
                // Generics and declarations Slang does not reflect have no LUSL extern form yet.
                return true;
        }
    }

    [[nodiscard]] bool addRecord(const ReflectedDeclaration& record) noexcept
    {
        return mResult.Declarations.GetSize() < MAX_REFLECTED_DECLARATIONS &&
               mResult.Declarations.TryAdd(record) == true;
    }

    [[nodiscard]] bool addMember(slang::VariableReflection* variable, ReflectedMember& inoutMember) noexcept
    {
        return appendText(variable->getName(), inoutMember.Name) == true &&
               reflectType(variable->getType(), inoutMember.Type) == true &&
               mResult.Members.GetSize() < MAX_REFLECTED_MEMBERS && mResult.Members.TryAdd(inoutMember) == true;
    }

    [[nodiscard]] bool appendText(const char* textOrNull, ReflectedText& outText) noexcept
    {
        const std::string_view text = textOrNull == nullptr ? std::string_view{} : std::string_view(textOrNull);
        outText.Offset = static_cast<uint32>(mResult.Text.GetSize());
        outText.Size = static_cast<uint32>(text.size());
        return mResult.Text.TryAddRange(std::span<const char>(text.data(), text.size())) == true;
    }

    [[nodiscard]] static bool mapScalar(slang::TypeReflection::ScalarType scalar, ReflectedScalar& outScalar) noexcept
    {
        switch (scalar)
        {
            case slang::TypeReflection::ScalarType::Bool:
                outScalar = ReflectedScalar::Bool;
                return true;
            case slang::TypeReflection::ScalarType::Int16:
                outScalar = ReflectedScalar::Int16;
                return true;
            case slang::TypeReflection::ScalarType::UInt16:
                outScalar = ReflectedScalar::Uint16;
                return true;
            case slang::TypeReflection::ScalarType::Int32:
                outScalar = ReflectedScalar::Int32;
                return true;
            case slang::TypeReflection::ScalarType::UInt32:
                outScalar = ReflectedScalar::Uint32;
                return true;
            case slang::TypeReflection::ScalarType::Int64:
                outScalar = ReflectedScalar::Int64;
                return true;
            case slang::TypeReflection::ScalarType::UInt64:
                outScalar = ReflectedScalar::Uint64;
                return true;
            case slang::TypeReflection::ScalarType::Float16:
                outScalar = ReflectedScalar::Float16;
                return true;
            case slang::TypeReflection::ScalarType::Float32:
                outScalar = ReflectedScalar::Float32;
                return true;
            case slang::TypeReflection::ScalarType::Float64:
                outScalar = ReflectedScalar::Float64;
                return true;
            default:
                return false;
        }
    }

    /// The scalar element of a scalar, vector or matrix type.
    [[nodiscard]] static slang::TypeReflection::ScalarType getElementScalar(slang::TypeReflection* type) noexcept
    {
        while (type != nullptr && (type->getKind() == slang::TypeReflection::Kind::Vector ||
                                   type->getKind() == slang::TypeReflection::Kind::Matrix))
        {
            type = type->getElementType();
        }
        return type == nullptr ? slang::TypeReflection::ScalarType::None : type->getScalarType();
    }

    [[nodiscard]] bool reflectType(slang::TypeReflection* typeOrNull, ReflectedType& outType) noexcept
    {
        outType = ReflectedType{};
        if (typeOrNull == nullptr)
        {
            return true;
        }
        slang::TypeReflection* type = typeOrNull;
        if (type->getKind() == slang::TypeReflection::Kind::Array)
        {
            outType.ArrayCount = static_cast<uint32>(type->getElementCount());
            type = type->getElementType();
            if (type == nullptr || type->getKind() == slang::TypeReflection::Kind::Array || outType.ArrayCount == 0)
            {
                outType.ArrayCount = 0;
                return appendText(typeOrNull->getName(), outType.Name);
            }
        }
        const slang::TypeReflection::Kind kind = type->getKind();
        if (kind == slang::TypeReflection::Kind::Scalar || kind == slang::TypeReflection::Kind::Vector ||
            kind == slang::TypeReflection::Kind::Matrix)
        {
            const slang::TypeReflection::ScalarType scalar = getElementScalar(type);
            if (kind == slang::TypeReflection::Kind::Scalar && scalar == slang::TypeReflection::ScalarType::Void)
            {
                outType.Shape = ReflectedShape::Void;
                return true;
            }
            if (mapScalar(scalar, outType.Scalar) == true)
            {
                outType.Shape = kind == slang::TypeReflection::Kind::Scalar   ? ReflectedShape::Scalar
                                : kind == slang::TypeReflection::Kind::Vector ? ReflectedShape::Vector
                                                                              : ReflectedShape::Matrix;
                outType.Rows = static_cast<uint8>(kind == slang::TypeReflection::Kind::Vector ? type->getElementCount()
                                                  : kind == slang::TypeReflection::Kind::Matrix ? type->getRowCount()
                                                                                                : 1);
                outType.Columns =
                    static_cast<uint8>(kind == slang::TypeReflection::Kind::Matrix ? type->getColumnCount() : 1);
                return true;
            }
        }
        if (kind == slang::TypeReflection::Kind::Struct || kind == slang::TypeReflection::Kind::Enum)
        {
            outType.Shape = kind == slang::TypeReflection::Kind::Struct ? ReflectedShape::Struct : ReflectedShape::Enum;
            return appendText(type->getName(), outType.Name);
        }
        const char* nameOrNull = type->getName();
        if (nameOrNull != nullptr && std::string_view(nameOrNull) == "void")
        {
            outType.Shape = ReflectedShape::Void;
            return true;
        }
        return appendText(nameOrNull, outType.Name);
    }

    DeclarationResult& mResult;
};
} // namespace

Compiler::Compiler() noexcept = default;
Compiler::~Compiler() noexcept = default;
Status Compiler::Initialize() noexcept
{
    if (mState.Get() != nullptr)
    {
        return Status::InvalidState;
    }
    State* allocationOrNull = new (std::nothrow) State;
    core::UniquePtr<State> state{allocationOrNull};
    if (state.Get() == nullptr)
    {
        return Status::OutOfMemory;
    }
    if (SLANG_FAILED(slang::createGlobalSession(state->Global.writeRef())) ||
        std::string_view(state->Global->getBuildTagString()) != LUDUS_SLANG_VERSION)
    {
        return Status::Unavailable;
    }
    // Slang loads its SPIR-V optimizer (the pinned slang-glslang library) on demand. When that
    // load fails it emits unoptimized SPIR-V without a diagnostic, so live output would silently
    // differ from slangc's build artifacts. Refuse such a compiler instead.
    if (SLANG_FAILED(state->Global->checkPassThroughSupport(SLANG_PASS_THROUGH_SPIRV_OPT)))
    {
        return Status::Unavailable;
    }
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    // DXIL is the Windows worker's native output. Refuse a compiler that cannot reach the pinned
    // DXC deployed beside the executable rather than failing each Direct3D 12 request later.
    PathText directory;
    if (GetExecutableDirectory(directory) == false)
    {
        return Status::Unavailable;
    }
    state->Global->setDownstreamCompilerPath(SLANG_PASS_THROUGH_DXC, directory.GetData());
    if (SLANG_FAILED(state->Global->checkPassThroughSupport(SLANG_PASS_THROUGH_DXC)))
    {
        return Status::Unavailable;
    }
#endif
    mState = Move(state);
    return Status::Ready;
}
Status Compiler::Compile(const Request& request, Result& outResult) noexcept
{
    outResult = Result{};
    outResult.Identity = request.Identity;
    if (mState.Get() == nullptr)
    {
        outResult.Outcome = Status::Unavailable;
        return outResult.Outcome;
    }
    if (internal::IsValid(request) == false)
    {
        return outResult.Outcome;
    }
    if (internal::IsLanguageSource(GetText(request.Source)) == false)
    {
        return compileSlang(request, outResult);
    }
    internal::LanguageLowering lowering;
    if (lowering.Lower(request, outResult) == false)
    {
        return outResult.Outcome;
    }
    static_cast<void>(compileSlang(lowering.GetSlangRequest(), outResult));
    lowering.Finish(request, outResult);
    outResult.Identity = request.Identity;
    return outResult.Outcome;
}
Status Compiler::ReflectDeclarations(const Request& request, DeclarationResult& outResult) noexcept
{
    outResult = DeclarationResult{};
    if (mState.Get() == nullptr)
    {
        outResult.Outcome = Status::Unavailable;
        return outResult.Outcome;
    }
    if (internal::IsValid(request) == false || internal::IsLanguageSource(GetText(request.Source)) == true)
    {
        return outResult.Outcome;
    }
    State& state = *mState;
    outResult.Outcome = state.Capture(request);
    if (outResult.Outcome != Status::Ready)
    {
        return outResult.Outcome;
    }
    std::string_view sourceText;
    for (usize i = 0; i < request.Files.GetSize(); ++i)
    {
        if (GetText(request.Files[i].Path) == GetText(request.Source))
        {
            sourceText = GetText(request.Files[i].Text);
        }
    }
    // Slang reads the module text from a terminated copy; the snapshot holds the same bytes.
    Array<char> source;
    if (source.TryAddRange(std::span<const char>(sourceText.data(), sourceText.size())) == false ||
        source.TryAdd('\0') == false)
    {
        outResult.Outcome = Status::OutOfMemory;
        return outResult.Outcome;
    }
    outResult.Outcome = Status::CompileFailed;
    Slang::ComPtr<slang::IBlob> diagnostics;
    slang::IModule* moduleOrNull = state.Session->loadModuleFromSourceString("ludus_reflected_declarations",
                                                                             state.Source.GetData(),
                                                                             source.GetData(),
                                                                             diagnostics.writeRef());
    if (diagnostics.get() != nullptr)
    {
        const usize count =
            diagnostics->getBufferSize() < MAX_DIAGNOSTIC_BYTES ? diagnostics->getBufferSize() : MAX_DIAGNOSTIC_BYTES;
        static_cast<void>(CopyBytes(diagnostics->getBufferPointer(), count, outResult.Diagnostics));
    }
    slang::DeclReflection* rootOrNull = moduleOrNull == nullptr ? nullptr : moduleOrNull->getModuleReflection();
    if (rootOrNull == nullptr)
    {
        return outResult.Outcome;
    }
    DeclarationReflector reflector(outResult);
    if (reflector.AddChildren(rootOrNull) == false)
    {
        outResult.Declarations.Clear();
        outResult.Members.Clear();
        outResult.Text.Clear();
        outResult.Outcome = Status::InvalidRequest;
        return outResult.Outcome;
    }
    // A later snapshot replaces the session; the module name is reused within this one.
    state.Session.setNull();
    outResult.Outcome = Status::Ready;
    return outResult.Outcome;
}
Status Compiler::compileSlang(const Request& request, Result& outResult) noexcept
{
#if LUDUS_TARGET_OS != LUDUS_OS_WINDOWS
    if (request.OutputTarget == Target::Dxil)
    {
        // The pinned DXC and its signing validator are Windows libraries.
        outResult.Outcome = Status::Unavailable;
        return outResult.Outcome;
    }
#endif
    State& state = *mState;
    outResult.Outcome = state.Capture(request);
    if (outResult.Outcome != Status::Ready)
    {
        return outResult.Outcome;
    }
    Slang::ComPtr<slang::IBlob> diagnostics;
    const auto captureDiagnostics = [&]() {
        if (diagnostics.get() != nullptr)
        {
            const usize count = diagnostics->getBufferSize() < MAX_DIAGNOSTIC_BYTES ? diagnostics->getBufferSize()
                                                                                    : MAX_DIAGNOSTIC_BYTES;
            static_cast<void>(CopyBytes(diagnostics->getBufferPointer(), count, outResult.Diagnostics));
            diagnostics.setNull();
        }
    };
    outResult.Outcome = Status::CompileFailed;
    EntryText entry;
    static_cast<void>(entry.TryAssign(GetText(request.Entry)));
    const SlangStage stage = request.EntryStage == Stage::Vertex     ? SLANG_STAGE_VERTEX
                             : request.EntryStage == Stage::Fragment ? SLANG_STAGE_FRAGMENT
                                                                     : SLANG_STAGE_COMPUTE;
    Slang::ComPtr<slang::ICompileRequest> compilation;
    if (SLANG_FAILED(state.Session->createCompileRequest(compilation.writeRef())))
    {
        return outResult.Outcome;
    }
    const int32 unit = compilation->addTranslationUnit(SLANG_SOURCE_LANGUAGE_SLANG, nullptr);
    compilation->addTranslationUnitSourceFile(unit, state.Source.GetData());
    static_cast<void>(compilation->addEntryPoint(unit, entry.GetData(), stage));
    SlangResult outcome = compilation->compile();
    static_cast<void>(compilation->getDiagnosticOutputBlob(diagnostics.writeRef()));
    captureDiagnostics();
    if (SLANG_FAILED(outcome))
    {
        return outResult.Outcome;
    }
    Slang::ComPtr<slang::IComponentType> composite;
    outcome = compilation->getProgramWithEntryPoints(composite.writeRef());
    if (SLANG_FAILED(outcome))
    {
        return outResult.Outcome;
    }
    Slang::ComPtr<slang::IComponentType> linked;
    outcome = composite->link(linked.writeRef(), diagnostics.writeRef());
    captureDiagnostics();
    if (SLANG_FAILED(outcome))
    {
        return outResult.Outcome;
    }
    const SlangInt target = GetSessionProfile(request.OutputTarget) == SessionProfile::Portable
                                ? static_cast<SlangInt>(request.OutputTarget)
                                : 0;
    slang::ProgramLayout* layoutOrNull = linked->getLayout(target, diagnostics.writeRef());
    captureDiagnostics();
    Slang::ComPtr<slang::IBlob> reflection;
    Slang::ComPtr<slang::IBlob> artifact;
    if (layoutOrNull == nullptr || SLANG_FAILED(layoutOrNull->toJson(reflection.writeRef())))
    {
        return outResult.Outcome;
    }
    outcome = linked->getEntryPointCode(0, target, artifact.writeRef(), diagnostics.writeRef());
    captureDiagnostics();
    if (SLANG_FAILED(outcome))
    {
        return outResult.Outcome;
    }
    if (artifact->getBufferSize() > MAX_ARTIFACT_BYTES || reflection->getBufferSize() > MAX_ARTIFACT_BYTES)
    {
        outResult.Outcome = Status::InvalidRequest;
        return outResult.Outcome;
    }
    if (request.OutputTarget == Target::Dxil &&
        IsSignedDxilContainer(artifact->getBufferPointer(), artifact->getBufferSize()) == false)
    {
        static_cast<void>(
            CopyBytes(UNSIGNED_DXIL_DIAGNOSTIC.data(), UNSIGNED_DXIL_DIAGNOSTIC.size(), outResult.Diagnostics));
        outResult.Outcome = Status::Unavailable;
        return outResult.Outcome;
    }
    Slang::ComPtr<slang::IMetadata> metadata;
    outcome = linked->getEntryPointMetadata(0, target, metadata.writeRef(), diagnostics.writeRef());
    captureDiagnostics();
    if (SLANG_FAILED(outcome) || metadata.get() == nullptr)
    {
        return outResult.Outcome;
    }
    if (CopyBytes(artifact->getBufferPointer(), artifact->getBufferSize(), outResult.Artifact) == false ||
        WriteReflection(*layoutOrNull, *metadata.get(), *reflection.get(), outResult.Reflection) == false)
    {
        outResult.Artifact.Clear();
        outResult.Reflection.Clear();
        outResult.Outcome = Status::OutOfMemory;
        return outResult.Outcome;
    }
    if (outResult.SourceReadMask.TryResize(state.Filesystem->Files.GetSize()) == false)
    {
        outResult.Artifact.Clear();
        outResult.Reflection.Clear();
        outResult.Outcome = Status::OutOfMemory;
        return outResult.Outcome;
    }
    for (usize i = 0; i < state.Filesystem->Files.GetSize(); ++i)
    {
        outResult.SourceReadMask[i] = state.Filesystem->Files[i].HasBeenRead == true ? 1 : 0;
    }
    outResult.Outcome = Status::Ready;
    return outResult.Outcome;
}
} // namespace ludus::tooling::shader_compiler
