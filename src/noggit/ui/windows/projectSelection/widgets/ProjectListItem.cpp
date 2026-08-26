// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/windows/projectSelection/widgets/LauncherArt.hpp>
#include <noggit/ui/windows/projectSelection/widgets/ProjectListItem.hpp>

#include <QColor>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QGraphicsColorizeEffect>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QSizePolicy>
#include <QStringList>
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
    // QGraphicsEffect is invisible to theme.qss, so these four have to be carried by hand when
    // the palette moves. Each one is measured against the surfaces a card can actually sit on.

    //! The accent. The favourite star is "the thing you are acting on", not brand furniture, so
    //! it stays GOLD while the rest of this window takes the crimson brand rank. 6.860:1 on the
    //! bg.panel card fill, 6.213:1 on the selected fill #3C2927 and 5.894:1 on the hover fill
    //! #34312C -- far over the 3:1 floor for a graphical mark on all three.
    constexpr QRgb ACCENT_GOLD = qRgb (0xDF, 0xA5, 0x2E);

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
    // The card's fill, its border and its three states are all in the theme. A plain QWidget only
    // paints a style sheet background when it is told to.
    setObjectName ("project-card");
    setAttribute (Qt::WA_StyledBackground, true);

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
      // carried forward by hand when the accent moves -- see ACCENT_GOLD above.
      auto const colour (new QGraphicsColorizeEffect (_project_favorite_icon));
      colour->setColor (QColor (ACCENT_GOLD));
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
