#include "internal/viewport_render_recovery.h"

#include "design_tokens.h"
#include "internal/render_graph_availability.h"
#include "internal/render_graph_diagnostics.h"
#include "internal/render_graph_preview.h"
#include "internal/render_graph_shader_language.h"
#include "internal/render_preview.h"
#include "internal/renderer_workspace.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
QString GetActionLabel(ViewportRenderAction action)
{
    switch (action)
    {
        case ViewportRenderAction::ShowDiagnostics:
            return QStringLiteral("Show diagnostics");
        case ViewportRenderAction::OpenShader:
            return QStringLiteral("Open shader");
        case ViewportRenderAction::ShowGraphNode:
            return QStringLiteral("Show graph node");
        case ViewportRenderAction::RebuildGraph:
            return QStringLiteral("Build && Preview");
        case ViewportRenderAction::UseProjectRenderer:
            return QStringLiteral("Use project renderer");
        case ViewportRenderAction::RestartPreview:
        case ViewportRenderAction::RestartRasterPreview:
            return QStringLiteral("Restore preview");
    }
    return {};
}

ViewportRenderAction
GetDiagnosticAction(const RenderGraphDocument& document, RenderGraphSource sourceVersion, const QJsonObject& location)
{
    const QString file = location.value(QStringLiteral("source")).toString();
    if (file.isEmpty() == true || location.contains(QStringLiteral("generated")) == true ||
        document.GetFiles().contains(file) == false ||
        (sourceVersion == RenderGraphSource::Saved && document.GetDraft(file) != document.GetSaved(file)))
    {
        return ViewportRenderAction::ShowDiagnostics;
    }
    const foundation::int32 line = location.value(QStringLiteral("line")).toInt();
    if (IsRenderGraphShaderSource(file) == true)
    {
        return ViewportRenderAction::OpenShader;
    }
    if (line == 0)
    {
        const QJsonObject target = ResolveRenderGraphDiagnosticTarget(document, location);
        if (target.value(QStringLiteral("node")).toString().isEmpty() == false)
        {
            return ViewportRenderAction::ShowGraphNode;
        }
    }
    return ViewportRenderAction::ShowDiagnostics;
}
} // namespace

