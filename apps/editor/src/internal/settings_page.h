#pragma once

// Building blocks of a settings document such as Project Settings: a readable-width column of titled sections whose
// forms share one left-aligned label column, read-only values that can be selected and copied, and a status chip.
// See docs/architecture/editor-art-direction.md (form follows function). GUI thread only.

#include <ludus/foundation/base/types.h>

#include <QLabel>
#include <QList>
#include <QPair>
#include <QString>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QEvent;
class QFormLayout;
class QPaintEvent;
class QVBoxLayout;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Widest a settings column grows, in average characters of its font: about two lines of a paragraph per line of
/// form, so a wide window keeps labels, values and their actions together instead of stretching them apart.
inline constexpr foundation::int32 SETTINGS_COLUMN_CHARACTERS = 110;

/// What a status chip says about the state it labels; each tone takes its status colour tokens.
enum class StatusTone : foundation::uint8
{
    /// Not known yet, or in progress.
    Neutral,
    /// Done, ready.
    Success,
    /// Needs attention.
    Warning,
    /// Failed or unavailable.
    Danger,
};

/// A short state word ("Ready", "Needs attention") on a rounded fill of its tone's surface colour, in its tone's
/// text colour; the words, not the colour alone, carry the state.
class StatusChip final : public QLabel
{
    Q_OBJECT
public:
    explicit StatusChip(QWidget* parentOrNull = nullptr);

    void SetStatus(StatusTone tone, const QString& text);
    [[nodiscard]] StatusTone GetTone() const noexcept
    {
        return mTone;
    }
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    StatusTone mTone = StatusTone::Neutral;
};

/// One titled group of a settings column: the title over a thin rule, then a form of labelled rows. The form keeps
/// its labels on the left on every platform (macOS would right-align them and centre the form) and grows its fields
/// to the column.
class SettingsSection final : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsSection(const QString& title, QWidget* parentOrNull = nullptr);

    [[nodiscard]] QFormLayout& GetForm() const noexcept
    {
        return *mForm;
    }
    /// Layout below the form, for a section's trailing captions, actions and disclosures.
    [[nodiscard]] QVBoxLayout& GetFooter() const noexcept
    {
        return *mFooter;
    }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QLabel* mTitle;
    QFormLayout* mForm;
    QVBoxLayout* mFooter;
};

/// The page column: at most SETTINGS_COLUMN_CHARACTERS wide, left-aligned in its page, with every SettingsSection
/// form under it, and any other form given to ShareLabelColumn, using one label width (the widest label) so the
/// fields of all sections start on one edge.
class SettingsColumn final : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsColumn(QWidget* parentOrNull = nullptr);

    /// Also align form, a form outside a SettingsSection (such as an embedded editor's).
    void ShareLabelColumn(QFormLayout& inoutForm);
    /// Re-measure the labels and apply the shared width; runs itself when the font or style changes.
    void AlignLabelColumn();
    [[nodiscard]] foundation::int32 GetLabelWidth() const noexcept
    {
        return mLabelWidth;
    }

protected:
    void changeEvent(QEvent* event) override;
    bool event(QEvent* event) override;

private:
    [[nodiscard]] QList<QFormLayout*> getForms() const;

    QList<QFormLayout*> mSharedForms;
    foundation::int32 mLabelWidth = 0;
};

/// Read-only value text for a form row: plain text that wraps, can be selected with the mouse or keyboard and
/// copied, and never widens its column.
void MarkSelectableValue(QLabel& inoutValue);

/// Lay form out like a SettingsSection's form: labels left-aligned, fields growing, rows never wrapped.
void ConfigureSettingsForm(QFormLayout& inoutForm);

/// The project setup check as the Build section shows it: a chip, one summary sentence, and the check's
/// "Name: value" lines (selected engine, revision, installation...) as rows.
struct ProjectSetupSummary final
{
    StatusTone Tone = StatusTone::Neutral;
    QString Chip;
    QString Summary;
    QList<QPair<QString, QString>> Details;
};

/// Read setupStatus (WorkspaceState::SetupStatus, after result codes are replaced with words) into a summary. An empty
/// status has not been checked. engineProblem, when not empty, is the engine pin's message and is added as an
/// "Engine" row.
[[nodiscard]] ProjectSetupSummary DescribeProjectSetup(const QString& setupStatus, const QString& engineProblem);
} // namespace ludus::editor
