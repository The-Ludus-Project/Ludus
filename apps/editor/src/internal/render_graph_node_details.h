#pragma once

#include <QDialog>
#include <QString>

QT_BEGIN_NAMESPACE
class QFormLayout;
class QLabel;
class QScrollArea;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Owns the modal detail surface and its control lifetime, part selection and diagnostic focus.
/// Borrows no document or workspace state. PopulateRequested is synchronous; the caller supplies
/// shared field controls and handles their document edits. GUI thread only.
class RenderGraphNodeDetails final : public QDialog
{
    Q_OBJECT
public:
    explicit RenderGraphNodeDetails(QWidget* parentOrNull = nullptr);
    /// Replace fields while preserving the selected pipeline part; retired controls stop emitting.
    void Refresh(const QString& title, const QString& path);
    /// True when an originating control belongs to the current detail fields.
    [[nodiscard]] bool ContainsControl(const QWidget* controlOrNull) const;
    /// Reveal and mark a field from a validator diagnostic; unknown fields leave focus unchanged.
    void RevealDiagnostic(const QString& field, const QString& description);
Q_SIGNALS:
    /// Fill the supplied nonnull form and parent with document-driven field controls.
    void PopulateRequested(QFormLayout* outFields, QWidget* parent);

private:
    // Retired controls stay owned but outside live field lookups until deferred deletion.
    QWidget mRetiredFields;
    QScrollArea* mScroll;
    QLabel* mPath;
};
} // namespace ludus::editor
