// L0 two-generation feasibility acceptance (tasks.md L0, design 5/6/7).
//
// Proves: A and B load concurrently from distinct immutable paths; each runs its
// own code (distinct identity + distinct Update output); the entry symbol has
// hidden visibility (not globally exported; on Windows, the DLL's sole export); and
// missing-entry / wrong-identity
// modules are rejected before Create. This is the gate the dependent reload
// phases build on.

#include "fixture_environment.h"
#include "internal/identity.h"
#include "internal/module_loader.h"

#include <ludus/runtime/game_api/api.h>
#include <ludus/runtime/game_host/host.h>

#include <catch2/catch_test_macros.hpp>

#include <ludus/foundation/base/config.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
#    include <windows.h>
#else
#    include <dlfcn.h>
#endif

using namespace ludus::runtime::game_host;
using ludus::runtime::game_api::CreateInfo;
using ludus::runtime::game_api::FrameInput;
using ludus::runtime::game_api::GameInstance;
using ludus::runtime::game_api::RenderParams;
using ludus::runtime::game_api::Status;
using ludus::runtime::game_host::tests::SetFixtureFailure;

namespace
{
LoadStatus Load(const char* path, LoadedModule& out, ludus::foundation::uint64 generation)
{
    return LoadModule(path, generation, CurrentHostIdentity(), CurrentAbiMajor(), CurrentAbiMinor(), out);
}
} // namespace

TEST_CASE("matching module A loads, queries and creates", "[loader]")
{
    LoadedModule module;
    REQUIRE(Load(LUDUS_FIXTURE_A_PATH, module, 1) == LoadStatus::Ok);
    REQUIRE(module.IsLoaded());
    REQUIRE(module.Metadata().AbiMajor == CurrentAbiMajor());
    REQUIRE(module.Table().Create != nullptr);
}

TEST_CASE("A and B coexist and run their own code", "[loader]")
{
    LoadedModule a;
    LoadedModule b;
    REQUIRE(Load(LUDUS_FIXTURE_A_PATH, a, 1) == LoadStatus::Ok);
    REQUIRE(Load(LUDUS_FIXTURE_B_PATH, b, 2) == LoadStatus::Ok);

    // Distinct immutable paths and distinct loader handles.
    REQUIRE(a.Path() != b.Path());

    // Create an instance in each and advance one frame; variant B renders a
    // distinctly brighter red (its own code), proving each ran its own copy.
    CreateInfo info = {};
    info.StructSize = static_cast<ludus::foundation::uint32>(sizeof(CreateInfo));

    GameInstance* ia = nullptr;
    GameInstance* ib = nullptr;
    REQUIRE(a.Table().Create(&info, &ia) == Status::Ok);
    REQUIRE(b.Table().Create(&info, &ib) == Status::Ok);

    FrameInput frame = {};
    frame.DeltaSeconds = 1.0 / 60.0;
    RenderParams ra = {};
    RenderParams rb = {};
    REQUIRE(a.Table().Update(ia, &frame, &ra) == Status::Ok);
    REQUIRE(b.Table().Update(ib, &frame, &rb) == Status::Ok);
    REQUIRE(rb.ClearRed > ra.ClearRed + 0.3F); // B's variant boost
    REQUIRE(rb.Uniform3 == 1.0F);
    REQUIRE(ra.Uniform3 == 0.0F);

    a.Table().Destroy(ia);
    b.Table().Destroy(ib);
}

#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
TEST_CASE("module DLL exports exactly the entry symbol", "[loader]")
{
    // A DLL exports only what it names: the entry must be its sole export, and it must
    // not appear in the executable's own export table.
    HMODULE module = ::LoadLibraryExW(std::filesystem::path(LUDUS_FIXTURE_A_PATH).wstring().c_str(),
                                      nullptr,
                                      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    REQUIRE(module != nullptr);
    REQUIRE(::GetProcAddress(module, "LudusGetGameApi") != nullptr);
    const unsigned char* base = reinterpret_cast<const unsigned char*>(module);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    REQUIRE(dos->e_magic == IMAGE_DOS_SIGNATURE);
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    REQUIRE(nt->Signature == IMAGE_NT_SIGNATURE);
    const IMAGE_DATA_DIRECTORY& exports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    REQUIRE(exports.VirtualAddress != 0);
    const IMAGE_EXPORT_DIRECTORY* directory =
        reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + exports.VirtualAddress);
    REQUIRE(directory->NumberOfNames == 1);
    const DWORD* names = reinterpret_cast<const DWORD*>(base + directory->AddressOfNames);
    CHECK(std::strcmp(reinterpret_cast<const char*>(base + names[0]), "LudusGetGameApi") == 0);
    CHECK(::GetProcAddress(::GetModuleHandleW(nullptr), "LudusGetGameApi") == nullptr);
    REQUIRE(::FreeLibrary(module) != FALSE);
}

