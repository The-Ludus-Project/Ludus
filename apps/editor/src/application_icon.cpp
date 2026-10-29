#include "internal/application_icon.h"

#include <QString>

namespace ludus::editor
{
namespace
{
const char* const SMALL_ICON_PATHS[] = {
    ":/ludus/small/ludus-icon-16.png",
    ":/ludus/small/ludus-icon-24.png",
    ":/ludus/small/ludus-icon-32.png",
};
const char* const REFERENCE_ICON_PATH = ":/ludus/ludus-icon.png";
} // namespace

QIcon MakeApplicationIcon()
{
    QIcon icon;
    for (const char* path : SMALL_ICON_PATHS)
    {
        icon.addFile(QString::fromLatin1(path));
    }
    icon.addFile(QString::fromLatin1(REFERENCE_ICON_PATH));
    return icon;
}
} // namespace ludus::editor
