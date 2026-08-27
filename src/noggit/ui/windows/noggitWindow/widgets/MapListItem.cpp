#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/windows/noggitWindow/widgets/MapListItem.hpp>
#include <noggit/ui/windows/noggitWindow/widgets/MapSelectionArt.hpp>

#include <QColor>
#include <QEvent>
#include <QFont>
#include <QGraphicsColorizeEffect>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <algorithm>
#include <initializer_list>

namespace Noggit::Ui::Widget
{
  namespace
  {
    // Same shape, and the same reasoning, as ProjectListItem: an icon column beside a text
    // column, built from real layouts. The previous revision here stack-allocated a QGridLayout,
    // handed it to setLayout and let it destruct on return -- QLayout's destructor clears the
    // widget's layout pointer, so the row ended up with NO layout and every label was placed by
    // absolute setGeometry against the list view it was parented to rather than against the row.
    // Nothing reflowed and nothing scaled with the font.

    // THE ROW IS A CARD NOW, not a strip in a well. Everything below moved with that: the
    // emblem is a baked disc rather than a square pixmap, the margins grew so the card has an
    // inside, and a chevron closes the right edge. What did NOT move is the mechanism -- this is
    // still a persistent editor installed with setItemWidget, and the height arithmetic further
    // down is the same arithmetic it always was.
    constexpr int ICON_EXTENT = 36;

    // A card needs an inside. 14 all round against the old 8/10/12/8, which put the emblem four
    // pixels from a border that did not exist yet; the same margin on every edge is what makes a
    // rounded rectangle read as a container rather than as a row that happens to have an outline.
    constexpr int ROW_MARGIN_LEFT = 14;
    constexpr int ROW_MARGIN_TOP = 14;
    constexpr int ROW_MARGIN_RIGHT = 14;
    constexpr int ROW_MARGIN_BOTTOM = 14;

    constexpr int COLUMN_SPACING = 12;

    //! Inside the trailing group -- between the type badge and the chevron. SPACE_8 rather than
    //! the root's SPACE_12, which is what makes the two read as one object at the card's right
    //! edge instead of as two more columns.
    constexpr int TRAILING_SPACING = 8;

    //! The chevron at the right edge. 14 rather than 16 so it stays subordinate to the badge
    //! beside it -- it is a direction, not a control.
    constexpr int CHEVRON_EXTENT = 14;

    //! The pin star, when a map is pinned. Same extent as the chevron so the two marks at the
    //! two ends of the card carry the same weight.
    constexpr int PIN_EXTENT = 14;

    // 3px, matching ProjectListItem, so the two windows separate a title from its metadata by
    // the same amount.
    constexpr int LINE_SPACING = 3;

    // BuildMapListComponent feeds minimumSizeHint() straight to QListWidgetItem::setSizeHint, so
    // this is the list row height. The emblem plus the vertical margins comes to
    // 36 + 14 + 14 = 64; stating it as a floor keeps every card the same height even if a future
    // font makes the text column shorter, which is what stops a list of cards looking ragged.
    constexpr int ROW_MIN_HEIGHT = ICON_EXTENT + ROW_MARGIN_TOP + ROW_MARGIN_BOTTOM;

    // WHY THE MAP NAMES LOST THEIR DESCENDERS.
    //
    // setItemWidget() installs this row as a PERSISTENT EDITOR on the item. Its geometry is
    // therefore not the item rect: it comes from QStyledItemDelegate::updateEditorGeometry(),
    // which asks the style for SE_ItemViewItemText -- and QStyleSheetStyle subtracts the view's
    // ::item margin, border and padding from that rect. _continents_table is a plain QListWidget
    // with no accessibleName, so under CrimsonSlate it matches
    //   QAbstractItemView::item { padding: 5px 6px; }
    // and loses 5px top + 5px bottom.
    //
    // BuildMapListComponent hands setSizeHint() exactly this row's minimum, so the row was then
    // handed TEN PIXELS LESS than its minimum and the QVBoxLayout squeezed the text column.
    // Measured with a standalone Qt probe against the shipped theme: the title label needed 19px
    // and was given 11, an 8px shortfall against a font whose descent is 4px. That is the whole
    // defect -- nothing here was mis-measuring the font.
    //
    // Two independent guards, because the loss belongs to whichever theme is loaded and cannot
    // simply be read back: querying the view's style for SE_ItemViewItemText directly reports a
    // loss of zero (also measured), so there is nothing honest to subtract.
    //
    //   * setMinimumHeight(contentMinimum()) in the constructor is the GUARANTEE.
    //     QWidget::setGeometry clamps to the widget minimum, so the delegate cannot squeeze the
    //     row below what its content needs no matter how much chrome a theme asks for. Verified
    //     against an exaggerated theme with 24px of item chrome: still zero clipping.
    //
    //   * ITEM_CHROME_HEADROOM is the TIDINESS. Added to the hint the item is given so that,
    //     under a normal theme, the row FITS inside the slot instead of clamping and overhanging
    //     it. 12 covers the measured 10 with 2px to spare.
    constexpr int ITEM_CHROME_HEADROOM = 12;

