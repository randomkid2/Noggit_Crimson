// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/windows/projectSelection/widgets/LauncherArt.hpp>
#include <noggit/ui/windows/projectSelection/widgets/ProjectListItem.hpp>

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QGraphicsColorizeEffect>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QRectF>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStringList>
#include <QStyle>
#include <QStyleOption>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <initializer_list>


namespace Noggit::Ui::Widget
{
  namespace
  {
    // THE ROW IS A CARD NOW. It used to be a transparent strip inside a bg.void well, drawing no
    // fill and no edge of its own, because the WELL was the object and the row was a line of text
    // in it. On a bg.void window ground that well was a box drawn around nothing, so the well is
    // gone and each project is a card: a bg.panel fill on the bg.void root, a 1.279:1 surface
    // step, with an edge #8A8378 border measuring 4.018:1 against its own fill and 5.138:1
    // against the root. Both clear the 3:1 floor WCAG 2.1 SC 1.4.11 sets for a graphical object,
    // which is what makes the card an object rather than a region.

    //! The circular expansion mark. 56 rather than the old 48 because it now has to hold its own
    //! against a 104x64 artwork tile on the other side of the card.
    constexpr int ICON_EXTENT = 56;

    //! The artwork slot. 13:8, which is the landscape proportion the mockup draws, and 64 tall so
    //! it is the tallest thing in the top row and therefore the row's height (see CARD_MIN_HEIGHT
    //! for the three candidates).
    constexpr int ART_WIDTH = 104;
    constexpr int ART_HEIGHT = 64;

    //! The little calendar beside the date.
    constexpr int DATE_GLYPH_EXTENT = 14;

    // The card's own frame. SPACE_16 top and bottom, SPACE_24 minus the 4px the 12px radius eats
    // optically at the sides -- a rounded container needs more horizontal inset than a square one
    // for its content to look equally inset.
    constexpr int CARD_MARGIN_LEFT = 20;
    constexpr int CARD_MARGIN_TOP = 16;
    constexpr int CARD_MARGIN_RIGHT = 20;
    constexpr int CARD_MARGIN_BOTTOM = 16;

    //! Between the icon column, the text column and the artwork.
    constexpr int COLUMN_SPACING = 16;

    //! Between the three text lines. SPACE_4: they are one unit.
    constexpr int LINE_SPACING = 4;

    //! Between the top row, the separator and the date row. SPACE_12, the step reserved for the
    //! gap between a control and a rule.
    constexpr int SECTION_SPACING = 12;

    constexpr int SEPARATOR_HEIGHT = 1;

    // The three text ranks the card carries, and the line box each one occupies. The line boxes
    // are the sheet's own measured figures (QFontMetrics, standalone probe, theme.qss TYPE SCALE
    // header): 11px -> 13, 12px -> 15, 17px -> 23. Weight does not change any of them.
    constexpr int NAME_LINE_BOX = 23;         // 17px / 600
    constexpr int PATH_LINE_BOX = 13;         // 11px / 400
    constexpr int EXPANSION_LINE_BOX = 15;    // 12px / 600
    constexpr int DATE_LINE_BOX = 13;         // 11px / 400

    //! The text column, from the three line boxes above and the two gaps between them:
    //!   23 + 4 + 13 + 4 + 15 = 59
    constexpr int TEXT_COLUMN_HEIGHT
      = NAME_LINE_BOX + LINE_SPACING + PATH_LINE_BOX + LINE_SPACING + EXPANSION_LINE_BOX;

    //! The top row is the tallest of its three columns: the 56px icon, the 59px text column and
    //! the 64px artwork. The artwork wins, so the row is 64.
    constexpr int TOP_ROW_HEIGHT
      = std::max ({ICON_EXTENT, TEXT_COLUMN_HEIGHT, ART_HEIGHT});

    //! The date row is the taller of the glyph and the date's line box: 14 against 13.
    constexpr int DATE_ROW_HEIGHT = std::max (DATE_GLYPH_EXTENT, DATE_LINE_BOX);

    //! The card's height, DERIVED rather than asserted, so it cannot drift away from the
    //! constants above:
    //!   16 top + 64 top row + 12 + 1 separator + 12 + 14 date row + 16 bottom = 135
    //! RecentProjectsComponent feeds minimumSizeHint() straight to QListWidgetItem::setSizeHint,
    //! so this plus ITEM_CHROME_HEADROOM is the list's row pitch.
    constexpr int CARD_MIN_HEIGHT
      = CARD_MARGIN_TOP + TOP_ROW_HEIGHT + SECTION_SPACING + SEPARATOR_HEIGHT
      + SECTION_SPACING + DATE_ROW_HEIGHT + CARD_MARGIN_BOTTOM;

