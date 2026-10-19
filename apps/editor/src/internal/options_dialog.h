#pragma once

// Tools > Options (private editor header, not installed).
//
// Like Visual Studio's Options dialog: a page list beside the selected page. It edits a
// copy of editor-wide preferences; the shell applies the result only when the dialog is
// accepted. Today it has one page, Render Graph, holding the build settings that used
// to be the Renderer document's Build Settings tab. See
// docs/development/render-graph-authoring.md#build-settings.

#include "internal/render_graph_build_settings.h"

#include <QDialog>
#include <QString>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QStackedWidget;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Modal editor preferences. Reads and writes nothing itself; the caller supplies the
/// current values and applies GetBuildSettings() after the dialog is accepted.
class OptionsDialog final : public QDialog
{
    Q_OBJECT
public:
    explicit OptionsDialog(QWidget* parentOrNull = nullptr);

    /// Show settings for editing; replaces every field.
    void SetBuildSettings(const RenderGraphBuildSettings& settings);
    /// The edited settings. Paths are trimmed; tool paths are kept when the override is off,
    /// so turning it back on restores them.
    [[nodiscard]] RenderGraphBuildSettings GetBuildSettings() const;
    /// Describe what the last automatic build resolved from the project's SDK (read-only).
    /// An empty sdk says no automatic build has run for this project yet.
    void SetResolvedBuildTools(const QString& sdk, const RenderGraphBuildTools& tools);

    /// Accept only consistent settings: an override needs the helper and the compiler.
    /// Otherwise the dialog stays open and says what is missing.
    void accept() override;

private:
    void updateToolFields();

    QListWidget* mPages;
    QStackedWidget* mStack;
    QCheckBox* mOverrideTools;
    QLineEdit* mHelper;
    QLineEdit* mCompiler;
    QLineEdit* mValidator;
    QLineEdit* mSpirvCross;
    QLineEdit* mWorkerSocket;
    QLabel* mResolved;
    QLabel* mProblem;
};
} // namespace ludus::editor
