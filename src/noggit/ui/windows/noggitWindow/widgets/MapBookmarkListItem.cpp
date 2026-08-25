#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/windows/noggitWindow/widgets/MapBookmarkListItem.hpp>

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
        // Identical to MapListItem, and for the identical reason: the previous revision
        // stack-allocated a QGridLayout, handed it to setLayout and let it destruct on return,
        // leaving the row with no layout and three labels parented to the LIST VIEW at fixed
        // setGeometry offsets.
        constexpr int ICON_EXTENT = 30;

        constexpr int ROW_MARGIN_LEFT = 10;
        constexpr int ROW_MARGIN_TOP = 8;
        constexpr int ROW_MARGIN_RIGHT = 12;
        constexpr int ROW_MARGIN_BOTTOM = 8;

        constexpr int COLUMN_SPACING = 10;
        constexpr int LINE_SPACING = 2;

        // NoggitWindow feeds minimumSizeHint() straight to QListWidgetItem::setSizeHint.
        constexpr int ROW_MIN_HEIGHT = ICON_EXTENT + ROW_MARGIN_TOP + ROW_MARGIN_BOTTOM;
        constexpr int ROW_HINT_WIDTH = 125;

        // Defaults through QFont, not through an inline style sheet -- see MapListItem.
        constexpr int TITLE_PIXEL_SIZE = 13;
        constexpr int INFORMATION_PIXEL_SIZE = 11;

        void applyFont (QLabel* label, int pixel_size, bool bold)
        {
            QFont font (label->font());
            font.setPixelSize (pixel_size);
            font.setBold (bold);
            label->setFont (font);
        }

        void makeElastic (QLabel* label)
        {
            label->setSizePolicy (QSizePolicy::Ignored, QSizePolicy::Fixed);
            label->setMinimumWidth (0);
        }
    }

    MapListBookmarkItem::MapListBookmarkItem(const MapListBookmarkData& data, QWidget* parent = nullptr) : QWidget(parent)
    {
        setObjectName ("project-list-item");
        setAttribute (Qt::WA_StyledBackground, true);

        setContextMenuPolicy(Qt::CustomContextMenu);

        map_icon = new QLabel("", this);
        map_icon->setObjectName("project-icon-label");
        map_icon->setPixmap(FontAwesomeIcon(FontAwesome::bookmark).pixmap(QSize(ICON_EXTENT, ICON_EXTENT)));
        map_icon->setFixedSize(ICON_EXTENT, ICON_EXTENT);

        // Font Awesome renders the glyph as a monochrome pixmap and the icon engine takes no
        // colour, so the gold has to be applied to the rendered pixels. Same effect as before,
        // re-parented to the label it tints rather than to the row.
        auto const colour = new QGraphicsColorizeEffect(map_icon);
        colour->setColor(QColor(224, 163, 62));
        colour->setStrength(1.0f);
        map_icon->setGraphicsEffect(colour);

        auto const map_name_text = toCamelCase(QString(data.MapName));
        map_name = new QLabel(map_name_text, this);
        map_name->setObjectName("project-title-label");
        applyFont (map_name, TITLE_PIXEL_SIZE, true);
        makeElastic (map_name);

        // QString::number, not a std::stringstream built to be thrown away one line later.
        auto const position_text = QString("%1, %2, %3")
                                     .arg(static_cast<int>(data.Position.x))
                                     .arg(static_cast<int>(data.Position.y))
                                     .arg(static_cast<int>(data.Position.z));

        map_position = new QLabel(position_text, this);
        map_position->setObjectName("project-information");
        applyFont (map_position, INFORMATION_PIXEL_SIZE, false);
        makeElastic (map_position);

        setToolTip(tr("%1\nBookmarked at %2").arg(map_name_text).arg(position_text));

        auto const text_column = new QVBoxLayout();
        text_column->setContentsMargins(0, 0, 0, 0);
        text_column->setSpacing(LINE_SPACING);
        text_column->addStretch(1);
        text_column->addWidget(map_name);
        text_column->addWidget(map_position);
        text_column->addStretch(1);

        auto const root = new QHBoxLayout(this);
        root->setContentsMargins(ROW_MARGIN_LEFT, ROW_MARGIN_TOP, ROW_MARGIN_RIGHT, ROW_MARGIN_BOTTOM);
        root->setSpacing(COLUMN_SPACING);
        root->addWidget(map_icon, 0, Qt::AlignVCenter);
        root->addLayout(text_column, 1);

        for (QLabel* label : {map_icon, map_name, map_position})
        {
            label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        }
    }

    QSize MapListBookmarkItem::minimumSizeHint() const
    {
        int const from_layout (QWidget::minimumSizeHint().height());

        return QSize (ROW_HINT_WIDTH, std::max (ROW_MIN_HEIGHT, from_layout));
    }

    QString MapListBookmarkItem::toCamelCase(const QString& s)
    {
        QStringList parts = s.split(' ', Qt::SplitBehaviorFlags::SkipEmptyParts);
        for (int i = 0; i < parts.size(); ++i)
            parts[i].replace(0, 1, parts[i][0].toUpper());

        return parts.join(" ");
    }
}
