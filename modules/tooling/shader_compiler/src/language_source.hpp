#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/strings/static_string.hpp>
#include <ludus/tooling/shader_compiler/compiler.hpp>
#include <ludus/tooling/shader_language/pipeline.hpp>

#include <string_view>

namespace ludus::tooling::shader_compiler::internal
{
/// True when `source` names a LUSL module, which Compile lowers before Slang sees it.
[[nodiscard]] bool IsLanguageSource(std::string_view source) noexcept;

/// @brief Lowers a `.lusl` Source and prepares the Slang request for its generated module
/// (docs/modules/tooling/shader-language.md, "ShaderCompiler").
///
/// The generated module joins the snapshot at `Source + ".slang"`, replacing any file there, so
/// its `#include` directives resolve beside the LUSL source. The prepared request borrows this
/// object and the original request; both must outlive its compilation.
class LanguageLowering final
{
public:
    /// @brief Lower `request`.
    /// @return False when lowering failed; `outResult` then holds the outcome and the LUSL
    /// diagnostics, and no Slang compilation should run.
    [[nodiscard]] bool Lower(const Request& request, Result& outResult) noexcept;

    /// The Slang request for the generated module, valid after a successful Lower.
    [[nodiscard]] const Request& GetSlangRequest() const noexcept
    {
        return mRequest;
    }

    /// @brief Complete `inoutResult` of the Slang compilation for `request`.
    ///
    /// Puts LUSL warnings before Slang's diagnostics, and on Ready maps the read mask back to
    /// `request`'s files, adding the files the LUSL preprocessor read, and attaches the configuration
    /// read mask and the generated module.
    void Finish(const Request& request, Result& inoutResult) noexcept;

private:
    using PathText = foundation::StaticString<foundation::filesystem::MAX_PATH_BYTES>;

    shader_language::LoweringResult mLowered;
    foundation::Array<SourceFile> mFiles;
    /// Index in mFiles of each request file, or the request's file count for the one replaced.
    foundation::Array<foundation::usize> mFileIndices;
    PathText mGeneratedPath;
    Request mRequest;
};
} // namespace ludus::tooling::shader_compiler::internal