    // WHAT THE VIEW TAKES BACK OFF THE ROW, and why the number changed.
    //
    // setItemWidget() installs the card as a PERSISTENT EDITOR, so its geometry comes from
    // QStyledItemDelegate::updateEditorGeometry() via SE_ItemViewItemText, and QStyleSheetStyle
    // subtracts the view's ::item margin, border and padding from that rect. listView carries
    // accessibleName "project_list", so under CrimsonSlate it matches
    //   QListWidget[accessibleName="project_list"]::item { margin: 0px 0px 12px 0px; }
    // and loses 12px at the bottom -- that margin is what separates one card from the next now
    // that the cards are opaque objects rather than rows in a well. It was 2px top + 2px bottom
    // when the rows were strips, which is why this constant was 6.
    //
    // The fix is in two halves, as in MapListItem.cpp -- see the long comment there for why the
    // loss cannot be read back from the style:
    //   * setMinimumHeight(contentMinimum()) is the guarantee; QWidget::setGeometry clamps.
    //   * ITEM_CHROME_HEADROOM keeps the card inside its slot under a normal theme.
    constexpr int ITEM_CHROME_HEADROOM = 12;

    // Deliberately narrow, and unchanged. The list widget always resizes the row to the viewport
    // width, so this number only decides whether the view thinks it needs a horizontal scroll
    // bar. A project path can be arbitrarily long, so the hint must not be derived from it.
    constexpr int ROW_HINT_WIDTH = 125;

    // COLOURS A STYLE SHEET CANNOT REACH. Everything drawn with a QPainter or fed to a
    // QGraphicsEffect is invisible to theme.qss, so every colour from here to the end of this
    // block has to be carried by hand when the palette moves. Each one is measured against the
    // surfaces a card can actually sit on.
    //
    // ACCENT_HI #F3687F for the favourite star, and the LIT step rather than plain accent for a
    // measured reason. This is precisely the kind of unreachable colour the paragraph above warns
    // about: it survived the gold-to-crimson migration as a stale #DFA52E because no grep of the
    // style sheet could see it.
    //
    // Plain accent #E5405C fails here: the row now LIGHTENS under the pointer and again on press,
    // and against the selected-hover fill #453331 it measures 2.947:1, under the 3:1 floor a
    // graphical mark has to clear. accent.hi holds on every surface a row can present -- 6.513 on
    // the bg.void well, 5.900 on an alternating row, 5.093 on the card, 4.440 hovered, 4.613
    // selected, 4.008 selected-and-hovered, and 3.714 at its worst on the pressed fill.
    constexpr QRgb ACCENT_STAR = qRgb (0xF3, 0x68, 0x7F);

    //! edge, the ramp's enabled-control edge, used as the ring around the circular expansion
    //! mark and around the artwork tile. 4.018:1 on the bg.panel card fill and 3.792:1 on the
    //! artwork tile's own #342825 -- the ring is what makes the tile an object, which is the
    //! whole reason the tile's interior is allowed to be as quiet as it is.
    constexpr QRgb RING_EDGE = qRgb (0x8A, 0x83, 0x78);

    //! The artwork tile's ground: brand.crimson at 6% straight sRGB alpha over bg.panel, which
    //! composites to #342825, 1.059:1 above the card fill. That is deliberately almost nothing --
    //! the tile is identified by its RING at 3.792:1 and by its pattern, not by its fill, and a
    //! fill that competed with the card would make every card look selected.
    constexpr QRgb ART_FILL = qRgb (0x34, 0x28, 0x25);

    //! brand.crimson, the ink the artwork's contour field is drawn in, at 5% to 13% alpha. Over
    //! the tile fill that composites to #3D2928 at the bottom of the range and #4B2B2C at the
    //! top, 1.047:1 and 1.139:1 -- texture, and nothing a user could mistake for a control.
    constexpr QRgb ART_INK = qRgb (0xE5, 0x40, 0x5C);

    //! text.dim, the calendar mark's pen. 7.585:1 on the card fill.
    constexpr QRgb DATE_GLYPH_INK = qRgb (0xBF, 0xB7, 0xAA);

    // THE INTERACTION STATES, AND WHY THEY ARE PAINTED HERE RATHER THAN LEFT TO THE SHEET.
    //
    // The sheet's QWidget#project-card:hover rule was measured not to reach this widget at all --
    // see the class comment in the header for the capture. So hover, press, opening and focus are
    // composited in paintEvent instead, over whatever fill the theme drew. Everything below is a
    // TRANSLUCENT wash or an edge and never an opaque plate, so the theme keeps ownership of the
    // card's colour and a theme that repaints the card still gets working states for free.

    //! The card's corner radius. It lives in theme.qss as
    //!   QWidget#project-card { border-radius: 12px; }
    //! and no API reads a style sheet property back out of QStyle, so the painted ring has to
    //! name the same number or it would cut across the corners the sheet rounded.
    constexpr int CARD_RADIUS = 12;

    // RING WIDTH IS THE PRIMARY CUE, and that is a MEASURED decision rather than a preference.
    // The expansion caption is info #6FAEDC and has to hold 4.5:1 against whatever the card fill
    // becomes, which puts a hard ceiling on how far that fill may be lifted: its luminance must
    // stay at or under 0.047375, so the whole lift budget is 1.3976:1 from the resting fill
    // #292621 and only 1.2658:1 from the selected fill #3C2927. This palette calls ~1.28:1 the
    // smallest step a large flat field can carry, so on a SELECTED card there is no text-safe
    // fill change that is also a reliable signal. Width has no such ceiling -- the ring sits in
    // the card's 20px margin, where no label reaches -- and 1px resting, 2px hovered, 3px pressed
    // is monotone and stays legible to a colour-blind user.
    constexpr int RING_HOVER_WIDTH = 2;
    constexpr int RING_ACTIVE_WIDTH = 3;

