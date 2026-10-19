#include "internal/preview_shader_reload.h"
#include "internal/render_preview.h"

#include <ludus/foundation/base/core.h>

#include <new>

#include <QAtomicInteger>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSettings>

#include "ludus_editor_shader_reload_paths.h"

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace compiler = tooling::shader_compiler;
/// Quiet period after the last file notification before a build starts; editors often write a
/// file several times per save.
constexpr int32 DEBOUNCE_MILLISECONDS = 250;
/// How long success notices stay in the viewport overlay.
constexpr int32 TRANSIENT_MILLISECONDS = 3000;
/// How long a launched worker may take to stop before it is killed.
constexpr int32 WORKER_STOP_MILLISECONDS = 2000;
/// Worker endpoint set in the Renderer workspace, shared with render-graph builds.
const QString WORKER_SOCKET_KEY = QStringLiteral("renderGraph/workerSocket");
/// Revisions are unique per process, so a recreated controller never reuses a presented revision.
QAtomicInteger<quint64> gNextRevision{1};

compiler::Target GetHostTarget() noexcept
{
#if defined(Q_OS_MACOS)
    return compiler::Target::Metal;
#else
    return compiler::Target::Spirv;
#endif
}
/// Shader files of directory with their size and modification time, so a directory notification
/// caused by unrelated files (editor swap and backup files) does not start a build.
QStringList GetDirectoryStamp(const QString& directory)
{
    QStringList stamp;
    const QFileInfoList entries =
        QDir(directory).entryInfoList({QStringLiteral("*.slang"), QStringLiteral("*.h"), QStringLiteral("*.json")},
                                      QDir::Files | QDir::NoDotAndDotDot,
                                      QDir::Name);
    for (const QFileInfo& entry : entries)
    {
        stamp.append(QStringLiteral("%1|%2|%3")
                         .arg(entry.fileName())
                         .arg(entry.size())
                         .arg(entry.lastModified().toMSecsSinceEpoch()));
    }
    return stamp;
}
PreviewShaderSources GetBuildSources()
{
    PreviewShaderSources sources;
    sources.RecipeDirectory = QString::fromUtf8(shader_reload_paths::RECIPE_DIRECTORY);
    for (const char* directory : shader_reload_paths::INCLUDE_DIRECTORIES)
    {
        sources.IncludeDirectories.append(QString::fromUtf8(directory));
    }
    return sources;
}
} // namespace

/// Owns the builder on the build thread; results return to the controller through queued calls.
class PreviewShaderReload::BuildLane final : public QObject
{
public:
    BuildLane(const PreviewShaderSources& sources, compiler::Target target, const QString& validatorPath)
        : mBuilder(sources, target, validatorPath)
    {
    }
    PreviewShaderBuilder mBuilder;
};

PreviewShaderReload::PreviewShaderReload(RenderPreview& inoutPreview, QObject* parentOrNull)
    : PreviewShaderReload(inoutPreview, GetBuildSources(), {}, GetHostTarget(), parentOrNull)
{
    mValidatorPath = QString::fromUtf8(shader_reload_paths::SPIRV_VALIDATOR);
}

PreviewShaderReload::PreviewShaderReload(RenderPreview& inoutPreview,
                                         const PreviewShaderSources& sources,
                                         UniquePtr<PreviewShaderCompiler> compiler,
                                         compiler::Target target,
                                         QObject* parentOrNull)
    : QObject(parentOrNull), mPreview(inoutPreview), mSources(sources), mTarget(target), mCompiler(Move(compiler))
{
    mIsCompilerInjected = mCompiler.Get() != nullptr;
    mThread.setObjectName(QStringLiteral("LudusShaderReload"));
    mDebounce.setSingleShot(true);
    mDebounce.setInterval(DEBOUNCE_MILLISECONDS);
    connect(&mDebounce, &QTimer::timeout, this, [this]() { startBuild(); });
    mTransient.setSingleShot(true);
    mTransient.setInterval(TRANSIENT_MILLISECONDS);
    connect(&mTransient, &QTimer::timeout, this, [this]() {
        if (mState == PreviewShaderReloadState::Idle && mStatusText.isEmpty() == false)
        {
            mStatusText.clear();
            Q_EMIT StatusChanged();
        }
    });
    connect(&mWatcher, &QFileSystemWatcher::fileChanged, this, [this](const QString& path) { NotifyChanged(path); });
    connect(&mWatcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString& path) {
        NotifyChanged(path);
    });
    connect(&inoutPreview, &RenderPreview::ShadersApplied, this, [this](uint64 revision) { handleApplied(revision); });
    connect(&inoutPreview, &RenderPreview::ShadersRejected, this, [this](uint64 revision, const QString& reason) {
        handleRejected(revision, reason);
    });
}