    // Only decides whether the view thinks it needs a horizontal scroll bar; the list always
    // resizes the row to the viewport width. A map name can be long, so it must not be derived
    // from the text.
    constexpr int ROW_HINT_WIDTH = 125;
    // ACCENT_HI #F3687F. A QGraphicsColorizeEffect is unreachable from theme.qss, so this pin
    // colour has to be carried by hand whenever the accent moves -- and it was not: it sat at the
    // retired gold #DFA52E through the crimson migration, and the comment that stood here still
    // justified itself by saying the star must not be left behind "while everything around it goes
    // gold". Nothing around it goes gold any more.
    //
    // The LIT step rather than plain accent, because a map row lightens under the pointer and the
    // pin has to stay a legible mark on every fill a row presents. Measured against the row
    // surfaces: 6.513:1 on the bg.void list well, 5.900:1 on an alternating bg.alt row and 5.093:1
    // on a bg.panel card, all over the 3:1 floor a graphical mark has to clear.
    constexpr QRgb ACCENT_STAR = qRgb (0xF3, 0x68, 0x7F);

    // THE EMBLEM'S DISC AND RING AND THE CHEVRON'S INK, spelled out here for exactly the same
    // reason the star's gold is: a QPainter is not reachable from a style sheet, so these three
    // are hand-carried copies of theme tokens and have to be moved by hand when the theme moves.
    //
    //! bg.void #100E0B under the crest. The card's own fill is bg.panel #292621, so the disc is
    //! a 1.279:1 step BELOW the card -- a well, not a plate. That is the right direction for
    //! something a picture sits in, and it is the same step the sheet uses between every other
    //! pair of adjacent surfaces.
    constexpr QRgb EMBLEM_FILL = qRgb(0x10, 0x0E, 0x0B);

    //! stroke.hi #746D64 for the ring. Measured against the two surfaces it separates: 3.775:1
    //! on the bg.void disc and 2.952:1 on the bg.panel card. The first is the one that matters --
    //! the ring's job is to close the disc, and it clears the 3:1 graphical floor against the
    //! disc it closes. Against the card it is a seam between two surfaces that are already
    //! 1.279:1 apart, and a seam carries no 3:1 duty. edge #8A8378 was the other candidate and
    //! was rejected for the opposite reason: 5.138:1 on the disc makes the ring louder than the
    //! crest it is meant to frame.
    constexpr QRgb EMBLEM_RING = qRgb(0x74, 0x6D, 0x64);

    //! text.off #7F786A for the chevron. It is the quietest mark on the card on purpose: it says
    //! which way the card opens and must never compete with the map name. 3.443:1 on the
    //! bg.panel card fill and 4.402:1 on the bg.void ground behind the list -- over the 3:1 floor
    //! for a graphical object on both, and under the 4.5 body floor on the card, which is
    //! correct: this is a mark, not text.
    constexpr QRgb CHEVRON_INK = qRgb(0x7F, 0x78, 0x6A);

    //! The expansion crest for one map, or a null icon for an expansion this build has no
    //! artwork for. Lifted out of the constructor because rebuildPaintedMarks() needs the same
    //! mapping when the window changes screen, and two copies of a nine-way switch is one copy
    //! too many.
    QIcon crestFor (int expansion_id)
    {
      switch (expansion_id)
      {
        case 0: return QIcon(":/icon-classic");
        case 1: return QIcon(":/icon-burning");
        case 2: return QIcon(":/icon-wrath");
        case 3: return QIcon(":/icon-cata");
        case 4: return QIcon(":/icon-panda");
        case 5: return QIcon(":/icon-warlords");
        case 6: return QIcon(":/icon-legion");
        case 7: return QIcon(":/icon-battle");
        case 8: return QIcon(":/icon-shadow");
        default: return QIcon();
      }
    }

