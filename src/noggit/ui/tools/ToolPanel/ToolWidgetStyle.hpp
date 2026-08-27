// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_TOOLS_TOOLPANEL_TOOLWIDGETSTYLE_HPP
#define NOGGIT_UI_TOOLS_TOOLPANEL_TOOLWIDGETSTYLE_HPP

#include <noggit/ui/DesignTokens.hpp>
#include <noggit/ui/FontNoggit.hpp>

#include <QtGui/QWindow>
#include <QtWidgets/QBoxLayout>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <QtCore/QLatin1String>
#include <QtCore/QSize>
#include <QtCore/QString>

// THE SHARED SHELL FOR EVERY WIDGET THAT LIVES IN THE RIGHT-HAND TOOL DOCK.
//
// WHY THIS EXISTS. ToolPanel::registerTool is called from sixteen places, all feeding the one
// dock slot that swaps on a keypress: fifteen real tool widgets and one bare placeholder QWidget
// for the impassibility tool. ELEVEN of the fifteen are dressed by this header -- they are
// exactly the eleven translation units that include it -- and until it they were eleven
// independent one-off constructions that disagreed on the two things the eye reads first when a
// panel is replaced under it: the outer gutter, and the gap between sections. The four tool
// widgets still untouched are AreaTriggerEditor, ChunkManipulatorPanel, scripting_tool and the
// stamp tool's BrushStack. (The first revision of this header said "thirteen tool widgets",
// which matched neither count: sixteen call sites, fifteen widgets, eleven dressed.)
//
// Measured across those eleven at 952185d4, before this change:
//
//     TerrainTool, texturing_tool          contents margins 9, 4, 9, 9   spacing 6 / 8
//     FlattenTool, ErosionToolSettings     unset -> QStyle::PM_LayoutLeftMargin, 13 on
//     ShaderTool, hole_tool, ObjectEditor  windowsvista on this machine  spacing unset -> 6
//     Water, MinimapCreator, ZoneIDBrowser
//     LightEditor
//
// So switching tools moved the left edge of the content by 4px and changed the section rhythm
// underneath it. ToolPanel.cpp used to record that disagreement in a long comment and defer it
// ("a sweep across roughly sixteen files and deliberately out of scope"). This header is that
// sweep, expressed once: the deferral is what made four consecutive appearance passes read as
// invisible, because the panel margin was fixed and the tools inside it were not.
//
// THE RULE. ToolPanelScroll.ui already insets the scroll content by S4 horizontally and S3
// vertically, and the ToolPanel dock is the ONE thing entitled to inset a tool. A tool widget
// therefore adds NOTHING of its own: margins are zero on all four sides and the only metric it
// owns is the gap BETWEEN its sections, which is S3 -- the design system's default gap between
// sibling controls. That is the SAME number the section layouts further down use inside a
// section, and deliberately so: what separates two sections is not the layout gap but the
// theme's QGroupBox box model stacked on top of it -- 12px of padding and a 4px margin below
// the first, a 22px title margin above the second -- so eight pixels reads as a section break
// out here and as a control gap in there.
//
// Nothing here changes a range, a step, a signal or the set of controls in any tool. It is
// margins, spacing, alignment and a minimum width.
namespace Noggit::Ui::Tools::ToolPanelStyle
{
  //! The width every tool widget in the dock is measured against.
  //!
  //! ToolPanelScroll.ui does NOT carry this number. Its scroll area asks for 274px, which is
  //! this floor PLUS the 12px the form insets the scroll content by on each side: the panel's
  //! margins are ADDED to the floor rather than taken out of it, and ToolPanel does the same
  //! again with PANEL_SIDE_MARGINS and a scroll-bar allowance. So a tool whose
  //! minimumSizeHint().width() stays at or under this value never gets a horizontal scroll bar
  //! at the dock's smallest size. (The first revision of this note said the form "carries the
  //! same floor"; the form carries 274, and ToolPanel.cpp's own second paragraph says so.)
  //!
  //! For reference, rendered with the shipped theme: the terrain tool asks for 194px and the
  //! texturing tool's Paint tab 229px. Both are inherited from the pass that measured them
  //! (1aa5871e) and were NOT re-measured here -- this pass cannot run the editor. 229 is the
  //! figure texturing_tool.cpp records against the tab itself ("takes the tab's minimum width
  //! from 229px to 262px") and the one ToolPanel.cpp repeats; the 224px the first revision of
  //! this header quoted appeared nowhere else in the tree and is the transcription error.
  constexpr int TOOL_CONTENT_WIDTH = 250;

