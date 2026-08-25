#ifndef NOGGIT_WIGDET_MAP_LIST_ITEM_HPP
#define NOGGIT_WIGDET_MAP_LIST_ITEM_HPP

#include <QSize>
#include <QString>
#include <QWidget>

class QEvent;
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

    protected:
        // Re-arms the height floor when the theme or the font changes underneath the row, so a
        // theme switch cannot leave the floor stale at the old font's metrics.
        void changeEvent(QEvent* event) override;

    private:
        QString toCamelCase(const QString& s);

        // The height the row's own content needs, before any allowance for the view's item
        // chrome. Used both for the height floor and as the base of the size hint.
        int contentMinimum() const;
    };
}

#endif //NOGGIT_WIGDET_MAP_LIST_ITEM_HPP