    // DEFAULTS, set through QFont rather than an inline style sheet -- a style sheet on the
    // widget itself outranks the application sheet, which is how the previous revision pinned
    // every row to 12px/10px no matter which theme was loaded. A theme's font-size still wins
    // over a font set this way, so CrimsonSlate dresses the row and a bare theme still gets a
    // readable hierarchy.
    constexpr int TITLE_PIXEL_SIZE = 13;
    constexpr int INFORMATION_PIXEL_SIZE = 11;

    void applyFont (QLabel* label, int pixel_size, bool bold)
    {
      QFont font (label->font());
      font.setPixelSize (pixel_size);
      font.setBold (bold);
      label->setFont (font);
    }

    // A long map name must not be allowed to set the row's minimum width, or it drags the whole
    // list wider than its viewport. Ignored means "take what is left over"; the text elides at
    // the right exactly as the fixed 300px geometry used to clip it.
    void makeElastic (QLabel* label)
    {
      label->setSizePolicy (QSizePolicy::Ignored, QSizePolicy::Fixed);
      label->setMinimumWidth (0);
    }
  }

  MapListItem::MapListItem(const MapListData& data, QWidget* parent = nullptr)
    : QWidget(parent)
    , _map_data(data)
  {
    // The card's fill, border, radius, hover and selected states are all in the theme, under
    // QWidget#map-card. This used to wear "project-list-item", which is the launcher's OLD strip
    // rank -- a transparent row with a translucent hover and no edge at all -- and the launcher
    // itself stopped using it when its rows became cards. A plain QWidget only paints a style
    // sheet background when it is told to.
    //
    // THE CARD IS OPAQUE, so the view's own selection wash is invisible underneath it. That is
    // why the sheet hands selection back to the card: QListWidget#map-list::item is transparent
    // in every state and QWidget#map-card carries :hover and [state="selected"] itself. It is
    // the same move the project cards on the launcher made, for the same reason.
    setObjectName ("map-card");
    setAttribute (Qt::WA_StyledBackground, true);

    setContextMenuPolicy(Qt::CustomContextMenu);

    _map_icon = new QLabel("", this);
    _map_icon->setObjectName("map-card-emblem");
    _map_icon->setFixedSize(ICON_EXTENT, ICON_EXTENT);

    auto project_name = toCamelCase(QString(_map_data.map_name));
    _map_name = new QLabel(project_name, this);
    _map_name->setObjectName("map-card-name");
    applyFont (_map_name, TITLE_PIXEL_SIZE, true);
    makeElastic (_map_name);

    // "530" on its own is a number with no noun. The row has the width for the word and the
    // detail header on the right-hand pane says the same thing the same way, so the two agree.
    _map_id = new QLabel(tr("Map %1").arg(_map_data.map_id), this);
    _map_id->setObjectName("map-card-id");
    applyFont (_map_id, INFORMATION_PIXEL_SIZE, false);
    makeElastic (_map_id);

    auto instance_type = QString("Unknown");
    switch (_map_data.map_type_id)
    {
      case 0: instance_type = "Continent"; break;
      case 1: instance_type = "Dungeon"; break;
      case 2: instance_type = "Raid"; break;
      case 3: instance_type = "Battleground"; break;
      case 4: instance_type = "Arena"; break;
      case 5: instance_type = "Scenario"; break;
      default: instance_type = "Unknown"; break;
    }

    _map_instance_type = new QLabel(instance_type, this);
    _map_instance_type->setObjectName("map-card-badge");
    applyFont (_map_instance_type, INFORMATION_PIXEL_SIZE, false);
    // Centred, not trailing. The badge was a right-aligned caption filling whatever the meta
    // row left over; it is a fixed-width chip now, and text pushed to one end of a pill looks
    // like a layout fault.
    _map_instance_type->setAlignment(Qt::AlignCenter);

    // The whole row is one hover surface and one context-menu target, so it also carries the
    // tooltip. Map id and instance type are the two facts the row cannot always show in full.
    setToolTip(tr("%1\nMap %2 -- %3").arg(project_name).arg(_map_data.map_id).arg(instance_type));

    // The badge is a BADGE now -- a bordered chip, not a second caption -- so it leaves the meta
    // row and joins the trailing group beside the chevron, which is where the mockup puts it.
    // Its own sheet rule sizes the chip; this only stops the layout stretching it.
    _map_instance_type->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    _map_chevron = new QLabel("", this);
    _map_chevron->setObjectName("map-card-chevron");
    _map_chevron->setFixedSize(CHEVRON_EXTENT, CHEVRON_EXTENT);

    auto const title_row = new QHBoxLayout();
    title_row->setContentsMargins(0, 0, 0, 0);
    title_row->setSpacing(6);
    title_row->addWidget(_map_name, 1);

    if (_map_data.pinned)
    {
      _map_pinned_label = new QLabel("", this);
      _map_pinned_label->setObjectName("project-pinned");
      _map_pinned_label->setFixedSize(PIN_EXTENT, PIN_EXTENT);
      _map_pinned_label->setToolTip(tr("Pinned map"));

      // Font Awesome renders the glyph as a monochrome pixmap and the icon engine takes no
      // colour, so the gold has to be applied to the rendered pixels. This is one of the few
      // colours in the application a style sheet cannot reach, which is exactly why it has to be
      // carried forward by hand when the accent moves -- see ACCENT_STAR above.
      auto const colour = new QGraphicsColorizeEffect(_map_pinned_label);
      colour->setColor(QColor(ACCENT_STAR));
      colour->setStrength(1.0f);
      _map_pinned_label->setGraphicsEffect(colour);

      title_row->addWidget(_map_pinned_label, 0, Qt::AlignRight | Qt::AlignVCenter);
    }

    // ALL THREE painted marks are baked here, in one call, after the last of them has been
    // constructed -- the star only exists when the map is pinned. Each is drawn at the WIDGET's
    // device pixel ratio rather than the primary screen's, and all three are re-baked together
    // if that ratio changes underneath the card. See rebuildPaintedMarks() for the 4x pixel
    // deficit this replaced.
    rebuildPaintedMarks();

    auto const text_column = new QVBoxLayout();
    text_column->setContentsMargins(0, 0, 0, 0);
    text_column->setSpacing(LINE_SPACING);
    text_column->addStretch(1);
    text_column->addLayout(title_row);
    text_column->addWidget(_map_id);
    text_column->addStretch(1);

    // FOUR COLUMNS, which is the mockup's card: the emblem, the two-line text block, the type
    // badge and the chevron. The badge used to share a row with the map id INSIDE the text
    // column, so a long map name and a long type name competed for one line and the badge's
    // right edge moved from card to card. Out here it is a chip in a column of its own and every
    // badge in the list lines up.
    auto const root = new QHBoxLayout(this);
    root->setContentsMargins(ROW_MARGIN_LEFT, ROW_MARGIN_TOP, ROW_MARGIN_RIGHT, ROW_MARGIN_BOTTOM);
    root->setSpacing(COLUMN_SPACING);
    root->addWidget(_map_icon, 0, Qt::AlignVCenter);
    root->addLayout(text_column, 1);

    // The badge and the chevron are ONE group at SPACE_8, not two more columns at the root's
    // SPACE_12. That is what the 22 in MAP_COLUMN_WIDTH's arithmetic counts: 14 for the chevron
    // and 8 for the gap in front of it.
    auto const trailing = new QHBoxLayout();
    trailing->setContentsMargins(0, 0, 0, 0);
    trailing->setSpacing(TRAILING_SPACING);
    trailing->addWidget(_map_instance_type, 0, Qt::AlignVCenter);
    trailing->addWidget(_map_chevron, 0, Qt::AlignVCenter);

    root->addLayout(trailing, 0);

    // The labels are children of the row now, not of the list view, so without this they would
    // be the widget under the cursor and the row's custom context menu would never fire.
    for (QLabel* label : {_map_icon, _map_name, _map_id, _map_instance_type, _map_chevron,
                          _map_pinned_label})
    {
      if (label)
        label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }

    // The theme is set on the application before any window is built, so polishing here is what
    // makes contentMinimum() see the style sheet's font sizes rather than the QFont defaults set
    // above. Without it the floor would be computed from the wrong metrics.
    ensurePolished();
    setMinimumHeight (contentMinimum());
  }