ViewportRenderRecovery RendererWorkspace::GetViewportRenderRecovery() const
{
    ViewportRenderRecovery recovery;
    if (mPreviewOrNull == nullptr)
    {
        return recovery;
    }
    const RenderGraphPreview& preview = *mPreviewOrNull;
    if (GetRenderProblem().isEmpty() == true)
    {
        return recovery;
    }

    const graphics::renderer::RenderGraphActivationReport report = preview.GetReport();
    const RenderGraphJobRequest& request = mLastResult.Request;
    const bool isCurrentBuildFailure =
        mBuildProblem.isEmpty() == false && mSourceProblem.isEmpty() == true && mHasResult == true &&
        mLastResult.IsSuccessful == false && mLastResult.IsCancelled == false && request.ShouldPreview == true &&
        request.InputIdentity.isEmpty() == false && request.InputIdentity == getBuildIdentity(request.Source);
    const bool hasDeviceFailure = preview.HasFailedDevice();
    const bool hasRuntimeFailure = mActivationProblem.isEmpty() == false && mSourceProblem.isEmpty() == true;
    const bool hasLoadFailure = hasRuntimeFailure == true && preview.GetLoadProblem().isEmpty() == false;
    const bool isRasterFailure = hasRuntimeFailure == true && preview.GetRasterFallbackPath().isEmpty() == false &&
                                 preview.GetAttemptedPackagePath() == preview.GetRasterFallbackPath() &&
                                 preview.GetAttemptedPackagePath() != preview.GetRequestedPackagePath();
    const bool shouldShowBuildFailure =
        isCurrentBuildFailure == true && isRasterFailure == false && hasDeviceFailure == false;
    const bool isRuntimeFailure = hasRuntimeFailure == true && shouldShowBuildFailure == false;
    const bool isActivationFailure = isRuntimeFailure == true && hasLoadFailure == false;
    const bool hasFrame = preview.GetPresentedGeneration() != 0;
    const QString problem = shouldShowBuildFailure == true ? mBuildProblem : GetRenderProblem();

    recovery.Message = isRasterFailure == true          ? QStringLiteral("Raster fallback could not start.")
                       : shouldShowBuildFailure == true ? QStringLiteral("The renderer build failed.")
                                                        : QStringLiteral("The renderer preview could not update.");
    recovery.Diagnostics = hasDeviceFailure == true ? preview.GetDisplayStatus() : problem;
    if (hasDeviceFailure == true)
    {
        recovery.Diagnostics.prepend(QStringLiteral("The graphics session stopped accepting work.\n"));
    }
    const RenderGraphDeviceProfile device = mCanvas->GetDeviceProfile();
    if (device.DeviceName.isEmpty() == false)
    {
        recovery.Diagnostics += QStringLiteral("\nDevice: %1").arg(device.DeviceName);
    }
    if (mDocument.IsLoaded() == true)
    {
        recovery.Diagnostics +=
            QStringLiteral("\nGraph: %1\nConfiguration: %2").arg(mDocument.GetRootGraph(), GetConfiguration());
    }
    if (preview.GetAttemptedPackagePath().isEmpty() == false)
    {
        recovery.Diagnostics += QStringLiteral("\nRuntime package: %1").arg(preview.GetAttemptedPackagePath());
    }
    if (isRuntimeFailure == true && preview.GetRequestedPackagePath() != preview.GetAttemptedPackagePath())
    {
        recovery.Diagnostics += QStringLiteral("\nRequested runtime: %1").arg(preview.GetRequestedPackagePath());
    }
    if (isActivationFailure == true)
    {
        recovery.Diagnostics += QStringLiteral("\nCandidate generation: %1").arg(report.CandidateGeneration);
    }
    if (hasFrame == true)
    {
        recovery.Diagnostics += QStringLiteral("\nDisplayed generation: %1\nDisplayed World revision: %2")
                                    .arg(preview.GetPresentedGeneration())
                                    .arg(preview.GetPresentedRevision());
    }
    recovery.CanUseRaster =
        hasDeviceFailure == false && mPreviewSourceOrNull != nullptr && preview.HasRasterRecovery() == true;

    using graphics::renderer::RenderGraphCandidateState;
    using graphics::renderer::RenderGraphFailureStage;
    if (hasDeviceFailure == true)
    {
        recovery.Message = QStringLiteral("The graphics processor stopped drawing the preview.");
        if (mPreviewSourceOrNull != nullptr)
        {
            const bool hasFallback = preview.HasReadableRasterFallback();
            recovery.Action =
                hasFallback == true ? ViewportRenderAction::RestartRasterPreview : ViewportRenderAction::RestartPreview;
            recovery.Guidance = hasFallback == true
                                    ? QStringLiteral("Restore preview reconnects the graphics processor and uses the "
                                                     "standard renderer so you can keep editing.")
                                    : QStringLiteral("Restore preview reconnects the graphics processor and tries "
                                                     "drawing this World again.");
        }
        else
        {
            recovery.Guidance = QStringLiteral("Open a World to restore the preview.");
        }
    }
    else if (isRasterFailure == true)
    {
        if (preview.ShouldForceRaster() == true)
        {
            recovery.Action = ViewportRenderAction::UseProjectRenderer;
            recovery.Guidance = QStringLiteral("Switch back to the project renderer to try drawing this World again.");
        }
        else if (CanBuild() == true)
        {
            recovery.Action = ViewportRenderAction::RebuildGraph;
        }
    }
    else if (isActivationFailure == true && report.Candidate == RenderGraphCandidateState::RestartRequired)
    {
        recovery.Guidance = QStringLiteral("Close and reopen Ludus, then open this World again.");
    }
    else if (isActivationFailure == true && report.CandidateFailureStage == RenderGraphFailureStage::Resize)
    {
        recovery.Message = QStringLiteral("The preview could not draw at this size.");
        recovery.Guidance =
            QStringLiteral("Make the viewport smaller or lower the graph's render scale, then try again.");
    }
    else if (isActivationFailure == true &&
             report.CandidateRejection != graphics::renderer::RenderGraphSceneRejection::None)
    {
        recovery.Message +=
            QLatin1Char(' ') + DescribeRenderGraphSceneRejection(report.CandidateRejection, report.CandidateReport);
    }
    else
    {
        const RenderGraphFailureStage stage = report.CandidateFailureStage;
        if (isActivationFailure == true && stage == RenderGraphFailureStage::Setup)
        {
            recovery.Message = QStringLiteral("The renderer could not set up its passes and shaders.");
        }
        else if (isActivationFailure == true && stage == RenderGraphFailureStage::PipelinePreparation)
        {
            recovery.Message = QStringLiteral("The GPU could not prepare the renderer's pipelines.");
        }
        else if (isActivationFailure == true && stage != RenderGraphFailureStage::None)
        {
            recovery.Message =
                QStringLiteral("The renderer preview failed %1").arg(DescribeRenderGraphFailureStage(stage));
        }
        if (CanBuild() == true && (isRuntimeFailure == true || problem == mBuildProblem))
        {
            recovery.Action = ViewportRenderAction::RebuildGraph;
            recovery.Guidance =
                QStringLiteral("Build & Preview rebuilds the graph and tries to draw this World again.");
        }
    }

    if (shouldShowBuildFailure == true)
    {
        recovery.Request = request.Request;
        for (foundation::int32 row = 0; row < GetDiagnosticCount(); ++row)
        {
            const QJsonObject diagnostic = mLastResult.GetDiagnostic(row);
            const QString severity = diagnostic.value(QStringLiteral("severity")).toString();
            if (severity.compare(QLatin1StringView("error"), Qt::CaseInsensitive) != 0 &&
                severity.compare(QLatin1StringView("fatal error"), Qt::CaseInsensitive) != 0)
            {
                continue;
            }
            const QJsonObject location = diagnostic.value(QStringLiteral("location")).toObject();
            const QString file = location.value(QStringLiteral("source")).toString();
            const QString target = location.value(QStringLiteral("node")).toString();
            const foundation::int32 line = location.value(QStringLiteral("line")).toInt();
            const foundation::int32 column = location.value(QStringLiteral("column")).toInt();
            QString address = file;
            if (line > 0)
            {
                address += QStringLiteral(":%1:%2").arg(line).arg(column);
            }
            if (target.isEmpty() == false)
            {
                address += QStringLiteral(" (%1)").arg(target);
            }
            const QString description = DescribeRenderGraphDiagnostic(diagnostic);
            recovery.Diagnostics += QStringLiteral("\n\n%1 %2\n%3")
                                        .arg(diagnostic.value(QStringLiteral("code")).toString(), address, description);
            if (recovery.DiagnosticRow >= 0)
            {
                continue;
            }
            const ViewportRenderAction action = GetDiagnosticAction(mDocument, request.Source, location);
            if (action != ViewportRenderAction::ShowDiagnostics)
            {
                recovery.DiagnosticRow = row;
                recovery.Action = action;
                recovery.Guidance = action == ViewportRenderAction::OpenShader
                                        ? QStringLiteral("Open the shader at the line that needs attention.")
                                        : QStringLiteral("Open the graph at the node that needs attention.");
            }
        }
    }
    if (hasFrame == true)
    {
        recovery.Message += QStringLiteral(" The last rendered image is still shown.");
    }
    if (recovery.Guidance.isEmpty() == true && recovery.Action == ViewportRenderAction::ShowDiagnostics)
    {
        recovery.Guidance =
            QStringLiteral("Copy the technical details to share with Ludus support if this keeps happening.");
    }
    return recovery;
}