PreviewShaderReload::~PreviewShaderReload()
{
    mCancel.storeRelaxed(1);
    mThread.quit();
    static_cast<void>(mThread.wait());
    delete mLaneOrNull.data();
    stopWorker();
}

bool PreviewShaderReload::IsSupported() noexcept
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW) && (defined(Q_OS_LINUX) || defined(Q_OS_MACOS))
    return true;
#else
    return false;
#endif
}

bool PreviewShaderReload::SetSources(const PreviewShaderSources& sources)
{
    if (IsEnabled() == true)
    {
        return false;
    }
    mSources = sources;
    // A lane keeps the previous builder; the next build starts a fresh one for these sources.
    if (mLaneOrNull.isNull() == false)
    {
        // Deleted on its thread after any queued build, or when the thread finishes.
        mLaneOrNull->deleteLater();
        mLaneOrNull.clear();
    }
    return true;
}

void PreviewShaderReload::SetEnabled(bool isEnabled)
{
    if (isEnabled == IsEnabled())
    {
        return;
    }
    if (isEnabled == false)
    {
        // A running build finishes on its thread; its serial no longer matches, so it is ignored.
        ++mBuildSerial;
        mIsBuilding = false;
        mHasPendingChange = false;
        mDebounce.stop();
        mTransient.stop();
        updateWatch({});
        setStatus(PreviewShaderReloadState::Off, {}, false);
        // Off means the shaders this editor was built with, as after a restart.
        if (mPreview.GetShaders().HasArtifacts() == true)
        {
            PreviewShaderSet builtIn;
            builtIn.SetRevision(gNextRevision.fetchAndAddRelaxed(1));
            mPreview.SetShaders(builtIn);
        }
        return;
    }
    mPresentedDigest = mPreview.GetShaders().ComputeDigest();
    setStatus(PreviewShaderReloadState::Compiling, QStringLiteral("Live shaders: compiling…"), false);
    startBuild();
}

void PreviewShaderReload::NotifyChanged(const QString& path)
{
    if (IsEnabled() == false)
    {
        return;
    }
    // Editors that save by replacing a file drop it from the watcher; watch the new file again.
    if (mWatchedPaths.contains(path) == true && mWatcher.files().contains(path) == false &&
        QFileInfo::exists(path) == true)
    {
        static_cast<void>(mWatcher.addPath(path));
    }
    if (mDirectoryStamps.contains(path) == true)
    {
        QStringList stamp = GetDirectoryStamp(path);
        if (stamp == mDirectoryStamps.value(path))
        {
            return;
        }
        mDirectoryStamps.insert(path, Move(stamp));
    }
    if (mIsBuilding == true)
    {
        mHasPendingChange = true;
        return;
    }
    mDebounce.start();
}

bool PreviewShaderReload::prepareCompiler(QString& outError)
{
    if (mCompiler.Get() != nullptr &&
        (mIsCompilerInjected == true || mWorkerOrNull == nullptr || mWorkerOrNull->state() != QProcess::NotRunning))
    {
        return true;
    }
    stopWorker();
    mCompiler.Reset();
    QString endpoint = qEnvironmentVariable("LUDUS_SHADER_WORKER_SOCKET");
    if (endpoint.isEmpty() == true)
    {
        endpoint = QSettings().value(WORKER_SOCKET_KEY).toString();
    }
    const QString executable = QString::fromUtf8(shader_reload_paths::WORKER_EXECUTABLE);
    if (endpoint.isEmpty() == true && executable.isEmpty() == false && QFileInfo(executable).isExecutable() == true)
    {
        mWorkerDirectoryOrNull =
            new (std::nothrow) QTemporaryDir(QDir::temp().filePath(QStringLiteral("ludus-shaders-XXXXXX")));
        if (mWorkerDirectoryOrNull == nullptr || mWorkerDirectoryOrNull->isValid() == false)
        {
            outError = QStringLiteral("Cannot create a private directory for the shader worker socket.");
            stopWorker();
            return false;
        }
        endpoint = mWorkerDirectoryOrNull->filePath(QStringLiteral("compiler.sock"));
        mWorkerOrNull = new QProcess(this);
        mWorkerOrNull->setProcessChannelMode(QProcess::ForwardedErrorChannel);
        mWorkerOrNull->start(executable, {QStringLiteral("--serve"), endpoint});
        if (mWorkerOrNull->waitForStarted() == false)
        {
            outError = QStringLiteral("Cannot start %1: %2")
                           .arg(QDir::toNativeSeparators(executable), mWorkerOrNull->errorString());
            stopWorker();
            return false;
        }
    }
    if (endpoint.isEmpty() == true)
    {
        outError = QStringLiteral("No shader compiler worker. Configure the engine with "
                                  "-DLUDUS_BUILD_SHADER_WORKER=ON (macOS 15 or Linux) and rebuild the editor, or set "
                                  "a running worker's socket in Renderer › Native shader worker.");
        return false;
    }
    mCompiler.Reset(new (std::nothrow) PreviewShaderWorkerCompiler(endpoint));
    if (mCompiler.Get() == nullptr)
    {
        outError = QStringLiteral("Out of memory while connecting to the shader worker.");
        stopWorker();
        return false;
    }
    return true;
}

