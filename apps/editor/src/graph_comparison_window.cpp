#include "internal/graph_comparison_window.h"

#include "internal/captured_image_view.h"
#include "internal/reference_capture_window.h"
#include "internal/render_graph_document.h"
#include "internal/render_preview.h"
#include "internal/renderer_workspace.h"
#include "internal/saved_graph_comparison_view.h"

#include <QColorSpace>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QJsonDocument>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QUrl>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
using namespace foundation;
constexpr int32 TARGET_COLUMN = 0;
constexpr int32 SAMPLES_COLUMN = 1;
constexpr int32 BOUNCES_COLUMN = 2;
constexpr int32 MODE_COLUMN = 3;
constexpr int32 RESULT_COLUMN = 4;
constexpr int32 METRICS_COLUMN = 5;
constexpr int32 DISPLAY_FRAME_INDEX = 1;
constexpr int32 DEFAULT_GRAPH_COMPARISON_SAMPLES = 16;
constexpr int32 SERVICE_INTERVAL_MILLISECONDS = 16;
constexpr int32 COMPARISON_WINDOW_WIDTH = 1100;
constexpr int32 COMPARISON_WINDOW_HEIGHT = 740;

QVector<RenderGraphCaptureTarget> GetComparisonTargets(RendererWorkspace& inoutWorkspace)
{
    RenderGraphDocument& document = inoutWorkspace.GetDocument();
    const QString root = document.GetRootGraph();
    const QJsonObject graph = QJsonDocument::fromJson(document.GetSaved(root)).object();
    const QString graphId = graph.value(QStringLiteral("id")).toString();
    QVector<RenderGraphCaptureTarget> targets;
    for (const QJsonValueConstRef value : graph.value(QStringLiteral("configurations")).toArray())
    {
        const QJsonObject configuration = value.toObject();
        const QString name = configuration.value(QStringLiteral("id")).toString();
        targets.append({root, name, {}, true});
        for (const QJsonValueConstRef nodeValue : configuration.value(QStringLiteral("nodes")).toArray())
        {
            const QJsonObject node = nodeValue.toObject();
            if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Export") &&
                node.value(QStringLiteral("type")).toString() == QStringLiteral("ColorTexture"))
            {
                targets.append(
                    {root, name, graphId + QLatin1Char('/') + node.value(QStringLiteral("id")).toString(), true});
            }
        }
    }
    targets.append(inoutWorkspace.GetCaptureTargets());
    return targets;
}

void ConfigureBudget(QSpinBox& inoutControl, uint32 maximum, uint32 value)
{
    inoutControl.setRange(1, static_cast<int32>(maximum));
    inoutControl.setValue(static_cast<int32>(value));
}

QImage ReadDisplayBaseline(const QString& path, QString& outError)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (size.isValid() == false || size.width() > static_cast<int32>(MAXIMUM_REFERENCE_LONG_SIDE) ||
        size.height() > static_cast<int32>(MAXIMUM_REFERENCE_LONG_SIDE))
    {
        outError = QStringLiteral("Export a readable PNG, JPEG or WebP with a long side no larger than %1.")
                       .arg(MAXIMUM_REFERENCE_LONG_SIDE);
        return {};
    }
    QImage source = reader.read();
    if (source.isNull() == true)
    {
        outError =
            QStringLiteral("Could not read the baseline: %1. Export the image as sRGB PNG.").arg(reader.errorString());
        return {};
    }
    const QColorSpace srgb(QColorSpace::SRgb);
    if (source.colorSpace().isValid() == true && source.colorSpace() != srgb)
    {
        source.convertToColorSpace(srgb);
    }
    QImage opaque(source.size(), QImage::Format_RGB32);
    if (opaque.isNull() == true)
    {
        outError = QStringLiteral("Could not allocate the baseline image. Export it at a smaller resolution.");
        return {};
    }
    opaque.fill(Qt::black);
    QPainter painter(&opaque);
    painter.drawImage(QPoint{}, source);
    painter.end();
    opaque.setColorSpace(srgb);
    return opaque;
}
} // namespace

