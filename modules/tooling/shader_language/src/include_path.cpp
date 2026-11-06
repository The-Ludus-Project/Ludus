#include "internal/include_path.hpp"

#include <span>

namespace ludus::tooling::shader_language::internal
{
using foundation::usize;

namespace
{
/// Remove the last component of `inoutPath`; false when it has none.
[[nodiscard]] bool RemoveLastComponent(foundation::Array<char>& inoutPath) noexcept
{
    if (inoutPath.IsEmpty() == true)
    {
        return false;
    }
    while (inoutPath.IsEmpty() == false && inoutPath.GetLast() != '/')
    {
        inoutPath.RemoveLast();
    }
    if (inoutPath.IsEmpty() == false)
    {
        inoutPath.RemoveLast();
    }
    return true;
}

/// Append the components of `path` to `inoutPath`, applying `.` and `..`.
[[nodiscard]] IncludePathOutcome
AppendComponents(std::string_view path, bool isDirectoryOnly, foundation::Array<char>& inoutPath) noexcept
{
    usize start = 0;
    while (start <= path.size())
    {
        usize end = path.find('/', start);
        const bool isLast = end == std::string_view::npos;
        if (isLast == true)
        {
            end = path.size();
        }
        const std::string_view component = path.substr(start, end - start);
        if (isLast == true && isDirectoryOnly == true)
        {
            return IncludePathOutcome::Ok;
        }
        if (component.empty() == true)
        {
            return IncludePathOutcome::Invalid;
        }
        if (component == "..")
        {
            if (RemoveLastComponent(inoutPath) == false)
            {
                return IncludePathOutcome::Invalid;
            }
        }
        else if (component != ".")
        {
            if (inoutPath.IsEmpty() == false && inoutPath.TryAdd('/') == false)
            {
                return IncludePathOutcome::OutOfMemory;
            }
            if (inoutPath.TryAddRange(std::span<const char>(component.data(), component.size())) == false)
            {
                return IncludePathOutcome::OutOfMemory;
            }
        }
        if (isLast == true)
        {
            break;
        }
        start = end + 1;
    }
    return IncludePathOutcome::Ok;
}
} // namespace

IncludePathOutcome
ResolveIncludePath(std::string_view includingFile, std::string_view path, foundation::Array<char>& outPath) noexcept
{
    outPath.Clear();
    if (path.empty() == true || path.front() == '/' || path.find('\\') != std::string_view::npos ||
        path.find('\0') != std::string_view::npos)
    {
        return IncludePathOutcome::Invalid;
    }
    if (includingFile.empty() == false)
    {
        const IncludePathOutcome directory = AppendComponents(includingFile, true, outPath);
        if (directory != IncludePathOutcome::Ok)
        {
            return directory;
        }
    }
    const IncludePathOutcome joined = AppendComponents(path, false, outPath);
    if (joined == IncludePathOutcome::Ok && outPath.IsEmpty() == true)
    {
        return IncludePathOutcome::Invalid;
    }
    return joined;
}

namespace
{
[[nodiscard]] std::string_view ViewOf(const foundation::BorrowedView<const char>& view) noexcept
{
    return view.IsValid() == true ? std::string_view(view.GetDataOrNull(), view.GetSize()) : std::string_view{};
}

/// Look `inoutPath` up in the request: Ok with `outIndex`, or NotFound.
[[nodiscard]] IncludePathOutcome
LookUp(const PreprocessRequest& request, const foundation::Array<char>& path, foundation::uint16& outIndex) noexcept
{
    const std::string_view text(path.GetData(), path.GetSize());
    for (usize i = 0; i < request.Files.GetSize(); ++i)
    {
        if (ViewOf(request.Files[i].Path) == text)
        {
            outIndex = static_cast<foundation::uint16>(i);
            return IncludePathOutcome::Ok;
        }
    }
    return IncludePathOutcome::NotFound;
}

/// Combine the outcomes of two candidates: a hit or an allocation failure decides; NotFound beats
/// Invalid, so a path that is valid somewhere is reported as missing rather than malformed.
[[nodiscard]] IncludePathOutcome Combine(IncludePathOutcome first, IncludePathOutcome second) noexcept
{
    if (first == IncludePathOutcome::NotFound || second == IncludePathOutcome::NotFound)
    {
        return IncludePathOutcome::NotFound;
    }
    return second;
}
} // namespace

IncludePathOutcome FindSnapshotFile(const PreprocessRequest& request,
                                    std::string_view includingFile,
                                    std::string_view path,
                                    foundation::Array<char>& inoutScratch,
                                    foundation::uint16& outIndex) noexcept
{
    IncludePathOutcome outcome = IncludePathOutcome::Invalid;
    const usize candidateCount = 2 + request.IncludeDirectories.GetSize();
    for (usize candidate = 0; candidate < candidateCount; ++candidate)
    {
        IncludePathOutcome resolved = IncludePathOutcome::Ok;
        if (candidate == 0)
        {
            resolved = ResolveIncludePath(includingFile, path, inoutScratch);
        }
        else if (candidate == 1)
        {
            resolved = ResolveIncludePath(std::string_view{}, path, inoutScratch);
        }
        else
        {
            // Join below the directory: resolve against a file name inside it.
            inoutScratch.Clear();
            resolved = AppendComponents(ViewOf(request.IncludeDirectories[candidate - 2]), false, inoutScratch);
            if (resolved == IncludePathOutcome::Ok)
            {
                resolved = AppendComponents(path, false, inoutScratch);
            }
        }
        if (resolved == IncludePathOutcome::OutOfMemory)
        {
            return resolved;
        }
        if (resolved == IncludePathOutcome::Ok && LookUp(request, inoutScratch, outIndex) == IncludePathOutcome::Ok)
        {
            return IncludePathOutcome::Ok;
        }
        outcome = Combine(outcome, resolved == IncludePathOutcome::Ok ? IncludePathOutcome::NotFound : resolved);
    }
    return outcome;
}

bool IsSnapshotPath(std::string_view path) noexcept
{
    if (path.empty() == true || path.front() == '/' || path.find('\\') != std::string_view::npos ||
        path.find('\0') != std::string_view::npos)
    {
        return false;
    }
    usize start = 0;
    while (true)
    {
        usize end = path.find('/', start);
        const bool isLast = end == std::string_view::npos;
        if (isLast == true)
        {
            end = path.size();
        }
        const std::string_view component = path.substr(start, end - start);
        if (component.empty() == true || component == "." || component == "..")
        {
            return false;
        }
        if (isLast == true)
        {
            return true;
        }
        start = end + 1;
    }
}

bool HasSuffix(std::string_view text, std::string_view suffix) noexcept
{
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}
} // namespace ludus::tooling::shader_language::internal
