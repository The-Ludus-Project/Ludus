#pragma once

class QTableView;
class QWidget;

namespace ludus::editor
{
/// Lets a Properties panel fit any dock width at or above the dock minimum instead of running past its right edge:
/// paragraphs wrap, a form row puts its field under its label when both do not fit, a choice shrinks and elides
/// its current text (shown in full in its tooltip), and a table scrolls inside itself rather than widening the
/// panel. Call it after the panel's controls are built; it changes only how they size, never their values.
/// GUI thread only.
void FitPropertiesToWidth(QWidget& inoutPanel);

/// Keeps a table's columns inside its own width, so it never scrolls sideways: each column takes the width its
/// header and cells ask for, and any spare width goes to the last plain-text column. When the table is narrower
/// than that, the widest columns shrink first (their cell text elides), none below a few characters. Refits
/// whenever the table resizes or its rows change; the table owns the fitter. GUI thread only.
void FitColumnsToWidth(QTableView& inoutTable);
} // namespace ludus::editor