  //! Apply the dock's shared metrics to a layout that is already installed on a tool widget.
  //! Split out from toolColumn/toolForm so a tool whose top-level layout is a grid, or one that
  //! builds its layout inside a .ui file, can still opt in with a single call.
  //!
  //! THE MINIMUM WIDTH IS THE FRAGILE ONE. QStyleSheetStyle::setGeometry() ASSIGNS a widget's
  //! minimum at polish time rather than raising it, so a QSS min-width overwrites a C++
  //! setMinimumWidth made in a constructor. This call survives only because nothing in the
  //! shipped sheet declares a min-width that a bare tool widget matches -- checked against every
  //! min-width rule in theme.qss, all of which are on QToolButton, QTabBar::tab, QSlider,
  //! QScrollBar, QDialogButtonBox QPushButton or an id/attribute selector. Add
  //! `QWidget { min-width: ... }` to the sheet and TOOL_CONTENT_WIDTH silently stops applying to
  //! all eleven tools at once.
  inline void dressToolLayout (QWidget* widget, QLayout* layout)
  {
    layout->setContentsMargins (0, 0, 0, 0);
    layout->setSpacing (Design::S3);
    layout->setAlignment (Qt::AlignTop);
    widget->setMinimumWidth (TOOL_CONTENT_WIDTH);
  }

  //! The standard tool column: sections stacked top to bottom, S3 apart, flush to the panel's
  //! own gutter. This is what a tool should use unless it has a reason not to.
  inline QVBoxLayout* toolColumn (QWidget* widget)
  {
    auto* const column (new QVBoxLayout (widget));
    dressToolLayout (widget, column);
    return column;
  }

  //! The same shell for a tool whose top level is a QFormLayout rather than a column. Water is
  //! the only caller today -- the first revision of this note said "the four tools", and there
  //! were never four; hole_tool's top level WAS a QFormLayout at 952185d4 but it takes a column
  //! now. QFormLayout::setSpacing sets the horizontal and the vertical gap together, and
  //! label-to-field is a tighter relationship than section-to-section, so the two are set
  //! separately here: S3 down the column to match toolColumn, S1 across because a label and the
  //! control it names are one unit.
  inline QFormLayout* toolForm (QWidget* widget)
  {
    auto* const form (new QFormLayout (widget));
    dressToolLayout (widget, form);
    form->setHorizontalSpacing (Design::S1);
    form->setVerticalSpacing (Design::S3);
    return form;
  }

  //! A titled section inside a tool column, created and appended in one call.
  //!
  //! The dock had FOUR different ideas of what a section is, counted over the eleven tools this
  //! header dresses, at 952185d4: QGroupBox in nine of them, ExpanderWidget in six, QTabWidget
  //! in three, and in hole_tool no section container at all -- and a tool with no section
  //! container reads as an unfinished version of the one it replaced. (The first revision of
  //! this note said five ideas and "in two tools nothing at all"; hole_tool was the only one.)
  //! QGroupBox is the majority idiom and the one the theme already dresses, so it is the one
  //! every tool uses for a plain section; ExpanderWidget stays for content that genuinely
  //! collapses.
  //!
  //! Maximum vertical policy is part of the shell: the tool column is AlignTop, so a section
  //! that would otherwise stretch pushes the sections under it apart by an arbitrary amount.
  inline QGroupBox* toolSection (QBoxLayout* column, QString const& title)
  {
    auto* const section (new QGroupBox (title, column->parentWidget()));
    section->setSizePolicy (QSizePolicy::Preferred, QSizePolicy::Maximum);
    column->addWidget (section);
    return section;
  }

