// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_CURRENTTEXTURE_H
#define NOGGIT_UI_CURRENTTEXTURE_H

#include <noggit/ui/ClickableLabel.hpp>
#include <noggit/ui/DesignTokens.hpp>

#include <QtGui/QColor>
#include <QtWidgets/QWidget>

#include <string>


class QMouseEvent;
class QDropEvent;
class QDragEnterEvent;

namespace Noggit
{
  namespace Ui
  {
    class texture_swapper;

    class current_texture : public ClickableLabel
    {
      Q_OBJECT

    public:
        bool _is_selected;
        bool _is_swap_selected;

    private:
      std::string _filename;
      bool _need_update;
      QPixmap _texture_save;

      // THE SELECTION BORDER, and why it is a two-tone band rather than a 4px line.
      //
      // This border is painted into the pixmap by createBorder, so no style sheet can reach
      // either colour, and both were off-palette: #5280B9 for selected and #FCBA03 for the swap
      // target. #FCBA03 is 3.8 degrees of hue and 1.273:1 of luminance from ACCENT #DFA52E --
      // it is the accent, misspelled -- and it measured 1.726:1 on a white texture, i.e. there
      // was no swap marker at all on snow or sand.
      //
      // The band has TWO neighbours and they are nothing like each other. Outward it abuts the
      // panel the swatch sits on, which is a known surface: ACCENT on bg.panel #292621 is
      // 6.860:1 and INFO 6.290:1, both far over the 3:1 graphical floor. Inward it abuts an
      // arbitrary BLP, which spans white to black, and NO flat colour survives that -- a flat
      // band is exactly 1.000:1 against a texture of its own luminance, which is why the old
      // 4px line could vanish completely. So the innermost pixel row is INK: for any texture
      // colour whatever, the better of the two measures at least 2.962:1 with ACCENT and
      // 2.836:1 with INFO, and that worst case occurs at one exact luminance (about #5B5B5B);
      // at the extremes it is 19.272:1 (INK on white) and 9.559:1 (ACCENT on black). Three
      // times the floor of the line it replaces, at the price of one pixel.
      //
      // WHICH COLOUR MEANS WHAT CHANGED, and it is the design system's rule rather than taste:
      // ACCENT means "the thing you are acting on", and the texture you are painting with is
      // that thing, so selection is now gold and the swap target is INFO. It used to be the
      // other way round.
      //
      // HONEST LIMIT, measured: ACCENT and INFO are 165.0 degrees apart in hue but only 1.091:1
      // in luminance, where the pair they replace was 2.363:1. TexturePicker can show both at
      // once on two different chunk-layer tiles, so those two tiles are told apart by hue and
      // not by lightness. Gold against blue is the one pair that survives protanopia and
      // deuteranopia, and no other palette token both carries a meaning that fits and sits far
      // enough from ACCENT in luminance; INK is the only thing here that adds separation, and
      // it is spent on the texture side where a flat colour has no floor at all.
      static constexpr int SWATCH_EXTENT = 128;
      static constexpr int BORDER_CORE = 3;
      static constexpr int BORDER_INNER = 1;

      const int _border_size = BORDER_CORE + BORDER_INNER;
      const QColor _border_color = Design::color (Design::ACCENT);
      const QColor _border_swap_color = Design::color (Design::INFO);
      const QColor _border_inner = Design::color (Design::INK);

      QImage createBorder(const QColor& color);

      virtual void resizeEvent (QResizeEvent*) override
      {
        update_texture_if_needed();
      }
      void update_texture_if_needed();

      virtual int heightForWidth (int) const override;

      QSize sizeHint() const override;

      QPoint _start_pos;

      signals:
        void texture_dropped(std::string const& filename);

        void texture_updated();


    public:
      current_texture(bool accept_drop, QWidget* parent = nullptr);

      std::string const& filename() { return _filename; };

      void set_texture (std::string const& texture);
      void unselect();
      void select();
      void unselectSwap();
      void selectSwap();

      void mouseMoveEvent(QMouseEvent* event) override;
      void mousePressEvent(QMouseEvent* event) override;

      void dragEnterEvent(QDragEnterEvent* event) override;
      void dropEvent(QDropEvent* event) override;
    };
  }
}

#endif // NOGGIT_UI_CURRENTTEXTURE_H
