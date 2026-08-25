#ifndef NOGGIT_WIGDET_MAP_LIST_ITEM_HPP
#define NOGGIT_WIGDET_MAP_LIST_ITEM_HPP

#include <QSize>
#include <QString>
#include <QWidget>

class QLabel;

namespace Noggit::Ui::Widget
{
    struct MapListData
    {
        QString map_name;
        int map_id;
        int map_type_id;
        int expansion_id;
        bool pinned;
        bool wmo_map;
    };

    class MapListItem : public QWidget
    {
        Q_OBJECT
    private:
        QLabel* _map_icon = nullptr;
        QLabel* _map_name = nullptr;
        QLabel* _map_id = nullptr;
        QLabel* _map_instance_type = nullptr;
        // Only built when the map is pinned. It was left uninitialised before, and the
        // constructor now walks the label list to make them transparent to the mouse.
        QLabel* _map_pinned_label = nullptr;
        int _max_width = 0;
        MapListData _map_data;

    public:
        MapListItem(const MapListData& data, QWidget* parent);
        QSize minimumSizeHint() const override;

        const QString name() const;;
        int id() const;;
        int type() const;;
        int expansion() const;;
        bool wmo_map() const;;

    private:
        QString toCamelCase(const QString& s);
    };
}

#endif //NOGGIT_WIGDET_MAP_LIST_ITEM_HPP
