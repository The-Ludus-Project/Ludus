#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp>

#include "internal/preview_shader_build.h"

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

namespace ludus::editor
{
class RenderPreview;
/// Live shader reload state of one viewport, mirroring the shader_development reload vocabulary.
enum class PreviewShaderReloadState : foundation::uint8
{
    Off,             ///< Not watching; the viewport keeps whatever shaders it draws with.
    Idle,            ///< Watching; the last build is presented (or unchanged).
    Compiling,       ///< A background build is running; the viewport keeps drawing.
    Applying,        ///< A new set was handed to the preview and awaits its first frame.
    Failed,          ///< The last build failed; the last good shaders stay active.
    RestartRequired, ///< The edit changed a shader interface; the last good shaders stay active.
    Unavailable,     ///< No compiler worker or shader sources; nothing is watched.
};

/// Watches the engine renderer shader sources of one RenderPreview, rebuilds them on a worker
/// thread through ludus-shader-worker after edits settle, and hands complete, interface-compatible
/// sets to the preview. The preview swaps them in through its generation restart, so the UI thread
/// never compiles and the GPU never waits; failed or incompatible edits leave the last good set in
/// place and explain why in GetStatusText.
///
/// Main-thread object. Destruction cancels and joins the build thread and stops a worker the
/// controller launched.
class PreviewShaderReload final : public QObject
{
    Q_OBJECT
public:
    /// Use this build's engine checkout and compiler worker. inoutPreview must outlive the controller.
    explicit PreviewShaderReload(RenderPreview& inoutPreview, QObject* parentOrNull = nullptr);
    /// Use explicit sources and compiler (tests). target selects SPIR-V or Metal artifacts.
    PreviewShaderReload(RenderPreview& inoutPreview,
                        const PreviewShaderSources& sources,
                        foundation::UniquePtr<PreviewShaderCompiler> compiler,
                        tooling::shader_compiler::Target target,
                        QObject* parentOrNull = nullptr);
    ~PreviewShaderReload() override;
    PreviewShaderReload(const PreviewShaderReload&) = delete;
    PreviewShaderReload& operator=(const PreviewShaderReload&) = delete;
    /// Whether this editor build can rebuild viewport shaders at all (native Linux/macOS preview).
    [[nodiscard]] static bool IsSupported() noexcept;
    /// Target code this editor's viewport draws, and so the target of its built-in renderer shaders:
    /// Metal source on macOS, SPIR-V elsewhere.
    [[nodiscard]] static tooling::shader_compiler::Target GetHostTarget() noexcept;
    /// Watch and rebuild a different source tree (for example a copy of the renderer shaders).
    /// Only while Off; returns false otherwise and keeps the current sources.
    [[nodiscard]] bool SetSources(const PreviewShaderSources& sources);
    /// Start or stop watching. Enabling builds at once, so sources edited before enabling apply;
    /// disabling returns the preview to the built-in shaders.
    void SetEnabled(bool isEnabled);
    [[nodiscard]] LUDUS_INLINE bool IsEnabled() const noexcept
    {
        return mState != PreviewShaderReloadState::Off;
    }
    [[nodiscard]] LUDUS_INLINE PreviewShaderReloadState GetState() const noexcept
    {
        return mState;
    }
    /// Overlay text for the viewport: errors and restart notices persist, success notices are
    /// transient. Empty while there is nothing to show.
    [[nodiscard]] LUDUS_INLINE QString GetStatusText() const
    {
        return mStatusText;
    }
    /// Whether GetStatusText describes a failure (drawn with error styling).
    [[nodiscard]] LUDUS_INLINE bool HasError() const noexcept
    {
        return mState == PreviewShaderReloadState::Failed || mState == PreviewShaderReloadState::RestartRequired ||
               mState == PreviewShaderReloadState::Unavailable;
    }
    /// Files currently watched for edits.
    [[nodiscard]] LUDUS_INLINE const QStringList& GetWatchedPaths() const noexcept
    {
        return mWatchedPaths;
    }
    /// Number of builds started since construction; debounced edits share one build.
    [[nodiscard]] LUDUS_INLINE foundation::int32 GetBuildCount() const noexcept
    {
        return mBuildCount;
    }
    /// Treat path as edited, as a file system notification would (tests and manual refresh).
    void NotifyChanged(const QString& path);

Q_SIGNALS:
    /// GetState or GetStatusText changed.
    void StatusChanged();

private:
    class BuildLane;
    void startBuild();
    void finishBuild(foundation::uint64 serial, const PreviewShaderBuildResult& result);
    void handleApplied(foundation::uint64 revision);
    void handleRejected(foundation::uint64 revision, const QString& reason);
    void updateWatch(const QStringList& paths);
    void setStatus(PreviewShaderReloadState state, const QString& text, bool isTransient);
    [[nodiscard]] bool prepareCompiler(QString& outError);
    void stopWorker();
    RenderPreview& mPreview;
    PreviewShaderSources mSources;
    tooling::shader_compiler::Target mTarget;
    QString mValidatorPath;
    foundation::UniquePtr<PreviewShaderCompiler> mCompiler;
    bool mIsCompilerInjected = false;
    QThread mThread;
    QPointer<BuildLane> mLaneOrNull;
    QAtomicInt mCancel;
    QFileSystemWatcher mWatcher;
    QStringList mWatchedPaths;
    /// Shader file stamps of each watched directory, by directory path.
    QHash<QString, QStringList> mDirectoryStamps;
    QTimer mDebounce;
    QTimer mTransient;
    QElapsedTimer mBuildClock;
    QProcess* mWorkerOrNull = nullptr;
    QTemporaryDir* mWorkerDirectoryOrNull = nullptr;
    PreviewShaderReloadState mState = PreviewShaderReloadState::Off;
    QString mStatusText;
    QByteArray mPresentedDigest;
    QByteArray mPendingDigest;
    foundation::uint64 mBuildSerial = 0;
    foundation::uint64 mPendingRevision = 0;
    foundation::int32 mBuildCount = 0;
    bool mIsBuilding = false;
    bool mHasPendingChange = false;
};
} // namespace ludus::editor
