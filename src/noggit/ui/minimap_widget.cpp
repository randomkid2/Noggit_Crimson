// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/Camera.hpp>
#include <noggit/Sky.h>
#include <noggit/ui/DesignTokens.hpp>
#include <noggit/ui/minimap_widget.hpp>
#include <noggit/World.h>

#include <QApplication>
#include <QEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QToolTip>

namespace
{
  // Caption rank, one step over the 12px interface size so it still reads when the pane is at
  // its 128px minimum, and far enough under a heading that it does not compete with one. The
  // family is NOT set here: updatePlaceholderFont takes the widget's own font, so the
  // placeholder follows the application font instead of naming a family this file cannot
  // guarantee is installed.
  int const PLACEHOLDER_FONT_PIXEL_SIZE = 13;

  // WHY EVERY MARKER ON THIS WIDGET IS DRAWN TWICE. NO EXCEPTIONS -- READ THIS BEFORE ADDING A
  // painter.setPen HERE.
  //
  // This is the one surface in the application where the design system's contrast tables do not
  // apply, because there is no surface token underneath: the markers are drawn over the map's
  // own minimap image, which spans the entire brightness range the game has. No single flat pen
  // survives that. Ratios below are WCAG 2.1, sRGB, (Lmax + 0.05) / (Lmin + 0.05), recomputed
  // for this comment rather than carried over from anywhere.
  //
  // The bounding case is not "snow" and "dark rock", it is white and black, because a minimap
  // tile can hold a specular highlight or an unlit cave mouth. Against BOTH extremes at once:
  //
  //                    on #FFFFFF   on #000000
  //     Qt::red           4.00          5.25     <- what the camera arrow used to be
  //     Qt::blue          8.59          2.44     <- what the sky spheres used to be
  //     #FFFF00           1.07         19.56     <- what the dirty-tile rectangle used to be
  //     ACCENT alone      2.20          9.56
  //     WARN alone        2.84          7.39
  //     INFO alone        2.40          8.76
  //     TEXT_DIM alone    1.99         10.57
  //
  // Every palette token fails on white, and by arithmetic that is unavoidable: a flat colour
  // clears 3:1 against black AND white only if its relative luminance lies in [0.100, 0.300],
  // and every token whose MEANING fits a marker here sits above that band (ACCENT 0.428,
  // WARN 0.319, INFO 0.388, TEXT_DIM 0.478). Swapping the pure primaries for palette tokens and
  // stopping there makes the markers WORSE on snow than the colours they replaced -- #FFFF00 to
  // WARN alone is 1.07 -> 2.84, still under the floor, and Qt::blue to INFO alone is 8.59 ->
  // 2.40, a token that PASSED on white turned into one that does not.
  //
  // So each marker is stroked in INK at MARKER_HALO_WIDTH first and then in its own token at
  // MARKER_PEN_WIDTH on top, and the legible mark is the PAIR. INK is 19.27:1 on white and the
  // token core is 7.39:1 to 10.57:1 on black, so whichever extreme the terrain reaches, exactly
  // one of the two strokes is far over the 3:1 graphical floor and neither extreme leaves both
  // weak. The marker also always reads against its own outline rather than against the map --
  // ACCENT on INK 8.77:1, WARN on INK 6.78:1, INFO on INK 8.04:1, TEXT_DIM on INK 9.70:1 -- and
  // not one of those four numbers moves with the terrain.
  //
  // Cost is one extra stroke per marker: one line for the camera, one rectangle for the global
  // WMO, one ellipse per sky, and one rectangle per dirty or selected tile. Sky counts are in
  // the tens; the tile loop is 64x64 but only marked tiles pay.
  //
  // Functions rather than namespace-scope QPen objects: a QPen built at static-initialisation
  // time runs before QApplication exists, and the ordering between translation units is not
  // defined. Both are trivial to construct and this widget already builds a QPainter per paint.
  constexpr qreal MARKER_PEN_WIDTH = 1.0;
  constexpr qreal MARKER_HALO_WIDTH = 3.0;

  QPen markerHaloPen (qreal width = MARKER_HALO_WIDTH)
  {
    return QPen (Noggit::Ui::Design::color (Noggit::Ui::Design::INK), width);
  }

  QPen markerPen (char const* token)
  {
    return QPen (Noggit::Ui::Design::color (token), MARKER_PEN_WIDTH);
  }

