// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <QFont>
#include <QPixmap>
#include <QPoint>
#include <QString>
#include <QWidget>
#include <glm/vec3.hpp>

#include <cstdint>

class QEvent;

namespace math
{
  struct vector_3d;
}
class World;

//! \todo add adt coordinates/name on mouseover
namespace Noggit
{
  class Camera;

  namespace Ui
  {
    //! \todo Make this a fixed square somehow.
    class minimap_widget : public QWidget
    {
      Q_OBJECT

    public:
      minimap_widget (QWidget* parent = nullptr);

      virtual QSize sizeHint() const override;

      const World* world (World* const world_);
      const World* world() const;

      const bool& draw_skies (const bool& draw_skies_);
      const bool& draw_skies() const;

      const bool& draw_boundaries (const bool& draw_boundaries_);
      const bool& draw_boundaries() const;

      const std::vector<char>* use_selection (std::vector<char>* selection_);
      const std::vector<char>* selection() const;

      void camera (Noggit::Camera* camera);
      void set_resizeable(bool state);;

    protected:
      virtual void paintEvent (QPaintEvent* paint_event) override;
      virtual void changeEvent (QEvent* event) override;
      virtual void mouseDoubleClickEvent (QMouseEvent*) override;
      virtual void mouseMoveEvent(QMouseEvent*) override;
      virtual void mousePressEvent(QMouseEvent* event) override;
      virtual void mouseReleaseEvent(QMouseEvent* event) override;
      virtual void wheelEvent(QWheelEvent* event) override;

      QPoint locateTile(QMouseEvent* event);

      //! Rebuilds the cached "Select a map" font from the widget's current font. Called from
      //! the constructor and from changeEvent, never from the paint path.
      void updatePlaceholderFont();

    signals:
      void map_clicked(const glm::vec3&);
      void tile_clicked(const QPoint&);
      void reset_selection();

    private:
      World* _world;
      Noggit::Camera* _camera;
      std::vector<char>* _selected_tiles;

      bool _draw_skies;
      //! Was uninitialised, and paintEvent branches on it. Every current owner happens to set it
      //! before the first paint -- SpawnTilePicker.cpp:98 says so in a comment and calls
      //! draw_boundaries(true) partly for that reason -- but "happens to" is not a guarantee, and
      //! reading it before then was undefined. `_draw_camera` used to sit between these two: it
      //! was declared here and read nowhere in the tree, so it is gone rather than initialised.
      bool _draw_boundaries = false;
      bool _resizeable;

      bool _use_selection = false;
      bool _is_selecting = false;

      QFont _placeholder_font;

      //! The minimap image scaled to the square this widget paints into, kept across paints.
      //!
      //! The source is a 1024x1024 ARGB32 QImage (map_horizon.cpp:253), and drawing it through
      //! `drawImage(rect, image)` resampled all 1,048,576 pixels to the destination rect AND
      //! converted the non-premultiplied ARGB32 to the raster engine's premultiplied format, from
      //! scratch, on every paint -- and this widget repaints once per frame while the camera moves
      //! (MapView.cpp:5457) and once per mouse-move event while a selection drag is held. Nothing
      //! about the image changes between those paints.
      //!
      //! Invalidated on the source image's QImage::cacheKey(), on the destination size AND on the
      //! device pixel ratio. cacheKey() is a correct key here even though map_horizon edits the
      //! image in place: QImage::setPixel writes through the non-const scanLine(), which calls
      //! detach(), and detach() increments `detach_no` unconditionally -- and cacheKey() is
      //! `(ser_no << 32) | detach_no`. So an in-place edit does move the key. A missed key can
      //! only ever cost exactly what the old code cost on every single paint.
      //!
      //! The ratio is part of the key because it is not a constant: dragging the window from a
      //! 200% monitor to a 100% one changes devicePixelRatioF() without changing either the
      //! logical size or the source image, and without this third term the widget would keep
      //! serving the pixmap it built for the old screen.
      QPixmap _scaled_minimap;
      std::int64_t _scaled_minimap_key = 0;
      qreal _scaled_minimap_dpr = 0.0;

      //! Last tile the cursor was over, and the tooltip built for it. mouseMoveEvent used to build
      //! "ADT: x_y" through two std::to_string calls, a std::string concatenation and a
      //! QString::fromStdString on every mouse-move event; the text only changes when the cursor
      //! crosses a tile boundary. QToolTip::showText is still called on every event, so the tip's
      //! own reposition and hide timing are untouched.
      QPoint _last_hovered_tile {-1, -1};
      QString _hovered_tile_tooltip;
    };
  }
}
