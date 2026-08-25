// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_DESIGNTOKENS_HPP
#define NOGGIT_UI_DESIGNTOKENS_HPP

#include <QtGui/QColor>

#include <QtCore/QString>

// The "Ironforge" design system, as reachable from C++.
//
// WHY THIS FILE EXISTS. Most of the interface is dressed by dist/noggit-themes/CrimsonSlate/
// theme.qss, and that sheet is the authority. But a style sheet cannot reach three kinds of
// thing, and every one of them had a hand-picked colour in it before this file:
//
//   * a QPainter. minimap_widget, SpawnTilePicker and the texture opacity slider all paint
//     themselves. QSS has no opinion about a QPainter's pen.
//   * a widget-level style sheet that has to exist. Three labels in Vector3Widget and the
//     invalid-path outline in the map wizard are per-instance state the sheet cannot know
//     about, so they are set from C++ -- and a sheet set on a widget OUTRANKS the application
//     sheet, which is exactly why a stale literal there survives any theme change.
//   * a QGraphicsColorizeEffect, which takes a QColor and nothing else.
//
// Those sites used Qt::red, Qt::blue, Qt::black, Qt::white and four different ad-hoc hexes. The
// values here are the same tokens the sheet uses, spelled once, so the painted half of the
// application and the styled half cannot drift apart again.
//
// SCOPE. This is not a theme system and must not grow into one -- it does not read the active
// theme and it does not change when the user switches sheets. It is the floor: the values the
// shipped theme uses, so that a painted marker agrees with the chrome around it. A site that
// CAN be reached by QSS must be left to QSS.
//
// CONTRAST. Every ratio quoted in this file is WCAG 2.1 relative luminance, sRGB, computed as
// (Lmax + 0.05) / (Lmin + 0.05) with the standard per-channel linearisation. They were measured
// with a script, not estimated, and the surface ladder below reproduces the design system's own
// published figures to three decimal places.

namespace Noggit::Ui::Design
{
  // === SURFACES =============================================================================
  //
  // The depth model: the window recedes, the panel is the working plane, things you PRESS come
  // forward, fields you TYPE INTO go back to the window's plane, things that FLOAT come
  // furthest forward. Measured adjacent steps, which is the whole point of this palette --
  // the previous scheme's four "levels" spanned 1.321:1 end to end and were one surface
  // wearing four names:
  //
  //   void -> panel    1.279:1      (previous scheme: 1.064)
  //   panel -> raised  1.281:1      (previous scheme: 1.101)
  //   raised -> overlay 1.256:1     (previous scheme: 1.128)
  //   overlay -> hover 1.292:1
  //   void -> alt      1.104:1      deliberately sub-threshold; zebra striping only
  //   void -> overlay  2.057:1 TOTAL (previous scheme: 1.321)
  //
  // Nothing above BG_OVERLAY may carry prose: body text on it measures 7.065:1 and one step
  // lighter breaks the 7:1 floor. BG_HOVER carries icons and short labels only.
  constexpr char const* BG_VOID    = "#100E0B";  // viewport surround, wells, menu/status bars
  constexpr char const* BG_ALT     = "#1D1916";  // alternating row; focused input fill
  constexpr char const* BG_PANEL   = "#292621";  // dock and dialog bodies, tab panes
  constexpr char const* BG_RAISED  = "#3C3732";  // buttons, header sections, docked toolbars
  constexpr char const* BG_OVERLAY = "#4A4640";  // menus, tooltips, floating viewport cards
  constexpr char const* BG_HOVER   = "#5B5651";  // hover plate on an overlay surface

  // === STROKES ==============================================================================
  constexpr char const* STROKE_SOFT = "#403B35"; // hairline inside a panel or a void well
  constexpr char const* STROKE      = "#565049"; // the visible 1px border of a control
  constexpr char const* STROKE_HI   = "#746D64"; // hover outline; separators on BG_OVERLAY

