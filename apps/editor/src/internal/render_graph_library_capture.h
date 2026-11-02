#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <QByteArray>
#include <QMap>
#include <QObject>
#include <QString>
#include <QVector>

namespace ludus::editor
{
/// Expanded ID of the Export that a library capture package captures: the generated root's ID and Export node
/// of ludus_tools.render_graph_library_capture (ROOT_ID and CAPTURE_OUTPUT), which a Python test keeps equal.
inline constexpr const char* LIBRARY_CAPTURE_EXPORT = "LibraryCapture/output";

/// One library configuration output that a frame capture can render in place of the viewport's output, such
/// as a slow reference template inserted beside the interactive graph. GUI thread only.
struct RenderGraphCaptureTarget final
{
    /// Package-relative library graph, such as PointLineMis.lurender.json.
    QString Library;
    /// Configuration of that library.
    QString Configuration;
    /// ColorTexture output of that configuration.
    QString Output;

    [[nodiscard]] bool operator==(const RenderGraphCaptureTarget& other) const = default;
    /// "Library#Configuration.output", as the capture window lists and reports it.
    [[nodiscard]] QString GetLabel() const;
};

/// Package-relative source paths and their bytes, ordered by path.
using RenderGraphSourceBytes = QMap<QString, QByteArray>;

/// The library configurations of one graph package and their ColorTexture outputs, in file then
/// declaration order. files maps package-relative paths to their bytes; only `.lurender.json` files other
/// than rootGraph are read. Malformed graphs are skipped: their build reports them.
[[nodiscard]] QVector<RenderGraphCaptureTarget> ListCaptureTargets(const RenderGraphSourceBytes& files,
                                                                   const QString& rootGraph);

/// Where the library capture of one target stands.
enum class LibraryCaptureState : foundation::uint8
{
    Idle,     ///< Nothing was requested.
    Building, ///< The workspace is compiling the library capture of GetTarget for GetGenerationIdentity.
    Ready, ///< GetPackagePath names the library capture's runtime package; GetProfilePath its capture profile or empty.
    Failed, ///< GetError explains why; a later Request of the same target retries.
};

/// The frame capture package of one library output of a render graph generation. RendererWorkspace answers a
/// request with the `render-graph capture-library` job, which rebuilds the generation's saved sources with a
/// generated root that routes the target's output to an Export, into its own build tree and runtime package.
/// The viewport's generation and runtime package are never written. See
/// docs/architecture/render-graph-frame-capture.md. GUI thread only.
class RenderGraphLibraryCapture final : public QObject
{
    Q_OBJECT
public:
    explicit RenderGraphLibraryCapture(QObject* parentOrNull = nullptr);
    /// Make target of generationIdentity the current library capture. Keeps a Building or Ready library capture of the
    /// same pair; otherwise, including after a failure, emits BuildRequested and becomes Building.
    void Request(const QByteArray& generationIdentity, const RenderGraphCaptureTarget& target);
    /// Report the compiled library capture; ignored unless it answers the current request.
    /// @param packagePath The library capture's runtime package (`.lurgrt`).
    /// @param profilePath Its capture profile (`.lurgcp`), or empty when the graph declares no larger budget.
    void AcceptBuild(const QByteArray& generationIdentity,
                     const RenderGraphCaptureTarget& target,
                     const QString& packagePath,
                     const QString& profilePath);
    /// Report that the library capture could not be built; ignored unless it answers the current request.
    void
    RejectBuild(const QByteArray& generationIdentity, const RenderGraphCaptureTarget& target, const QString& reason);
    [[nodiscard]] LUDUS_INLINE LibraryCaptureState GetState() const noexcept
    {
        return mState;
    }
    [[nodiscard]] LUDUS_INLINE const RenderGraphCaptureTarget& GetTarget() const noexcept
    {
        return mTarget;
    }
    [[nodiscard]] LUDUS_INLINE const QByteArray& GetGenerationIdentity() const noexcept
    {
        return mGenerationIdentity;
    }
    /// The Ready library capture's runtime package.
    [[nodiscard]] LUDUS_INLINE const QString& GetPackagePath() const noexcept
    {
        return mPackagePath;
    }
    /// The Ready library capture's capture profile, or empty when its runtime package is the capture package.
    [[nodiscard]] LUDUS_INLINE const QString& GetProfilePath() const noexcept
    {
        return mProfilePath;
    }
    /// Why the current request failed; empty otherwise.
    [[nodiscard]] LUDUS_INLINE const QString& GetError() const noexcept
    {
        return mError;
    }
    /// True when target's library capture of generationIdentity is Ready.
    [[nodiscard]] bool IsReadyFor(const QByteArray& generationIdentity,
                                  const RenderGraphCaptureTarget& target) const noexcept;

Q_SIGNALS:
    /// Compile target's library capture of generationIdentity and answer with AcceptBuild or RejectBuild.
    void BuildRequested(const QByteArray& generationIdentity, const RenderGraphCaptureTarget& target);
    /// The state, paths or error changed.
    void Changed();

private:
    [[nodiscard]] bool isCurrent(const QByteArray& generationIdentity,
                                 const RenderGraphCaptureTarget& target) const noexcept;
    LibraryCaptureState mState = LibraryCaptureState::Idle;
    QByteArray mGenerationIdentity;
    RenderGraphCaptureTarget mTarget;
    QString mPackagePath;
    QString mProfilePath;
    QString mError;
};
} // namespace ludus::editor
