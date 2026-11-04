#include "internal/saved_graph_comparison_view.h"

#include "internal/graph_comparison_run.h"
#include "internal/viewport_reference_capture.h"

#include <ludus/foundation/base/types.h>

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QMessageBox>
#include <QPair>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QStringList>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

namespace ludus::editor
{
namespace
{
using namespace foundation;
constexpr qint64 MAXIMUM_MANIFEST_BYTES = 1024 * 1024;
constexpr int32 VIEW_WIDTH = 1280;
constexpr int32 VIEW_HEIGHT = 800;
constexpr int32 PREVIEW_WIDTH = 300;
constexpr int32 PREVIEW_HEIGHT = 260;
constexpr int32 METRIC_PRECISION = 6;
constexpr float64 PERCENT = 100;

QLabel* AddText(QVBoxLayout& inoutLayout, const QString& text)
{
    QLabel* label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    inoutLayout.addWidget(label);
    return label;
}

QJsonObject ReadManifest(const QString& path, QString& outError)
{
    QFile file(path);
    if (file.open(QIODevice::ReadOnly) == false)
    {
        outError = QStringLiteral("Could not open %1. %2").arg(path, file.errorString());
        return {};
    }
    const QByteArray bytes = file.read(MAXIMUM_MANIFEST_BYTES + 1);
    if (bytes.size() > MAXIMUM_MANIFEST_BYTES)
    {
        outError = QStringLiteral("Choose the comparison.json file from a Compare graphs results folder.");
        return {};
    }
    QJsonParseError parseError;
    const QJsonObject run = QJsonDocument::fromJson(bytes, &parseError).object();
    if (parseError.error != QJsonParseError::NoError)
    {
        outError = QStringLiteral("The comparison file could not be read. %1. Choose an intact comparison.json.")
                       .arg(parseError.errorString());
        return {};
    }
    const QJsonValue candidates = run.value(QStringLiteral("candidates"));
    const QJsonValue results = run.value(QStringLiteral("results"));
    const QSize extent(run.value(QStringLiteral("width")).toInt(), run.value(QStringLiteral("height")).toInt());
    if (run.value(QStringLiteral("baseline")).isObject() == false || candidates.isArray() == false ||
        results.isArray() == false || candidates.toArray().size() > MAXIMUM_GRAPH_COMPARISON_CANDIDATES ||
        results.toArray().size() > candidates.toArray().size() || extent.isEmpty() == true ||
        extent.width() > static_cast<int32>(MAXIMUM_REFERENCE_LONG_SIDE) ||
        extent.height() > static_cast<int32>(MAXIMUM_REFERENCE_LONG_SIDE))
    {
        outError = QStringLiteral("Choose the comparison.json file from a Compare graphs results folder.");
        return {};
    }
    return run;
}

void AddImage(QVBoxLayout& inoutLayout,
              const QDir& directory,
              const QString& name,
              const QString& title,
              const QSize& extent)
{
    AddText(inoutLayout, title);
    if (name.isEmpty() == true)
    {
        AddText(inoutLayout, QStringLiteral("No image was saved."));
        return;
    }
    const QFileInfo file(directory.filePath(name));
    const QString path = file.canonicalFilePath();
    if (file.fileName() != name || file.suffix() != QStringLiteral("png") || path.isEmpty() == true ||
        QFileInfo(path).absolutePath() != directory.canonicalPath())
    {
        AddText(inoutLayout, QStringLiteral("Image unavailable. Restore %1 in the results folder.").arg(name));
        return;
    }
    QImageReader reader(path, QByteArrayLiteral("png"));
    if (reader.size() != extent)
    {
        AddText(inoutLayout, QStringLiteral("Image size does not match the run. Restore the saved %1.").arg(name));
        return;
    }
    reader.setScaledSize(extent.scaled(PREVIEW_WIDTH, PREVIEW_HEIGHT, Qt::KeepAspectRatio));
    const QImage image = reader.read();
    if (image.isNull() == true)
    {
        AddText(inoutLayout, QStringLiteral("Could not read %1. Restore the saved image.").arg(name));
        return;
    }
    QLabel* label = new QLabel;
    label->setObjectName(QStringLiteral("savedComparisonThumbnail"));
    label->setAccessibleName(title);
    label->setAlignment(Qt::AlignCenter);
    label->setPixmap(QPixmap::fromImage(image));
    inoutLayout.addWidget(label);
    QPushButton* open = new QPushButton(QStringLiteral("Open full image"));
    open->setAccessibleName(QStringLiteral("Open full image for %1").arg(title));
    inoutLayout.addWidget(open);
    QObject::connect(open, &QPushButton::clicked, open, [open, path]() {
        if (QDesktopServices::openUrl(QUrl::fromLocalFile(path)) == false)
        {
            QMessageBox::warning(open,
                                 QStringLiteral("Could not open image"),
                                 QStringLiteral("Open %1 from the results folder.").arg(path));
        }
    });
}

void AddMetrics(QVBoxLayout& inoutLayout, const QJsonObject& row)
{
    const QJsonObject metrics = row.value(QStringLiteral("metrics")).toObject();
    if (metrics.isEmpty() == true)
    {
        AddText(inoutLayout, QStringLiteral("No comparison metrics were saved."));
    }
    else
    {
        const QString space = metrics.value(QStringLiteral("space")).toString();
        AddText(inoutLayout,
                space == QStringLiteral("linear") ? QStringLiteral("Scene-linear RGB")
                                                  : QStringLiteral("sRGB display"));
        const QString summary = row.value(QStringLiteral("summary")).toString();
        if (summary.contains(QStringLiteral("partial diagnostic")) == true)
        {
            AddText(inoutLayout, QStringLiteral("Partial diagnostic"));
        }
        const QJsonValue compared = metrics.value(QStringLiteral("comparedPixels"));
        const QJsonValue total = metrics.value(QStringLiteral("totalPixels"));
        if (compared.isDouble() == true && total.isDouble() == true)
        {
            AddText(inoutLayout,
                    QStringLiteral("Compared pixels  %1 / %2").arg(compared.toInteger()).arg(total.toInteger()));
        }
        const QPair<QString, QString> fields[]{
            {QStringLiteral("meanAbsoluteError"), QStringLiteral("MAE")},
            {QStringLiteral("rootMeanSquareError"), QStringLiteral("RMSE")},
            {QStringLiteral("maximumAbsoluteError"), QStringLiteral("Max error")},
            {QStringLiteral("meanSignedLuminanceError"), QStringLiteral("Luminance bias")},
            {QStringLiteral("relativeMeanSquareError"), QStringLiteral("Relative MSE")},
            {QStringLiteral("peakSignalToNoiseRatio"), QStringLiteral("PSNR (dB)")},
        };
        const QJsonValue visiblyDifferent = metrics.value(QStringLiteral("visiblyDifferentFraction"));
        if (visiblyDifferent.isDouble() == true)
        {
            AddText(inoutLayout,
                    QStringLiteral("Visibly different  %1%")
                        .arg(QString::number(visiblyDifferent.toDouble() * PERCENT, 'g', METRIC_PRECISION)));
        }
        for (const QPair<QString, QString>& field : fields)
        {
            const QJsonValue value = metrics.value(field.first);
            if (value.isDouble() == true)
            {
                AddText(inoutLayout,
                        QStringLiteral("%1  %2").arg(field.second,
                                                     QString::number(value.toDouble(), 'g', METRIC_PRECISION)));
            }
        }
    }
    for (const QString& field : {QStringLiteral("message"),
                                 QStringLiteral("exportError"),
                                 QStringLiteral("comparisonError"),
                                 QStringLiteral("differenceError")})
    {
        const QString message = row.value(field).toString();
        if (message.isEmpty() == false)
        {
            AddText(inoutLayout, message);
        }
    }
}

void AddCandidate(QVBoxLayout& inoutLayout,
                  const QDir& directory,
                  const QSize& extent,
                  qsizetype index,
                  const QJsonObject& candidate,
                  const QJsonObject& row,
                  const QString& runState)
{
    const QString label =
        row.value(QStringLiteral("label")).toString(candidate.value(QStringLiteral("label")).toString());
    const QString missingState =
        runState == QStringLiteral("running") ? QStringLiteral("pending at save") : QStringLiteral("skipped");
    const QString state = row.value(QStringLiteral("state")).toString(missingState);
    QGroupBox* card = new QGroupBox;
    card->setObjectName(QStringLiteral("savedComparisonCandidate"));
    QVBoxLayout* layout = new QVBoxLayout(card);
    AddText(*layout, QStringLiteral("%1. %2 · %3").arg(index + 1).arg(label, state));
    QHBoxLayout* columns = new QHBoxLayout;
    QVBoxLayout* render = new QVBoxLayout;
    QVBoxLayout* difference = new QVBoxLayout;
    QVBoxLayout* metrics = new QVBoxLayout;
    AddImage(*render, directory, row.value(QStringLiteral("image")).toString(), QStringLiteral("Render"), extent);
    AddImage(*difference,
             directory,
             row.value(QStringLiteral("difference")).toString(),
             QStringLiteral("Saved difference"),
             extent);
    AddMetrics(*metrics, row);
    metrics->addStretch();
    columns->addLayout(render, 1);
    columns->addLayout(difference, 1);
    columns->addLayout(metrics, 1);
    layout->addLayout(columns);
    inoutLayout.addWidget(card);
}
} // namespace

QWidget* CreateSavedGraphComparisonView(const QString& manifestPath, QWidget* parentOrNull, QString& outError)
{
    const QJsonObject run = ReadManifest(manifestPath, outError);
    if (run.isEmpty() == true)
    {
        return nullptr;
    }
    const QDir directory = QFileInfo(manifestPath).absoluteDir();
    const QSize extent(run.value(QStringLiteral("width")).toInt(), run.value(QStringLiteral("height")).toInt());
    const QString state = run.value(QStringLiteral("state")).toString();
    QWidget* window = new QWidget(parentOrNull, Qt::Window);
    window->setObjectName(QStringLiteral("savedGraphComparisonView"));
    window->setWindowTitle(QStringLiteral("Saved graph comparison"));
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->resize(VIEW_WIDTH, VIEW_HEIGHT);
    QVBoxLayout* layout = new QVBoxLayout(window);
    AddText(*layout,
            QStringLiteral("%1 · %2 × %3 · %4")
                .arg(directory.dirName())
                .arg(extent.width())
                .arg(extent.height())
                .arg(state));
    AddText(*layout,
            QStringLiteral("Saved differences show per-pixel luminance changes. Red is brighter, blue is darker, "
                           "and grey marks pixels without a comparison."));
    QHBoxLayout* body = new QHBoxLayout;
    QVBoxLayout* baselineLayout = new QVBoxLayout;
    const QJsonObject baseline = run.value(QStringLiteral("baseline")).toObject();
    AddImage(*baselineLayout, directory, QStringLiteral("baseline.png"), QStringLiteral("Shared baseline"), extent);
    const QString source = baseline.value(QStringLiteral("source")).toString();
    AddText(
        *baselineLayout,
        QStringLiteral("%1 · %2").arg(source.isEmpty() == true ? QStringLiteral("RT1") : QFileInfo(source).fileName(),
                                      baseline.value(QStringLiteral("state")).toString()));
    const QString traversal = baseline.value(QStringLiteral("traversal")).toString();
    if (traversal.isEmpty() == false)
    {
        AddText(*baselineLayout, traversal);
    }
    const QString error = baseline.value(QStringLiteral("error")).toString();
    if (error.isEmpty() == false)
    {
        AddText(*baselineLayout, error);
    }
    baselineLayout->addStretch();
    body->addLayout(baselineLayout);

    QScrollArea* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    QWidget* series = new QWidget;
    QVBoxLayout* seriesLayout = new QVBoxLayout(series);
    const QJsonArray candidates = run.value(QStringLiteral("candidates")).toArray();
    const QJsonArray results = run.value(QStringLiteral("results")).toArray();
    for (qsizetype index = 0; index < candidates.size(); ++index)
    {
        const QJsonObject result = index < results.size() ? results.at(index).toObject() : QJsonObject{};
        AddCandidate(*seriesLayout, directory, extent, index, candidates.at(index).toObject(), result, state);
    }
    seriesLayout->addStretch();
    scroll->setWidget(series);
    body->addWidget(scroll, 1);
    layout->addLayout(body);
    return window;
}

void OpenSavedGraphComparison(QWidget& inoutParent, const QString& directory)
{
    const QString path = QFileDialog::getOpenFileName(&inoutParent,
                                                      QStringLiteral("Open saved graph comparison"),
                                                      directory,
                                                      QStringLiteral("Comparison files (*.json)"));
    if (path.isEmpty() == true)
    {
        return;
    }
    QString error;
    QWidget* windowOrNull = CreateSavedGraphComparisonView(path, &inoutParent, error);
    if (windowOrNull == nullptr)
    {
        QMessageBox::warning(&inoutParent, QStringLiteral("Could not open comparison"), error);
        return;
    }
    windowOrNull->show();
}
} // namespace ludus::editor