    //! The focus hairline's inset. Its centreline lands at 4.5, so a 1px pen colours exactly one
    //! pixel row and always leaves at least one row of fill between it and the 3px ring: text.hi
    //! on brand.crimson.hi is only 2.876:1 and the two must not be allowed to touch.
    constexpr int FOCUS_RING_INSET = 4;

    //! The opening state's busy bar, drawn immediately inside the 3px ring so the card's bottom
    //! edge thickens from 3px to 7px. STATIC, deliberately: the project load that follows the
    //! double click blocks the event loop, so an animation would freeze on its first frame and
    //! read as a hang -- which is the exact complaint this state exists to answer.
    constexpr int BUSY_BAR_HEIGHT = 4;

    // THE TWO WASHES, as 8-bit alphas because that is what QColor::setAlpha takes and what
    // QPainter's SourceOver composites with. Straight sRGB alpha, dst + a * (src - dst), the same
    // model the sheet's own brand-ramp comment works in. The composites quoted are for the two
    // fills the sheet can hand over; a real repaint may land +-1 per channel because Qt
    // composites in premultiplied ARGB32.

    //! text.hi at 13/255 = 0.050980. NEUTRAL, so a hovered card can never be mistaken for a
    //! selected one: over the resting fill #292621 it composites to #33302B, 1.147:1 above it,
    //! and over the selected fill #3C2927 to #453331, 1.151:1 above it. Body text on those two --
    //! text.hi 11.544 and 10.419, text.dim 6.612 and 5.968, info 5.483 and 4.949 -- all clear the
    //! 4.5:1 floor.
    constexpr int HOVER_WASH_ALPHA = 13;
    constexpr QRgb HOVER_WASH_INK = qRgb (0xF3, 0xF0, 0xE9);

    //! brand.crimson at 51/255 = 0.200000, and crimson because crimson is this application's
    //! interactive accent. Over #292621 it composites to #4F2B2D, 1.231:1 above the resting fill;
    //! over #3C2927 to #5E2E32, 1.243:1 above the selected fill. Those are the largest text-safe
    //! lifts available: text.hi 10.753 and 9.646, text.dim 6.160 and 5.525, info 5.107 and 4.582.
    //! Against the hover wash the luminance step is only 1.073:1 and 1.080:1, which is precisely
    //! why the ring WIDTH and not the fill is what tells hover from pressed.
    constexpr int PRESS_WASH_ALPHA = 51;
    constexpr QRgb PRESS_WASH_INK = qRgb (0xE5, 0x40, 0x5C);

    //! text.dim, the hover ring on an UNSELECTED card -- the colour the sheet's own (dead) :hover
    //! rule asks for, kept so the painted state matches the theme author's intent. 6.612:1 on the
    //! hover fill it encloses and 9.699:1 on the bg.void root outside it, both far over the 3:1
    //! floor WCAG 2.1 SC 1.4.11 sets for a graphical object.
    constexpr QRgb RING_HOVER_INK = qRgb (0xBF, 0xB7, 0xAA);

    //! brand.crimson.hi, the ring for every state the sheet already treats as active: a hovered
    //! SELECTED card, a pressed card and an opening card. Measured inside each fill it encloses
    //! -- 4.013 on #33302B, 3.622 on #453331, 3.739 on #4F2B2D, 3.354 on #5E2E32 -- and 5.887 on
    //! the bg.void root. The worst of those is 3.354:1 and still clears 3:1.
    constexpr QRgb RING_ACTIVE_INK = qRgb (0xF0, 0x5A, 0x73);

    //! The focus hairline, in text.hi and NOT in the gold accent. That is the rule the sheet
    //! already sets for a crimson-bordered control, at
    //! QPushButton#launcher-action[state="primary"]:focus -- gold sits 50.5 degrees from
    //! brand.crimson and would have to be told apart from a border drawn right beside it. text.hi
    //! carries 11.992:1 on the selected fill, 10.419 on selected+hover and 9.646 on
    //! selected+pressed.
    constexpr QRgb FOCUS_RING_INK = qRgb (0xF3, 0xF0, 0xE9);

    // The type ranks. These are DEFAULTS, set through QFont rather than through an inline style
    // sheet: a style sheet on the widget itself outranks the application sheet, which is how an
    // early revision pinned every row to one size no matter which theme was loaded. A theme's
    // font-size still wins over a font set this way, so CrimsonSlate dresses the card and a theme
    // that says nothing about it still gets a readable hierarchy.
    constexpr int NAME_PIXEL_SIZE = 17;
    constexpr int PATH_PIXEL_SIZE = 11;
    constexpr int EXPANSION_PIXEL_SIZE = 12;

    void applyFont (QLabel* label, int pixel_size, bool bold)
    {
      QFont font (label->font());
      font.setPixelSize (pixel_size);
      font.setBold (bold);
      label->setFont (font);
    }