  int MapListItem::contentMinimum() const
  {
    // There IS a layout, so the height can be asked for rather than reconstructed from the
    // offsets the constructor used.
    int const from_layout (QWidget::minimumSizeHint().height());

    return std::max (ROW_MIN_HEIGHT, from_layout);
  }

  void MapListItem::changeEvent (QEvent* event)
  {
    QWidget::changeEvent (event);

    if (event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange)
    {
      int const floor (contentMinimum());

      // Guarded: setMinimumHeight schedules a relayout, and re-setting the value it already
      // holds on every unrelated style change is pure churn.
      if (minimumHeight() != floor)
        setMinimumHeight (floor);
    }

    // The two painted marks are baked at ONE device pixel ratio, so a window dragged to a screen
    // with a different one would leave them at the old resolution -- soft on a denser screen,
    // oversized in memory on a coarser one. ScreenChangeInternal is the event Qt delivers for
    // exactly that, and rebuilding on it is what keeps the pair honest without redrawing them on
    // every unrelated style change.
    if (event->type() == QEvent::ScreenChangeInternal)
      rebuildPaintedMarks();
  }

  void MapListItem::rebuildPaintedMarks()
  {
    // WHAT THE OLD CODE DID AND WHY IT WAS THE BLUR. The emblem used to be
    // icon.pixmap(QSize(ICON_EXTENT, ICON_EXTENT)). QIcon::pixmap takes a LOGICAL size, and
    // before Qt::AA_UseHighDpiPixmaps it handed back a ratio-1 bitmap -- so on this display,
    // whose devicePixelRatio measures 2, a 32px request produced 32 device pixels stretched
    // across 64 in each direction: a QUARTER of the pixels the slot could show. Asking for
    // extent x ratio and stamping the ratio back on is the fix, and it is a fix that does not
    // depend on that flag being set.
    //
    // The ratio comes from the WIDGET, not from the primary screen, so a window on a second
    // monitor gets that monitor's answer.
    qreal const ratio (devicePixelRatioF());

    if (_map_icon)
    {
      _map_icon->setPixmap
        ( MapSelectionArt::emblem ( crestFor (_map_data.expansion_id)
                                  , ICON_EXTENT
                                  , QColor (EMBLEM_FILL)
                                  , QColor (EMBLEM_RING)
                                  , ratio
                                  )
        );
    }

    if (_map_chevron)
    {
      _map_chevron->setPixmap
        (MapSelectionArt::chevronGlyph (CHEVRON_EXTENT, QColor (CHEVRON_INK), ratio));
    }

    if (_map_pinned_label)
    {
      // ASK BIG, THEN STATE THE LOGICAL SIZE FROM WHAT CAME BACK.
      //
      // This used to request size * ratio and then setDevicePixelRatio(ratio), which was right
      // when Qt::AA_UseHighDpiPixmaps was NOT set. It is set now (ApplicationEntry.cpp), and with
      // it QIcon::pixmap(QSize) already multiplies the size it is given by the device ratio and
      // stamps the ratio on the result -- so passing a device size AND re-stamping applied the
      // ratio twice and the art came out proportionally oversized.
      //
      // Deriving the ratio from the returned pixmap instead is correct with the flag on or off,
      // and correct whatever size the engine actually chose to hand back: logical width is
      // width() / (width() / target) == target, exactly, by construction. The large request is
      // kept deliberately -- it is what makes the engine rasterise at full device resolution
      // rather than at the logical size, which is the whole point of the exercise.
      //
      // The comment that stood here claimed AA_UseHighDpiPixmaps "only reaches the QWindow
      // overload and the style-drawn path". That is not so: the QSize overload forwards to
      // pixmap(0, size), which applies the ratio like every other overload. The claim was the
      // reason this site double-scaled, so it is corrected rather than deleted.
      QPixmap star
        ( FontAwesomeIcon (FontAwesome::star).pixmap
            (QSize (qRound (PIN_EXTENT * ratio), qRound (PIN_EXTENT * ratio)))
        );

      if (!star.isNull() && star.width() > 0)
      {
        star.setDevicePixelRatio (qreal (star.width()) / qreal (PIN_EXTENT));
      }

      _map_pinned_label->setPixmap (star);
    }
  }

  QSize MapListItem::minimumSizeHint() const
  {
    // This is only ever consumed as the LIST ITEM's size hint, so it states what the item has to
    // reserve -- the row's own content plus the chrome the delegate will take back off it.
    return QSize (ROW_HINT_WIDTH, contentMinimum() + ITEM_CHROME_HEADROOM);
  }

  const QString MapListItem::name() const
  {
    return _map_data.map_name;
  }

  int MapListItem::id() const
  {
    return _map_data.map_id;
  }

  int MapListItem::type() const
  {
    return _map_data.map_type_id;
  }

  int MapListItem::expansion() const
  {
    return _map_data.expansion_id;
  }

  bool MapListItem::wmo_map() const
  {
    return _map_data.wmo_map;
  }

  QString MapListItem::toCamelCase(const QString& s)
  {
    QStringList parts = s.split(' ', Qt::SplitBehaviorFlags::SkipEmptyParts);
    for (int i = 0; i < parts.size(); ++i)
      parts[i].replace(0, 1, parts[i][0].toUpper());

    return parts.join(" ");
  }
}
