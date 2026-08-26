// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/ClickableLabel.hpp>

namespace Noggit
{
  namespace Ui
  {
    ClickableLabel::ClickableLabel(QWidget * parent) : QLabel(parent)
    {
      // NOTHING IN THIS CLASS WAS EVER DISCOVERABLE, and the fix has to be made from C++.
      //
      // A QLabel has no cursor of its own -- it takes the arrow from its parent -- and it is
      // not a QAbstractButton, so Qt never sets the :hover pseudo-state on one. There was
      // therefore no selector a style sheet could have used: `Noggit--Ui--ClickableLabel:hover`
      // would simply never have matched. That is why seven of the biggest click targets in the
      // editor -- the 128x128 texture swatch, the four TexturePicker chunk-layer tiles, and the
      // brush-mask thumbnail in three separate tools -- looked exactly like decoration until you
      // clicked one. setCursor(Qt::PointingHandCursor) appears three times in the whole tree
      // outside this class and all three are in windows a user sees BEFORE the 3D view exists.
      //
      // WHAT WAS REMOVED, AND WHY IT IS A REMOVAL RATHER THAN A COMPLETION. This constructor
      // also set Qt::WA_Hover and published a "hovered" dynamic property, and enterEvent /
      // leaveEvent re-published it through style()->unpolish() + style()->polish(). The intent
      // was for the theme to carry the colour half with
      // `Noggit--Ui--ClickableLabel[hovered="true"]`. That rule was never written. Checked
      // across the whole tree: `[hovered` matches nothing in dist/noggit-themes/CrimsonSlate/
      // theme.qss, nothing in dist/themes/Dark or dist/themes/McNet, and no C++ outside the
      // three lines that wrote it ever read it back. So the property selected nothing and the
      // repolish it triggered changed nothing on screen -- while costing a full style
      // re-resolution (cache eviction, unsetPalette, WA_StyleSheet toggle, setGeometry,
      // setProperties, repaint) on a widget holding a 128x128 pixmap, once per pointer crossing.
      //
      // It is removed rather than finished because the theme is the wrong place for the other
      // half anyway: a hover tint on the current-texture swatch would sit UNDER the pixmap that
      // fills it and be invisible on exactly the widget the affordance is for. The cursor is
      // the affordance, it works, and it is the part that landed.
      setCursor(Qt::PointingHandCursor);
    }

    void ClickableLabel::mouseReleaseEvent (QMouseEvent* event)
    {
        emit clicked();

        if (event->button() == Qt::MiddleButton)
            emit middleClicked();

        if (event->button() == Qt::LeftButton)
            emit leftClicked();

        if (event->button() == Qt::RightButton)
            emit rightClicked();
    }
  }
}
