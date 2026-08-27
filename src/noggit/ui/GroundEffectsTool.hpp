// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_GROUNDEFFECTSTOOL_HPP
#define NOGGIT_UI_GROUNDEFFECTSTOOL_HPP

#include <noggit/DBCFile.h>
#include <noggit/TileIndex.hpp>

#include <QtCore/QPointer>
#include <QtWidgets/QWidget>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class World;
class MapView;
class MapChunk;
class MapTile;

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTimer;

namespace Noggit
{
  class Action;

  namespace Ui
  {
    namespace Tools
    {
      class PreviewRenderer;

      namespace UiCommon
      {
        class ExtendedSlider;
      }
    }

    class GroundEffectSetEditor;
    class texturing_tool;

    struct ground_effect_doodad
    {
      unsigned int ID = 0;
      std::string filename = "";
      // Flag (useless in 3.3.5).

      bool empty() const;

      bool operator== (ground_effect_doodad* doodad2);
    };

    struct ground_effect_set
    {
    public:
      void load_from_id(unsigned int effect_id);

      bool empty() const;

      // only ignores id and name (use filename to compare doodads)
      bool operator== (ground_effect_set* effect2);

      // Everything about the set that decides whether two rows are the same effect wearing two
      // ids: terrain type, density, the four filenames and the four weights. Used as a hash key so
      // duplicate merging costs one map lookup per set instead of a deep compare against every set
      // already loaded -- which is what made "load every row in the DBC" impossible before.
      std::string signature() const;

      // Created by the user or auto generated.
      std::string Name = "";

      // Where this set was found on the terrain, as "Zone: Subzone". Empty when the set came
      // straight out of the DBC rather than off a chunk.
      std::string Zone = "";

      unsigned int ID = 0;
      // TODO: can pack doodad and weight in a struct
      ground_effect_doodad Doodads[4];
      unsigned int Weights[4]{ 1, 1, 1, 1 };
      unsigned int Amount = 0;
      unsigned int TerrainType = 0;
    };

    enum class ground_effect_brush_mode
    {
      none,
      exclusion,
      effect
    };

    class GroundEffectsTool : public QWidget
    {
      Q_OBJECT

    public:
      GroundEffectsTool(texturing_tool* texturing_tool, MapView* map_view, QWidget* parent = nullptr);
      void updateTerrainUniformParams();
      // Delete renderer.
      ~GroundEffectsTool();
      float radius() const;
      ground_effect_brush_mode brush_mode() const;
      bool render_mode() const;
      void delete_renderer();

      // Whether the viewport preview and the ground-effect brush mode are live.
      //
      // This is deliberately NOT isVisible(). A Qt::Tool window owned by the main window is
      // hidden by the window manager when that window is minimised, and Qt delivers that as an
      // ordinary QHideEvent -- indistinguishable, at the event, from the user closing the tool.
      // Keying the preview on visibility therefore tore the overlay down on minimise, and worse,
      // texturing_tool::getTexturingMode reads the same visibility to decide whether Shift+LMB
      // paints ground-effect exclusion or paints a TEXTURE. Losing the mode silently turns a
      // preview session into destructive texture painting on the next click.
      //
      // The arm/disarm points are showEvent and a genuine dismissal, so a minimise-restore cycle
      // leaves both the overlay and the brush exactly as they were.
      bool previewActive() const;

      // Turn the preview off for good: disarms, clears the shader uniforms and hides the window.
      // This is what "the user is done with ground effects" means, as opposed to "the window is
      // off screen for a moment".
      void dismissPreview();

      // Recompute the per-chunk overlay data for every loaded tile and refresh the coverage
      // counters. Cheap enough to run on every finished edit; see the cost note in the
      // implementation for the one part that is not.
      void refreshOverlay();

      // Assign (or clear) the selected set's effect id on every chunk under the brush, on the
      // layer that uses the currently selected texture.
      //
      // An effect id lives on the MCLY layer, not on a chunk unit, so "painting" one can only ever
      // mean per chunk -- there is no sub-chunk resolution to paint at. Both open and close their
      // own undo action, so a stroke is undoable even though the caller is a per-tick handler.
      void paintEffect(glm::vec3 const& pos);
      void eraseEffect(glm::vec3 const& pos);

    protected:
      void showEvent(QShowEvent* event) override;

      //Close event triggers, hide event.
      void hideEvent(QHideEvent* event) override;

    public:
      // Selected texture was changed.
      void TextureChanged();

      bool render_active_sets_overlay() const;

      bool render_placement_map_overlay() const;