  // The tile markers are the one place the halo cannot simply be MARKER_HALO_WIDTH. They are
  // stroked on a path inset 1px inside a cell that is (min(w,h)/64) pixels across, so the cell
  // is 16px at this widget's 1024px maximum, 8px at its 512px sizeHint and 2px at its 128px
  // minimum. A 3px pen is centred on the path and therefore reaches 1.5px outside it, which is
  // half a pixel past the cell edge -- harmless at 8px and above, but at a 2px cell it would
  // paint a 3x3 ink blob over both neighbours and turn a sparse selection into a dark smear.
  // Clamped to the cell width, and floored at MARKER_PEN_WIDTH + 1 so there is always at least
  // half a pixel of ink showing on each side of the core and the halo never degenerates into
  // being completely overpainted by it.
  qreal tileMarkerHaloWidth (int tile_size)
  {
    return qBound (MARKER_PEN_WIDTH + 1.0, qreal (tile_size), MARKER_HALO_WIDTH);
  }
}

namespace Noggit
{
  namespace Ui
  {
    minimap_widget::minimap_widget (QWidget* parent)
      : QWidget (parent)
      , _world (nullptr)
      , _camera (nullptr)
      , _draw_skies (false)
      , _selected_tiles(nullptr)
      , _resizeable(false)
    {
      setSizePolicy (QSizePolicy::Preferred, QSizePolicy::Preferred);
      setMouseTracking(true);
      setMaximumSize(QSize(1024, 1024));
      setMinimumSize(QSize(128, 128));

      updatePlaceholderFont();



      connect(this, &minimap_widget::tile_clicked
        , [this](QPoint tile)
        {
          if (!_selected_tiles)
            return;

          if (QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier))
          {
            int x = tile.x() - 1;
            int y = tile.y() - 1;

            for (int i = 0; i < 3; ++i)
            {
              for (int j = 0; j < 3; ++j)
              {
                if (_world->mapIndex.hasTile(TileIndex(x + i, y + j)))
                {
                  (*_selected_tiles)[64 * (x + i) + (y + j)]
                    = !QApplication::keyboardModifiers().testFlag(Qt::ControlModifier);
                }

              }
            }
          }
          else
          {
            if (_world->mapIndex.hasTile(TileIndex(tile.x(), tile.y())))
            {
              (*_selected_tiles)[64 * tile.x() + tile.y()]
                = !QApplication::keyboardModifiers().testFlag(Qt::ControlModifier);
            }
          }

          update();
        }
      );

      connect(this, &minimap_widget::reset_selection
        , [this]()
        {
          if (!_selected_tiles)
            return;

          std::memset(_selected_tiles->data(), false, _selected_tiles->size());
        }
      );
    }

    void minimap_widget::updatePlaceholderFont()
    {
      _placeholder_font = font();
      _placeholder_font.setPixelSize (PLACEHOLDER_FONT_PIXEL_SIZE);
    }

    // The only event that can invalidate the cached font. It arrives when the application font
    // is set (ApplicationEntry does that at startup, after this widget may already exist) and
    // whenever a style sheet gives this widget or an ancestor a different one, so the cache
    // cannot go stale behind a theme change.
    void minimap_widget::changeEvent (QEvent* event)
    {
      if (event->type() == QEvent::FontChange)
      {
        updatePlaceholderFont();
      }

      QWidget::changeEvent (event);
    }


    void minimap_widget::wheelEvent(QWheelEvent* event)
    {
      if (!_resizeable)
        return;

      if (QApplication::keyboardModifiers().testFlag(Qt::ControlModifier))
      {
        const int degrees = event->angleDelta().y() / 8;
        int steps = degrees / 15;

        auto base_size = width();

        if (steps > 0)
        {
          auto new_size = std::min(std::max(128, base_size + 64), 4096);
          setFixedSize(new_size, new_size);
        }
        else
        {
          auto new_size = std::max(std::min(4096, base_size - 64), 128);
          setFixedSize(new_size, new_size);
        }

        event->ignore();
      }

    }

    QSize minimap_widget::sizeHint() const
    {
      return QSize (512, 512);
    }

    const World* minimap_widget::world(World* const world_)
    {
      _world = world_;
      update();
      return _world;
    }

    const World* minimap_widget::world() const
    {
      return _world;
    }

    const bool& minimap_widget::draw_skies(const bool& draw_skies_)
    {
      _draw_skies = draw_skies_;
      update();
      return _draw_skies;
    }

    const bool& minimap_widget::draw_skies() const
    {
      return _draw_skies;
    }

