// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_WINDOWS_PROJECTSELECTION_WIDGETS_BRANDBANNER_HPP
#define NOGGIT_UI_WINDOWS_PROJECTSELECTION_WIDGETS_BRANDBANNER_HPP

#include <QColor>
#include <QPixmap>
#include <QSize>
#include <QWidget>

class QPaintEvent;

namespace Noggit::Ui::Widget
{
  // The band across the head of the project-selection window: the product mark, the two-line
  // wordmark, a seeded topographic texture and the rule that closes it.
  //
  // THIS IS THE BRAND RANK AND IT LIVES ON THIS WINDOW ONLY. Crimson is the product's identity
  // colour; gold is the application's accent and means "the thing you are acting on". The two do
  // not mix, and nothing in this file is reachable from the editor's chrome. The rule that keeps
  // it honest is written out in full at the head of the launcher section of theme.qss.
  //
  // WHY THE WORDMARK IS PAINTED RATHER THAN BEING A PAIR OF QLabels. Two things the mockup asks
  // for are not expressible in a Qt style sheet, and one of them is not expressible in a QLabel
  // either:
  //
  //   * LETTERSPACING. Qt style sheets have no letter-spacing property -- check_qss.py's property
  //     list is the Qt 5.15 reference list and does not contain one, so writing it would be
  //     silently dropped. It has to be QFont::setLetterSpacing.
  //   * TEXT-TRANSFORM. There is none either, so the capitals live in the string.
  //   * THE METALLIC RAMP. A QLabel draws its text with a plain pen colour. A gradient FILL on
  //     glyphs needs QPen(QBrush(QLinearGradient)), and only a QPainter has one.
  //
  // Painting the wordmark also removes the question of whether a font attribute survives the
  // style sheet's font resolution, because no style-sheet font resolution happens in a
  // paintEvent at all.
  //
  // ACCESSIBILITY COST, and how it is paid. Painted text is invisible to a screen reader, so the
  // widget carries an accessible name that reads the wordmark out.
  class BrandBanner : public QWidget
  {
    Q_OBJECT

    // The theme owns every colour here. A QPainter would otherwise be the one place in the
    // application where the palette is unreachable, and a theme that changed the surfaces would
    // leave this band behind. qproperty- resolves ONCE, at polish time, which is before the first
    // paint -- so the setters only have to drop the cached texture, never force a repaint chain.
    Q_PROPERTY (QColor bandLeft READ bandLeft WRITE setBandLeft)
    Q_PROPERTY (QColor bandRight READ bandRight WRITE setBandRight)
    Q_PROPERTY (QColor contourInk READ contourInk WRITE setContourInk)
    Q_PROPERTY (QColor ruleInk READ ruleInk WRITE setRuleInk)
    Q_PROPERTY (QColor wordmarkTop READ wordmarkTop WRITE setWordmarkTop)
    Q_PROPERTY (QColor wordmarkMid READ wordmarkMid WRITE setWordmarkMid)
    Q_PROPERTY (QColor wordmarkBottom READ wordmarkBottom WRITE setWordmarkBottom)
    Q_PROPERTY (QColor wordmarkAccent READ wordmarkAccent WRITE setWordmarkAccent)

  public:
    explicit BrandBanner (QWidget* parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    QColor bandLeft() const { return _band_left; }
    QColor bandRight() const { return _band_right; }
    QColor contourInk() const { return _contour_ink; }
    QColor ruleInk() const { return _rule_ink; }
    QColor wordmarkTop() const { return _wordmark_top; }
    QColor wordmarkMid() const { return _wordmark_mid; }
    QColor wordmarkBottom() const { return _wordmark_bottom; }
    QColor wordmarkAccent() const { return _wordmark_accent; }

    void setBandLeft (QColor const& colour);
    void setBandRight (QColor const& colour);
    void setContourInk (QColor const& colour);
    void setRuleInk (QColor const& colour);
    void setWordmarkTop (QColor const& colour);
    void setWordmarkMid (QColor const& colour);
    void setWordmarkBottom (QColor const& colour);
    void setWordmarkAccent (QColor const& colour);

  protected:
    void paintEvent (QPaintEvent* event) override;

  private:
    //! Drops the cached texture and asks for a repaint. Cheap: the texture is rebuilt lazily on
    //! the next paint, so a burst of property writes during polish costs one rebuild, not eight.
    void invalidate();

    QColor _band_left;
    QColor _band_right;
    QColor _contour_ink;
    QColor _rule_ink;
    QColor _wordmark_top;
    QColor _wordmark_mid;
    QColor _wordmark_bottom;
    QColor _wordmark_accent;

    //! Cached at the widget's current logical size AND device pixel ratio. Both are checked at
    //! paint time rather than being tracked through resizeEvent and a screen-change event: a
    //! window dragged to a screen with a different ratio does not always deliver an event this
    //! widget can see, but it always repaints, and comparing two numbers is cheaper than being
    //! wrong.
    QPixmap _texture;
    QPixmap _mark;
  };
}

#endif // NOGGIT_UI_WINDOWS_PROJECTSELECTION_WIDGETS_BRANDBANNER_HPP