void PreviewShaderReload::stopWorker()
{
    if (mWorkerOrNull != nullptr)
    {
        if (mWorkerOrNull->state() != QProcess::NotRunning)
        {
            mWorkerOrNull->terminate();
            if (mWorkerOrNull->waitForFinished(WORKER_STOP_MILLISECONDS) == false)
            {
                mWorkerOrNull->kill();
                static_cast<void>(mWorkerOrNull->waitForFinished());
            }
        }
        delete mWorkerOrNull;
        mWorkerOrNull = nullptr;
    }
    delete mWorkerDirectoryOrNull;
    mWorkerDirectoryOrNull = nullptr;
}

void PreviewShaderReload::startBuild()
{
    if (IsEnabled() == false || mIsBuilding == true)
    {
        return;
    }
    QString error;
    if (prepareCompiler(error) == false)
    {
        updateWatch({});
        setStatus(PreviewShaderReloadState::Unavailable, QStringLiteral("Live shaders unavailable\n") + error, false);
        return;
    }
    if (mLaneOrNull.isNull() == true)
    {
        BuildLane* lane = new (std::nothrow) BuildLane(mSources, mTarget, mValidatorPath);
        if (lane == nullptr)
        {
            setStatus(PreviewShaderReloadState::Unavailable,
                      QStringLiteral("Live shaders unavailable\nOut of memory."),
                      false);
            return;
        }
        lane->moveToThread(&mThread);
        mLaneOrNull = lane;
        mThread.start();
    }
    mIsBuilding = true;
    mHasPendingChange = false;
    ++mBuildCount;
    const uint64 serial = ++mBuildSerial;
    mBuildClock.start();
    if (mState != PreviewShaderReloadState::Failed && mState != PreviewShaderReloadState::RestartRequired)
    {
        setStatus(PreviewShaderReloadState::Compiling, QStringLiteral("Live shaders: compiling…"), false);
    }
    PreviewShaderCompiler* compiler = mCompiler.Get();
    BuildLane* lane = mLaneOrNull.data();
    QPointer<PreviewShaderReload> owner(this);
    // The compiler and lane are destroyed only after the thread is joined in the destructor.
    static_cast<void>(QMetaObject::invokeMethod(
        lane,
        [lane, compiler, owner, serial, this]() {
            const PreviewShaderBuildResult result = lane->mBuilder.Build(*compiler, mCancel);
            if (owner.isNull() == false)
            {
                static_cast<void>(QMetaObject::invokeMethod(
                    owner.data(),
                    [this, serial, result]() { finishBuild(serial, result); },
                    Qt::QueuedConnection));
            }
        },
        Qt::QueuedConnection));
}