GraphComparisonWindow::GraphComparisonWindow(RenderPreview& inoutPreview,
                                             RendererWorkspace& inoutWorkspace,
                                             const ViewportReferenceSettings& reference,
                                             bool hasSelectedTraversal,
                                             QWidget* parent)
    : QWidget(parent, Qt::Window), mPreview(&inoutPreview), mWorkspace(&inoutWorkspace), mSettings(reference),
      mTargets(GetComparisonTargets(inoutWorkspace)), mHasSelectedTraversal(hasSelectedTraversal),
      mCandidates(new QTableWidget(this)), mLongSide(new QSpinBox(this)), mGraphSamples(new QSpinBox(this)),
      mGraphBounces(new QSpinBox(this)), mReferenceSamples(new QSpinBox(this)), mReferenceDepth(new QSpinBox(this)),
      mReferenceTraversal(new QComboBox(this)), mAdd(new QPushButton(QStringLiteral("Add graph"), this)),
      mRemove(new QPushButton(QStringLiteral("Remove selected"), this)),
      mChoose(new QPushButton(QStringLiteral("Choose display baseline…"), this)),
      mUseRt1(new QPushButton(QStringLiteral("Use RT1"), this)),
      mStart(new QPushButton(QStringLiteral("Run and save…"), this)),
      mStop(new QPushButton(QStringLiteral("Stop"), this)),
      mOpenResults(new QPushButton(QStringLiteral("Open results folder"), this)),
      mBaselineLabel(new QLabel(QStringLiteral("Generate RT1 once for all candidates"), this)),
      mStatus(new QLabel(this)), mDetails(new QLabel(this)),
      mBaselineView(new CapturedImageView(QStringLiteral("baseline"), this)),
      mCandidateView(new CapturedImageView(QStringLiteral("candidate"), this)),
      mDifferenceView(new CapturedImageView(QStringLiteral("difference"), this))
{
    setWindowTitle(QStringLiteral("Compare render graphs"));
    setObjectName(QStringLiteral("graphComparisonWindow"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(COMPARISON_WINDOW_WIDTH, COMPARISON_WINDOW_HEIGHT);

    mLongSide->setRange(static_cast<int32>(MINIMUM_REFERENCE_LONG_SIDE),
                        static_cast<int32>(MAXIMUM_REFERENCE_LONG_SIDE));
    mLongSide->setValue(static_cast<int32>(reference.LongSide));
    ConfigureBudget(*mGraphSamples, MAXIMUM_REFERENCE_SAMPLES, DEFAULT_GRAPH_COMPARISON_SAMPLES);
    mGraphBounces->setRange(0, static_cast<int32>(MAXIMUM_REFERENCE_DEPTH));
    mGraphBounces->setSpecialValueText(QStringLiteral("Authored / unknown"));
    ConfigureBudget(*mReferenceSamples, MAXIMUM_REFERENCE_SAMPLES, reference.Samples);
    ConfigureBudget(*mReferenceDepth, MAXIMUM_REFERENCE_DEPTH, reference.MaxDepth);
    mReferenceTraversal->setObjectName(QStringLiteral("graphComparisonReferenceTraversal"));
    PopulateReferenceTraversalChoices(*mReferenceTraversal, reference.Traversal);
    updateTraversalAvailability();

    QVBoxLayout* layout = new QVBoxLayout(this);
    QFormLayout* form = new QFormLayout;
    form->addRow(QStringLiteral("Long side (px)"), mLongSide);
    form->addRow(QStringLiteral("Graph default samples/pixel"), mGraphSamples);
    form->addRow(QStringLiteral("Graph expected bounces"), mGraphBounces);
    form->addRow(QStringLiteral("RT1 samples/pixel"), mReferenceSamples);
    form->addRow(QStringLiteral("RT1 max depth"), mReferenceDepth);
    form->addRow(QStringLiteral("RT1 traversal"), mReferenceTraversal);
    layout->addLayout(form);
    QLabel* explanation =
        new QLabel(QStringLiteral("Use compatible authored graph variants for bounce matching. "
                                  "Expected bounces are recorded; actual graph limits are shader-authored "
                                  "and unknown to capture. RT1 uses the pixel sampling "
                                  "selected in Reference Capture."),
                   this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    QHBoxLayout* baseline = new QHBoxLayout;
    baseline->addWidget(mBaselineLabel, 1);
    baseline->addWidget(mChoose);
    baseline->addWidget(mUseRt1);
    layout->addLayout(baseline);
    mBaselineLabel->setTextFormat(Qt::PlainText);
    mCandidates->setObjectName(QStringLiteral("graphComparisonCandidates"));
    mCandidates->setColumnCount(METRICS_COLUMN + 1);
    mCandidates->setHorizontalHeaderLabels({QStringLiteral("Graph / configuration / output"),
                                            QStringLiteral("Samples/pixel"),
                                            QStringLiteral("Expected bounces"),
                                            QStringLiteral("Mode"),
                                            QStringLiteral("Result"),
                                            QStringLiteral("RMSE / space")});
    mCandidates->horizontalHeader()->setSectionResizeMode(TARGET_COLUMN, QHeaderView::Stretch);
    mCandidates->horizontalHeader()->setSectionResizeMode(RESULT_COLUMN, QHeaderView::ResizeToContents);
    mCandidates->horizontalHeader()->setSectionResizeMode(METRICS_COLUMN, QHeaderView::ResizeToContents);
    mCandidates->setSelectionBehavior(QAbstractItemView::SelectRows);
    mCandidates->setSelectionMode(QAbstractItemView::SingleSelection);
    mCandidates->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(mCandidates, 1);
    QHBoxLayout* actions = new QHBoxLayout;
    actions->addWidget(mAdd);
    actions->addWidget(mRemove);
    actions->addStretch(1);
    actions->addWidget(mStart);
    actions->addWidget(mStop);
    actions->addWidget(mOpenResults);
    QPushButton* openSaved = new QPushButton(QStringLiteral("Open saved comparison…"), this);
    actions->addWidget(openSaved);
    connect(openSaved, &QPushButton::clicked, this, [this]() { OpenSavedGraphComparison(*this, mRun.GetDirectory()); });
    layout->addLayout(actions);
    for (QLabel* label : {mStatus, mDetails})
    {
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(label);
    }
    QHBoxLayout* images = new QHBoxLayout;
    const QStringList labels{QStringLiteral("Baseline"),
                             QStringLiteral("Selected graph"),
                             QStringLiteral("Difference")};
    qsizetype index = 0;
    for (CapturedImageView* view : {mBaselineView, mCandidateView, mDifferenceView})
    {
        QVBoxLayout* column = new QVBoxLayout;
        column->addWidget(new QLabel(labels.at(index++), this));
        column->addWidget(view, 1);
        images->addLayout(column, 1);
    }
    layout->addLayout(images, 2);

    mTimer.setInterval(SERVICE_INTERVAL_MILLISECONDS);
    connect(&mTimer, &QTimer::timeout, this, [this]() {
        mRun.Service();
        refresh();
    });
    connect(mAdd, &QPushButton::clicked, this, &GraphComparisonWindow::addCandidate);
    connect(mRemove, &QPushButton::clicked, this, [this]() {
        if (mCandidates->currentRow() >= 0)
        {
            mCandidates->removeRow(mCandidates->currentRow());
        }
        setRunning(false);
    });
    connect(mChoose, &QPushButton::clicked, this, &GraphComparisonWindow::selectBaseline);
    connect(mUseRt1, &QPushButton::clicked, this, [this]() {
        mBaselinePath.clear();
        mBaselineLabel->setText(QStringLiteral("Generate RT1 once for all candidates"));
        setRunning(false);
    });
    connect(mStart, &QPushButton::clicked, this, &GraphComparisonWindow::start);
    connect(mStop, &QPushButton::clicked, this, [this]() {
        mRun.Stop();
        refresh();
    });
    connect(mOpenResults, &QPushButton::clicked, this, [this]() {
        if (QDesktopServices::openUrl(QUrl::fromLocalFile(mRun.GetDirectory())) == false)
        {
            mStatus->setText(QStringLiteral("Open the results folder manually at %1.").arg(mRun.GetDirectory()));
        }
    });
    connect(mCandidates, &QTableWidget::itemSelectionChanged, this, &GraphComparisonWindow::showSelection);
    connect(mReferenceTraversal, &QComboBox::activated, this, [this]() { mHasSelectedTraversal = true; });
    connect(&inoutPreview, &RenderPreview::Changed, this, &GraphComparisonWindow::updateTraversalAvailability);
    connect(&inoutPreview, &RenderPreview::Stopped, this, &GraphComparisonWindow::updateTraversalAvailability);
    connect(&inoutPreview, &RenderPreview::DeviceStopping, this, [this]() {
        mRun.Stop();
        refresh();
    });
    addCandidate();
    mStatus->setText(QStringLiteral("Add the graph outputs you want to render against one shared baseline."));
}

GraphComparisonWindow::~GraphComparisonWindow()
{
    mRun.Stop();
}

void GraphComparisonWindow::addCandidate()
{
    const int32 row = mCandidates->rowCount();
    if (row >= MAXIMUM_GRAPH_COMPARISON_CANDIDATES)
    {
        return;
    }
    mCandidates->insertRow(row);
    QComboBox* target = new QComboBox(mCandidates);
    target->addItem(QStringLiteral("Viewport output (current configuration)"));
    for (const RenderGraphCaptureTarget& library : mTargets)
    {
        target->addItem(library.GetLabel());
    }
    target->setCurrentIndex(qMin(row, target->count() - 1));
    mCandidates->setCellWidget(row, TARGET_COLUMN, target);
    QSpinBox* samples = new QSpinBox(mCandidates);
    samples->setRange(0, static_cast<int32>(MAXIMUM_REFERENCE_SAMPLES));
    samples->setSpecialValueText(QStringLiteral("Default"));
    mCandidates->setCellWidget(row, SAMPLES_COLUMN, samples);
    QSpinBox* bounces = new QSpinBox(mCandidates);
    bounces->setRange(0, static_cast<int32>(MAXIMUM_REFERENCE_DEPTH));
    bounces->setSpecialValueText(QStringLiteral("Default"));
    mCandidates->setCellWidget(row, BOUNCES_COLUMN, bounces);
    QComboBox* mode = new QComboBox(mCandidates);
    mode->addItems({QStringLiteral("Accumulate"), QStringLiteral("Display frame")});
    mCandidates->setCellWidget(row, MODE_COLUMN, mode);
    mCandidates->setItem(row, RESULT_COLUMN, new QTableWidgetItem(QStringLiteral("Ready")));
    mCandidates->setItem(row, METRICS_COLUMN, new QTableWidgetItem);
    const auto updateMode = [this, target, mode, samples, bounces]() {
        const bool isViewport = target->currentIndex() == 0;
        const bool canUseDisplayFrame =
            isViewport == true || (mTargets.at(target->currentIndex() - 1).IsRootConfiguration == true &&
                                   mTargets.at(target->currentIndex() - 1).Output.isEmpty() == true);
        if (canUseDisplayFrame == false)
        {
            mode->setCurrentIndex(0);
        }
        mode->setEnabled(canUseDisplayFrame);
        const bool isAccumulating = mode->currentIndex() != DISPLAY_FRAME_INDEX;
        samples->setEnabled(isAccumulating);
        bounces->setEnabled(isAccumulating);
    };
    connect(target, &QComboBox::currentIndexChanged, this, updateMode);
    connect(mode, &QComboBox::currentIndexChanged, this, updateMode);
    updateMode();
    if (target->currentIndex() == 0 && mWorkspace.isNull() == false)
    {
        const RenderGraphPreview* graphOrNull = mWorkspace->GetPreviewOrNull();
        if (graphOrNull != nullptr && FindViewportGraphCaptureProblem(*graphOrNull).isEmpty() == false)
        {
            mode->setCurrentIndex(DISPLAY_FRAME_INDEX);
        }
    }
    mCandidates->selectRow(row);
    setRunning(false);
}

void GraphComparisonWindow::selectBaseline()
{
    const QString path = QFileDialog::getOpenFileName(this,
                                                      QStringLiteral("Choose display baseline"),
                                                      {},
                                                      QStringLiteral("Display images (*.png *.jpg *.jpeg *.webp)"));
    if (path.isEmpty() == false)
    {
        mBaselinePath = path;
        mBaselineLabel->setText(QStringLiteral("Supplied sRGB baseline · %1").arg(QFileInfo(path).fileName()));
        setRunning(false);
    }
}

void GraphComparisonWindow::start()
{
    if (mPreview.isNull() == true || mWorkspace.isNull() == true)
    {
        mStatus->setText(QStringLiteral("Reopen Reference Capture from a World with a project graph."));
        return;
    }
    QString error;
    QImage baseline;
    if (mBaselinePath.isEmpty() == false)
    {
        baseline = ReadDisplayBaseline(mBaselinePath, error);
        if (baseline.isNull() == true)
        {
            mStatus->setText(error);
            return;
        }
    }
    RenderGraphPreview* graphOrNull = mWorkspace->GetPreviewOrNull();
    if (graphOrNull == nullptr)
    {
        mStatus->setText(QStringLiteral("Build and preview the project graph, then start the comparison."));
        return;
    }
    QVector<GraphComparisonCandidate> candidates;
    for (int32 row = 0; row < mCandidates->rowCount(); ++row)
    {
        const QComboBox* targetOrNull = qobject_cast<QComboBox*>(mCandidates->cellWidget(row, TARGET_COLUMN));
        const QSpinBox* samplesOrNull = qobject_cast<QSpinBox*>(mCandidates->cellWidget(row, SAMPLES_COLUMN));
        const QSpinBox* bouncesOrNull = qobject_cast<QSpinBox*>(mCandidates->cellWidget(row, BOUNCES_COLUMN));
        const QComboBox* modeOrNull = qobject_cast<QComboBox*>(mCandidates->cellWidget(row, MODE_COLUMN));
        if (targetOrNull == nullptr || samplesOrNull == nullptr || bouncesOrNull == nullptr || modeOrNull == nullptr)
        {
            mStatus->setText(QStringLiteral("A candidate row is missing its controls. Remove and add it again."));
            return;
        }
        GraphComparisonCandidate candidate;
        candidate.HasSavedTarget = targetOrNull->currentIndex() != 0;
        candidate.IsDisplayFrame = modeOrNull->currentIndex() == DISPLAY_FRAME_INDEX;
        if (candidate.HasSavedTarget == true)
        {
            candidate.Target = mTargets.at(targetOrNull->currentIndex() - 1);
        }
        if (candidate.IsDisplayFrame == false)
        {
            candidate.Samples =
                static_cast<uint32>(samplesOrNull->value() == 0 ? mGraphSamples->value() : samplesOrNull->value());
            candidate.ExpectedBounces =
                static_cast<uint32>(bouncesOrNull->value() == 0 ? mGraphBounces->value() : bouncesOrNull->value());
        }
        candidates.append(candidate);
    }
    const QString parentDirectory = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose results folder"));
    if (parentDirectory.isEmpty() == true)
    {
        return;
    }
    QTemporaryDir directory(QDir(parentDirectory).filePath(QStringLiteral("render-comparison-XXXXXX")));
    if (directory.isValid() == false)
    {
        mStatus->setText(QStringLiteral("Could not create a results folder: %1. Choose a writable location.")
                             .arg(directory.errorString()));
        return;
    }
    mSettings.LongSide = static_cast<uint32>(mLongSide->value());
    mSettings.Samples = static_cast<uint32>(mReferenceSamples->value());
    mSettings.MaxDepth = static_cast<uint32>(mReferenceDepth->value());
    updateTraversalAvailability();
    mSettings.Traversal = static_cast<graphics::renderer::ReferenceTraversal>(mReferenceTraversal->currentIndex());
    if (mRun.Start(*mPreview, *mWorkspace, mSettings, candidates, baseline, mBaselinePath, directory.path(), error) ==
        false)
    {
        mStatus->setText(error);
        return;
    }
    directory.setAutoRemove(false);
    mDisplayedResults = 0;
    for (int32 row = 0; row < mCandidates->rowCount(); ++row)
    {
        QTableWidgetItem* itemOrNull = mCandidates->item(row, RESULT_COLUMN);
        if (itemOrNull != nullptr)
        {
            itemOrNull->setText(QStringLiteral("Queued"));
            itemOrNull->setData(Qt::UserRole, {});
        }
        QTableWidgetItem* metricsOrNull = mCandidates->item(row, METRICS_COLUMN);
        if (metricsOrNull != nullptr)
        {
            metricsOrNull->setText({});
        }
    }
    mBaselineView->SetImage({});
    mCandidateView->SetImage({});
    mDifferenceView->SetImage({});
    mDetails->clear();
    mTimer.start();
    refresh();
}

void GraphComparisonWindow::refresh()
{
    const QJsonArray& results = mRun.GetResults();
    while (mDisplayedResults < results.size())
    {
        const QJsonObject result = results.at(mDisplayedResults).toObject();
        const int32 row = static_cast<int32>(mDisplayedResults++);
        QTableWidgetItem* itemOrNull = mCandidates->item(row, RESULT_COLUMN);
        if (itemOrNull != nullptr)
        {
            itemOrNull->setText(result.value(QStringLiteral("state")).toString());
            itemOrNull->setData(Qt::UserRole, QVariant::fromValue(result));
        }
        QTableWidgetItem* metricsOrNull = mCandidates->item(row, METRICS_COLUMN);
        const QJsonObject metrics = result.value(QStringLiteral("metrics")).toObject();
        if (metricsOrNull != nullptr && metrics.isEmpty() == false)
        {
            metricsOrNull->setText(QStringLiteral("%1 · %2")
                                       .arg(metrics.value(QStringLiteral("rootMeanSquareError")).toDouble())
                                       .arg(metrics.value(QStringLiteral("space")).toString()));
        }
        mCandidates->selectRow(row);
        showSelection();
    }
    if (mRun.IsRunning() == false && mTimer.isActive() == true)
    {
        mTimer.stop();
        for (int32 row = static_cast<int32>(mDisplayedResults); row < mCandidates->rowCount(); ++row)
        {
            QTableWidgetItem* itemOrNull = mCandidates->item(row, RESULT_COLUMN);
            if (itemOrNull != nullptr)
            {
                itemOrNull->setText(QStringLiteral("Skipped"));
            }
        }
    }
    setRunning(mRun.IsRunning());
    if (mRun.GetStatus().isEmpty() == false)
    {
        mStatus->setText(mRun.GetStatus());
    }
}

void GraphComparisonWindow::showSelection()
{
    const QDir directory(mRun.GetDirectory());
    if (mRun.GetDirectory().isEmpty() == false && mBaselineView->GetImage().isNull() == true)
    {
        mBaselineView->SetImage(QImage(directory.filePath(QStringLiteral("baseline.png"))));
    }
    const QTableWidgetItem* itemOrNull = mCandidates->item(mCandidates->currentRow(), RESULT_COLUMN);
    const QJsonObject result =
        itemOrNull == nullptr ? QJsonObject{} : itemOrNull->data(Qt::UserRole).value<QJsonObject>();
    const QString image = result.value(QStringLiteral("image")).toString();
    const QString difference = result.value(QStringLiteral("difference")).toString();
    mCandidateView->SetImage(image.isEmpty() == true ? QImage{} : QImage(directory.filePath(image)));
    mDifferenceView->SetImage(difference.isEmpty() == true ? QImage{} : QImage(directory.filePath(difference)));
    QStringList detail;
    for (const QString& field : {QStringLiteral("summary"),
                                 QStringLiteral("message"),
                                 QStringLiteral("exportError"),
                                 QStringLiteral("comparisonError"),
                                 QStringLiteral("differenceError")})
    {
        const QString message = result.value(field).toString();
        if (message.isEmpty() == false)
        {
            detail.append(message);
        }
    }
    mDetails->setText(detail.join(QLatin1Char('\n')));
}

void GraphComparisonWindow::updateTraversalAvailability()
{
    if (mRun.IsRunning() == false)
    {
        UpdateReferenceTraversalAvailability(*mReferenceTraversal, mPreview.data(), mHasSelectedTraversal == false);
    }
}

void GraphComparisonWindow::setRunning(bool isRunning)
{
    const bool hasFileBaseline = mBaselinePath.isEmpty() == false;
    mLongSide->setEnabled(isRunning == false && hasFileBaseline == false);
    mGraphSamples->setEnabled(isRunning == false);
    mGraphBounces->setEnabled(isRunning == false);
    mReferenceSamples->setEnabled(isRunning == false && hasFileBaseline == false);
    mReferenceDepth->setEnabled(isRunning == false && hasFileBaseline == false);
    mReferenceTraversal->setEnabled(isRunning == false && hasFileBaseline == false);
    mCandidates->setEnabled(isRunning == false);
    mAdd->setEnabled(isRunning == false && mCandidates->rowCount() < MAXIMUM_GRAPH_COMPARISON_CANDIDATES);
    mRemove->setEnabled(isRunning == false && mCandidates->rowCount() > 0);
    mChoose->setEnabled(isRunning == false);
    mUseRt1->setEnabled(isRunning == false && hasFileBaseline == true);
    mStart->setEnabled(isRunning == false && mCandidates->rowCount() > 0);
    mStop->setEnabled(isRunning);
    mOpenResults->setEnabled(mRun.GetDirectory().isEmpty() == false);
}
} // namespace ludus::editor
