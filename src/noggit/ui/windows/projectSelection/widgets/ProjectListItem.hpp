// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_WIGDET_PROJECT_LIST_ITEM_HPP
#define NOGGIT_WIGDET_PROJECT_LIST_ITEM_HPP

#include <noggit/project/ApplicationProject.h>

#include <QSize>
#include <QString>
#include <QWidget>

class QEvent;
class QLabel;

namespace Noggit::Ui::Widget
{
    struct ProjectListItemData
    {
        QString project_name;
        QString project_directory;
        QString project_last_edited;
        Project::ProjectVersion project_version;
        bool is_favorite;
    };

    class ProjectListItem : public QWidget
    {
        Q_OBJECT
    private:
        QLabel* _project_version_icon = nullptr;
        QLabel* _project_name_label = nullptr;
        QLabel* _project_directory_label = nullptr;
        QLabel* _project_version_label = nullptr;
        QLabel* _project_last_edited_label = nullptr;
        QLabel* _project_favorite_icon = nullptr;
    public:
        ProjectListItem(const ProjectListItemData& data, QWidget* parent);
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

#endif //NOGGIT_WIGDET_PROJECT_LIST_ITEM_HPP
