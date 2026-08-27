// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_GROUNDEFFECTSETEDITOR_HPP
#define NOGGIT_UI_GROUNDEFFECTSETEDITOR_HPP

#include <noggit/TileIndex.hpp>

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtWidgets/QDialog>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

class DBCFile;
class MapChunk;
class MapTile;
class MapView;

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;

namespace Noggit::Ui
{
  // Authoring for ground effect sets, and bulk assignment of one to a texture.
  //
  // Noggit could already do most of what a ground effect needs -- it derives the per-cell layer
  // map from the alpha maps in TextureSet::updateDoodadMapping, paints the exclusion stencil with
  // undo, and assigns an effect id per texture layer. The one thing it could not do was create an
  // effect in the first place: GroundEffectsTool only ever calls getByID and CheckIfIdExists, so a
  // new set meant leaving the editor for a standalone DBC tool and coming back.
  //
  // This closes that. It writes GroundEffectTexture.dbc and GroundEffectDoodad.dbc through
  // DBCFile::save(), which emits them under the project's own DBFilesClient/ rather than touching
  // the client -- so the result is a patch, and the stock client stays the DBC baseline that
  // display-id resolution depends on.
  //
  // Deliberately not merged into GroundEffectsTool. That tool is bound to the texturing workflow
  // and to a selected chunk; this is a library editor and a bulk operation, and it needs neither.
  class GroundEffectSetEditor : public QDialog
  {
    Q_OBJECT

    public:
      explicit GroundEffectSetEditor(MapView* map_view, QWidget* parent = nullptr);

      // Bring the set with this id on screen and select it, clearing the filter if that is what it
      // takes to reach it. This is what lets GroundEffectsTool hand over the set the user was
      // looking at on the terrain instead of making them find it again.
      void showSet(std::uint32_t id);

    private:
      // One row of GroundEffectTexture.dbc, flattened for editing.
      struct EffectSet
      {
        std::uint32_t id = 0;
        std::uint32_t doodad_ids[4]{0, 0, 0, 0};
        std::string doodad_files[4];
        std::uint32_t weights[4]{1, 1, 1, 1};
        std::uint32_t amount = 0;
        std::uint32_t terrain_type = 0;

        // The slot holds a doodad id that GroundEffectDoodad.dbc has no row for, so there is no
        // path to show and the id is all that is left of it. Tracked because the save path decides
        // whether to zero a slot from whether its path is empty, and without this an incomplete
        // GroundEffectDoodad.dbc turned every such reference in the file into a permanent 0 the
        // first time anyone pressed Save.
        bool doodad_unresolved[4]{false, false, false, false};
      };

      // What a scope selection resolves to: the tiles to walk, and for the area and zone scopes
      // the per-chunk test that decides which of their chunks are in it.
      //
      // Area and zone are not tile-shaped. An ADT routinely carries three or four areas, and a zone
      // spans tiles in both directions, so neither can be expressed by choosing tiles alone -- the
      // filter has to run per chunk, against MCNK's own area id.
      struct BulkScope
      {
        std::vector<MapTile*> tiles;
        bool filter_by_area = false;
        bool filter_by_zone = false;
        std::uint32_t area_id = 0;
        std::uint32_t zone_id = 0;
        QString description;
        // Non-empty when the scope could not be resolved at all, and why.
        QString problem;
      };

      // What an apply would touch, counted without touching it.
      struct BulkTally
      {
        std::size_t tiles = 0;
        std::size_t chunks = 0;
        std::size_t layers = 0;
      };

      void reloadFromDbc();
      void showSelected();
      void applyEditsToSelected();
      void updateWeightPercentages();

      void onNewSet();
      void onDuplicateSet();
      void onDeleteSet();
      void onSaveDbc();
      void onBulkApply();
      void onBulkCount();