    // A label whose text may be long must not be allowed to set the card's minimum width, or a
    // deep project path drags the whole list wider than its viewport. Ignored means "the layout
    // gives you what is left over"; the text clips at the right exactly as it did before.
    void makeElastic (QLabel* label)
    {
      label->setSizePolicy (QSizePolicy::Ignored, QSizePolicy::Fixed);
      label->setMinimumWidth (0);
    }

    struct VersionPresentation
    {
      char const* resource;
      char const* expansion;
    };

    // EVERY version gets a mark and a name now. Only WOTLK and SL had either before, so a project
    // on any of the other seven drew an empty 48px hole and an empty caption line -- and the card
    // layout makes both of those far more visible than the old strip did. The nine resources are
    // already in resources.qrc and were already being shipped; two of them were simply the only
    // two anything referenced.
    VersionPresentation presentationFor (Project::ProjectVersion version)
    {
      switch (version)
      {
        case Project::ProjectVersion::VANILLA:  return {":/icon-classic",  "Classic"};
        case Project::ProjectVersion::BC:       return {":/icon-burning",  "The Burning Crusade"};
        case Project::ProjectVersion::WOTLK:    return {":/icon-wrath",    "Wrath Of The Lich King"};
        case Project::ProjectVersion::CATA:     return {":/icon-cata",     "Cataclysm"};
        case Project::ProjectVersion::PANDARIA: return {":/icon-panda",    "Mists Of Pandaria"};
        case Project::ProjectVersion::WOD:      return {":/icon-warlords", "Warlords Of Draenor"};
        case Project::ProjectVersion::LEGION:   return {":/icon-legion",   "Legion"};
        case Project::ProjectVersion::BFA:      return {":/icon-battle",   "Battle For Azeroth"};
        case Project::ProjectVersion::SL:       return {":/icon-shadow",   "Shadowlands"};
      }

      // Not reachable through the enum, but a value read off disk is not obliged to be one of the
      // nine. The product mark is never wrong and never empty.
      return {":/icon", ""};
    }
  }

