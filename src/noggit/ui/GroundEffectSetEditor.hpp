// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_GROUNDEFFECTSETEDITOR_HPP
#define NOGGIT_UI_GROUNDEFFECTSETEDITOR_HPP

#include <QtWidgets/QDialog>

#include <cstdint>
#include <string>
#include <vector>

class DBCFile;
class MapView;

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QTimer;

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
      };

      void reloadFromDbc();
      void showSelected();
      void applyEditsToSelected();

      void onNewSet();
      void onDuplicateSet();
      void onSaveDbc();
      void onBulkApply();

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

      MapView* _map_view;

      std::vector<EffectSet> _sets;

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

      QListWidget* _set_list;
      QLineEdit* _filter;
      QCheckBox* _hide_empty;
      QComboBox* _target_texture;
      QLineEdit* _doodad_path[4];
      QSpinBox* _doodad_weight[4];
      QSpinBox* _amount;
      QComboBox* _terrain_type;
      QComboBox* _bulk_scope;
      QLabel* _status;
  };
}

#endif
