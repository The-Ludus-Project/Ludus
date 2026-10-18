#include "source_resolver.h"

#include "../native_files.h"

#include <ludus/foundation/containers/static_array.hpp>

namespace ludus::content::scene_cook::importers
{
namespace
{
using foundation::StringStatus;

/// Next '/'-separated component of path starting at inoutAt; advances past the separator.
[[nodiscard]] std::string_view TakeComponent(std::string_view path, usize& inoutAt) noexcept
{
    const usize start = inoutAt;
    while (inoutAt < path.size() && path[inoutAt] != '/')
    {
        ++inoutAt;
    }
    const std::string_view component = path.substr(start, inoutAt - start);
    if (inoutAt < path.size())
    {
        ++inoutAt;
    }
    return component;
}
} // namespace

bool IsPlainRelativePath(std::string_view name) noexcept
{
    if (name.empty() == true || name.front() == '/' || name.find('\\') != std::string_view::npos ||
        name.find('\0') != std::string_view::npos)
    {
        return false;
    }
    usize at = 0;
    while (at < name.size())
    {
        const std::string_view component = TakeComponent(name, at);
        if (component.empty() == true || component == "." || component == "..")
        {
            return false;
        }
    }
    return name.back() != '/';
}

std::string_view GetDirectoryPrefix(std::string_view relativePath) noexcept
{
    const usize slash = relativePath.rfind('/');
    return slash == std::string_view::npos ? std::string_view() : relativePath.substr(0, slash + 1);
}

bool JoinRelativePath(std::string_view directoryPrefix, std::string_view name, String& outPath) noexcept
{
    if (name.empty() == true || name.front() == '/' || name.find('\\') != std::string_view::npos ||
        name.find('\0') != std::string_view::npos)
    {
        return false;
    }
    // Fold both parts component by component; a ".." above the root fails.
    constexpr usize MAX_COMPONENTS = 256;
    foundation::StaticArray<std::string_view, MAX_COMPONENTS> components{};
    usize count = 0;
    for (const std::string_view part : {directoryPrefix, name})
    {
        usize at = 0;
        while (at < part.size())
        {
            const std::string_view component = TakeComponent(part, at);
            if (component.empty() == true || component == ".")
            {
                continue;
            }
            if (component == "..")
            {
                if (count == 0)
                {
                    return false;
                }
                --count;
                continue;
            }
            if (count == MAX_COMPONENTS)
            {
                return false;
            }
            components[count++] = component;
        }
    }
    if (count == 0)
    {
        return false;
    }
    outPath.Clear();
    for (usize i = 0; i < count; ++i)
    {
        if ((i != 0 && outPath.TryAppend("/") != StringStatus::Ok) ||
            outPath.TryAppend(components[i]) != StringStatus::Ok)
        {
            return false;
        }
    }
    return true;
}

bool NativeSourceResolver::Resolve(std::string_view name, ResolvedSource& outSource) noexcept
{
    if (mRoot.empty() == true || name.empty() == true || name.front() == '/' ||
        name.find('\\') != std::string_view::npos || name.find('\0') != std::string_view::npos)
    {
        return false;
    }
    String joined;
    const std::string_view parts[] = {mRoot, std::string_view("/"), name};
    if (joined.TryAppendMany(parts) != StringStatus::Ok ||
        detail::ResolvePath(joined.GetView(), outSource.Native) != Status::Ok)
    {
        return false;
    }
    // realpath follows symlinks, so a link that leaves the root fails this prefix test.
    const std::string_view native = outSource.Native.GetView();
    if (native.size() <= mRoot.size() + 1 || native.substr(0, mRoot.size()) != mRoot || native[mRoot.size()] != '/')
    {
        return false;
    }
    return outSource.Relative.TryAssign(native.substr(mRoot.size() + 1)) == StringStatus::Ok;
}

Status NativeSourceResolver::Read(const ResolvedSource& source, usize maximumBytes, Array<uint8>& outBytes) noexcept
{
    Array<uint8> bytes;
    const Status status = detail::ReadFileBounded(source.Native.GetView(), maximumBytes, bytes);
    if (status == Status::Ok)
    {
        outBytes.Swap(bytes);
    }
    return status;
}

bool MemorySourceResolver::Resolve(std::string_view name, ResolvedSource& outSource) noexcept
{
    return IsPlainRelativePath(name) == true && outSource.Relative.TryAssign(name) == StringStatus::Ok &&
           outSource.Native.TryAssign(name) == StringStatus::Ok;
}

Status MemorySourceResolver::Read(const ResolvedSource& source, usize maximumBytes, Array<uint8>& outBytes) noexcept
{
    for (const MemorySourceFile& file : mFiles)
    {
        if (file.Name != source.Native.GetView())
        {
            continue;
        }
        if (file.Bytes.size() > maximumBytes)
        {
            return Status::Limit;
        }
        Array<uint8> bytes;
        if (bytes.TryAddRange(file.Bytes) == false)
        {
            return Status::OutOfMemory;
        }
        outBytes.Swap(bytes);
        return Status::Ok;
    }
    return Status::NotFound;
}

Status FileByteSource::Read(std::span<uint8> outBuffer, usize& outCount) noexcept
{
    return mFile.Read(outBuffer, outCount);
}
} // namespace ludus::content::scene_cook::importers