  ProjectListItem::ProjectListItem (ProjectListItemData const& data, QWidget* parent)
    : QWidget (parent)
  {
    // The card's FILL, its border and its SELECTED state are the theme's; hover, press, opening
    // and focus are painted in paintEvent. A plain QWidget only paints a style sheet background
    // when it is told to.
    setObjectName ("project-card");
    setAttribute (Qt::WA_StyledBackground, true);

    // Qt::WA_Hover IS GONE, and it is worth being exact about what that does and does not buy.
    //
    // It was set here so the sheet's QWidget#project-card:hover rule could fire, and that rule
    // was then measured NOT to fire on this widget -- see the class comment in the header for the
    // capture. The most likely reason is that nothing was repainting the card: WA_Hover asks the
    // style system to track the pointer, and on a persistent editor installed through
    // setItemWidget that tracking evidently did not reach it. Dropping the attribute withdraws a
    // request that was not being honoured anyway.
    //
    // WHAT IT DOES NOT DO is guarantee the sheet's hover rule can never resolve. QStyleSheetStyle
    // reads PseudoClass_Hover out of State_MouseOver, and QStyleOption::initFrom() sets that from
    // QWidget::underMouse(), which Qt maintains from Enter and Leave WITHOUT WA_Hover -- and
    // QStyleSheetStyle::polish() is at liberty to set the attribute back. So the guarantee is
    // made where it can actually be made, in paintEvent, by clearing State_MouseOver before the
    // background is drawn. See the comment there.
    //
    // Enter and Leave themselves were never part of that mechanism: Qt delivers them to any
    // widget that is not WA_TransparentForMouseEvents whether or not WA_Hover is set, which is
    // why enterEvent and leaveEvent work with the attribute gone. The child labels all ARE
    // transparent to the mouse (see the loop near the end of this constructor), so the pointer is
    // always over the CARD and the state does not flicker as it crosses the name, the path or the
    // artwork.

    setContextMenuPolicy (Qt::CustomContextMenu);

    qreal const ratio (devicePixelRatioF());

    VersionPresentation const presentation (presentationFor (data.project_version));

    // ------------------------------------------------------------------ the mark --
    //
    // CIRCULAR, AND THE CIRCLE IS BAKED. A style sheet cannot do this: border-radius rounds a
    // widget's BACKGROUND box, and a QLabel's pixmap is painted over that box unclipped, so a
    // rounded QLabel holding a square picture is still a square picture.
    _project_version_icon = new QLabel ("", this);
    _project_version_icon->setObjectName ("project-card-icon");
    _project_version_icon->setPixmap
      ( LauncherArt::circularIcon
          (QIcon (QString::fromUtf8 (presentation.resource)), ICON_EXTENT, QColor (RING_EDGE), ratio)
      );
    _project_version_icon->setFixedSize (ICON_EXTENT, ICON_EXTENT);

    // ------------------------------------------------------------------ the text --
    _project_name_label = new QLabel (toCamelCase (QString (data.project_name)), this);
    _project_name_label->setObjectName ("project-card-name");
    applyFont (_project_name_label, NAME_PIXEL_SIZE, true);
    makeElastic (_project_name_label);

    _project_directory_label = new QLabel (data.project_directory, this);
    _project_directory_label->setObjectName ("project-card-path");
    _project_directory_label->setToolTip (data.project_directory);
    applyFont (_project_directory_label, PATH_PIXEL_SIZE, false);
    makeElastic (_project_directory_label);

    // The expansion is INFORMATION, not an action, so the theme gives it info #6FAEDC and not the
    // accent -- the same reason a hyperlink takes info in this sheet. 6.289:1 on the card fill.
    // It used to share the path's caption rank and colour, which said the two lines were the same
    // kind of thing when one is an identity and the other is a location.
    _project_version_label = new QLabel (QString::fromUtf8 (presentation.expansion), this);
    _project_version_label->setObjectName ("project-card-expansion");
    applyFont (_project_version_label, EXPANSION_PIXEL_SIZE, true);
    makeElastic (_project_version_label);
    _project_version_label->setVisible (!_project_version_label->text().isEmpty());

    // --------------------------------------------------------------- the artwork --
    _project_art = new QLabel ("", this);
    _project_art->setObjectName ("project-card-art");
    _project_art->setPixmap
      ( LauncherArt::projectArtwork
          ( data.project_directory
          , QSize (ART_WIDTH, ART_HEIGHT)
          , QColor (ART_FILL)
          , QColor (ART_INK)
          , QColor (RING_EDGE)
          , ratio
          )
      );
    _project_art->setFixedSize (ART_WIDTH, ART_HEIGHT);

    // ------------------------------------------------------------------ the date --
    _project_last_edited_label = new QLabel (data.project_last_edited, this);
    _project_last_edited_label->setObjectName ("project-card-date");
    applyFont (_project_last_edited_label, PATH_PIXEL_SIZE, false);
    _project_last_edited_label->setAlignment (Qt::AlignRight | Qt::AlignVCenter);

    _project_date_glyph = new QLabel ("", this);
    _project_date_glyph->setObjectName ("project-card-date-glyph");
    _project_date_glyph->setPixmap
      (LauncherArt::calendarGlyph (DATE_GLYPH_EXTENT, QColor (DATE_GLYPH_INK), ratio));
    _project_date_glyph->setFixedSize (DATE_GLYPH_EXTENT, DATE_GLYPH_EXTENT);

    // The whole card shows the project path and reacts to the pointer as one surface.
    setToolTip (data.project_directory);

    // ----------------------------------------------------------------- the layout --
    auto const title_row (new QHBoxLayout());
    title_row->setContentsMargins (0, 0, 0, 0);
    title_row->setSpacing (6);
    title_row->addWidget (_project_name_label, 1);

    if (data.is_favorite)
    {
      _project_favorite_icon = new QLabel ("", this);
      _project_favorite_icon->setObjectName ("project-favorite");
      _project_favorite_icon->setPixmap
        (FontAwesomeIcon (FontAwesome::star).pixmap (QSize (16, 16)));
      _project_favorite_icon->setFixedSize (16, 16);
      _project_favorite_icon->setToolTip ("Favourite project -- loaded automatically on start");

      // Font Awesome renders the glyph as a monochrome pixmap and the icon engine takes no
      // colour, so the gold has to be applied to the rendered pixels. This is one of the few
      // colours in the application a style sheet cannot reach, which is exactly why it has to be
      // carried forward by hand when the accent moves -- see ACCENT_STAR above.
      auto const colour (new QGraphicsColorizeEffect (_project_favorite_icon));
      colour->setColor (QColor (ACCENT_STAR));
      colour->setStrength (1.0f);
      _project_favorite_icon->setGraphicsEffect (colour);

      title_row->addWidget (_project_favorite_icon, 0, Qt::AlignRight | Qt::AlignVCenter);
    }

    auto const text_column (new QVBoxLayout());
    text_column->setContentsMargins (0, 0, 0, 0);
    text_column->setSpacing (LINE_SPACING);
    text_column->addStretch (1);
    text_column->addLayout (title_row);
    text_column->addWidget (_project_directory_label);
    text_column->addWidget (_project_version_label);
    text_column->addStretch (1);

    auto const top_row (new QHBoxLayout());
    top_row->setContentsMargins (0, 0, 0, 0);
    top_row->setSpacing (COLUMN_SPACING);
    top_row->addWidget (_project_version_icon, 0, Qt::AlignVCenter);
    top_row->addLayout (text_column, 1);
    top_row->addWidget (_project_art, 0, Qt::AlignVCenter);

    _project_separator = new QFrame (this);
    _project_separator->setObjectName ("project-card-separator");
    _project_separator->setFrameShape (QFrame::HLine);
    _project_separator->setFrameShadow (QFrame::Plain);
    _project_separator->setLineWidth (SEPARATOR_HEIGHT);
    _project_separator->setFixedHeight (SEPARATOR_HEIGHT);

    // The date row is a WIDGET rather than a bare layout so the whole thing -- glyph, label and
    // the layout spacing around them -- can be hidden in one call when there is no date to show.
    // QBoxLayout skips a hidden widget AND the spacing that would have gone with it; it does not
    // skip a nested layout whose children happen to be hidden.
    _project_date_row = new QWidget (this);
    _project_date_row->setObjectName ("project-card-date-row");

    auto const date_row (new QHBoxLayout (_project_date_row));
    date_row->setContentsMargins (0, 0, 0, 0);
    date_row->setSpacing (6);
    date_row->addStretch (1);
    date_row->addWidget (_project_date_glyph, 0, Qt::AlignVCenter);
    date_row->addWidget (_project_last_edited_label, 0, Qt::AlignVCenter);

    // A CARD WITH AN EMPTY DATE ROW IS BETTER THAN A CARD WITH A WRONG ONE, and until this
    // revision every card showed today's date because the value was literally
    // QDateTime::currentDateTime(). RecentProjectsComponent now reads the .noggitproj file's
    // modification time and leaves the string empty when it cannot, and an empty string takes the
    // separator down with the row so the card closes cleanly instead of ending on a rule.
    bool const has_date (!data.project_last_edited.trimmed().isEmpty());
    _project_separator->setVisible (has_date);
    _project_date_row->setVisible (has_date);

    auto const root (new QVBoxLayout (this));
    root->setContentsMargins
      (CARD_MARGIN_LEFT, CARD_MARGIN_TOP, CARD_MARGIN_RIGHT, CARD_MARGIN_BOTTOM);
    root->setSpacing (SECTION_SPACING);
    root->addLayout (top_row);
    root->addWidget (_project_separator);
    root->addWidget (_project_date_row);

    // Every child stays out of the way of the mouse, so the CARD is always the widget under the
    // cursor: it is what carries the custom context menu policy the recent-projects list connects
    // to, and it is what the theme's :hover rule selects on. Mouse events it does not handle
    // still propagate to the list viewport, so the double-click that opens a project is
    // untouched.
    auto const passThrough
      ( [] (QWidget* child)
        {
          if (child)
          {
            child->setAttribute (Qt::WA_TransparentForMouseEvents, true);
          }
        }
      );

    passThrough (_project_version_icon);
    passThrough (_project_name_label);
    passThrough (_project_directory_label);
    passThrough (_project_version_label);
    passThrough (_project_art);
    passThrough (_project_separator);
    passThrough (_project_date_row);
    passThrough (_project_date_glyph);
    passThrough (_project_last_edited_label);
    passThrough (_project_favorite_icon);

    // KEYBOARD FOCUS IS NOT THIS WIDGET'S OWN. The card is a persistent editor with no focus
    // policy; the QListWidget above it takes the keyboard, and its arrow keys move the current
    // item, which is what RecentProjectsComponent turns into the selected state. So the focus
    // ring has to follow the VIEW, and the only notification a child gets of that is the
    // application-wide signal.
    //
    // `this` is the connection's CONTEXT OBJECT, so Qt tears the connection down with the card and
    // the lambda can never run against a destroyed widget. That matters here: these cards are
    // rebuilt from scratch on every create, forget and favourite change.
    connect ( qApp
            , &QApplication::focusChanged
            , this
            , [this] (QWidget*, QWidget*) { refreshOwnerFocus(); }
            );

    // The theme is set on the application before any window is built, so polishing here is what
    // makes contentMinimum() see the style sheet's font sizes rather than the QFont defaults set
    // above.
    ensurePolished();
    setMinimumHeight (contentMinimum());
  }

