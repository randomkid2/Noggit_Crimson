// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_LIGHTBROWSER_HPP
#define NOGGIT_LIGHTBROWSER_HPP

#include <QWidget>

#include <string>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace Noggit::Ui::Tools
{
  class LightEditor;

  // Every light in Light.dbc, from every map, grouped and searchable.
  //
  // Separate from LightEditor because it answers a different question. The panel edits the loaded
  // map's lighting; this is a catalogue of all 2538 Blizzard lights plus whatever the project has
  // added, and its only outputs are "copy that one" and "paste that one here". It reads Light.dbc
  // directly rather than going through Skies, because Skies only ever holds the loaded map -- that
  // is exactly the limitation this window exists to get around.
  //
  // Nothing here writes. Copy fills the process-wide clipboard; paste asks LightEditor, which asks
  // Skies, which creates an unsaved light. The Save button on the Edit Light tab remains the only
  // path to a DBC write.
  class LightBrowser : public QWidget
  {
    Q_OBJECT

  public:
    LightBrowser(LightEditor* editor, QWidget* parent = nullptr);

  signals:
    void copyLightRequested(int light_id);
    void pasteLightRequested(int light_id, bool deep_copy_params);

  private:
    // One Light.dbc row, flattened. Held so the tree can be rebuilt on a filter change without
    // walking the DBC again -- there are thousands of rows and the filter runs on every keystroke.
    struct Entry
    {
      int light_id;
      int map_id;
      float x;
      float y;
      float z;
      float inner_radius;
      float outer_radius;
      bool global;

      // Precomputed once, because it is what the filter matches against and what the row shows.
      // Lower-cased separately so the search does not case-fold thousands of strings per keystroke.
      std::string label;
      std::string search_key;
    };

    // Map.dbc InstanceType (column 2), which is what separates a continent from a dungeon.
    enum class MapCategory
    {
      CurrentMap,
      Continent,
      Dungeon,
      Raid,
      Battleground,
      Arena,
      Other
    };

    void loadEntries();
    void rebuildTree();

    [[nodiscard]] int selectedLightId() const;

    LightEditor* _editor;

    std::vector<Entry> _entries;

    QLineEdit* _search;
    QTreeWidget* _tree;
    QLabel* _status;
    QCheckBox* _deep_copy_chk;
    QPushButton* _copy_button;
    QPushButton* _paste_button;
  };
}

#endif //NOGGIT_LIGHTBROWSER_HPP