    const bool& minimap_widget::draw_boundaries(const bool& draw_boundaries_)
    {
      _draw_boundaries = draw_boundaries_;
      update();
      return _draw_boundaries;
    }

    const bool& minimap_widget::draw_boundaries() const
    {
      return _draw_boundaries;
    }

    const std::vector<char>* minimap_widget::use_selection(std::vector<char>* selection_)
    {
      _use_selection = selection_;
      _selected_tiles = selection_;
      update();
      return _selected_tiles;
    }

    const std::vector<char>* minimap_widget::selection() const
    {
      return _selected_tiles;
    }

    void minimap_widget::camera(Noggit::Camera* camera)
    {
      _camera = camera;
    }

    void minimap_widget::set_resizeable(bool state)
    {
      _resizeable = state;
    }

    //! \todo Only redraw stuff as told in event.
    // called by _minimap->update()
    // \todo : massive performance drop after clicking the minimap once until moving cursor out of frame, paintEvent gets called repeatidly
    void minimap_widget::paintEvent (QPaintEvent* paint_event)
    {
        /*
        auto rectangle = paint_event->rect();
        auto left = rectangle.left();
        auto rectop = rectangle.top();
        auto recwidth = rectangle.width();
        auto recheight = rectangle.height();
        */

      //! \note Only take multiples of 1.0 pixels per tile.
      const int smaller_side ((qMin (rect().width(), rect().height()) / 64) * 64);
      const QRect drawing_rect (0, 0, smaller_side, smaller_side);

      const int tile_size (smaller_side / 64);
      const qreal scale_factor (tile_size / TILESIZE);

      QPainter painter (this);
      painter.setRenderHints ( QPainter::Antialiasing
                             | QPainter::TextAntialiasing
                             | QPainter::SmoothPixmapTransform
                             );



      if (world())
      {
        painter.drawImage (drawing_rect, world()->horizon._qt_minimap);

        if (draw_boundaries())
        {
          //! \todo Draw non-existing tiles aswell?
          painter.setBrush (QColor (255, 255, 255, 30));
          for (int i (0); i < 64; ++i)
          {
            for (int j (0); j < 64; ++j)
            {
              TileIndex const tile (i, j);
              bool changed = false;

              if (world()->mapIndex.hasTile (tile))
              {
                if (world()->mapIndex.tileLoaded (tile))
                {
                  if (world()->mapIndex.has_unsaved_changes(tile))
                  {
                    changed = true;
                  }

                  painter.setPen(QColor::fromRgbF(0.f, 0.f, 0.f, 0.6f));
                }
                else if (world()->mapIndex.isTileExternal(tile))
                {
                  painter.setPen(QColor::fromRgbF(1.0f, 0.7f, 0.5f, 0.6f));
                }
                else
                {
                  painter.setPen (QColor::fromRgbF (0.8f, 0.8f, 0.8f, 0.4f));
                }
              }
              else
              {
                painter.setPen (QColor::fromRgbF (1.0f, 1.0f, 1.0f, 0.05f));
              }

              painter.drawRect ( QRect ( tile_size * i
                                       , tile_size * j
                                       , tile_size
                                       , tile_size
                                       )
                               );

              QRect const marker_rect ( tile_size * i + 1
                                      , tile_size * j + 1
                                      , tile_size - 2
                                      , tile_size - 2
                                      );

              if (changed)
              {
                // WARN. The design system names this exact state -- an edited tile that has not
                // been written out is `db.dirty`, and dirty is warn everywhere in the editor.
                // It was pure #FFFF00, which is not in the palette, is the single most
                // eye-grabbing colour available, and made an ordinary unsaved tile shout louder
                // than the selection did.
                //
                // Haloed like every other marker on this widget, and this is the marker where
                // skipping it does the most damage: #FFFF00 was 1.07:1 on white but WARN alone
                // is only 2.84:1, so a bare token pen leaves the ONE mark that says "you have
                // not saved this" under the 3:1 floor on any snowfield. Haloed it is 19.27:1
                // there and 7.39:1 on black.
                painter.setPen (markerHaloPen (tileMarkerHaloWidth (tile_size)));
                painter.drawRect (marker_rect);

                painter.setPen (markerPen (Design::WARN));
                painter.drawRect (marker_rect);
              }

              if (_use_selection && _selected_tiles->at(64 * i + j))
              {
                // ACCENT, matching every other selection in the application. This was pure
                // #FF0000 -- the same red as the camera arrow, the same red as the global-WMO
                // outline, and under the previous scheme the same red as "error" and "delete".
                // Selecting a tile is not a warning about it.
                //
                // Haloed: ACCENT alone is 2.20:1 on white, i.e. a bare token pen would hide the
                // selection on exactly the terrain #FF0000 was still visible on (4.00:1). The
                // pair is 19.27:1 on white and 9.56:1 on black.
                //
                // Drawn on the same path as the dirty marker above and therefore still covering
                // it when a tile is both, which is what the bare pens did too -- unchanged on
                // purpose.
                painter.setPen (markerHaloPen (tileMarkerHaloWidth (tile_size)));
                painter.drawRect (marker_rect);

                painter.setPen (markerPen (Design::ACCENT));
                painter.drawRect (marker_rect);
              }
            }
          }
        }

        if (draw_skies() && _world->renderer()->skies())
        {
          foreach (Sky sky, _world->renderer()->skies()->skies)
          {
            //! \todo Get actual color from sky.
            //! \todo Get actual radius.
            //! \todo Inner and outer radius?

            // Halo first, marker second -- see markerHaloPen for why every marker on this
            // widget is drawn twice. INFO rather than the Qt::blue this was: pure #0000FF
            // measures 2.44:1 against black, so a sky sphere over anywhere that was not snow
            // was effectively invisible. The haloed pair is 19.27:1 on white, 8.76:1 on black.
            painter.setPen (markerHaloPen());
            painter.drawEllipse ( QPointF ( sky.pos.x * scale_factor
                                          , sky.pos.z * scale_factor
                                          )
                                , 10.0 // radius
                                , 10.0
                                );

            painter.setPen (markerPen (Design::INFO));
            painter.drawEllipse ( QPointF ( sky.pos.x * scale_factor
                                          , sky.pos.z * scale_factor
                                          )
                                , 10.0 // radius
                                , 10.0
                                );
          }
        }

        if (_camera)
        {

          // hackfix
          auto yaw = _camera->yaw();
          yaw._ -= 90.0f;

          while (yaw._ < -180.0f)
              yaw._ += 360.0f;

          QLineF camera_vector ( QPointF ( _camera->position.x * scale_factor
                                         , _camera->position.z * scale_factor
                                         )
                               , QPointF ( _camera->position.x * scale_factor
                                         , _camera->position.z * scale_factor
                                         )
                               + QPointF ( glm::cos(math::radians(yaw)._) * scale_factor
                                         , -glm::sin(math::radians(yaw)._) * scale_factor
                                         )
                               );
          camera_vector.setLength (15.0);

          // ACCENT, because the camera arrow is the one mark on this widget that says where YOU
          // are -- the design system's accent means "the thing you are acting on" and nothing
          // else on a minimap qualifies. It was Qt::red, i.e. pure #FF0000, which under the old
          // scheme was also the selection colour, the error colour and the global-WMO outline
          // below, so four unrelated things on one widget were the same red. Haloed pair:
          // 19.27:1 on white, 9.56:1 on black.
          painter.setPen (markerHaloPen());
          painter.drawLine (camera_vector);

          painter.setPen (markerPen (Design::ACCENT));
          painter.drawLine (camera_vector);
        }

        if (_world->mapIndex.hasAGlobalWMO())
        {
            // TEXT_DIM: this rectangle is the extent of the map's single global WMO, which is
            // structure rather than status, and it is deliberately the quietest of the three
            // markers so it cannot compete with the camera. It was pure red too.
            //
            // Quietest does NOT mean unhaloed. TEXT_DIM is the lightest token used as a marker
            // here and so the worst of them on white -- 1.99:1 alone, under the floor and below
            // even the #FF0000 it replaced (4.00:1). Haloed the mark is 19.27:1 on white and
            // 10.57:1 on black. The pens are set below, once the rectangle is known, so the
            // halo and the core are stroked on one shared path.
            auto extents = _world->mWmoEntry.extents;

            // WMOInstance inst(_world->mWmoFilename, &_world->mWmoEntry, _world->_context);

            float pos = tile_size * 64 / 2; // TODO : convert wmo pos 

            float min_point_x = pos + (extents[0][0] / TILESIZE * tile_size); // extents[min][x]
            float min_point_y = pos + (extents[0][1] / TILESIZE * tile_size); // extents[min][y]
            float max_point_x = pos + (extents[1][0] / TILESIZE * tile_size);
            float max_point_y = pos + (extents[1][1] / TILESIZE * tile_size);
            // tile_size = 14 | max size = 896

            float width = max_point_x - min_point_x;
            float height = max_point_y - min_point_y;

            QRectF const wmo_rect (min_point_x
                , min_point_y
                , width // width
                , height // height
            );

            painter.setPen (markerHaloPen());
            painter.drawRect (wmo_rect);

            painter.setPen (markerPen (Design::TEXT_DIM));
            painter.drawRect (wmo_rect);
        }
}
      else
      {
        //! \todo Draw something so user realizes this will become the minimap.

        // A placeholder, not a headline. This used to be QFont("Arial", 30) in WindowText: a
        // hardcoded family at 30 POINTS, in the loudest text role the palette has, filling an
        // otherwise empty pane and reading as the most important thing on the window. It was
        // also too big to say what it says. Measured with QFontMetrics under a probe: "Select
        // a map" at Arial 30pt is 348x67 px, while this widget has a 128px minimum and the
        // square it draws into is (min(w,h)/64)*64. AlignCenter with no wrap flag clips rather
        // than shrinks, so at 128px AND at 256px the pane read "elect a ma" -- a placeholder
        // that was itself unreadable at every size below 348px.
        //
        // It is now the interface font at the caption size (75x17 px, fits from the 128px
        // minimum up) in PlaceholderText, which darkPalette() sets to the design system's
        // TEXT_DIM token #BFB7AA -- 9.70:1 on the BG_VOID #100E0B the minimap holder is
        // painted, 7.58:1 on the BG_PANEL #292621 the other hosts sit on. Both clear the 7:1
        // body floor with room, and both are deliberately under the body-text RANK, because
        // the whole point is that it recedes once a map is chosen. (Under the previous
        // near-neutral scheme the same two readings were 5.72:1 and 5.38:1 -- legal, but only
        // just, and on a surface ladder whose four levels spanned 1.321:1 in total.)
        //
        // The font is a member rather than a temporary: paintEvent runs on every mouse move
        // over this widget (see the performance \todo above), and QFont construction resolves
        // a family through the font database. It is rebuilt in changeEvent instead, which is
        // the one place the widget's font can actually change.
        painter.setPen (palette().color (QPalette::PlaceholderText));
        painter.setFont (_placeholder_font);
        painter.drawText ( drawing_rect
                         , Qt::AlignCenter
                         , tr ("Select a map")
                         );
      }
    }