  // === TEXT, four ranks =====================================================================
  //
  //             void   panel  raised overlay hover
  //   TEXT_HI  16.93   13.24  10.34   8.23    6.37
  //   TEXT     14.53   11.36   8.87   7.07    5.47
  //   TEXT_DIM  9.70    7.58   5.92   4.72    3.65
  //   TEXT_OFF  4.40    3.44   2.69   2.14    1.66
  //
  // TEXT_OFF is the one documented sub-threshold token: disabled controls are exempt under
  // WCAG 2.1 SC 1.4.3 and being under the floor IS the disabled signal. It is never used for
  // enabled text.
  constexpr char const* TEXT_HI  = "#F3F0E9";
  constexpr char const* TEXT     = "#E4DFD7";
  constexpr char const* TEXT_DIM = "#BFB7AA";
  constexpr char const* TEXT_OFF = "#7F786A";

  // === ACCENT ===============================================================================
  //
  // ONE meaning: "the thing you are acting on." The accent moved from crimson to gold because
  // crimson could not be both that and "the thing that will destroy your data" -- the old
  // accent #E8543F and the old error #E0574B measured 1.024:1 apart in luminance and 4.0
  // degrees in hue, i.e. they were the same colour, and red therefore meant selected, focused,
  // checked, active, pressed, danger, error and delete simultaneously. Gold against BAD is
  // 1.389:1 and 34.5 degrees.
  //
  // INK is the text drawn ON any accent or status fill. It is BG_VOID by value, and is spelled
  // separately because the two mean different things at a call site.
  constexpr char const* ACCENT       = "#DFA52E";  // ink on it 8.77:1
  constexpr char const* ACCENT_HI    = "#F0BA4A";  // hover on a filled accent surface; ink 10.85:1
  constexpr char const* ACCENT_PRESS = "#B8801F";  // pressed on a filled accent surface; ink 5.64:1
  constexpr char const* INK          = "#100E0B";

  // === STATUS -- meaning, never decoration ==================================================
  //
  //          panel  raised  ink-on-it
  //   OK     6.09   4.76    7.79
  //   WARN   5.30   4.14    6.78
  //   BAD    4.94   3.86    6.31
  //   INFO   6.29   4.91    8.04
  //
  // Status colours are TEXT only on BG_VOID and BG_PANEL. On BG_RAISED and BG_OVERLAY they may
  // appear only as 1px borders and fills, where the floor is 3:1 and all four clear it.
  //
  // HONEST LIMIT: ACCENT vs WARN is 15.8 degrees and WARN vs BAD is 18.8. Four warm signals
  // cannot all sit 30 degrees apart. WARN and BAD are an escalation pair and adjacency is the
  // traffic-light convention. ACCENT vs WARN is disambiguated by FORM and that is a hard rule:
  // a warn or error input takes a coloured border AND a tinted fill; a focused input takes a
  // 1px accent border and NEVER a tint. Never rely on the hue alone.
  constexpr char const* OK   = "#4FB87E";
  constexpr char const* WARN = "#E2803C";
  constexpr char const* BAD  = "#E86F62";
  constexpr char const* INFO = "#6FAEDC";

  // Input fills for the two invalid states, pre-composited so a widget-level style sheet can
  // name one literal. These ARE the values theme.qss writes -- straight sRGB alpha
  // compositing of the edge colour at 0.14 over BG_VOID -- and that is the whole point: a
  // widget-level sheet outranks the application sheet, so an error field dressed from C++ sits
  // beside one dressed by QSS and the two must not be different colours.
  //
  // They previously read #2B1613 and #2A1911, two and three levels darker, under a comment
  // that asserted the sheet wrote them. It did not; it wrote the pair below, at theme.qss
  // "Error and warning states on an input". The comment was the defect, not the sheet.
  //
  // Measured on these values: TEXT_HI on the error tint 14.25:1, on the warn tint 14.13:1;
  // each edge colour against its own tint, BAD 5.32:1 and WARN 5.66:1, so the border stays
  // visible against the fill it encloses.
  constexpr char const* FILL_ERROR = "#2E1C17";
  constexpr char const* FILL_WARN  = "#2D1E12";

