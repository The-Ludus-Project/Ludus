#include <ludus/tooling/shader_compiler/compiler.hpp>

#include "codec.hpp"

#include <ludus/foundation/base/compiler.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/parsing/json.hpp>
#include <ludus/foundation/strings/static_string.hpp>

#include <cstring>
#include <new>
#include <string_view>

#include <slang-com-ptr.h>
#include <slang.h>

// Thanks to the Slang authors, "Using the Slang Compilation API", sections
// "About Sessions" and "Load Modules", https://docs.shader-slang.org/en/latest/compilation-api.html.
// Reuse the global compiler and an unchanged snapshot, but replace the session on
// any source edit. A closed in-memory filesystem prevents uncaptured disk inputs.
namespace ludus::tooling::shader_compiler
{
using namespace foundation;
namespace
{
constexpr usize TARGET_COUNT = 3;
constexpr usize COMPONENT_COUNT = 2;
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
    Slang::ComPtr<slang::IModule> Module;
    PathText Source;
    bool HasNativeRayQueries = false;

    Status Capture(const Request& request) noexcept
    {
        const bool hasNativeRayQueries = request.OutputTarget == Target::SpirvRayQuery;
        if (Session.get() != nullptr && HasNativeRayQueries == hasNativeRayQueries &&
            Source.GetView() == GetText(request.Source) && Filesystem->Matches(request) == true)
        {
            return Status::Ready;
        }
        Module.setNull();
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
        StaticArray<slang::TargetDesc, TARGET_COUNT> targets{};
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
        slang::SessionDesc description;
        description.targets = targets.GetData();
        description.targetCount = hasNativeRayQueries == true ? 1 : static_cast<SlangInt>(TARGET_COUNT);
        description.fileSystem = Filesystem.get();
        HasNativeRayQueries = hasNativeRayQueries;
        return SLANG_SUCCEEDED(Global->createSession(description, Session.writeRef())) ? Status::Ready
                                                                                       : Status::Unavailable;
    }
};
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
    if (state.Module.get() == nullptr)
    {
        state.Module =
            Slang::ComPtr<slang::IModule>(state.Session->loadModule(state.Source.GetData(), diagnostics.writeRef()));
        captureDiagnostics();
    }
    outResult.Outcome = Status::CompileFailed;
    if (state.Module.get() == nullptr)
    {
        return outResult.Outcome;
    }
    EntryText entry;
    static_cast<void>(entry.TryAssign(GetText(request.Entry)));
    const SlangStage stage = request.EntryStage == Stage::Vertex     ? SLANG_STAGE_VERTEX
                             : request.EntryStage == Stage::Fragment ? SLANG_STAGE_FRAGMENT
                                                                     : SLANG_STAGE_COMPUTE;
    Slang::ComPtr<slang::IEntryPoint> point;
    SlangResult outcome =
        state.Module->findAndCheckEntryPoint(entry.GetData(), stage, point.writeRef(), diagnostics.writeRef());
    captureDiagnostics();
    if (SLANG_FAILED(outcome))
    {
        return outResult.Outcome;
    }
    StaticArray<slang::IComponentType*, COMPONENT_COUNT> components{state.Module.get(), point.get()};
    Slang::ComPtr<slang::IComponentType> composite;
    outcome = state.Session->createCompositeComponentType(components.GetData(),
                                                          static_cast<SlangInt>(COMPONENT_COUNT),
                                                          composite.writeRef(),
                                                          diagnostics.writeRef());
    captureDiagnostics();
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
    const SlangInt target =
        request.OutputTarget == Target::SpirvRayQuery ? 0 : static_cast<SlangInt>(request.OutputTarget);
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