  int ProjectListItem::contentMinimum() const
  {
    // There IS a layout, so the height can simply be asked for rather than reconstructed from the
    // offsets the constructor used.
    int const from_layout (QWidget::minimumSizeHint().height());

    return std::max (CARD_MIN_HEIGHT, from_layout);
  }

  void ProjectListItem::changeEvent (QEvent* event)
  {
    QWidget::changeEvent (event);

    if (event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange)
    {
      int const floor (contentMinimum());

      if (minimumHeight() != floor)
      {
        setMinimumHeight (floor);
      }
    }
  }

  QSize ProjectListItem::minimumSizeHint() const
  {
    // Consumed only as the LIST ITEM's size hint, so it states what the item has to reserve: the
    // card's own content plus the chrome the delegate will take back off it.
    return QSize (ROW_HINT_WIDTH, contentMinimum() + ITEM_CHROME_HEADROOM);
  }

  ProjectListItem::Interaction ProjectListItem::currentInteraction() const
  {
    // THE PRECEDENCE, once, in one place. Opening outranks pressed because the button is released
    // while the project is still loading and the card must not drop back to a hover look under
    // the pointer. Pressed outranks hover because the pointer is by definition still inside the
    // card while a press is held.
    if (_opening)
    {
      return Interaction::Opening;
    }

    if (_pressed)
    {
      return Interaction::Pressed;
    }

    if (_hovered)
    {
      return Interaction::Hover;
    }

    return Interaction::Resting;
  }

  bool ProjectListItem::isSelectedCard() const
  {
    // READ ONLY. The property belongs to RecentProjectsComponent's currentItemChanged handler,
    // which sets it through Style::applyState; the card never writes it, so the selection axis and
    // the interaction axis cannot overwrite one another's storage.
    return property ("state").toString() == QLatin1String ("selected");
  }