  //! The inside of a section. Margins are zero because the theme already reserves everything a
  //! section needs: its QGroupBox rule pads 12px top and bottom and, deliberately, NOTHING left
  //! or right -- a section costs the panel no width at all -- and carries a 22px top margin to
  //! clear its title. A layout margin on top of that is a second inset inside the first.
  //!
  //! No tool ever set one explicitly; the cost was the style default nobody turned off, which
  //! is the same PM_LayoutLeftMargin as the table at the top of this file: 13px a side on this
  //! machine, so 26px of width and 26px of height per group box. (The first revision of this
  //! note said "roughly 24px ... in the tools that set one" -- 26px, and none of them set it.)
  inline void dressSectionLayout (QLayout* layout)
  {
    layout->setContentsMargins (0, 0, 0, 0);
    layout->setSpacing (Design::S3);
  }

  //! Sections whose contents are label/control pairs.
  inline QFormLayout* sectionForm (QWidget* section)
  {
    auto* const form (new QFormLayout (section));
    dressSectionLayout (form);
    form->setHorizontalSpacing (Design::S1);
    form->setVerticalSpacing (Design::S3);
    return form;
  }

  //! Sections whose contents are full-width controls stacked vertically -- a column of
  //! ExtendedSliders, a stack of check boxes.
  inline QVBoxLayout* sectionColumn (QWidget* section)
  {
    auto* const column (new QVBoxLayout (section));
    dressSectionLayout (column);
    return column;
  }

  // === THE SEGMENTED CONTROL ==================================================================
  //
  // A row of mutually exclusive chips -- brush falloff type in TerrainTool, flatten type in
  // FlattenTool. The two tools occupy the same dock slot and swap on a keypress, so the user
  // watched a grid of segmented chips turn into a 2x2 block of radio buttons and back.
  //
  // The implementation lives here rather than being copied into the second tool because the
  // padding rule below is a WIDGET-LEVEL style sheet, and a widget-level sheet reaches only the
  // widget it is set on. TerrainTool's copy could never have dressed FlattenTool's group box no
  // matter what object name that group box carried, so "give it the same object name" would
  // have shipped an unstyled control; the sheet has to be applied to each section, from one
  // place, or the two drift.

  //! The object name both segmented sections carry, so the selector below can find them.
  constexpr char const* SEGMENTED_SECTION_NAME = "toolSegments";

