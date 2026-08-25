// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_WINDOWS_UISTYLE_HPP
#define NOGGIT_UI_WINDOWS_UISTYLE_HPP

#include <QLatin1String>
#include <QString>
#include <QStyle>
#include <QWidget>

// The design system's spacing scale and type ranks, in one place, for the three windows a user
// meets before the 3D view exists: project selection, map selection, and the editor's own chrome.
//
// WHY THIS FILE EXISTS. Every margin in these windows used to be a local decision -- the project
// selection window had a 5px spacer beside a 10px margin beside uic's default 6px gap, and the
// map list column had 8px margins next to a 9px one -- so nothing lined up with anything and the
// result read as ad hoc. A shared scale is the only way "rhythm" is a property of the application
// rather than of whichever function was edited last.
//
// HOW A TYPE RANK IS APPLIED, and why it is not QWidget::setFont. Measured with a standalone Qt
// 5.15.2 probe running against the real CrimsonSlate sheet:
//
//   label                     asked for        resolved to
//   setFont(15px, DemiBold)   15px / 63        12px / 63     <- SIZE LOST
//   setFont(11px, Normal)     11px / 50        12px / 50     <- SIZE LOST
//   setFont(20px)             20px            12px          <- SIZE LOST
//
// The sheet opens with a bare `QWidget { font-family: "Segoe UI"; font-size: 12px; }` rule
// (theme.qss, the "global" block). Every widget in the application matches it, and a style sheet
// font outranks setFont, so setFont can no longer change a size anywhere in this program. The
// weight survives only because that rule does not mention font-weight.
//
// So a rank has to be written as a style sheet, and applyRank writes the SMALLEST style sheet
// that expresses one: font-size and font-weight, nothing else. It deliberately sets no colour --
// measured in the same probe, a font-only widget sheet still lets the application sheet colour
// the label (#E4DFD7 from the theme's QLabel rule came through unchanged), and combining it with
// a published rank selector such as [state="value"] gives size from here and colour from the
// theme: 15px / 600 / #F3F0E9. Colour stays where colour belongs.
//
// Nothing in this header changes a range, a step, a signal or the set of controls. It is
// spacing, font and property only.
namespace Noggit::Ui::Windows::Style
{
  // ------------------------------------------------------------------ spacing --
  //
  // One 4px-based scale. Every margin, padding and layout gap in these windows comes from this
  // list and nowhere else. The rule, in one line: use the smallest step that still separates two
  // things that are not part of the same unit. Within a unit SPACE_4. Between units SPACE_8.
  // Between sections SPACE_16. Between regions SPACE_24.

  //! The reserve for a state rule that must not reflow when it appears, and the gap between an
  //! icon and its label inside a compact control.
  constexpr int SPACE_2 = 2;

  //! Inside a UNIT -- between the chips of a segmented row, grid gutters.
  constexpr int SPACE_4 = 4;

  //! Vertical padding inside a dense row.
  constexpr int SPACE_6 = 6;

  //! The DEFAULT gap between two sibling controls in a column.
  constexpr int SPACE_8 = 8;

  //! Panel side margins, and the gap between a control and the section rule below it.
  constexpr int SPACE_12 = 12;

  //! Between SECTIONS inside a panel; dialog content margins.
  constexpr int SPACE_16 = 16;

  //! Between major regions; dialog outer margins.
  constexpr int SPACE_24 = 24;

  //! The only step above SPACE_24 -- the column gutter on the project selection window.
  constexpr int SPACE_32 = 32;

  // --------------------------------------------------------------- type ranks --
  //
  // No rank shares both a size and a weight with any other, because Qt style sheets have neither
  // text-transform nor letter-spacing: size and weight are the only two axes available, and
  // colour can only reinforce a separation those two have already made.
  //
  // The weights are CSS weights, because that is what a style sheet declaration takes. Qt maps
  // 400 to QFont::Normal and 600 to QFont::Bold when it resolves them.

  //! Window titles -- the product line on the project selection window, the head of a pane.
  constexpr int RANK_WINDOW_TITLE_PIXELS = 15;
  constexpr int RANK_WINDOW_TITLE_WEIGHT = 600;

  //! Section headings -- a group box title, the heading over a list.
  constexpr int RANK_SECTION_PIXELS = 13;
  constexpr int RANK_SECTION_WEIGHT = 700;

  //! The global default: labels, check boxes, buttons, menu items.
  constexpr int RANK_LABEL_PIXELS = 12;
  constexpr int RANK_LABEL_WEIGHT = 400;

  //! A live value -- a readout, the contents of a field, the thing being edited.
  constexpr int RANK_VALUE_PIXELS = 12;
  constexpr int RANK_VALUE_WEIGHT = 600;

  //! Captions, units, ranges, secondary lines.
  constexpr int RANK_SECONDARY_PIXELS = 11;
  constexpr int RANK_SECONDARY_WEIGHT = 400;

  // --------------------------------------------- object names the theme publishes --
  //
  // Where the active sheet already carries the rank that is wanted, the object name IS the rank
  // and applyRank is not needed at all -- that is always the better of the two, because the theme
  // keeps ownership of both the size and the colour. These two are the sheet's published label
  // ranks and are reused by the project rows, the map rows and both windows' headers.

  //! 17px / 600 / text.hi -- heads a whole window column.
  inline QLatin1String const NAME_SECTION_TITLE("project-section-title");

  //! 11px / 400 / text.dim -- the secondary rank. Captions, units, paths, hints, empty states.
  inline QLatin1String const NAME_SECONDARY("project-information");

  // ------------------------------------------------------------------ helpers --

  //! Writes one type rank onto a widget as a font-only style sheet. See the note at the head of
  //! this file for why this cannot be setFont, and for the measurement showing that a font-only
  //! widget sheet leaves the application sheet's colour intact.
  //!
  //! Do not call this on a widget that already carries a style sheet of its own -- it would be
  //! replaced. Nothing in these windows does.
  inline void applyRank(QWidget* widget, int pixel_size, int weight)
  {
    if (!widget)
      return;

    widget->setStyleSheet(QStringLiteral("font-size: %1px; font-weight: %2;")
                              .arg(pixel_size)
                              .arg(weight));
  }

  //! Sets the dynamic `state` property the sheets select on -- "value", "primary", "danger".
  //!
  //! Qt only re-evaluates style sheet rules that select on a dynamic property when the widget is
  //! unpolished and polished again, so a property set AFTER the first polish is silently inert
  //! unless that is done. Setting it at construction needs neither, but a helper that only works
  //! at construction is a trap, so this does the repolish whenever the value actually changes.
  inline void applyState(QWidget* widget, QString const& state)
  {
    if (!widget || widget->property("state").toString() == state)
      return;

    widget->setProperty("state", state);

    if (QStyle* const style = widget->style())
    {
      style->unpolish(widget);
      style->polish(widget);
      widget->update();
    }
  }

  //! The one accent-filled control in a window: the action the window exists to perform.
  inline void markPrimary(QWidget* widget) { applyState(widget, QStringLiteral("primary")); }

  //! A label carrying a live value. Also the only published route to the text.hi colour, so a
  //! header whose text changes with the selection takes this and then overrides the size with
  //! applyRank.
  inline void markValue(QWidget* widget) { applyState(widget, QStringLiteral("value")); }
}

#endif // NOGGIT_UI_WINDOWS_UISTYLE_HPP
