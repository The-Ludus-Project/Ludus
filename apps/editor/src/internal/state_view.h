#pragma once

// One empty or error state for every surface (docs/architecture/editor-art-direction.md): an icon, one
// sentence, at most one primary action with its shortcut, and, for an error, the cause, the fix and a Copy
// Details button. The Start Page, the Inspector with no world, the viewport without a GPU device and a failed
// content operation all use it, so each reads the same way.

#include "internal/editor_icons.h"

#include <QColor>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;

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
    QLabel* mMessageOrNull = nullptr;
    QLabel* mCauseOrNull = nullptr;
    QLabel* mFixOrNull = nullptr;
    QLabel* mShortcutOrNull = nullptr;
    QPushButton* mActionOrNull = nullptr;
    QPushButton* mCopyOrNull = nullptr;
};
} // namespace ludus::editor
