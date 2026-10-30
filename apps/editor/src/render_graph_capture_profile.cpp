#include "internal/render_graph_capture_profile.h"

namespace ludus::editor
{
RenderGraphCaptureProfile::RenderGraphCaptureProfile(QObject* parentOrNull) : QObject(parentOrNull) {}

void RenderGraphCaptureProfile::Request(const QByteArray& generationIdentity)
{
    if (generationIdentity == mGenerationIdentity &&
        (mState == CaptureProfileState::Building || mState == CaptureProfileState::Ready))
    {
        return;
    }
    mGenerationIdentity = generationIdentity;
    mPackagePath.clear();
    mError.clear();
    mState = CaptureProfileState::Building;
    Q_EMIT Changed();
    // The workspace may answer synchronously, for a cached profile or a rejected job.
    Q_EMIT BuildRequested(generationIdentity);
}

void RenderGraphCaptureProfile::AcceptBuild(const QByteArray& generationIdentity, const QString& packagePath)
{
    if (generationIdentity != mGenerationIdentity || mState != CaptureProfileState::Building)
    {
        return;
    }
    mPackagePath = packagePath;
    mState = CaptureProfileState::Ready;
    Q_EMIT Changed();
}

void RenderGraphCaptureProfile::RejectBuild(const QByteArray& generationIdentity, const QString& reason)
{
    if (generationIdentity != mGenerationIdentity || mState != CaptureProfileState::Building)
    {
        return;
    }
    mError = reason;
    mState = CaptureProfileState::Failed;
    Q_EMIT Changed();
}

bool RenderGraphCaptureProfile::IsReadyFor(const QByteArray& generationIdentity) const noexcept
{
    return mState == CaptureProfileState::Ready && generationIdentity == mGenerationIdentity;
}
} // namespace ludus::editor