  bool ProjectListItem::ownerHasKeyboardFocus() const
  {
    QWidget const* const top (window());

    if (!top)
    {
      return false;
    }

    QWidget const* const focus (top->focusWidget());

    if (!focus)
    {
      return false;
    }

    // The card is never itself the focus widget, so the WALK is what matters: setItemWidget
    // reparents the card to the view's viewport, and the viewport's parent is the QListWidget --
    // which is what actually takes the keyboard.
    for (QWidget const* widget (this); widget; widget = widget->parentWidget())
    {
      if (widget == focus)
      {
        return true;
      }
    }

    return false;
  }

  void ProjectListItem::refreshOwnerFocus()
  {
    bool const focused (ownerHasKeyboardFocus());

    if (focused != _owner_focused)
    {
      _owner_focused = focused;
      update();
    }
  }

  void ProjectListItem::armOpeningReset()
  {
    if (!_opening_reset)
    {
      // PARENTED TO THE CARD. QObject destroys its children before it finishes destroying itself,
      // so the timer dies with the card and a timeout already queued for a destroyed QTimer is
      // dropped -- the lambda's `this` cannot outlive the object it points at. That is the whole
      // reason this is a member and not a free QTimer::singleShot: the selection window is torn
      // down the moment the project opens, and anything armed here has to be safe against being
      // destroyed first.
      _opening_reset = new QTimer (this);
      _opening_reset->setSingleShot (true);

      // ZERO, not a guessed duration. The interval expires as soon as the event loop is free
      // again, which on the path that WORKS is after the selection window has already closed and
      // this card is gone or hidden -- so it does nothing there. It only ever acts on the FAILURE
      // path, where loadProject returned nothing and the window is still up: the card leaves the
      // busy state at once instead of sitting there claiming to be opening something.
      _opening_reset->setInterval (0);

      connect ( _opening_reset
              , &QTimer::timeout
              , this
              , [this]
                {
                  if (_opening)
                  {
                    _opening = false;
                    update();
                  }
                }
              );
    }

    _opening_reset->start();
  }

  void ProjectListItem::enterEvent (QEvent* event)
  {
    QWidget::enterEvent (event);

    if (!_hovered)
    {
      _hovered = true;
      update();
    }
  }

  void ProjectListItem::leaveEvent (QEvent* event)
  {
    QWidget::leaveEvent (event);

    // THE PRESS GOES WITH IT. A real button that is dragged off releases, and once the pointer
    // leaves, the list viewport holds the implicit mouse grab -- so this is the last event the
    // card is guaranteed to see until the pointer comes back.
    if (_hovered || _pressed)
    {
      _hovered = false;
      _pressed = false;
      update();
    }
  }

  void ProjectListItem::mousePressEvent (QMouseEvent* event)
  {
    if (event->button() == Qt::LeftButton && !_pressed)
    {
      _pressed = true;

      // repaint() AND NOT update(), because this feedback has a deadline. update() only POSTS a
      // paint event, and the first half of a double click can be shorter than one frame, so the
      // pressed state would be scheduled and then superseded before it was ever drawn. repaint()
      // paints into the backing store and flushes it inside this call.
      repaint();
    }

    // NOT CONSUMED. The base implementation ignores the event, and that is what propagates it to
    // the list viewport -- where selection, the double click that opens a project and the context
    // menu all live. None of the three change; this override only adds a repaint on the way past.
    QWidget::mousePressEvent (event);
  }

  void ProjectListItem::mouseReleaseEvent (QMouseEvent* event)
  {
    if (_pressed)
    {
      _pressed = false;
      update();
    }

    QWidget::mouseReleaseEvent (event);
  }

  void ProjectListItem::mouseDoubleClickEvent (QMouseEvent* event)
  {
    if (event->button() == Qt::LeftButton)
    {
      // THE DEAD SECOND THE USER REPORTED. Propagating this event runs
      // NoggitProjectSelectionWindow's doubleClicked handler SYNCHRONOUSLY: it reads the project
      // off disk, builds the editor window and closes this one. Nothing in that sequence returns
      // to the event loop, so a posted repaint would never be serviced and the click looks
      // ignored for as long as the load takes. repaint() is the whole point of this override --
      // it puts the busy state on the screen BEFORE the blocking work starts.
      _opening = true;
      _pressed = false;
      repaint();
      armOpeningReset();
    }

    QWidget::mouseDoubleClickEvent (event);
  }

  void ProjectListItem::showEvent (QShowEvent* event)
  {
    QWidget::showEvent (event);

    // Not the constructor: setItemWidget only reparents the card into the view's viewport after
    // this object is built, so this is the first moment ownerHasKeyboardFocus() can be right.
    refreshOwnerFocus();
  }

