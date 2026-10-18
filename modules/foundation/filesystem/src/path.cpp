#include <ludus/foundation/filesystem/filesystem.hpp>

namespace ludus::foundation::filesystem
{
namespace
{
bool ValidUtf8(std::string_view text) noexcept
{
    usize i = 0;
    while (i < text.size())
    {
        const auto lead = static_cast<uint8>(text[i++]);
        if (lead < 128)
        {
            continue;
        }
        const uint32 count = lead >= 0xc2 && lead <= 0xdf   ? 1
                             : lead >= 0xe0 && lead <= 0xef ? 2
                             : lead >= 0xf0 && lead <= 0xf4 ? 3
                                                            : 0;
        if (count == 0 || text.size() - i < count)
        {
            return false;
        }
        uint32 code = lead & ((1U << (6 - count)) - 1U);
        for (uint32 j = 0; j < count; ++j)
        {
            const auto next = static_cast<uint8>(text[i++]);
            if ((next & 0xc0U) != 0x80U)
            {
                return false;
            }
            code = (code << 6) | (next & 0x3fU);
        }
        if ((count == 1 && code < 128) || (count == 2 && code < 2048) || (count == 3 && code < 65536) ||
            code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
        {
            return false;
        }
    }
    return true;
}
} // namespace
bool ValidPath(std::string_view path) noexcept
{
    if (path.empty() || path.size() > MAX_PATH_BYTES || ValidUtf8(path) == false)
    {
        return false;
    }
    usize begin = 0;
    for (usize i = 0; i <= path.size(); ++i)
    {
        if (i < path.size() && (path[i] == '\\' || path[i] == ':' || static_cast<uint8>(path[i]) < 32 ||
                                static_cast<uint8>(path[i]) == 127))
        {
            return false;
        }
        if (i == path.size() || path[i] == '/')
        {
            const auto segment = path.substr(begin, i - begin);
            if (segment.empty() || segment == "." || segment == "..")
            {
                return false;
            }
            begin = i + 1;
        }
    }
    return true;
}
} // namespace ludus::foundation::filesystem