  // === THE VALUE RAMP =======================================================================
  //
  // The one gradient in the design, used where a groove is DATA rather than a rail -- the
  // texture brush opacity slider. Not pure black and not pure white, per the palette rule that
  // the interface must stay legible beside a bright 3D viewport.
  constexpr char const* RAMP_HI = "#F2F0EC";
  constexpr char const* RAMP_LO = "#0C0A08";

  // === TYPE SCALE ===========================================================================
  //
  // Qt QSS has no text-transform and no letter-spacing, so every rank is separable by SIZE and
  // WEIGHT alone with colour only reinforcing. No rank shares both a size and a weight with
  // any other. The global base stays 12px, which is what Qt's 9pt Windows default resolves to
  // at 96 dpi.
  //
  // Weights are QFont::Weight values: 400 regular is QFont::Normal (50), 600 is
  // QFont::DemiBold (63), 700 is QFont::Bold (75). Qt 5's setWeight takes the 0-99 scale, not
  // the CSS one, and passing 600 to it silently clamps -- hence the named constants.
  constexpr int FONT_WINDOW_TITLE   = 15;  // weight DEMIBOLD, TEXT_HI
  constexpr int FONT_DOCK_TITLE     = 14;  // weight BOLD,     TEXT_HI
  constexpr int FONT_SECTION        = 13;  // weight BOLD,     TEXT_HI
  constexpr int FONT_LABEL          = 12;  // weight REGULAR,  TEXT      -- the global default
  constexpr int FONT_VALUE          = 12;  // weight DEMIBOLD, TEXT_HI
  constexpr int FONT_SECONDARY      = 11;  // weight REGULAR,  TEXT_DIM
  constexpr int FONT_TABLE_HEADER   = 11;  // weight BOLD,     TEXT_DIM
  constexpr int FONT_MONO           = 12;  // weight REGULAR,  TEXT, family MONO_FAMILY

  constexpr int WEIGHT_REGULAR  = 50;  // QFont::Normal   ~ CSS 400
  constexpr int WEIGHT_DEMIBOLD = 63;  // QFont::DemiBold ~ CSS 600
  constexpr int WEIGHT_BOLD     = 75;  // QFont::Bold     ~ CSS 700

  // Reports, the Lua log and the SQL changeset preview. Tabular figures matter in all three.
  // Consolas ships with Windows and is not redistributed here; the fallbacks are what Qt will
  // find on a machine without it.
  constexpr char const* MONO_FAMILY = "Consolas";

  // === SPACING ==============================================================================
  //
  // One 4px-based scale. Every margin, padding and layout gap comes from this list and nowhere
  // else. THE RULE: use the smallest step that still separates two things that are not part of
  // the same unit. Within a unit S1. Between units S3. Between sections S5. Between regions
  // S6. Never an odd number, never an invented step -- if something needs 10px it needs 8 or
  // 12.
  constexpr int S0 = 2;   // a state rule's reserve; icon-to-label inside a compact control
  constexpr int S1 = 4;   // inside a unit: label to its control, chips of a segmented row
  constexpr int S2 = 6;   // vertical padding inside a dense row
  constexpr int S3 = 8;   // DEFAULT gap between sibling controls; horizontal padding in inputs
  constexpr int S4 = 12;  // panel side margins; horizontal padding inside a button
  constexpr int S5 = 16;  // between SECTIONS inside a panel; dialog content margins
  constexpr int S6 = 24;  // between major regions; dialog outer margins
  constexpr int S7 = 32;  // the only step above S6: the project-selection column gutter

  // === RADII ================================================================================
  constexpr int RADIUS_CONTROL   = 5;  // buttons, inputs, combos, tool buttons
  constexpr int RADIUS_CONTAINER = 8;  // item-view wells, floating cards, the minimap holder
  constexpr int RADIUS_INDICATOR = 4;  // check box, scrollbar thumb

  // === QColor accessors =====================================================================
  //
  // Inline rather than a table so each is a compile-time-known literal at its one call site,
  // and returning by value because QColor is 16 bytes and a static would need a guard on every
  // read. QColor's QString constructor parses "#RRGGBB" and is the same path QSS takes.
  inline QColor color (char const* token) { return QColor (QString::fromLatin1 (token)); }
}

#endif // NOGGIT_UI_DESIGNTOKENS_HPP
