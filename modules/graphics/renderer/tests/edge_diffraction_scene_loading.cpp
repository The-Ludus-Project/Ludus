#include "edge_diffraction_scene_loading.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/filesystem/persistence.hpp>

#include <ludus/content/scene_source/scene_source.h>

namespace ludus::graphics::renderer::testing
{
using namespace foundation;
namespace
{
/// A path's directory (or "." when it has none) and file name.
struct PathParts final
{
    std::string_view Directory;
    std::string_view Name;
};

[[nodiscard]] PathParts SplitPath(std::string_view path) noexcept
{
    usize slash = path.size();
    for (usize i = path.size(); i > 0; --i)
    {
        if (path[i - 1] == '/')
        {
            slash = i - 1;
            break;
        }
    }
    if (slash == path.size())
    {
        return {".", path};
    }
    const usize directoryLength = slash == 0 ? 1 : slash;
    // NOLINTBEGIN(bugprone-suspicious-stringview-data-usage): each part is a view with its own size.
    return {std::string_view{path.data(), directoryLength},
            std::string_view{path.data() + slash + 1, path.size() - slash - 1}};
    // NOLINTEND(bugprone-suspicious-stringview-data-usage)
}
} // namespace

bool TryReadDiffractionFile(std::string_view path, Array<uint8>& outBytes) noexcept
{
    const PathParts parts = SplitPath(path);
    filesystem::Directory directory;
    filesystem::File file;
    if (directory.Open(parts.Directory).Succeeded() == false ||
        directory.OpenRead(parts.Name, file).Succeeded() == false)
    {
        return false;
    }
    Array<uint8> bytes;
    if (bytes.TryResize(static_cast<usize>(file.Size())) == false)
    {
        return false;
    }
    const filesystem::ReadResult read = file.ReadAt(0, bytes.AsSpan());
    if (read.Outcome.Succeeded() == false || read.BytesRead != bytes.GetSize())
    {
        return false;
    }
    outBytes = Move(bytes);
    return true;
}

// A directory, then a file name in it.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool TryWriteDiffractionFile(std::string_view directory, std::string_view name, std::span<const uint8> bytes) noexcept
{
    filesystem::WriteDirectory output;
    if (output.Open(directory).Succeeded() == false)
    {
        return false;
    }
    filesystem::WriteOptions options;
    options.MaxBytes = bytes.size();
    const filesystem::PublicationResult result = output.Publish(name, bytes, options);
    return result.Published == true;
}

bool TryLoadDiffractionScene(std::string_view path, EdgeDiffractionScene& outScene) noexcept
{
    Array<uint8> bytes;
    if (TryReadDiffractionFile(path, bytes) == false)
    {
        return false;
    }
    content::scene_source::SceneSource source;
    content::scene_source::SceneSourceError error;
    const std::string_view text{reinterpret_cast<const char*>(bytes.GetData()), bytes.GetSize()};
    if (content::scene_source::ParseSceneSource(text, source, error) !=
        content::scene_source::SceneSourceStatus::Success)
    {
        return false;
    }
    return BuildEdgeDiffractionScene(source, outScene) == DiffractionSceneStatus::Success;
}
} // namespace ludus::graphics::renderer::testing