      bool render_exclusion_map_overlay() const;

      void change_radius(float change);

    private:
      std::optional<ground_effect_set> getSelectedGroundEffect();
      std::optional<glm::vec3> getSelectedEffectColor();
      void setActiveGroundEffect(ground_effect_set const& effect);
      void updateDoodadPreviewRender(int slot_index);
      void scanTileForEffects(TileIndex tile_index);
      void loadAllSetsFromDbc();
      void applyEffectUnderBrush(glm::vec3 const& pos, unsigned int effect_id);
      void updateSetsList();
      void rebuildEffectColors();
      void clearOverlayUniforms();
      void openSetEditor();

      // True when this hide is the window manager taking the window away with the main window,
      // rather than the user dismissing the tool.
      bool hiddenByWindowManager() const;

      // The active texture, or an empty string when there is none worth scanning for. The
      // texturing tool defaults to `tileset\generic\black.blp`, a placeholder that is never on
      // real terrain, so it is treated as "nothing selected" here.
      std::string activeTexture() const;

      // "820 - StlGra01.m2, StlGra01.m2 - Stranglethorn Vale: Zuuldaia Ruins"
      QString setLabel(ground_effect_set const& effect) const;

      // Remembers the set the effect id belongs to, resolved once. Adds the effect id itself and,
      // when duplicate merging is on, every other id whose signature matches.
      void indexEffect(unsigned int effect_id, ground_effect_set const& effect, int list_index);

      std::vector<ground_effect_set> _loaded_effects;
      // Store them for faster iteration on duplicates.
      std::unordered_map<unsigned int, ground_effect_set> _ground_effect_cache;
      std::vector<glm::vec3> _effects_colors;

      // effect id -> index into _loaded_effects/_effects_colors, or -1 for "known, but not one of
      // the loaded sets". Replaces the linear scan over _loaded_effects that the per-chunk colour
      // loop used to do, which made the cost of colouring O(chunks x sets).
      std::unordered_map<unsigned int, int> _color_index_by_id;
      // set signature -> index into _loaded_effects, for duplicate merging in O(1).
      std::unordered_map<std::string, int> _index_by_signature;

      MapView* _map_view;
      texturing_tool* _texturing_tool;
      Tools::PreviewRenderer* _preview_renderer;

      // QPointer<QWidget> rather than QPointer<GroundEffectSetEditor>: the dialog is created
      // with WA_DeleteOnClose, so a raw pointer here dangles the moment the user closes it, and
      // QWidget is the one type this header already has in full.
      QPointer<QWidget> _set_editor;

      QGroupBox* _render_group_box;
      QButtonGroup* _render_type_group;
      // Render all the loaded effect sets for this texture in various colors.
      QRadioButton* _render_active_sets;
      // Only for the active/selected set of the current texture:
      // - Render as red if set is present in the chunk and NOT the current active layer.
      // - Render as green if set is present in the chunk and is the current active layer.
      // - Render as black is set is not present.
      QRadioButton* _render_placement_map;
      // Render chunk units where effect doodads are disabled as white, rest as black.
      QRadioButton* _render_exclusion_map;
      QCheckBox* _chkbox_highlight_unassigned;
      QLabel* _coverage_label;
      QCheckBox* _chkbox_merge_duplicates;
      QLineEdit* _set_filter;
      QListWidget* _effect_sets_list;
      // For render previews.
      QListWidget* _object_list;
      // Weight and percentage customization.
      QListWidget* _weight_list;
      QSpinBox* _weight_spin[4];
      QLabel* _weight_percent[4];
      QSpinBox* _spinbox_doodads_amount;
      QComboBox* _cbbox_terrain_type;
      QGroupBox* _brush_grup_box;
      QButtonGroup* _brush_type_group;
      QRadioButton* _paint_effect;
      QRadioButton* _paint_exclusion;
      Noggit::Ui::Tools::UiCommon::ExtendedSlider* _effect_radius_slider;

      // Coalesces the refreshes that arrive from the action manager. Holding Ctrl+Z fires one
      // signal per undone action; without this each of them would run a full rescan of every
      // loaded tile.
      QTimer* _refresh_timer;

      bool _preview_armed = false;

      // Id of the set the read-only mirror on the right currently shows, or 0 for none.
      //
      // Redrawing it costs four PreviewRenderer::renderToPixmap calls, which are offscreen GL
      // renders. The filter box rebuilds the list on every keystroke and re-selects its first row
      // each time, so without this the mirror would re-render four models per character typed.
      unsigned int _mirrored_set_id = 0;
    };
  }
}

#endif