TEST_CASE("a closed generation is unmapped and its file is released", "[loader][reload]")
{
    // Windows locks a loaded DLL, so each generation is a distinct file. Closing the
    // last reference must unmap it and release the file for deletion.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("ludus-loader-" + std::to_string(::GetCurrentProcessId()));
    std::filesystem::remove_all(root);
    REQUIRE(std::filesystem::create_directories(root));
    const std::filesystem::path generation = root / "generation-1.dll";
    std::filesystem::copy_file(LUDUS_FIXTURE_A_PATH, generation);
    // LoadModule takes UTF-8; std::filesystem::path::string() uses the ANSI code page here.
    const std::u8string utf8 = generation.u8string();
    const std::string path(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    LoadedModule module;
    REQUIRE(Load(path.c_str(), module, 1) == LoadStatus::Ok);
    CHECK(::GetModuleHandleW(generation.wstring().c_str()) != nullptr);
    CHECK(::DeleteFileW(generation.wstring().c_str()) == FALSE);
    REQUIRE(module.Close());
    CHECK(::GetModuleHandleW(generation.wstring().c_str()) == nullptr);
    CHECK(::DeleteFileW(generation.wstring().c_str()) != FALSE);
    std::filesystem::remove_all(root);
}
#else
TEST_CASE("module entry symbol has hidden visibility", "[loader]")
{
    // The entry must resolve from the module's own handle but NOT leak into the
    // global namespace. Open with RTLD_LOCAL and confirm a global lookup fails.
    void* handle = ::dlopen(LUDUS_FIXTURE_A_PATH, RTLD_NOW | RTLD_LOCAL);
    REQUIRE(handle != nullptr);
    REQUIRE(::dlsym(handle, "LudusGetGameApi") != nullptr); // visible via the handle

    // A fresh RTLD_LOCAL open of a different library must not see A's symbol in
    // the global scope: look it up in the default (global) namespace handle.
    void* global = ::dlopen(nullptr, RTLD_NOW);
    REQUIRE(global != nullptr);
    REQUIRE(::dlsym(global, "LudusGetGameApi") == nullptr);
    ::dlclose(global);
    ::dlclose(handle);
}
#endif

TEST_CASE("missing entry symbol is rejected explicitly", "[loader]")
{
    LoadedModule module;
    REQUIRE(Load(LUDUS_FIXTURE_NOENTRY_PATH, module, 1) == LoadStatus::EntryMissing);
    REQUIRE_FALSE(module.IsLoaded());
}

TEST_CASE("wrong identity is rejected before create", "[loader]")
{
    LoadedModule module;
    REQUIRE(Load(LUDUS_FIXTURE_BADID_PATH, module, 1) == LoadStatus::IdentityRejected);
    REQUIRE_FALSE(module.IsLoaded());
}

TEST_CASE("non-absolute path is rejected", "[loader]")
{
    LoadedModule module;
    REQUIRE(Load("relative/path.so", module, 1) == LoadStatus::PathInvalid);
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    // Drive-relative, rooted-without-drive and UNC names are not leased generation paths.
    REQUIRE(Load("C:relative.dll", module, 1) == LoadStatus::PathInvalid);
    REQUIRE(Load("\\generation.dll", module, 1) == LoadStatus::PathInvalid);
    REQUIRE(Load("\\\\server\\share\\generation.dll", module, 1) == LoadStatus::PathInvalid);
#endif
}

TEST_CASE("host Run drives the mandatory lifecycle headlessly", "[loader]")
{
    HostConfig config;
    config.Source = GameplaySource::DynamicModule;
    config.Mode = Presentation::Headless;
    config.ModulePath = LUDUS_FIXTURE_A_PATH;
    config.MaxFrames = 120;
    REQUIRE(Run(config) == RunResult::Ok);
}

TEST_CASE("host Run rejects an incompatible module", "[loader]")
{
    HostConfig config;
    config.Source = GameplaySource::DynamicModule;
    config.Mode = Presentation::Headless;
    config.ModulePath = LUDUS_FIXTURE_BADID_PATH;
    config.MaxFrames = 1;
    REQUIRE(Run(config) == RunResult::IncompatibleModule);
}

TEST_CASE("host Run distinguishes a missing module from an incompatible module", "[loader]")
{
    HostConfig config;
    config.Source = GameplaySource::DynamicModule;
    config.Mode = Presentation::Headless;
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    const std::u8string missingUtf8 =
        (std::filesystem::temp_directory_path() / "ludus-missing-live-reload-module.dll").u8string();
    const std::string missing(reinterpret_cast<const char*>(missingUtf8.data()), missingUtf8.size());
    config.ModulePath = missing;
#else
    config.ModulePath = "/tmp/ludus-missing-live-reload-module.so";
#endif
    config.MaxFrames = 1;
    REQUIRE(Run(config) == RunResult::ModuleLoadFailed);
}

TEST_CASE("ScriptDebug requires minor 1 and a complete negotiated callback", "[loader][scripting]")
{
    REQUIRE(std::getenv("LUDUS_FIXTURE_FAIL") == nullptr);
    const char* cases[] = {"script-debug-null", "script-debug-truncated", "script-debug-minor"};
    for (const char* failure : cases)
    {
        REQUIRE(SetFixtureFailure(failure));
        LoadedModule module;
        const LoadStatus result = Load(LUDUS_FIXTURE_A_PATH, module, 1);
        (void)SetFixtureFailure(nullptr);
        CHECK(result == LoadStatus::MetadataInconsistent);
        CHECK_FALSE(module.IsLoaded());
    }
}

TEST_CASE("ABI 1.0 module without ScriptDebug retains its common prefix", "[loader][scripting]")
{
    REQUIRE(std::getenv("LUDUS_FIXTURE_FAIL") == nullptr);
    REQUIRE(SetFixtureFailure("script-debug-legacy"));
    LoadedModule module;
    const LoadStatus result = Load(LUDUS_FIXTURE_A_PATH, module, 1);
    (void)SetFixtureFailure(nullptr);
    REQUIRE(result == LoadStatus::Ok);
    CHECK(module.Metadata().AbiMinor == 0);
    CHECK(module.Table().StructSize == offsetof(ludus::runtime::game_api::GameApiTable, ProcessScriptDebug));
    CHECK(module.Table().ProcessScriptDebug == nullptr);
}
