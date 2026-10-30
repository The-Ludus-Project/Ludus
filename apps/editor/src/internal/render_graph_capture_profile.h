#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <QByteArray>
#include <QObject>
#include <QString>

namespace ludus::editor
{
/// Where the capture profile of the viewport's drawing generation stands.
enum class CaptureProfileState : foundation::uint8
{
    Idle,     ///< No generation was requested.
    Building, ///< The workspace is compiling the profile of GetGenerationIdentity.
    Ready,    ///< GetPackagePath names the profile; empty means the runtime package is the capture package.
    Failed,   ///< GetError explains why; a later Request of the same generation retries.
};

/// The frame capture profile (`.lurgcp`) of one render graph generation, which links the graph's
/// scene_interface.capture_visibility_queries in place of its interactive budget. The Reference window
/// requests the profile of the viewport's drawing generation before a graph capture; RendererWorkspace owns
/// the `render-graph capture-profile` job that compiles it from that generation's saved sources and answers
/// with AcceptBuild or RejectBuild. A generation without a larger capture budget, or without saved sources,
/// such as the prebuilt engine default, is Ready with an empty path: the runtime package captures it. See
/// docs/architecture/render-graph-frame-capture.md. GUI thread only.
class RenderGraphCaptureProfile final : public QObject
{
    Q_OBJECT
public:
    explicit RenderGraphCaptureProfile(QObject* parentOrNull = nullptr);

    /// Make generationIdentity's profile the current one. Keeps a Building or Ready profile of the same
    /// generation; otherwise, including after a failure, emits BuildRequested and becomes Building.
    /// @param generationIdentity RenderGraphPackage::GetGenerationIdentity of the drawing runtime package.
    void Request(const QByteArray& generationIdentity);
    /// Report the compiled profile of generationIdentity; ignored unless it is the current request.
    /// @param packagePath The `.lurgcp` file, or empty when the runtime package is the capture package.
    void AcceptBuild(const QByteArray& generationIdentity, const QString& packagePath);
    /// Report that generationIdentity's profile could not be built; ignored unless it is the current request.
    void RejectBuild(const QByteArray& generationIdentity, const QString& reason);

    [[nodiscard]] LUDUS_INLINE CaptureProfileState GetState() const noexcept
    {
        return mState;
    }
    [[nodiscard]] LUDUS_INLINE const QByteArray& GetGenerationIdentity() const noexcept
    {
        return mGenerationIdentity;
    }
    /// The Ready profile's `.lurgcp`, or empty when the runtime package is the capture package.
    [[nodiscard]] LUDUS_INLINE const QString& GetPackagePath() const noexcept
    {
        return mPackagePath;
    }
    /// Why the current request failed; empty otherwise.
    [[nodiscard]] LUDUS_INLINE const QString& GetError() const noexcept
    {
        return mError;
    }
    /// True when generationIdentity's profile is Ready.
    [[nodiscard]] bool IsReadyFor(const QByteArray& generationIdentity) const noexcept;

Q_SIGNALS:
    /// Compile the profile of generationIdentity and answer with AcceptBuild or RejectBuild.
    void BuildRequested(const QByteArray& generationIdentity);
    /// The state, path or error changed.
    void Changed();

private:
    CaptureProfileState mState = CaptureProfileState::Idle;
    QByteArray mGenerationIdentity;
    QString mPackagePath;
    QString mError;
};
} // namespace ludus::editor
