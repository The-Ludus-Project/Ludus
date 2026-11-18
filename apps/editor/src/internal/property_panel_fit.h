#pragma once

class QWidget;

namespace ludus::editor
{
/// Lets a Properties panel fit any dock width at or above the dock minimum instead of running past its right edge:
/// paragraphs wrap, a form row puts its field under its label when both do not fit, a choice shrinks and elides
/// its current text (shown in full in its tooltip), and a table scrolls inside itself rather than widening the
/// panel. Call it after the panel's controls are built; it changes only how they size, never their values.
/// GUI thread only.
void FitPropertiesToWidth(QWidget& inoutPanel);
} // namespace ludus::editor
