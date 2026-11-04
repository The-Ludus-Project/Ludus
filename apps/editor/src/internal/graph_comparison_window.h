#pragma once

#include "internal/graph_comparison_run.h"

#include <QTimer>
#include <QWidget>

class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace ludus::editor
{
class CapturedImageView;

/// Native Reference Capture companion for configuring and inspecting a matched multi-graph run.
class GraphComparisonWindow final : public QWidget
{
public:
    GraphComparisonWindow(RenderPreview& inoutPreview,
                          RendererWorkspace& inoutWorkspace,
                          const ViewportReferenceSettings& reference,
                          QWidget* parent);
    ~GraphComparisonWindow() override;

private:
    void addCandidate();
    void selectBaseline();
    void start();
    void refresh();
    void showSelection();
    void setRunning(bool isRunning);

    QPointer<RenderPreview> mPreview;
    QPointer<RendererWorkspace> mWorkspace;
    ViewportReferenceSettings mSettings;
    QVector<RenderGraphCaptureTarget> mTargets;
    GraphComparisonRun mRun;
    QTimer mTimer;
    QString mBaselinePath;
    qsizetype mDisplayedResults = 0;
    QTableWidget* const mCandidates;
    QSpinBox* const mLongSide;
    QSpinBox* const mGraphSamples;
    QSpinBox* const mGraphBounces;
    QSpinBox* const mReferenceSamples;
    QSpinBox* const mReferenceDepth;
    QPushButton* const mAdd;
    QPushButton* const mRemove;
    QPushButton* const mChoose;
    QPushButton* const mUseRt1;
    QPushButton* const mStart;
    QPushButton* const mStop;
    QPushButton* const mOpenResults;
    QLabel* const mBaselineLabel;
    QLabel* const mStatus;
    QLabel* const mDetails;
    CapturedImageView* const mBaselineView;
    CapturedImageView* const mCandidateView;
    CapturedImageView* const mDifferenceView;
};
} // namespace ludus::editor
