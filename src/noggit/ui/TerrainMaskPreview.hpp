// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_TERRAINMASKPREVIEW_HPP
#define NOGGIT_UI_TERRAINMASKPREVIEW_HPP

#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtWidgets/QWidget>

#include <glm/vec3.hpp>

#include <string>

class MapView;

class QTimer;

namespace Noggit::Ui
{
  // A stable colour for one mask NAME, so that "which mask am I painting" has an answer the eye
  // can read without moving off the viewport.
  //
  // Derived from the name rather than stored on the mask, which means it needs no field in the
  // sidecar format, survives a save and reload, and is the same colour in the preview, on the
  // panel's swatch and on the brush ring in the 3D view. Two masks can collide on a hue; the name
  // is printed next to the swatch for exactly that reason. Renaming a mask changes its colour,
  // which is a fair trade for not versioning the file format to store a preference.
  QColor maskIdentityColor(std::string const& name);

  // THE ANSWER TO "A MASK YOU CANNOT SEE IS UNPAINTABLE IN PRACTICE".
  //
  // A top-down loupe on the active mask, centred on the terrain cursor and drawn at the mask's
  // own resolution, with the brush's outer and inner radius on it. The brush ring in the 3D
  // viewport says WHERE the stroke will land; this says WHAT IS ALREADY THERE, which is the half
  // the viewport cannot show without a terrain overlay pass -- and a terrain overlay pass needs a
  // per-chunk texture upload and a shader branch in rendering/, which this round does not own.
  //
  // WHY A LOUPE AND NOT A MAP. The window is CURSOR-CENTRED, not camera-centred or tile-aligned.
  // A mask is painted at brush scale: at the default 15 yd radius a whole 533.33 yd tile drawn
  // into 224 px puts the brush at 12.6 px across, which is not a picture of anything. Following
  // the cursor keeps the stroke at a usable size, and it costs a view that slides -- which is the
  // correct trade, because the thing being looked at is the stroke, not the geography.
  //
  // ORIENTATION MATCHES THE MINIMAP: screen X is world X and screen Y DOWNWARD is world Z. That
  // is the mapping minimap_widget uses when it turns a click back into a position
  // (minimap_widget.cpp:749-750), and having the two disagree would be worse than either choice.
  class TerrainMaskPreview : public QWidget
  {
    public:
      explicit TerrainMaskPreview(MapView* map_view, QWidget* parent = nullptr);

      // Side of the square window, in yards.
      void setWindowYards(float yards);

      // The brush, so the rings drawn on the loupe are the ones the stroke will use.
      void setBrush(float radius, float hardness);

      // Draw the paint layer alone instead of the composited field. The paint layer is what the
      // brush actually writes and what survives a rebake; the composite is what the brushes read.
      // Being able to see them apart is what tells a user whether a region is theirs or the
      // filter stack's.
      void setShowPaintOnly(bool paint_only);

      // Something painted. Forces the next timer tick to repaint even if the cursor has not
      // moved, which is the case of a brush held still over one spot.
      void markDirty();

      // Mask value under the cursor, in [0, 1], as of the last repaint. -1 when there is nothing
      // to report: no mask selected, or a cursor that is not on the map.
      float valueUnderCursor() const;

      QSize sizeHint() const override;

    protected:
      void paintEvent(QPaintEvent* event) override;
      void showEvent(QShowEvent* event) override;
      void hideEvent(QHideEvent* event) override;

    private:
      // Repaints only when the cursor moved or something painted, so a panel left open on a still
      // camera costs one float comparison per tick instead of 36'864 field samples.
      void refreshIfChanged();

      MapView* _map_view;

      QTimer* _timer;

      float _window_yards = 128.0f;
      float _brush_radius = 15.0f;
      float _brush_hardness = 0.5f;
      bool _paint_only = false;

      bool _dirty = true;
      glm::vec3 _last_cursor = {0.0f, 0.0f, 0.0f};

      float _value_under_cursor = -1.0f;

      // Reused between repaints so a 15 Hz refresh does not allocate 144 KiB fifteen times a
      // second. Resized only when the sample resolution changes.
      QImage _field;
  };
}

#endif // NOGGIT_UI_TERRAINMASKPREVIEW_HPP
