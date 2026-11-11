#include "filesystem_fixture.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#if defined(Q_OS_WIN)
#    include <windows.h>
#else
#    include <unistd.h>
#endif

bool CreateTestSymbolicLink(const QString& target, const QString& link)
{
#if defined(Q_OS_WIN)
    const QString nativeTarget = QDir::toNativeSeparators(target);
    const QString nativeLink = QDir::toNativeSeparators(link);
    const DWORD directoryFlag = QFileInfo(target).isDir() == true ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
    if (CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(nativeLink.utf16()),
                            reinterpret_cast<LPCWSTR>(nativeTarget.utf16()),
                            directoryFlag | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0)
    {
        return true;
    }
    // Older Windows versions reject the developer-mode flag. Elevated CI can
    // still create the link using its explicit symbolic-link privilege.
    return GetLastError() == ERROR_INVALID_PARAMETER &&
           CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(nativeLink.utf16()),
                               reinterpret_cast<LPCWSTR>(nativeTarget.utf16()),
                               directoryFlag) != 0;
#else
    return symlink(QFile::encodeName(target).constData(), QFile::encodeName(link).constData()) == 0;
#endif
}