  // THE EIGHT COMBINATIONS, and what draws each one. The sheet owns both FILLS and both resting
  // borders; every other cell is painted below, on top of them.
  //
  //   selection   interaction   fill               ring                   extra
  //   ---------   -----------   ----------------   --------------------   --------------
  //   --          resting       #292621 (sheet)    1px #8A8378 (sheet)    --
  //   --          hover         #33302B            2px #BFB7AA            --
  //   --          pressed       #4F2B2D            3px #F05A73            --
  //   --          opening       #4F2B2D            3px #F05A73            4px bottom bar
  //   selected    resting       #3C2927 (sheet)    1px #E5405C (sheet)    --
  //   selected    hover         #453331            2px #F05A73            --
  //   selected    pressed       #5E2E32            3px #F05A73            --
  //   selected    opening       #5E2E32            3px #F05A73            4px bottom bar
  //
  // and, on any SELECTED row while the list holds the keyboard, a 1px #F3F0E9 hairline inset 4px.
  // No row is blank, and no two rows share both a fill and a ring.
  void ProjectListItem::paintEvent (QPaintEvent* event)
  {
    // Documented no-op, called so the base contract is honoured rather than assumed away.
    QWidget::paintEvent (event);

    QPainter painter (this);
    painter.setRenderHint (QPainter::Antialiasing, true);

    // THE THEME'S OWN FILL AND BORDER, DRAWN HERE, WITH THE POINTER BIT CLEARED.
    //
    // A WA_StyledBackground widget normally has QStyle::PE_Widget run for it by
    // QWidgetPrivate::paintBackground() before the paint event is delivered, and that call builds
    // its QStyleOption with initFrom(), which sets State_MouseOver from QWidget::underMouse().
    // QStyleSheetStyle turns that bit straight into PseudoClass_Hover. underMouse() is set from
    // Enter and Leave and does NOT need WA_Hover, so now that this widget repaints on those two
    // events, that automatic call would begin resolving the sheet's QWidget#project-card:hover
    // rule -- the rule this card was measured not to be able to use -- and ITS fill would land
    // underneath the wash below. Every composite quoted in this file would then be wrong by one
    // unaccounted layer.
    //
    // Re-running PE_Widget with State_MouseOver cleared costs one opaque rounded-rect fill and
    // buys two things: the base of every composite is deterministically the resting fill or, when
    // the dynamic property says so, the selected fill; and the card still has a fill and a border
    // even if the automatic pass did not happen. Where it DID happen the two draws are the same
    // opaque geometry, so only the corner arcs' antialiased fringe composites twice -- a fraction
    // of one pixel, and it makes the corner marginally crisper rather than different.
    painter.save();

    QStyleOption background;
    background.initFrom (this);
    background.state &= ~QStyle::State_MouseOver;
    style()->drawPrimitive (QStyle::PE_Widget, &background, &painter, this);

    painter.restore();

    Interaction const interaction (currentInteraction());
    bool const selected (isSelectedCard());
    bool const focus_ring (selected && _owner_focused);

    if (interaction == Interaction::Resting && !focus_ring)
    {
      return;
    }

    painter.setBrush (Qt::NoBrush);

    QRectF const box (rect());

    QPainterPath card;
    card.addRoundedRect (box, CARD_RADIUS, CARD_RADIUS);

    if (interaction != Interaction::Resting)
    {
      bool const hovering (interaction == Interaction::Hover);

      QColor wash (hovering ? QColor (HOVER_WASH_INK) : QColor (PRESS_WASH_INK));
      wash.setAlpha (hovering ? HOVER_WASH_ALPHA : PRESS_WASH_ALPHA);
      painter.fillPath (card, wash);

      if (interaction == Interaction::Opening)
      {
        // Clipped to the card so the bar's ends follow the corner radius instead of running out
        // past it, and drawn before the ring so the ring's inner antialiased edge lands on top.
        painter.save();
        painter.setClipPath (card);
        painter.fillRect
          ( QRectF ( box.left()
                   , box.bottom() - RING_ACTIVE_WIDTH - BUSY_BAR_HEIGHT
                   , box.width()
                   , BUSY_BAR_HEIGHT
                   )
          , QColor (RING_ACTIVE_INK)
          );
        painter.restore();
      }

      // The ring is centred on half its own width, so it covers the sheet's 1px border exactly
      // and then some: a 2px pen centred at 1.0 colours rows 0 and 1, a 3px pen centred at 1.5
      // colours rows 0 to 2. Insetting the radius by the same half keeps the arc concentric with
      // the one the sheet rounded, so the corners do not double up.
      int const ring_width (hovering ? RING_HOVER_WIDTH : RING_ACTIVE_WIDTH);
      qreal const half (ring_width / 2.0);

      painter.setPen
        (QPen (QColor (hovering && !selected ? RING_HOVER_INK : RING_ACTIVE_INK), ring_width));
      painter.drawRoundedRect
        ( box.adjusted (half, half, -half, -half)
        , CARD_RADIUS - half
        , CARD_RADIUS - half
        );
    }

    if (focus_ring)
    {
      // Last, so nothing can cover it, and half a pixel off the integer grid so a 1px pen lands
      // on one whole row instead of straddling two.
      qreal const inset (FOCUS_RING_INSET + 0.5);

      painter.setPen (QPen (QColor (FOCUS_RING_INK), 1));
      painter.drawRoundedRect
        ( box.adjusted (inset, inset, -inset, -inset)
        , CARD_RADIUS - inset
        , CARD_RADIUS - inset
        );
    }
  }

  QString ProjectListItem::toCamelCase (QString const& s)
  {
    QStringList parts = s.split (' ', Qt::SplitBehaviorFlags::SkipEmptyParts);

    for (int i = 0; i < parts.size(); ++i)
    {
      parts[i].replace (0, 1, parts[i][0].toUpper());
    }

    return parts.join (" ");
  }
}