    QPoint minimap_widget::locateTile(QMouseEvent* event)
    {
      const int smaller_side ((qMin (rect().width(), rect().height()) / 64) * 64);
      const int tile_size (smaller_side / 64);
      //! \note event->pos() / tile_size seems to be using floating point arithmetic, therefore getting wrong results.
      const QPoint tile ( event->pos().x() / float(tile_size)
          , event->pos().y() / float(tile_size)
      );

      return tile;
    }

    void minimap_widget::mouseDoubleClickEvent (QMouseEvent* event)
    {
      if (event->button() != Qt::LeftButton || !_world)
      {
        event->ignore();
        return;
      }

      QPoint tile = locateTile(event);

      if (!world()->mapIndex.hasTile (TileIndex (tile.x(), tile.y())) && !_world->mapIndex.hasAGlobalWMO())
      {
        event->ignore();
        return;
      }

      event->accept();

      emit map_clicked(::glm::vec3 ( tile.x() * TILESIZE + TILESIZE / 2
                                         , 0.0f, tile.y() * TILESIZE + TILESIZE / 2));
    }

    void minimap_widget::mousePressEvent(QMouseEvent* event)
    {
      if (!_world)
      {
        event->ignore();
        return;
      }


      if (event->button() == Qt::RightButton)
      {
        _is_selecting = false;
        emit reset_selection();

        return;
      }

      QPoint tile = locateTile(event);
      emit tile_clicked(tile);
      _is_selecting = true;

      update();
    }

    void minimap_widget::mouseReleaseEvent(QMouseEvent* event)
    {

      if (!_world)
      {
        event->ignore();
        return;
      }

      _is_selecting = false;
      update();
    }

    void minimap_widget::mouseMoveEvent(QMouseEvent* event)
    {
      if (!_world)
      {
        event->ignore();
        return;
      }

      QPoint tile = locateTile(event);

      std::string str("ADT: " + std::to_string(tile.x()) + "_" + std::to_string(tile.y()));

      QToolTip::showText(mapToGlobal(QPoint(event->pos().x(), event->pos().y() + 5)), QString::fromStdString(str));

      if (_is_selecting)
      {
        emit tile_clicked(tile);
      }

      // update();
    }
  }
}