  //! A titled section holding a segmented control.
  //!
  //! WHY THE PADDING IS OVERRIDDEN. The theme's QPushButton rule pads 5px 12px inside a 1px
  //! border, so a chip's box is its text advance + 26px across. That is sized for a standalone
  //! button with room around it. Nine chips three abreast in a 250px dock is a different
  //! problem.
  //!
  //! Text advances measured with GetTextExtentPoint32 against the shipped Segoe UI at the sizes
  //! the sheet declares -- 12px for a plain QPushButton, 11px for a chip -- filling the grid the
  //! way TerrainTool does (row 0 Flat/Linear/Smooth, row 1 Polynomial/Trigonom/Quadratic,
  //! row 2 Gaussian/Vertex/Script):
  //!
  //!     column   widest label   12px   11px
  //!     0        Polynomial       60     56
  //!     1        Trigonom         53     49
  //!     2        Quadratic        52     50
  //!
  //! A QGridLayout's minimum is the sum of its COLUMN maxima, not three times the widest chip.
  //! At the theme's own padding that is (60+26) + (53+26) + (52+26) + 2 x S1 = 251px -- one
  //! pixel OVER TOOL_CONTENT_WIDTH, so the terrain tool would carry a horizontal scroll bar at
  //! the dock's smallest size. At 5px side padding and 11px text it is (56+12) + (49+12) +
  //! (50+12) + 2 x S1 = 199px, 51px inside the floor. Almost nothing between the chips and that
  //! floor spends any of it: toolColumn zeroes the tool's own margins, the theme's QGroupBox rule
  //! has no left or right padding, and segmentGrid zeroes its own. The one open question is the
  //! sheet's `QWidget#scrollAreaWidgetContents { padding: 0px 4px; }` -- whether Qt honours
  //! padding as a layout inset on a plain QWidget is exactly what that sheet's own HONEST LIMIT
  //! note declines to claim for its QScrollArea twin, and this pass cannot settle it either. If
  //! it IS honoured every figure here loses 8px of room: the override still fits with 43px to
  //! spare, the theme's own padding is 9px over instead of 1px.
  //!
  //! HOW MUCH TO TRUST THOSE TWO TOTALS. They are arithmetic on measured advances, not a
  //! screenshot -- this pass cannot run the editor. GDI's hinted advances run 0-3px wider than
  //! the font's own linear hmtx advances at these sizes; redoing the sum from the linear
  //! advances gives 249px and 192px instead of 251px and 199px. The conclusion is the same
  //! either way: the theme's padding puts the row at the floor or just over it, the override
  //! leaves ~50px of slack.
  //!
  //! (The first revision of this note derived the same conclusion from "4px 10px", ~82px per
  //! chip and rows of ~254px and ~218px "inside the ~232px the dock has after its own gutters".
  //! 4px 10px was the QPushButton padding at 1aa5871e; 952185d4 moved it to 5px 12px and the
  //! note was never re-derived. The ~82px was right FOR THAT PADDING -- 60 + 20 + 2 -- but the
  //! rows were three times the widest chip rather than the column maxima, and the 232px matched
  //! nothing: the dock's gutters are added to the 250px floor, not taken out of it, so a
  //! segmented row has the whole 250px.)
  //!
  //! PADDING, FONT SIZE, AND AN EXPLICIT ZERO MIN-WIDTH -- nothing else. The zero is stated
  //! rather than assumed: the application sheet gives a plain QPushButton no min-width today
  //! (the only one it declares is 88px, on QDialogButtonBox QPushButton, which cannot match a
  //! chip), and a chip must not inherit one if that ever changes. Every colour, border, radius
  //! and state still comes from the application sheet -- QPushButton:checked is already the
  //! theme's "one chosen thing out of a set" mark -- so a palette change upstream still reaches
  //! this control and it never needs a colour of its own.
  inline QGroupBox* segmentedSection (QBoxLayout* column, QString const& title)
  {
    auto* const section (toolSection (column, title));
    section->setObjectName (QLatin1String (SEGMENTED_SECTION_NAME));
    section->setStyleSheet
      ( QString::fromLatin1
          ( "QGroupBox#%1 QPushButton {"
            "  padding: 4px 5px;"
            "  font-size: 11px;"
            "  min-width: 0px;"
            "}"
          ).arg (QString::fromLatin1 (SEGMENTED_SECTION_NAME))
      );
    return section;
  }

  //! One chip. Checkable so QPushButton:checked carries the mark, and explicitly not a default
  //! button so Return in a neighbouring spin box cannot fire it. That last call is belt and
  //! braces rather than a fix: QPushButton::autoDefault only defaults to true under a QDialog
  //! parent, and a dock panel is not one -- it states the intent for the day a chip is reused
  //! inside a dialog.
  inline QPushButton* segmentButton (QWidget* parent, QString const& text)
  {
    auto* const button (new QPushButton (text, parent));
    button->setCheckable (true);
    button->setAutoDefault (false);
    button->setSizePolicy (QSizePolicy::Preferred, QSizePolicy::Fixed);
    return button;
  }

  //! The grid the chips sit in. S1 is the "inside a unit" step: the chips of one segmented row
  //! are one control, not a set of siblings.
  inline QGridLayout* segmentGrid (QWidget* section)
  {
    auto* const grid (new QGridLayout (section));
    grid->setContentsMargins (0, 0, 0, 0);
    grid->setHorizontalSpacing (Design::S1);
    grid->setVerticalSpacing (Design::S1);
    return grid;
  }

