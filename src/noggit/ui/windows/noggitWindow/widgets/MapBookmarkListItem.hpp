#ifndef NOGGIT_WIGDET_MAP_BOOKMARK_LIST_ITEM_HPP
#define NOGGIT_WIGDET_MAP_BOOKMARK_LIST_ITEM_HPP

#include <glm/vec3.hpp>

#include <QSize>
#include <QString>
#include <QWidget>

class QEvent;
class QLabel;

namespace Noggit::Ui::Widget
{
    struct MapListBookmarkData
    {
        QString MapName;
        glm::vec3 Position;
    };

    class MapListBookmarkItem : public QWidget
    {
        Q_OBJECT
    private:
        QLabel* map_icon;
        QLabel* map_name;
        QLabel* map_position;
        int _maxWidth;
    public:
        MapListBookmarkItem(const MapListBookmarkData& data, QWidget* parent);
        QSize minimumSizeHint() const override;
    protected:
        // Re-arms the height floor when the theme or the font changes underneath the row.
        void changeEvent(QEvent* event) override;
    private:
        QString toCamelCase(const QString& s);

        // The height the row's own content needs, before any allowance for the view's item
        // chrome. Used both for the height floor and as the base of the size hint.
        int contentMinimum() const;
    };
}

#endif //NOGGIT_WIGDET_MAP_BOOKMARK_LIST_ITEM_HPP