void PreviewShaderReload::finishBuild(uint64 serial, const PreviewShaderBuildResult& result)
{
    if (serial != mBuildSerial || IsEnabled() == false)
    {
        return;
    }
    mIsBuilding = false;
    updateWatch(result.WatchedPaths);
    if (mHasPendingChange == true)
    {
        // Sources changed during the build; its output may already be stale.
        mHasPendingChange = false;
        mDebounce.start();
        return;
    }
    const qint64 elapsed = mBuildClock.elapsed();
    switch (result.Status)
    {
        case PreviewShaderBuildStatus::Ready: {
            const QByteArray digest = result.Shaders.ComputeDigest();
            if (digest == mPresentedDigest)
            {
                // Saving without an effective change stays quiet; an error overlay still clears.
                QString text;
                if (mBuildCount == 1)
                {
                    text = QStringLiteral("Live shaders on · watching %1 files").arg(result.SourceFiles);
                }
                else if (mState != PreviewShaderReloadState::Compiling)
                {
                    text = QStringLiteral("Shaders unchanged (%1 ms)").arg(elapsed);
                }
                setStatus(PreviewShaderReloadState::Idle, text, text.isEmpty() == false);
                return;
            }
            PreviewShaderSet shaders = result.Shaders;
            mPendingRevision = gNextRevision.fetchAndAddRelaxed(1);
            mPendingDigest = digest;
            shaders.SetRevision(mPendingRevision);
            setStatus(PreviewShaderReloadState::Applying, QStringLiteral("Live shaders: applying…"), false);
            mPreview.SetShaders(shaders);
            if (mPreview.HasDevice() == false)
            {
                // Nothing presents now; the set applies when the viewport starts.
                mPresentedDigest = digest;
                setStatus(PreviewShaderReloadState::Idle,
                          QStringLiteral("Shaders compiled (%1 ms); they apply when the viewport starts").arg(elapsed),
                          true);
            }
            return;
        }
        case PreviewShaderBuildStatus::CompileFailed:
            setStatus(PreviewShaderReloadState::Failed,
                      QStringLiteral("Shader error · last good shader kept\n") +
                          FormatPreviewShaderDiagnostic(result.Diagnostic),
                      false);
            return;
        case PreviewShaderBuildStatus::RestartRequired:
            setStatus(PreviewShaderReloadState::RestartRequired,
                      QStringLiteral("Shader interface changed · restart required\n%1\nRebuild and restart the editor "
                                     "to draw with this change; the last good shaders stay active.")
                          .arg(result.Message),
                      false);
            return;
        case PreviewShaderBuildStatus::Unavailable:
            setStatus(PreviewShaderReloadState::Unavailable,
                      QStringLiteral("Live shaders unavailable\n") + result.Message,
                      false);
            return;
        case PreviewShaderBuildStatus::Cancelled:
            return;
    }
}

void PreviewShaderReload::handleApplied(uint64 revision)
{
    if (revision != mPendingRevision || IsEnabled() == false)
    {
        return;
    }
    mPresentedDigest = mPendingDigest;
    setStatus(PreviewShaderReloadState::Idle,
              QStringLiteral("Shader reloaded (%1 ms)").arg(mBuildClock.elapsed()),
              true);
}

void PreviewShaderReload::handleRejected(uint64 revision, const QString& reason)
{
    if (revision != mPendingRevision || IsEnabled() == false)
    {
        return;
    }
    setStatus(
        PreviewShaderReloadState::Failed,
        QStringLiteral("Shader error · last good shader kept\nThe GPU rejected the rebuilt shaders: %1").arg(reason),
        false);
}

void PreviewShaderReload::updateWatch(const QStringList& paths)
{
    QStringList wanted = paths;
    for (const QString& path : paths)
    {
        const QString directory = QFileInfo(path).absolutePath();
        if (wanted.contains(directory) == false)
        {
            wanted.append(directory);
        }
    }
    if (wanted == mWatchedPaths)
    {
        return;
    }
    mDirectoryStamps.clear();
    for (const QString& path : wanted)
    {
        if (paths.contains(path) == false)
        {
            mDirectoryStamps.insert(path, GetDirectoryStamp(path));
        }
    }
    const QStringList watched = mWatcher.files() + mWatcher.directories();
    if (watched.isEmpty() == false)
    {
        static_cast<void>(mWatcher.removePaths(watched));
    }
    mWatchedPaths = wanted;
    if (wanted.isEmpty() == false)
    {
        static_cast<void>(mWatcher.addPaths(wanted));
    }
}

void PreviewShaderReload::setStatus(PreviewShaderReloadState state, const QString& text, bool isTransient)
{
    mTransient.stop();
    if (isTransient == true)
    {
        mTransient.start();
    }
    if (state == mState && text == mStatusText)
    {
        return;
    }
    mState = state;
    mStatusText = text;
    Q_EMIT StatusChanged();
}
} // namespace ludus::editor
