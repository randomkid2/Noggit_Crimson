// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_WIGDET_PROJECT_LIST_ITEM_HPP
#define NOGGIT_WIGDET_PROJECT_LIST_ITEM_HPP

#include <noggit/project/ApplicationProject.h>

#include <QSize>
#include <QString>
#include <QWidget>

class QEvent;
class QFrame;
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

    // One project, drawn as a CARD: a circular expansion mark, the project name, its path, its
    // expansion in the information blue, an artwork tile floated right, a hairline separator and
    // a dated footer. The fill, the border and the three surface states are the theme's; every
    // pixmap here is painted, because a style sheet can neither clip a QLabel's pixmap to a
    // circle nor invent a per-project picture.
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

        //! The card artwork: a sidecar picture if the project directory carries one, otherwise a
        //! tile seeded from the project path. See LauncherArt::projectArtwork for why the slot is
        //! always filled rather than being hidden when there is no picture.
        QLabel* _project_art = nullptr;

        //! The calendar mark and the hairline above the dated footer. Both are hidden, together,
        //! when the project has no readable modification date -- an empty footer under a rule
        //! looks like a card that failed to load.
        QLabel* _project_date_glyph = nullptr;
        QFrame* _project_separator = nullptr;
        QWidget* _project_date_row = nullptr;
    public:
        ProjectListItem(const ProjectListItemData& data, QWidget* parent);
        QSize minimumSizeHint() const override;
    protected:
        // Re-arms the height floor when the theme or the font changes underneath the card.
        void changeEvent(QEvent* event) override;
    private:
        QString toCamelCase(const QString& s);

        // The height the card's own content needs, before any allowance for the view's item
        // chrome. Used both for the height floor and as the base of the size hint.
        int contentMinimum() const;
    };
}

#endif //NOGGIT_WIGDET_PROJECT_LIST_ITEM_HPP