      // Lowest id at or above the custom range that this DBC does not already hold, so a new row
      // cannot collide with a Blizzard one. Consults only the file, so it is the right allocator
      // for GroundEffectDoodad rows, which are created and appended in the same statement.
      std::uint32_t nextFreeId(DBCFile& dbc) const;

      // The same, but also skipping every id held by a set in _sets.
      //
      // Effect sets exist in _sets before they exist in the DBC -- onSaveDbc is what appends them
      // -- so nextFreeId alone hands out the same id twice, and the second set overwrites the
      // first. Anything that mints an effect set id has to use this instead.
      std::uint32_t nextFreeSetId() const;

      // Everything wrong with the current _sets that would make a DBC write meaningless or
      // unloadable, as one message per problem. Empty means the save may proceed.
      //
      // Run in full BEFORE the first addRecord, not as the writer goes: onSaveDbc mutates two
      // process-wide DBCFile objects in place, so aborting halfway leaves them holding rows that
      // were never written and cannot be rolled back.
      QStringList validateSets() const;

      MapView* _map_view;

      std::vector<EffectSet> _sets;

      // How many doodad slots came back from the DBC pointing at a row that is not there. Reported
      // once on load rather than per slot, because on a project with a trimmed
      // GroundEffectDoodad.dbc it can be hundreds.
      std::size_t _unresolved_doodads = 0;

      // area id -> parent zone id. Mutable because the zone scope test is a const query that is
      // asked once per chunk, and AreaDB::get_area_parent behind it is a linear DBC scan.
      mutable std::map<std::uint32_t, std::uint32_t> _area_parent_cache;

      // effect id -> "Zone: Subzone" for the first place on the loaded terrain that uses it.
      //
      // Rebuilt by the same scan that fills the texture list. This is what makes the filter box
      // answer "zuuldaia" as well as "820" and "StlGra01" -- finding the right set among thousands
      // is the whole problem, and a zone name is how a level designer actually thinks about it.
      std::map<std::uint32_t, std::string> _zone_by_set_id;

      // Rebuilds the list against the filter. Kept separate from reloadFromDbc so typing in the
      // filter does not re-read the DBC.
      void rebuildList();

      // Index into _sets for the highlighted row, or -1.
      //
      // Carried in the item's data rather than taken from currentRow(): once the list is filtered
      // the row number and the _sets index are different things, and confusing them edits a
      // different set than the one on screen.
      int selectedSetIndex() const;

      // Highlights the set with this id, clearing the filter if that is what it takes to reach it.
      void selectSetById(std::uint32_t id);

      // Rescans the chosen scope and fills the target-texture list with the textures actually
      // painted there, each with the number of layers using it.
      //
      // This replaced reading the Texturing tool's selection, which was the wrong source: it
      // defaults to `tileset\generic\black.blp`, a placeholder that is never on real terrain, so
      // the first Apply anyone tried reported "nothing changed" and looked broken. Offering only
      // textures that exist in scope makes an empty result impossible to reach by accident.
      void refreshTextureList();

      // Resolves the scope combo into tiles and a chunk filter.
      BulkScope resolveScope() const;
      bool chunkInScope(BulkScope const& scope, MapChunk* chunk) const;

      // Counts what an apply of `effect_id` to `texture` would change, changing nothing.
      BulkTally tally(BulkScope const& scope, std::string const& texture, std::uint32_t effect_id) const;

      // How many layers on the loaded terrain still point at this effect id.
      std::size_t countReferences(std::uint32_t effect_id) const;

      QString summariseSet(EffectSet const& set) const;

      QListWidget* _set_list;
      QLineEdit* _filter;
      QCheckBox* _hide_empty;
      QComboBox* _target_texture;
      QLineEdit* _doodad_path[4];
      QSpinBox* _doodad_weight[4];
      QLabel* _weight_percent[4];
      QSpinBox* _amount;
      QComboBox* _terrain_type;
      QComboBox* _bulk_scope;
      QLabel* _status;
  };
}

#endif
