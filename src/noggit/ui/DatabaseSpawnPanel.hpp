// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_DATABASESPAWNPANEL_HPP
#define NOGGIT_UI_DATABASESPAWNPANEL_HPP

#include <QtWidgets/QWidget>

#include <cstdint>

class MapView;

class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace Noggit::Ui
{
  // The editing surface for TrinityCore world-database spawns.
  //
  // Everything it drives already existed and was proven before this panel was written --
  // SpawnSceneCache::moveTo, dirtyEntries and MapView::saveDatabaseChanges were exercised over
  // the dev bridge and verified against the database, coordinates read back exactly. This is the
  // buttons on top of that, not new machinery.
  //
  // > [!important] "Save to database" writes a file, and that is deliberate
  // > HARD RULE 2 requires every edit to become a reviewable DELETE-then-INSERT changeset before
  // > it touches anything, and HARD RULE 1 permits writes only against the configured dev schema.
  // > So the button emits .sql, and the apply checkbox runs it against the dev schema and nowhere
  // > else -- WorldDatabaseConnection refuses to construct write-capable against any other, so a
  // > live world database cannot be written to by this path even if the settings point at one.
  class DatabaseSpawnPanel : public QWidget
  {
    Q_OBJECT

    public:
      explicit DatabaseSpawnPanel(MapView* map_view, QWidget* parent = nullptr);

      // True while the user is placing spawns, so MapView knows a click means "move the selection
      // here" rather than whatever the active tool would normally do.
      bool moveMode() const;

      // guid of the highlighted spawn, or 0 when nothing is selected.
      std::uint32_t selectedGuid() const;

      // Called by MapView after a click has moved something, and after a load, so the list and the
      // pending count follow the world rather than drifting from it.
      void refresh();

    private:
      void onLoad(bool all_tiles);
      void onSave();
      void onDiscard();

      // Points the camera at the selected spawn from a short distance.
      //
      // The outline alone is not enough when the spawn is off screen or behind terrain, which is
      // most of the time on a populated tile -- you cannot look for a highlight you cannot see.
      void onFocus();

      void onSelectionChanged();

      MapView* _map_view;

      QListWidget* _spawn_list;
      QCheckBox* _move_mode;
      QSpinBox* _orientation;
      QCheckBox* _apply_to_dev;
      QLabel* _pending;
      QPushButton* _save_button;
      QPushButton* _discard_button;
  };
}

#endif
