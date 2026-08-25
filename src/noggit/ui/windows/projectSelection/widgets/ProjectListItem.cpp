// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/windows/projectSelection/widgets/ProjectListItem.hpp>

#include <QColor>
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
    // The row is an icon column beside a three-line text column. Everything below is a real
    // layout constant, not a setGeometry offset: the previous revision built a STACK-allocated
    // QGridLayout, handed it to setLayout and let it destruct on return, so the widget ended up
    // with no layout at all and every label was placed by absolute geometry -- and parented to
    // the LIST VIEW rather than to the row.
    constexpr int ICON_EXTENT = 48;

    constexpr int ROW_MARGIN_LEFT = 12;
    constexpr int ROW_MARGIN_TOP = 10;
    constexpr int ROW_MARGIN_RIGHT = 14;
    constexpr int ROW_MARGIN_BOTTOM = 10;

    // Between the icon column and the text column, and between the three text lines.
    constexpr int COLUMN_SPACING = 12;
    constexpr int LINE_SPACING = 3;

    // RecentProjectsComponent feeds minimumSizeHint() straight to QListWidgetItem::setSizeHint,
    // so this is the list row height. The icon plus the vertical margins already comes to 68;
    // stating it as a floor keeps the row comfortable even if a future font makes the text
    // column shorter than the icon.
    constexpr int ROW_MIN_HEIGHT = ICON_EXTENT + ROW_MARGIN_TOP + ROW_MARGIN_BOTTOM;

    // Deliberately narrow, and unchanged from the previous revision. The list widget always
    // resizes the row to the viewport width, so this number only decides whether the view
    // thinks it needs a horizontal scroll bar. A project path can be arbitrarily long, so the
    // hint must not be derived from it.
    constexpr int ROW_HINT_WIDTH = 125;

    // The three text ranks. These are DEFAULTS, set through QFont rather than through an inline
    // style sheet: a style sheet on the widget itself outranks the application sheet, which is
    // how the previous revision pinned every row to one size no matter which theme was loaded.
    // A theme's font-size still wins over a font set this way, so CrimsonSlate dresses the row
    // and a theme that says nothing about it still gets a readable hierarchy.
    constexpr int TITLE_PIXEL_SIZE = 14;
    constexpr int INFORMATION_PIXEL_SIZE = 11;

    void applyFont (QLabel* label, int pixel_size, bool bold)
    {
      QFont font (label->font());
      font.setPixelSize (pixel_size);
      font.setBold (bold);
      label->setFont (font);
    }

    // A label whose text may be long must not be allowed to set the row's minimum width, or a
    // deep project path drags the whole list wider than its viewport. Ignored means "the layout
    // gives you what is left over"; the text clips at the right exactly as it did before.
    void makeElastic (QLabel* label)
    {
      label->setSizePolicy (QSizePolicy::Ignored, QSizePolicy::Fixed);
      label->setMinimumWidth (0);
    }
  }

  ProjectListItem::ProjectListItem(const ProjectListItemData& data, QWidget* parent) : QWidget(parent)
  {
    // The hover plate, and the transparent background that lets the view's own selection wash
    // show through, are both in the theme. A plain QWidget only paints a style sheet background
    // when it is told to.
    setObjectName ("project-list-item");
    setAttribute (Qt::WA_StyledBackground, true);

    setContextMenuPolicy(Qt::CustomContextMenu);

    QIcon icon;
    if (data.project_version == Project::ProjectVersion::WOTLK)
      icon = QIcon(":/icon-wrath");
    if (data.project_version == Project::ProjectVersion::SL)
      icon = QIcon(":/icon-shadow");

    _project_version_icon = new QLabel("", this);
    _project_version_icon->setObjectName("project-icon-label");
    _project_version_icon->setPixmap(icon.pixmap(QSize(ICON_EXTENT, ICON_EXTENT)));
    _project_version_icon->setFixedSize(ICON_EXTENT, ICON_EXTENT);

    auto project_name = toCamelCase(QString(data.project_name));
    _project_name_label = new QLabel(project_name, this);
    _project_name_label->setObjectName("project-title-label");
    applyFont (_project_name_label, TITLE_PIXEL_SIZE, true);
    makeElastic (_project_name_label);

    _project_directory_label = new QLabel(data.project_directory, this);
    _project_directory_label->setObjectName("project-information");
    _project_directory_label->setToolTip(data.project_directory);
    applyFont (_project_directory_label, INFORMATION_PIXEL_SIZE, false);
    makeElastic (_project_directory_label);

    QString version;
    if (data.project_version == Project::ProjectVersion::WOTLK)
      version = "Wrath Of The Lich King";
    if (data.project_version == Project::ProjectVersion::SL)
      version = "Shadowlands";

    _project_version_label = new QLabel(version, this);
    _project_version_label->setObjectName("project-information");
    applyFont (_project_version_label, INFORMATION_PIXEL_SIZE, false);
    makeElastic (_project_version_label);

    _project_last_edited_label = new QLabel(data.project_last_edited, this);
    _project_last_edited_label->setObjectName("project-information");
    applyFont (_project_last_edited_label, INFORMATION_PIXEL_SIZE, false);
    _project_last_edited_label->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);

    // The whole row shows the project path and reacts to the pointer as one surface. The labels
    // stay out of the way of the mouse so that the row -- which is what carries the custom
    // context menu policy the recent-projects list connects to -- is always the widget under the
    // cursor. Mouse events it does not handle still propagate to the list viewport, so the
    // double-click that opens a project is untouched.
    setToolTip(data.project_directory);

    auto const text_column = new QVBoxLayout();
    text_column->setContentsMargins(0, 0, 0, 0);
    text_column->setSpacing(LINE_SPACING);

    auto const title_row = new QHBoxLayout();
    title_row->setContentsMargins(0, 0, 0, 0);
    title_row->setSpacing(6);
    title_row->addWidget(_project_name_label, 1);

    if (data.is_favorite)
    {
      _project_favorite_icon = new QLabel("", this);
      _project_favorite_icon->setObjectName("project-favorite");
      _project_favorite_icon->setPixmap(FontAwesomeIcon(FontAwesome::star).pixmap(QSize(16, 16)));
      _project_favorite_icon->setFixedSize(16, 16);
      _project_favorite_icon->setToolTip("Favourite project -- loaded automatically on start");

      // Font Awesome renders the glyph as a monochrome pixmap and the icon engine takes no
      // colour, so the gold has to be applied to the rendered pixels. Same effect as before.
      auto const colour = new QGraphicsColorizeEffect(_project_favorite_icon);
      colour->setColor(QColor(224, 163, 62));
      colour->setStrength(1.0f);
      _project_favorite_icon->setGraphicsEffect(colour);

      title_row->addWidget(_project_favorite_icon, 0, Qt::AlignRight | Qt::AlignVCenter);
    }

    auto const meta_row = new QHBoxLayout();
    meta_row->setContentsMargins(0, 0, 0, 0);
    meta_row->setSpacing(6);
    meta_row->addWidget(_project_version_label, 1);
    meta_row->addWidget(_project_last_edited_label, 0, Qt::AlignRight | Qt::AlignVCenter);

    text_column->addStretch(1);
    text_column->addLayout(title_row);
    text_column->addWidget(_project_directory_label);
    text_column->addLayout(meta_row);
    text_column->addStretch(1);

    auto const root = new QHBoxLayout(this);
    root->setContentsMargins(ROW_MARGIN_LEFT, ROW_MARGIN_TOP, ROW_MARGIN_RIGHT, ROW_MARGIN_BOTTOM);
    root->setSpacing(COLUMN_SPACING);
    root->addWidget(_project_version_icon, 0, Qt::AlignVCenter);
    root->addLayout(text_column, 1);

    for (QLabel* label : {_project_version_icon, _project_name_label, _project_directory_label,
                          _project_version_label, _project_last_edited_label, _project_favorite_icon})
    {
      if (label)
        label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }
  }

  QSize ProjectListItem::minimumSizeHint() const
  {
    // There IS a layout now, so the height can simply be asked for rather than reconstructed
    // from the offsets the constructor used -- which is what the previous revision had to do,
    // and why it drifted out of step with the placement it was describing.
    int const from_layout (QWidget::minimumSizeHint().height());

    return QSize (ROW_HINT_WIDTH, std::max (ROW_MIN_HEIGHT, from_layout));
  }

  QString ProjectListItem::toCamelCase(const QString& s)
  {
    QStringList parts = s.split(' ', Qt::SplitBehaviorFlags::SkipEmptyParts);
    for (int i = 0; i < parts.size(); ++i)
      parts[i].replace(0, 1, parts[i][0].toUpper());

    return parts.join(" ");
  }
}
