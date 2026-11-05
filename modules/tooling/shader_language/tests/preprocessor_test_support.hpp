#pragma once

#include <ludus/foundation/containers/array.hpp>
#include <ludus/tooling/shader_language/preprocessor.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace ludus::tooling::shader_language::test
{
/// A snapshot built from owned strings; views into it stay valid while the object lives.
class Snapshot final
{
public:
    Snapshot() = default;
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;

    Snapshot& AddFile(std::string path, std::string text)
    {
        mPaths.Add(std::move(path));
        mTexts.Add(std::move(text));
        return *this;
    }

    Snapshot& AddConfiguration(std::string name, std::string value)
    {
        mNames.Add(std::move(name));
        mValues.Add(std::move(value));
        return *this;
    }

    /// Preprocess `root`; the result stays valid until the next call.
    Status Run(std::string_view root = "main.lusl")
    {
        mRoot = root;
        mFiles.Clear();
        for (foundation::usize i = 0; i < mPaths.GetSize(); ++i)
        {
            shader_compiler::SourceFile file;
            file.Path = foundation::BorrowedView<const char>(mPaths[i].data(), mPaths[i].size());
            file.Text = foundation::BorrowedView<const char>(mTexts[i].data(), mTexts[i].size());
            mFiles.Add(file);
        }
        mConfiguration.Clear();
        for (foundation::usize i = 0; i < mNames.GetSize(); ++i)
        {
            ConfigurationValue value;
            value.Name = foundation::BorrowedView<const char>(mNames[i].data(), mNames[i].size());
            value.Value = foundation::BorrowedView<const char>(mValues[i].data(), mValues[i].size());
            mConfiguration.Add(value);
        }
        mRequest.Files =
            foundation::BorrowedView<const shader_compiler::SourceFile>(mFiles.GetData(), mFiles.GetSize());
        mRequest.Source = foundation::BorrowedView<const char>(mRoot.data(), mRoot.size());
        mRequest.Configuration =
            foundation::BorrowedView<const ConfigurationValue>(mConfiguration.GetData(), mConfiguration.GetSize());
        return Preprocess(mRequest, mResult);
    }

    [[nodiscard]] const PreprocessRequest& GetRequest() const
    {
        return mRequest;
    }

    [[nodiscard]] const PreprocessResult& GetResult() const
    {
        return mResult;
    }

    /// Output spellings separated by single spaces.
    [[nodiscard]] std::string GetText() const
    {
        std::string text;
        for (const Token& token : mResult.Tokens)
        {
            if (text.empty() == false)
            {
                text += ' ';
            }
            text += GetTokenSpelling(mRequest, mResult, token);
        }
        return text;
    }

    /// True when a diagnostic with `code` was reported.
    [[nodiscard]] bool HasDiagnostic(DiagnosticCode code) const
    {
        for (const Diagnostic& diagnostic : mResult.Diagnostics)
        {
            if (diagnostic.Code == code)
            {
                return true;
            }
        }
        return false;
    }

    /// The first diagnostic with `code`, or null.
    [[nodiscard]] const Diagnostic* FindDiagnosticOrNull(DiagnosticCode code) const
    {
        for (const Diagnostic& diagnostic : mResult.Diagnostics)
        {
            if (diagnostic.Code == code)
            {
                return &diagnostic;
            }
        }
        return nullptr;
    }

private:
    foundation::Array<std::string> mPaths;
    foundation::Array<std::string> mTexts;
    foundation::Array<std::string> mNames;
    foundation::Array<std::string> mValues;
    foundation::Array<shader_compiler::SourceFile> mFiles;
    foundation::Array<ConfigurationValue> mConfiguration;
    std::string mRoot;
    PreprocessRequest mRequest;
    PreprocessResult mResult;
};
} // namespace ludus::tooling::shader_language::test
