// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/CurrentTexture.h>
#include <noggit/TextureManager.h>

#include <QtGui/QMouseEvent>
#include <QtWidgets/QApplication>
#include <QtGui/QDrag>
#include <QtGui/QPainter>
#include <QMimeData>

namespace Noggit
{
  namespace Ui
  {
    current_texture::current_texture(bool accept_drop, QWidget* parent)
      : ClickableLabel (parent)
      , _filename("tileset\\generic\\black.blp")
      , _need_update(true)
      , _is_selected(false)
      , _is_swap_selected(false)
    {
      QSizePolicy policy (QSizePolicy::Fixed, QSizePolicy::Fixed);
      setSizePolicy (policy);
      setMinimumSize(SWATCH_EXTENT, SWATCH_EXTENT);
      setAcceptDrops(accept_drop);

      update_texture_if_needed();
    }

    QSize current_texture::sizeHint() const
    {
      return QSize(SWATCH_EXTENT, SWATCH_EXTENT);
    }

    int current_texture::heightForWidth (int width) const
    {
      return width;
    }

    void current_texture::set_texture (std::string const& texture)
    {
      _filename = texture;
      _need_update = true;
      update_texture_if_needed();

      emit texture_updated();
    }

    QImage current_texture::createBorder(const QColor &color)
    {
        QImage _border(QSize(SWATCH_EXTENT, SWATCH_EXTENT), QImage::Format_ARGB32);
        _border.fill(qRgba(0,0,0,0));

        // Ring 0 is the OUTERMOST pixel row of the swatch and ring _border_size - 1 the
        // innermost, so the loop runs outward-in: the first BORDER_CORE rings carry the state
        // colour, which is the half that faces the panel, and the last BORDER_INNER ring is INK,
        // which is the half that faces the texture. The reasoning and every ratio are on the
        // declarations in CurrentTexture.h; the only thing this loop has to get right is the
        // order, because putting INK on the outside would spend it against a surface it
        // measures 1.279:1 on.
        //
        // Each ring is drawn only across ITS OWN span, n from i to far_edge, rather than the
        // full width. That is new and it is not a tidy-up: with one flat colour the four passes
        // could overlap at the corners harmlessly, but with two colours the inner INK ring's
        // full-width pass would have overwritten the first BORDER_CORE pixels of every corner
        // and left four ink stubs across the state colour. Spanning each ring to itself mitres
        // the frame instead.
        //
        // The saving is 4 * b * (b - 1) setPixelColor calls, where b is _border_size -- 48 of
        // them here, not the "2 * _border_size^2" (32) this comment claimed until it was
        // checked. Derivation, with E = SWATCH_EXTENT: the old loop ran n across all E columns
        // for each of the b rings, four writes per step, so 4 * E * b = 4 * 128 * 4 = 2048. The
        // new one runs n from i to E - 1 - i, which is E - 2i steps, so the total is
        // sum(4 * (E - 2i), i = 0 .. b-1) = 4 * E * b - 4 * b * (b - 1) = 2000. 2048 - 2000 = 48.
        //
        // The saving is small because it is not the point: the SET of pixels touched is the same
        // 1984 either way (E^2 - (E - 2b)^2 = 16384 - 14400), and all that changes is which pass
        // owns each corner pixel. Correctness is the reason; the 48 is a footnote.
        for (int i = 0; i < _border_size; ++i)
        {
            QColor const& ring (i < BORDER_CORE ? color : _border_inner);
            // NOT named 'far': <windows.h> still defines 'far' and 'near' as empty macros for
            // 16-bit source compatibility, so 'int const far (...)' preprocesses to
            // 'int const (...)' and the loop condition loses its operand.
            int const far_edge (SWATCH_EXTENT - 1 - i);

            for (int n = i; n <= far_edge; ++n)
            {
                _border.setPixelColor(QPoint(n, i), ring);
                _border.setPixelColor(QPoint(n, far_edge), ring);
                _border.setPixelColor(QPoint(i, n), ring);
                _border.setPixelColor(QPoint(far_edge, n), ring);
            }
        }

        return _border;
    }

    void current_texture::update_texture_if_needed()
    {
      if (!_need_update)
      {
        return;
      }

      _need_update = false;

      show();
      _texture_save = *BLPRenderer::getInstance().render_blp_to_pixmap(_filename, SWATCH_EXTENT, SWATCH_EXTENT);
      setPixmap (_texture_save);
      setToolTip(QString::fromStdString(_filename));
    }

    void current_texture::mousePressEvent(QMouseEvent* event)
    {
      if (event->button() == Qt::LeftButton)
      {
        _start_pos = event->pos();
      }

      ClickableLabel::mousePressEvent(event);
    }

    void current_texture::mouseMoveEvent(QMouseEvent* event)
    {
      ClickableLabel::mouseMoveEvent(event);

      if (!(event->buttons() & Qt::LeftButton))
      {
        return;
      }

      int drag_dist = (event->pos() - _start_pos).manhattanLength();

      if (drag_dist < QApplication::startDragDistance())
      {
        return;
      }

      QMimeData* mimeData = new QMimeData;
      mimeData->setText(QString(_filename.c_str()));

      QDrag* drag = new QDrag(this);
      drag->setMimeData(mimeData);
      QPixmap pm = pixmap(Qt::ReturnByValueConstant());
      drag->setPixmap(pm);
      drag->exec();
    }

    void current_texture::dragEnterEvent(QDragEnterEvent* event)
    {
      if (event->mimeData()->hasText())
      {
        event->accept();
      }
    }

    void current_texture::dropEvent(QDropEvent* event)
    {
      std::string filename = event->mimeData()->text().toStdString();

      set_texture(filename);
      emit texture_dropped(filename);
    }

    void current_texture::unselect()
    {
        if (_is_swap_selected)
            return;

         setPixmap(_texture_save);
        _is_selected = false;
    }

    void current_texture::select()
    {
        QPixmap _new_pixmap = _texture_save;
        QPainter _painter(&_new_pixmap);
        _painter.drawImage(QPoint(0,0), createBorder(_border_color));
        setPixmap(_new_pixmap);
        _is_selected = true;
    }

    void current_texture::unselectSwap()
    {
        if (_is_selected)
            return;

        setPixmap(_texture_save);
        _is_swap_selected = false;
    }

    void current_texture::selectSwap()
    {
        QPixmap _new_pixmap = _texture_save;
        QPainter _painter(&_new_pixmap);
        _painter.drawImage(QPoint(0,0), createBorder(_border_swap_color));
        setPixmap(_new_pixmap);
        _is_swap_selected = true;
    }
  }
}
