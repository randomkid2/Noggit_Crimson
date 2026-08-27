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
class QMouseEvent;
class QPaintEvent;
class QShowEvent;
class QTimer;

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
    // a dated footer. The fill, the border and the selected state are the theme's; every pixmap
    // here is painted, because a style sheet can neither clip a QLabel's pixmap to a circle nor
    // invent a per-project picture.
    //
    // THE CARD IS A CONTROL, AND IT PAINTS ITS OWN INTERACTION STATES. The sheet's :hover rule on
    // this widget was measured NOT to fire -- the application was launched, the real cursor driven
    // over the card with SetCursorPos, and the border sampled from a PrintWindow capture before
    // and after: it stayed at the non-hover value. The card is installed through
    // QListWidget::setItemWidget, i.e. as a PERSISTENT EDITOR, which is the likely reason; the
    // [state="selected"] attribute rule on the same widget does work, so the failure is specific
    // to the pointer pseudo-state and not to style sheets in general. Hover, press, opening and
    // focus therefore go through paintEvent, composited on top of whatever fill the sheet drew.
    class ProjectListItem : public QWidget
    {
        Q_OBJECT
    private:
        //! WHAT THE CARD IS DOING, as ONE value. The three booleans below are event bookkeeping
        //! and are never painted independently -- currentInteraction() folds them into this in a
        //! fixed precedence, so there is exactly one place where "pressed while hovered" or
        //! "opening after the pointer left" is decided, and no combination can leave the card
        //! with nothing drawn.
        //!
        //! The SECOND axis, selection, stays where it already worked: the dynamic `state`
        //! property that RecentProjectsComponent sets through Style::applyState and that the
        //! sheet's QWidget#project-card[state="selected"] rule paints. This enum composites ON
        //! TOP of that rule's fill rather than replacing it, which is why the two axes cannot
        //! overwrite one another. Eight combinations, all distinct: see the state table in the
        //! .cpp above paintEvent.
        enum class Interaction
        {
            Resting,
            Hover,
            Pressed,
            Opening
        };

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

        //! The pointer is inside the card. Driven by enterEvent/leaveEvent, which are delivered to
        //! any widget that is not WA_TransparentForMouseEvents and are entirely independent of the
        //! WA_Hover/QStyleSheetStyle path that was measured not to work here.
        bool _hovered = false;

        //! The left button went down on the card and has not come back up. Cleared on release AND
        //! on leave, so dragging off a held card releases it the way a real button does.
        bool _pressed = false;

        //! An activating gesture has been handed on and the project is being read off disk. The
        //! card holds the pressed look and grows a busy bar until the window is torn down.
        bool _opening = false;

        //! The view that owns this card has the keyboard. NOT this widget's own focus: a
        //! persistent editor never takes focus, the QListWidget does, and its arrow keys are what
        //! move the current item. See ownerHasKeyboardFocus().
        bool _owner_focused = false;

        //! Clears _opening if the project FAILS to load and the window therefore stays up.
        //! Parented to the card, so it cannot fire into a destroyed widget. Created on first use.
        QTimer* _opening_reset = nullptr;
    public:
        ProjectListItem(const ProjectListItemData& data, QWidget* parent);
        QSize minimumSizeHint() const override;
    protected:
        // Re-arms the height floor when the theme or the font changes underneath the card.
        void changeEvent(QEvent* event) override;

        // The four interaction states and the focus ring, composited over the sheet's fill.
        void paintEvent(QPaintEvent* event) override;

        // Hover in and out. Neither consumes anything: they only set state and repaint.
        void enterEvent(QEvent* event) override;
        void leaveEvent(QEvent* event) override;

        // All three call the base implementation, which ignores the event, so it still propagates
        // to the list viewport exactly as it did when this widget handled no mouse event at all.
        // Selection, opening and the context menu are untouched.
        void mousePressEvent(QMouseEvent* event) override;
        void mouseReleaseEvent(QMouseEvent* event) override;
        void mouseDoubleClickEvent(QMouseEvent* event) override;

        // The card is only reparented to the view's viewport once setItemWidget installs it, so
        // the first honest answer about the owning view's focus is available here and not in the
        // constructor.
        void showEvent(QShowEvent* event) override;
    private:
        QString toCamelCase(const QString& s);

        // The height the card's own content needs, before any allowance for the view's item
        // chrome. Used both for the height floor and as the base of the size hint.
        int contentMinimum() const;

        //! The one place the three interaction booleans become a single value.
        Interaction currentInteraction() const;

        //! Reads the dynamic property the sheet selects on. The card never WRITES it -- that
        //! belongs to RecentProjectsComponent's currentItemChanged handler -- so selection and
        //! interaction cannot fight over one another's storage.
        bool isSelectedCard() const;

        //! Walks from the card up to the top level looking for the widget that holds the focus.
        bool ownerHasKeyboardFocus() const;

        //! Recomputes _owner_focused and repaints only when the answer actually changed.
        void refreshOwnerFocus();

        //! Starts the single-shot that takes the card back out of the opening state.
        void armOpeningReset();
    };
}

#endif //NOGGIT_WIGDET_PROJECT_LIST_ITEM_HPP
