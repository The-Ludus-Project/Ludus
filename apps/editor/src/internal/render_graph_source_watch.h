#pragma once

#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QtGlobal>
#if !defined(Q_OS_WASM)
#    include <QFileSystemWatcher>
#endif

namespace ludus::editor
{
class RenderGraphDocument;
/// Bounded authored-package watches; directory notifications cover atomic-save replacements.
/// Output/build directories never enter the watch set. Owns no document or compiler.
class RenderGraphSourceWatch final : public QObject
{
    Q_OBJECT
public:
    explicit RenderGraphSourceWatch(QObject* parentOrNull = nullptr);
    /// Replace watches for authored files and project graph discovery; never writes files.
    void SetDocument(const RenderGraphDocument& document, const QString& projectRoot);
Q_SIGNALS:
    void SourcesChanged();

private:
    QTimer mDebounce;
#if !defined(Q_OS_WASM)
    QFileSystemWatcher mWatcher;
#endif
};
} // namespace ludus::editor
