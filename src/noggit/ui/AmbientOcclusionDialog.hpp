// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_AMBIENTOCCLUSIONDIALOG_HPP
#define NOGGIT_UI_AMBIENTOCCLUSIONDIALOG_HPP

#include <noggit/terrain/AmbientOcclusion.hpp>

#include <QtWidgets/QDialog>

#include <cstddef>
#include <vector>

class MapTile;
class MapView;

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressDialog;
class QSpinBox;

namespace Noggit::Ui
{
  // Bakes horizon-sampled ambient occlusion into terrain vertex colour (MCCV).
  //
  // Everything numerical lives in noggit/terrain/AmbientOcclusion.hpp, which is deliberately free of Qt,
  // glm and MapChunk so it can be unit tested standalone. This class is the other half: scope
  // selection, the undo action, progress, and the one hazard the pure layer cannot see -- that
  // MCCV is frequently hand-painted and that a bake can destroy it.
  //
  // A QDialog rather than a Tool, for the same reason GroundEffectSetEditor is: editing_mode::mccv
  // already belongs to VertexPainterTool and ShaderTool. This is a one-shot bulk operation with a
  // parameter sheet, not a brush.
  class AmbientOcclusionDialog : public QDialog
  {
    Q_OBJECT

    public:
      explicit AmbientOcclusionDialog(MapView* map_view, QWidget* parent = nullptr);

    private:
      // Widget state -> the pure layer's settings structs, already sanitized so the margin the
      // height field is built with is the radius the estimator will actually search.
      Noggit::AmbientOcclusionSettings aoSettings() const;
      Noggit::AmbientOcclusionColorSettings colorSettings() const;
      Noggit::AoBlendMode blendMode() const;

      // Loaded, fully parsed tiles the current scope selects. Never contains a tile the async
      // loader is still filling in -- addTileToField and bakeChunk both read mVertices.
      std::vector<MapTile*> tilesInScope() const;

      // Counts chunks that already carry a vertex colour layer, so the user is told what a bake is
      // about to land on top of before they press it rather than after.
      void onScanScope();

      void onBake();

      // What one tile's pass produced.
      //
      // chunks_snapshotted counts chunks handed to an undo action, which is a different question
      // from how many were written: the snapshot is taken before the bake and is kept even when
      // the bake decides the chunk needs no change. It is the count that decides whether the tile
      // must stay marked unsaved, because an undo action holding a MapChunk* of a tile that is
      // free to unload is a dangling pointer.
      struct TileBakeResult
      {
        std::size_t chunks_baked = 0;
        std::size_t chunks_snapshotted = 0;
      };

      // One tile: build the height field (plus loaded neighbours for margin), then bake its 256
      // chunks, registering each chunk's previous colour with an undo action first.
      //
      // Opens that action lazily, on the first chunk it actually snapshots, and does NOT close it
      // -- the caller does, on every path including the throwing one. That is what keeps one tile
      // to one undo step without leaving an empty action behind for a tile that had no chunk data.
      //
      // Accumulates into `result` rather than returning it, so a throw partway through still
      // reports what was snapshotted before the throw. Sets `cancelled` when the user pressed
      // Cancel.
      void bakeOneTile( MapTile* tile
                      , Noggit::AmbientOcclusionSettings const& ao_settings
                      , Noggit::AmbientOcclusionColorSettings const& color_settings
                      , Noggit::AoBlendMode blend
                      , QProgressDialog& progress
                      , int& progress_value
                      , bool& cancelled
                      , TileBakeResult& result
                      );

      MapView* _map_view;

      QComboBox* _scope;
      QComboBox* _blend_mode;
      QLabel* _blend_explainer;

      QSpinBox* _direction_count;
      QSpinBox* _step_count;
      QDoubleSpinBox* _max_radius;
      QDoubleSpinBox* _bias_degrees;
      QCheckBox* _cosine_weighted;

      QDoubleSpinBox* _strength;
      QDoubleSpinBox* _open_factor;
      QDoubleSpinBox* _occluded_factor;
      QDoubleSpinBox* _contrast;
      // Indexed r, g, b. Three spin boxes rather than a colour picker because the pure layer's
      // domain is three independent floats in [0, 1] and a QColor round trip would hide that.
      QDoubleSpinBox* _shadow_tint[3];

      QCheckBox* _use_neighbours;

      QLabel* _status;
  };
}

#endif
