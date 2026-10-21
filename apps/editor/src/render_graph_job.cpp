#include "internal/render_graph_job.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#if defined(Q_OS_WASM)
// Browser builds have no child processes; render-graph jobs report unavailable.
#else
#    include <QProcess>
#    include <QProcessEnvironment>
#endif

namespace ludus::editor
{
using namespace foundation;

int32 RenderGraphJobResult::GetDiagnosticCount() const
{
    const bool hasBatch = Diagnostics.isEmpty() == false && Code.isEmpty() == false &&
                          Diagnostics.first().toObject().value(QStringLiteral("code")).toString() == Code;
    return IsSuccessful == true ? 1 : static_cast<int32>(Diagnostics.size()) + (hasBatch == true ? 0 : 1);
}

QJsonObject RenderGraphJobResult::GetDiagnostic(int32 row) const
{
    if (row < 0 || row >= GetDiagnosticCount() || IsSuccessful == true)
    {
        return {};
    }
    const bool hasBatch = Diagnostics.isEmpty() == false && Code.isEmpty() == false &&
                          Diagnostics.first().toObject().value(QStringLiteral("code")).toString() == Code;
    if (hasBatch == true)
    {
        return Diagnostics.at(row).toObject();
    }
    return row == 0 ? QJsonObject{{QStringLiteral("code"), Code},
                                  {QStringLiteral("severity"), QStringLiteral("error")},
                                  {QStringLiteral("message"), Message},
                                  {QStringLiteral("location"), Location}}
                    : Diagnostics.at(row - 1).toObject();
}

QString DescribeRenderGraphDiagnostic(const QJsonObject& diagnostic)
{
    QString description = diagnostic.value(QStringLiteral("message")).toString();
    for (const QString& key : {QStringLiteral("expected"), QStringLiteral("actual"), QStringLiteral("repair")})
    {
        const QString detail = diagnostic.value(key).toString();
        if (detail.isEmpty() == false)
        {
            description += QStringLiteral("\n%1: %2").arg(key, detail);
        }
    }
    return description;
}

RenderGraphJob::RenderGraphJob(QObject* parentOrNull) : QObject(parentOrNull) {}

#if defined(Q_OS_WASM)
RenderGraphJob::~RenderGraphJob() = default;

bool RenderGraphJob::Start(const ToolingPaths&, const RenderGraphJobRequest&)
{
    return false;
}

void RenderGraphJob::Cancel() {}

void RenderGraphJob::finish(int32, bool) {}
#else
RenderGraphJob::~RenderGraphJob()
{
    if (mProcessOrNull != nullptr)
    {
        mProcessOrNull->disconnect(this);
        mProcessOrNull->kill();
        mProcessOrNull->waitForFinished(1000);
    }
}

bool RenderGraphJob::Start(const ToolingPaths& tooling, const RenderGraphJobRequest& request)
{
    const QString package = QDir(tooling.ToolingRoot).filePath(QStringLiteral("scripts/python"));
    if (mProcessOrNull != nullptr || tooling.ToolingRoot.isEmpty() == true ||
        QFileInfo(QDir(package).filePath(QStringLiteral("ludus_tools/render_graph.py"))).isFile() == false)
    {
        return false;
    }
    mRequest = request;
    mOutput.clear();
    mIsCancelled = false;
    mProcessOrNull = new QProcess(this);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONPATH"), package);
    environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    mProcessOrNull->setProcessEnvironment(environment);
    mProcessOrNull->setProcessChannelMode(QProcess::SeparateChannels);
    connect(mProcessOrNull, &QProcess::readyReadStandardOutput, this, [this]() {
        const QByteArray chunk = mProcessOrNull->readAllStandardOutput();
        if (mOutput.size() + chunk.size() > MAX_OUTPUT_BYTES)
        {
            mProcessOrNull->kill();
            return;
        }
        mOutput.append(chunk);
    });
    connect(mProcessOrNull, &QProcess::readyReadStandardError, this, [this]() {
        // Human diagnostics are not parsed; the JSON result is authoritative.
        static_cast<void>(mProcessOrNull->readAllStandardError());
    });
    connect(mProcessOrNull, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        finish(exitCode, status == QProcess::CrashExit);
    });
    connect(mProcessOrNull, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
        {
            finish(-1, true);
        }
    });
    const QString python = tooling.PythonPath.isEmpty() == false ? tooling.PythonPath : QStringLiteral("python3");
    QStringList arguments{QStringLiteral("-m"),
                          QStringLiteral("ludus_tools"),
                          QStringLiteral("--json"),
                          QStringLiteral("render-graph"),
                          request.Action};
    arguments += request.Arguments;
    mProcessOrNull->start(python, arguments);
    return true;
}

void RenderGraphJob::Cancel()
{
    if (mProcessOrNull != nullptr)
    {
        mIsCancelled = true;
        mProcessOrNull->kill();
    }
}

void RenderGraphJob::finish(int32 exitCode, bool isCrashed)
{
    if (mProcessOrNull == nullptr)
    {
        return;
    }
    mOutput.append(mProcessOrNull->readAllStandardOutput());
    QProcess* process = mProcessOrNull;
    mProcessOrNull = nullptr;
    process->disconnect(this);
    process->deleteLater();
    RenderGraphJobResult result;
    if (mIsCancelled == true)
    {
        result.Request = mRequest;
        result.IsCancelled = true;
        result.Code = QStringLiteral("RenderGraphCancelled");
        result.Message = QStringLiteral("Cancelled; previous results and packages are unchanged.");
    }
    else if (isCrashed == true && mOutput.isEmpty() == true)
    {
        result.Request = mRequest;
        result.Code = QStringLiteral("RenderGraphToolUnavailable");
        result.Message = QStringLiteral("The render-graph tool could not run. Check the editor's Python tooling.");
    }
    else
    {
        result = ParseOutput(mRequest, mOutput, exitCode);
    }
    Q_EMIT Finished(result);
}
#endif

bool RenderGraphJob::IsActive() const noexcept
{
    return mProcessOrNull != nullptr;
}

RenderGraphJobResult
RenderGraphJob::ParseOutput(const RenderGraphJobRequest& request, const QByteArray& output, int32 exitCode)
{
    RenderGraphJobResult result;
    result.Request = request;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(output.trimmed(), &error);
    const QJsonObject object = document.object();
    if (error.error != QJsonParseError::NoError || document.isObject() == false ||
        object.value(QStringLiteral("schema")).toInt() != 1)
    {
        result.Code = QStringLiteral("RenderGraphToolOutput");
        result.Message =
            QStringLiteral("The render-graph tool returned no versioned JSON result (exit %1).").arg(exitCode);
        return result;
    }
    if (object.contains(QStringLiteral("error")) == true)
    {
        const QJsonObject failure = object.value(QStringLiteral("error")).toObject();
        result.Code = failure.value(QStringLiteral("code")).toString();
        result.Message = failure.value(QStringLiteral("message")).toString();
        result.Location = failure.value(QStringLiteral("location")).toObject();
        result.Diagnostics = failure.value(QStringLiteral("diagnostics")).toArray();
        return result;
    }
    result.Payload = object;
    result.IsSuccessful = exitCode == 0;
    if (result.IsSuccessful == false)
    {
        result.Code = QStringLiteral("RenderGraphRejected");
        result.Message = QStringLiteral("The render-graph tool reported a rejected result.");
    }
    return result;
}

} // namespace ludus::editor
