#pragma once

// One empty or error state for every surface (docs/architecture/editor-art-direction.md): an icon, one
// sentence, at most one primary action with its shortcut, and, for an error, the cause, the fix and a Copy
// Details button. The Start Page, the Inspector with no world, the viewport without a GPU device and a failed
// content operation all use it, so each reads the same way.

#include "internal/editor_icons.h"

#include <QColor>
#include <QLabel>
#include <QSize>
#include <QString>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QResizeEvent;

namespace ludus::editor
{
class StateIcon;

/// What a StateView says. Empty strings leave that row out.
struct StateViewContent final
{
    EditorIcon Icon = EditorIcon::Information;
    /// The one sentence: what is the state, in the user's words.
    QString Message;
    /// Why it happened (errors only).
    QString Cause;
    /// What the user can do about it (errors only).
    QString Fix;
    /// Label of the single primary action; empty for none.
    QString ActionLabel;
    /// Shortcut shown beside the action, such as "Ctrl+O"; empty for none.
    QString ShortcutHint;
    /// Raw text the Copy Details button puts on the clipboard, after the sentence, cause and fix. Empty hides
    /// the button.
    QString Details;
};

/// Colours for a StateView that sits on a surface with fixed colours, such as the viewport.
struct StateViewColours final
{
    QColor Text;
    QColor Secondary;
};

/// The text Copy Details copies: the sentence, cause, fix and raw details, one per line.
[[nodiscard]] QString GetStateDetailsText(const StateViewContent& content);

/// The state shown in place of the viewport when its GPU device or presentation fails: one sentence, the
/// reported reason as the cause, what to do, and Copy Details. status is the reason as reported.
[[nodiscard]] StateViewContent MakeViewportProblem(const QString& status);

/// A wrapping label for text that holds IDs and paths. A word too long for the label's width, such as a resource ID,
/// cannot wrap, and an ordinary label then asks for that whole width and clips against its dock. This one shortens
/// such a word in the middle to fit, keeps the whole text in its tooltip, and asks for almost no width of its own, so
/// the text wraps at whatever width its panel has. text() is the text as shown; GetFullText() is the whole text.
/// GUI thread only.
class LongWordLabel final : public QLabel
{
    Q_OBJECT
public:
    explicit LongWordLabel(QWidget* parentOrNull);
    /// Show text, wrapped at the label's width, with any word wider than that shortened in the middle.
    void SetFullText(const QString& text);
    [[nodiscard]] const QString& GetFullText() const noexcept;
    /// Whether a word had to be shortened to fit the current width.
    [[nodiscard]] bool IsShortened() const;
    [[nodiscard]] QSize minimumSizeHint() const override;
    [[nodiscard]] bool hasHeightForWidth() const override;
    [[nodiscard]] int heightForWidth(int width) const override;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    /// The text with every word wider than width shortened in the middle to width.
    [[nodiscard]] QString getTextForWidth(int width) const;
    [[nodiscard]] int getTextWidth(int width) const;
    void showTextForWidth(int width);

    QString mFullText;
};

class StateView final : public QWidget
{
    Q_OBJECT
public:
    explicit StateView(QWidget* parentOrNull = nullptr);

    void SetContent(const StateViewContent& content);
    [[nodiscard]] const StateViewContent& GetContent() const noexcept;

    /// Draw on a surface whose colours do not follow the theme. The default follows the theme.
    void SetFixedColours(const StateViewColours& colours);

Q_SIGNALS:
    /// The primary action was chosen.
    void ActionTriggered();

private:
    void applyColours();

    StateViewContent mContent;
    StateViewColours mColours;
    bool mHasFixedColours = false;
    StateIcon* mIconOrNull = nullptr;
    LongWordLabel* mMessageOrNull = nullptr;
    LongWordLabel* mCauseOrNull = nullptr;
    LongWordLabel* mFixOrNull = nullptr;
    QLabel* mShortcutOrNull = nullptr;
    QPushButton* mActionOrNull = nullptr;
    QPushButton* mCopyOrNull = nullptr;
    QPushButton* mDetailsToggleOrNull = nullptr;
    QPlainTextEdit* mDetailsTextOrNull = nullptr;
};
} // namespace ludus::editor