  // === THE KEYBIND LEGEND =====================================================================
  //
  // "<modifier> + <button>  Do the thing", the hint row FlattenTool and ErosionToolSettings show
  // at the top of the panel. At 952185d4 it was hand-written THREE times in two files -- twice
  // in FlattenTool, once in ErosionToolSettings, about twenty lines each -- and the copies had
  // drifted in the one way that matters: ErosionToolSettings built its row layout parentless
  // (`new QHBoxLayout`), FlattenTool passed a widget that already owned a layout
  // (`new QHBoxLayout(this)` after `new QVBoxLayout(this)`) and then handed that same layout to
  // a fresh container with setLayout. That is the shape Qt complains about on stderr; the exact
  // warning count per flatten tool was quoted as three in the first revision of this note and is
  // NOT re-measured here, because doing so means running the editor. The defect is visible in
  // the source either way, and both copies now come from keybindRow below, which builds the row
  // widget first and gives the layout that widget as its parent.

  //! The glyph extent, in LOGICAL pixels.
  constexpr int KEYBIND_GLYPH_EXTENT = 20;

  //! The row height. Fixed so a legend row cannot grow with its glyphs and shove the first
  //! section down the panel.
  constexpr int KEYBIND_ROW_HEIGHT = 25;

  //! One glyph.
  //!
  //! WHY THE QWindow OVERLOAD -- AND WHY IT WAS NEVER THE THING THAT FIXED THIS. The symptom
  //! recorded here is real: these glyphs were rasterised at 20 device pixels and stretched to
  //! fill a box of 20 * ratio, softer than every other mark on the panel. The diagnosis was
  //! wrong. It is not that QIcon::pixmap(int, int) hands back a ratio-1 pixmap and the QWindow
  //! overload does not; BOTH overloads route through QIcon::pixmap(QWindow*, QSize), and the
  //! first line of that function is qt_effective_device_pixel_ratio, which in qicon.cpp returns
  //! a flat qreal(1.0) for everyone unless Qt::AA_UseHighDpiPixmaps is set. It was not set, so
  //! passing a window changed nothing at all and this call was as soft as the one it replaced.
  //!
  //! The attribute is set now, in ApplicationEntry.cpp, and that is what made these crisp:
  //! the ratio resolves to 2.0 here, the engine is asked for 40x40, and QIcon stamps ratio 2 on
  //! the result so QLabel lays it out at 20x20 logical.
  //!
  //! Keep the overload anyway. It is the only form that can ask the ratio of the screen the
  //! widget is actually on, which is the one that differs on a mixed-DPI desktop; a null handle
  //! (the widget is not shown yet when a tool builds itself) falls back to the application
  //! ratio, which is right on a uniform-DPI setup and never worse than the int overload.
  inline QLabel* keybindGlyph (QWidget* parent, FontNoggit::Icons icon)
  {
    auto* const label (new QLabel (parent));
    label->setPixmap
      ( FontNoggitIcon (icon).pixmap
          ( parent->window()->windowHandle()
          , QSize (KEYBIND_GLYPH_EXTENT, KEYBIND_GLYPH_EXTENT)
          )
      );
    return label;
  }

  //! One legend row, as a widget ready to be added to a tool column.
  inline QWidget* keybindRow
    (QWidget* parent, FontNoggit::Icons modifier, FontNoggit::Icons button, QString const& action)
  {
    auto* const row (new QWidget (parent));
    row->setFixedHeight (KEYBIND_ROW_HEIGHT);

    auto* const row_layout (new QHBoxLayout (row));
    row_layout->setAlignment (Qt::AlignLeft | Qt::AlignTop);
    row_layout->setContentsMargins (0, 0, 0, 0);
    row_layout->setSpacing (Design::S0);

    row_layout->addWidget (keybindGlyph (row, modifier));
    row_layout->addWidget (new QLabel (QStringLiteral ("+"), row));
    row_layout->addWidget (keybindGlyph (row, button));
    row_layout->addWidget (new QLabel (action, row));

    return row;
  }
}

#endif // NOGGIT_UI_TOOLS_TOOLPANEL_TOOLWIDGETSTYLE_HPP
