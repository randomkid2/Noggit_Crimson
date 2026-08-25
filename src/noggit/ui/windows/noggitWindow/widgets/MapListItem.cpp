#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/windows/noggitWindow/widgets/MapListItem.hpp>

#include <QColor>
#include <QEvent>
#include <QFont>
#include <QGraphicsColorizeEffect>
#include <QHBoxLayout>
#include <QLabel>
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
    constexpr int ICON_EXTENT = 32;

    constexpr int ROW_MARGIN_LEFT = 10;
    constexpr int ROW_MARGIN_TOP = 8;
    constexpr int ROW_MARGIN_RIGHT = 12;
    constexpr int ROW_MARGIN_BOTTOM = 8;

    constexpr int COLUMN_SPACING = 10;

    // 3px, matching ProjectListItem, so the two windows separate a title from its metadata by
    // the same amount.
    constexpr int LINE_SPACING = 3;

    // BuildMapListComponent feeds minimumSizeHint() straight to QListWidgetItem::setSizeHint, so
    // this is the list row height. The icon plus the vertical margins comes to 48; stating it as
    // a floor keeps the row square even if a future font makes the text column shorter.
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
    // The hover plate and the transparent background that lets the view's own selection wash
    // show through are both in the theme. A plain QWidget only paints a style sheet background
    // when it is told to.
    setObjectName ("project-list-item");
    setAttribute (Qt::WA_StyledBackground, true);

    setContextMenuPolicy(Qt::CustomContextMenu);

    QIcon icon;
    switch (_map_data.expansion_id)
    {
      case 0: icon = QIcon(":/icon-classic"); break;
      case 1: icon = QIcon(":/icon-burning"); break;
      case 2: icon = QIcon(":/icon-wrath"); break;
      case 3: icon = QIcon(":/icon-cata"); break;
      case 4: icon = QIcon(":/icon-panda"); break;
      case 5: icon = QIcon(":/icon-warlords"); break;
      case 6: icon = QIcon(":/icon-legion"); break;
      case 7: icon = QIcon(":/icon-battle"); break;
      case 8: icon = QIcon(":/icon-shadow"); break;
      default: break;
    }

    _map_icon = new QLabel("", this);
    _map_icon->setObjectName("project-icon-label");
    _map_icon->setPixmap(icon.pixmap(QSize(ICON_EXTENT, ICON_EXTENT)));
    _map_icon->setFixedSize(ICON_EXTENT, ICON_EXTENT);

    auto project_name = toCamelCase(QString(_map_data.map_name));
    _map_name = new QLabel(project_name, this);
    _map_name->setObjectName("project-title-label");
    applyFont (_map_name, TITLE_PIXEL_SIZE, true);
    makeElastic (_map_name);

    _map_id = new QLabel(QString::number(_map_data.map_id), this);
    _map_id->setObjectName("project-information");
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
    _map_instance_type->setObjectName("project-information");
    applyFont (_map_instance_type, INFORMATION_PIXEL_SIZE, false);
    _map_instance_type->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);

    // The whole row is one hover surface and one context-menu target, so it also carries the
    // tooltip. Map id and instance type are the two facts the row cannot always show in full.
    setToolTip(tr("%1\nMap %2 -- %3").arg(project_name).arg(_map_data.map_id).arg(instance_type));

    auto const title_row = new QHBoxLayout();
    title_row->setContentsMargins(0, 0, 0, 0);
    title_row->setSpacing(6);
    title_row->addWidget(_map_name, 1);

    if (_map_data.pinned)
    {
      _map_pinned_label = new QLabel("", this);
      _map_pinned_label->setObjectName("project-pinned");
      _map_pinned_label->setPixmap(FontAwesomeIcon(FontAwesome::star).pixmap(QSize(14, 14)));
      _map_pinned_label->setFixedSize(14, 14);
      _map_pinned_label->setToolTip(tr("Pinned map"));

      // Font Awesome renders the glyph as a monochrome pixmap and the icon engine takes no
      // colour, so the gold has to be applied to the rendered pixels. Same effect as before.
      auto const colour = new QGraphicsColorizeEffect(_map_pinned_label);
      colour->setColor(QColor(224, 163, 62));
      colour->setStrength(1.0f);
      _map_pinned_label->setGraphicsEffect(colour);

      title_row->addWidget(_map_pinned_label, 0, Qt::AlignRight | Qt::AlignVCenter);
    }

    auto const meta_row = new QHBoxLayout();
    meta_row->setContentsMargins(0, 0, 0, 0);
    meta_row->setSpacing(6);
    meta_row->addWidget(_map_id, 1);
    meta_row->addWidget(_map_instance_type, 0, Qt::AlignRight | Qt::AlignVCenter);

    auto const text_column = new QVBoxLayout();
    text_column->setContentsMargins(0, 0, 0, 0);
    text_column->setSpacing(LINE_SPACING);
    text_column->addStretch(1);
    text_column->addLayout(title_row);
    text_column->addLayout(meta_row);
    text_column->addStretch(1);

    auto const root = new QHBoxLayout(this);
    root->setContentsMargins(ROW_MARGIN_LEFT, ROW_MARGIN_TOP, ROW_MARGIN_RIGHT, ROW_MARGIN_BOTTOM);
    root->setSpacing(COLUMN_SPACING);
    root->addWidget(_map_icon, 0, Qt::AlignVCenter);
    root->addLayout(text_column, 1);

    // The labels are children of the row now, not of the list view, so without this they would
    // be the widget under the cursor and the row's custom context menu would never fire.
    for (QLabel* label : {_map_icon, _map_name, _map_id, _map_instance_type, _map_pinned_label})
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