bool RendererWorkspace::OpenViewportRenderDiagnostic(foundation::uint64 request, foundation::int32 row)
{
    if (request == 0 || row < 0 || mLastResult.Request.Request != request)
    {
        return false;
    }
    const ViewportRenderRecovery current = GetViewportRenderRecovery();
    if (current.Request != request || current.DiagnosticRow != row)
    {
        return false;
    }
    const QJsonObject location = getDiagnosticLocation(row);
    if (current.Action == ViewportRenderAction::OpenShader && location.value(QStringLiteral("line")).toInt() <= 0)
    {
        openSourceLine(location.value(QStringLiteral("source")).toString(), 1);
        return true;
    }
    return ActivateDiagnostic(row);
}

bool RendererWorkspace::RestoreViewportPreview(bool shouldUseRaster)
{
    if (mPreviewSourceOrNull == nullptr)
    {
        return false;
    }
    RenderPreview& preview = *mPreviewSourceOrNull;
    preview.Stop();
    if (shouldUseRaster == true)
    {
        preview.SetForceRaster(true);
    }
    preview.SetProjectGraphProblem({});
    return preview.Start();
}

ViewportRenderRecoveryNotice::ViewportRenderRecoveryNotice(RendererWorkspace& inoutWorkspace, QWidget* parent)
    : QWidget(parent), mWorkspace(inoutWorkspace), mMessage(new QLabel(this)), mActionProblem(new QLabel(this)),
      mPrimary(new QPushButton(this)), mDetails(new QPushButton(QStringLiteral("Show diagnostics"), this)),
      mRaster(new QPushButton(QStringLiteral("Use raster preview"), this))
{
    setObjectName(QStringLiteral("viewportRenderRecovery"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    mMessage->setObjectName(QStringLiteral("viewportRenderRecoveryMessage"));
    mMessage->setTextFormat(Qt::PlainText);
    mMessage->setWordWrap(true);
    mActionProblem->setTextFormat(Qt::PlainText);
    mActionProblem->setWordWrap(true);
    mActionProblem->hide();
    mPrimary->setObjectName(QStringLiteral("viewportRenderRecoveryPrimary"));
    mPrimary->setProperty("visualRole", QStringLiteral("primary"));
    mDetails->setObjectName(QStringLiteral("viewportRenderRecoveryDetails"));
    mRaster->setObjectName(QStringLiteral("viewportRenderRecoveryRaster"));
    QGridLayout* layout = new QGridLayout(this);
    layout->setContentsMargins(design::SPACING_SMALL,
                               design::SPACING_SMALL,
                               design::SPACING_SMALL,
                               design::SPACING_SMALL);
    layout->setColumnStretch(2, 1);
    constexpr foundation::int32 COLUMN_COUNT = 3;
    layout->addWidget(mMessage, 0, 0, 1, COLUMN_COUNT);
    layout->addWidget(mPrimary, 1, 0);
    layout->addWidget(mDetails, 1, 1);
    layout->addWidget(mRaster, 1, 2, Qt::AlignLeft);
    layout->addWidget(mActionProblem, 2, 0, 1, COLUMN_COUNT);

    connect(mPrimary, &QPushButton::clicked, this, &ViewportRenderRecoveryNotice::activatePrimary);
    connect(mDetails, &QPushButton::clicked, this, &ViewportRenderRecoveryNotice::showDiagnostics);
    connect(mRaster, &QPushButton::clicked, this, [this]() {
        if (mWorkspace.GetViewportRenderRecovery().CanUseRaster == true)
        {
            if (RenderPreview* previewOrNull = mWorkspace.GetPreviewSourceOrNull(); previewOrNull != nullptr)
            {
                previewOrNull->SetForceRaster(true);
            }
            refresh();
        }
    });
    if (RenderGraphPreview* previewOrNull = mWorkspace.GetPreviewOrNull(); previewOrNull != nullptr)
    {
        connect(previewOrNull,
                &RenderGraphPreview::Changed,
                this,
                &ViewportRenderRecoveryNotice::refresh,
                Qt::QueuedConnection);
    }
    connect(&mWorkspace, &RendererWorkspace::RenderProblemChanged, this, &ViewportRenderRecoveryNotice::refresh);
    connect(&mWorkspace, &RendererWorkspace::DocumentChanged, this, &ViewportRenderRecoveryNotice::refresh);
    connect(&mWorkspace, &RendererWorkspace::JobFinished, this, &ViewportRenderRecoveryNotice::refresh);
    connect(&mWorkspace, &RendererWorkspace::CommandsChanged, this, &ViewportRenderRecoveryNotice::refresh);
    refresh();
}

bool ViewportRenderRecoveryNotice::HasProblem() const
{
    return mRecovery.Message.isEmpty() == false;
}

void ViewportRenderRecoveryNotice::refresh()
{
    const bool hasPreviousProblem = HasProblem();
    const QString previousDiagnostic = mRecovery.Diagnostics;
    mRecovery = mWorkspace.GetViewportRenderRecovery();
    if (previousDiagnostic != mRecovery.Diagnostics)
    {
        mActionProblem->hide();
    }
    if (HasProblem() == true)
    {
        mMessage->setText(mRecovery.Guidance.isEmpty() == true
                              ? mRecovery.Message
                              : mRecovery.Message + QLatin1Char('\n') + mRecovery.Guidance);
        mPrimary->setText(GetActionLabel(mRecovery.Action));
        mDetails->setVisible(mRecovery.Action != ViewportRenderAction::ShowDiagnostics);
        mRaster->setVisible(mRecovery.CanUseRaster);
    }
    setVisible(HasProblem());
    if (hasPreviousProblem != HasProblem())
    {
        Q_EMIT ProblemVisibilityChanged(HasProblem());
    }
}

void ViewportRenderRecoveryNotice::activatePrimary()
{
    switch (mRecovery.Action)
    {
        case ViewportRenderAction::OpenShader:
        case ViewportRenderAction::ShowGraphNode:
            if (mWorkspace.OpenViewportRenderDiagnostic(mRecovery.Request, mRecovery.DiagnosticRow) == false)
            {
                refresh();
                mActionProblem->setText(QStringLiteral("The source location is unavailable. Build & Preview to refresh "
                                                       "it, or open the diagnostics for this failure."));
                mActionProblem->show();
            }
            return;
        case ViewportRenderAction::RebuildGraph:
            if (mWorkspace.BuildAndPreview() == false)
            {
                refresh();
                if (HasProblem() == true && mWorkspace.IsJobActive() == false)
                {
                    mActionProblem->setText(mWorkspace.GetStatusText());
                    mActionProblem->show();
                }
            }
            return;
        case ViewportRenderAction::UseProjectRenderer:
            if (RenderPreview* previewOrNull = mWorkspace.GetPreviewSourceOrNull(); previewOrNull != nullptr)
            {
                previewOrNull->SetForceRaster(false);
            }
            refresh();
            return;
        case ViewportRenderAction::RestartPreview:
        case ViewportRenderAction::RestartRasterPreview: {
            const bool shouldUseRaster = mRecovery.Action == ViewportRenderAction::RestartRasterPreview;
            const ViewportRenderRecovery failure = mRecovery;
            if (mWorkspace.RestoreViewportPreview(shouldUseRaster) == false)
            {
                mRecovery = failure;
                mRecovery.Message = QStringLiteral("The preview still couldn't start.");
                mRecovery.Guidance = QStringLiteral("Close and reopen Ludus, then open this World again.");
                mRecovery.Action = ViewportRenderAction::ShowDiagnostics;
                mMessage->setText(mRecovery.Message + QLatin1Char('\n') + mRecovery.Guidance);
                mPrimary->setText(GetActionLabel(mRecovery.Action));
                mDetails->hide();
                mRaster->hide();
                show();
            }
            return;
        }
        case ViewportRenderAction::ShowDiagnostics:
            showDiagnostics();
            return;
    }
}

void ViewportRenderRecoveryNotice::showDiagnostics()
{
    QDialog* dialog = new QDialog(this);
    dialog->setWindowTitle(QStringLiteral("Renderer diagnostics"));
    dialog->setObjectName(QStringLiteral("viewportRenderDiagnosticsDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    QLabel* message = new QLabel(mRecovery.Message, dialog);
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    QLabel* guidance = new QLabel(mRecovery.Guidance, dialog);
    guidance->setTextFormat(Qt::PlainText);
    guidance->setWordWrap(true);
    QToolButton* technicalDetails = new QToolButton(dialog);
    technicalDetails->setText(QStringLiteral("Technical details"));
    technicalDetails->setCheckable(true);
    QPlainTextEdit* details = new QPlainTextEdit(mRecovery.Diagnostics, dialog);
    details->setReadOnly(true);
    details->hide();
    connect(technicalDetails, &QToolButton::toggled, details, &QWidget::setVisible);
    details->setAccessibleName(QStringLiteral("Renderer failure details"));
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    QPushButton* copy = buttons->addButton(QStringLiteral("Copy diagnostics"), QDialogButtonBox::ActionRole);
    copy->setObjectName(QStringLiteral("viewportCopyRenderDiagnostics"));
    QVBoxLayout* layout = new QVBoxLayout(dialog);
    layout->addWidget(message);
    layout->addWidget(guidance);
    const ViewportRenderAction action = mRecovery.Action;
    if (action != ViewportRenderAction::ShowDiagnostics)
    {
        QPushButton* next = buttons->addButton(GetActionLabel(action), QDialogButtonBox::ActionRole);
        next->setDefault(true);
        connect(next, &QPushButton::clicked, this, [this, dialog, action]() {
            dialog->accept();
            if (HasProblem() == true && mRecovery.Action == action)
            {
                activatePrimary();
            }
        });
    }
    layout->addWidget(technicalDetails, 0, Qt::AlignLeft);
    layout->addWidget(details, 1);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(copy, &QPushButton::clicked, dialog, [details]() {
        QApplication::clipboard()->setText(details->toPlainText());
    });
    dialog->open();
}
} // namespace ludus::editor
