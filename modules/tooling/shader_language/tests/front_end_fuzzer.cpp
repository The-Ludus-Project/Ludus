// libFuzzer entry point: the preprocessor, parser and formatter must accept or reject any input
// without crashing. The input is split at the first zero byte into a root module and one header it
// may include; the root is also formatted.

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/tooling/shader_language/formatter.hpp>
#include <ludus/tooling/shader_language/preprocessor.hpp>
#include <ludus/tooling/shader_language/syntax.hpp>

#include <string_view>

namespace
{
using ludus::foundation::BorrowedView;
using ludus::foundation::uint8;
using ludus::foundation::usize;
namespace shader_language = ludus::tooling::shader_language;

constexpr std::string_view ROOT_PATH = "main.lusl";
constexpr std::string_view HEADER_PATH = "h.luslh";
constexpr std::string_view MODE_NAME = "MODE";
constexpr std::string_view MODE_VALUE = "1 + A";
/// Files in the fuzzed snapshot.
constexpr usize FILE_COUNT = 2;
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8* data, usize size)
{
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    const usize split = input.find('\0');
    const std::string_view root = split == std::string_view::npos ? input : input.substr(0, split);
    const std::string_view header = split == std::string_view::npos ? std::string_view{} : input.substr(split + 1);
    ludus::foundation::StaticArray<ludus::tooling::shader_compiler::SourceFile, FILE_COUNT> files{};
    files[0].Path = BorrowedView<const char>(ROOT_PATH.data(), ROOT_PATH.size());
    files[0].Text = BorrowedView<const char>(root.data(), root.size());
    files[1].Path = BorrowedView<const char>(HEADER_PATH.data(), HEADER_PATH.size());
    files[1].Text = BorrowedView<const char>(header.data(), header.size());
    shader_language::ConfigurationValue mode;
    mode.Name = BorrowedView<const char>(MODE_NAME.data(), MODE_NAME.size());
    mode.Value = BorrowedView<const char>(MODE_VALUE.data(), MODE_VALUE.size());
    shader_language::PreprocessRequest request;
    request.Files = BorrowedView<const ludus::tooling::shader_compiler::SourceFile>(files.GetData(), FILE_COUNT);
    request.Source = BorrowedView<const char>(ROOT_PATH.data(), ROOT_PATH.size());
    request.Configuration = BorrowedView<const shader_language::ConfigurationValue>(&mode, 1);
    shader_language::PreprocessResult result;
    if (shader_language::Preprocess(request, result) == shader_language::Status::Ready)
    {
        shader_language::SyntaxTree tree;
        (void)shader_language::Parse(request, result, tree);
    }
    shader_language::FormatResult formatted;
    (void)shader_language::FormatSource(root, formatted);
    return 0;
}
