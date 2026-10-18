#pragma once

#include <QtGlobal>
#if !defined(Q_OS_WASM)
#    include <QFileSystemWatcher>
#endif
#include <QObject>
#include <QString>
#include <QTimer>

namespace ludus::editor
{
// Opt-in notifications only. Canonical publication still checks source hashes.
// A single pending rebuild is retained while the build lane is occupied.
class SourceWatch final : public QObject
{
    Q_OBJECT
public:
    explicit SourceWatch(QObject* parent = nullptr);
    [[nodiscard]] bool Start(const QString& projectDirectory);
    void Stop();
    void SetBusy(bool busy);
    [[nodiscard]] bool Active() const noexcept
    {
        return Project_.isEmpty() == false;
    }

Q_SIGNALS:
    void BuildRequested();
    void Failed(const QString& message);

private:
    [[nodiscard]] bool Rearm();
    void Changed();
#if !defined(Q_OS_WASM)
    QFileSystemWatcher Watcher_;
#endif
    QTimer Debounce_;
    QString Project_;
    bool Busy_ = true;
    bool Pending_ = false;
};
} // namespace ludus::editor
