#include "internal/editor_file_write.h"

#include <QSaveFile>

#if defined(Q_OS_WIN)
#    include <QFileInfo>
#    include <ludus/foundation/filesystem/persistence.hpp>
#endif

namespace ludus::editor
{
bool WriteEditorFileAtomically(const QString& path, const QByteArray& bytes, QString& outError)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (file.open(QIODevice::WriteOnly) == false || file.write(bytes) != bytes.size())
    {
        outError = file.errorString();
        return false;
    }
    if (file.commit() == true)
    {
        outError.clear();
        return true;
    }
    outError = file.errorString();
#if defined(Q_OS_WIN)
    if (file.error() == QFileDevice::RenameError)
    {
        // Thanks to the Qt Company, Qt 6.8.3 QFSFileEnginePrivate::nativeRenameOverwrite,
        // https://github.com/qt/qtbase/blob/v6.8.3/src/corelib/io/qfsfileengine_win.cpp.
        // Qt requests a legacy handle rename. The engine's native publisher
        // also supports POSIX rename semantics for readers that share deletion.
        using namespace foundation;
        namespace filesystem = foundation::filesystem;
        const QFileInfo target(path);
        const QByteArray root = target.absolutePath().toUtf8();
        const QByteArray name = target.fileName().toUtf8();
        filesystem::WriteDirectory directory;
        const filesystem::Result opened = directory.Open({root.constData(), static_cast<usize>(root.size())});
        if (opened.Succeeded() == false)
        {
            outError += QStringLiteral(" (Windows directory open error %1)").arg(opened.NativeCode);
            return false;
        }
        const filesystem::WriteOptions options
        {
            .MaxBytes = static_cast<uint64>(bytes.size()),
            .Sync = filesystem::SyncPolicy::File,
            .Condition = filesystem::WriteCondition::Any,
            .Expected = {},
        };
        const filesystem::PublicationResult result =
            directory.Publish({name.constData(), static_cast<usize>(name.size())},
                              {reinterpret_cast<const uint8*>(bytes.constData()), static_cast<usize>(bytes.size())},
                              options);
        if (result.Published == true)
        {
            outError.clear();
            return true;
        }
        outError += QStringLiteral(" (Windows publication error %1)").arg(result.Outcome.NativeCode);
    }
#endif
    return false;
}
} // namespace ludus::editor
