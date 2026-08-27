// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "LightBrowser.hpp"
#include "LightEditor.hpp"

#include <noggit/DBC.h>
#include <noggit/MapView.h>
#include <noggit/World.h>
#include <noggit/ui/FontAwesome.hpp>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <sstream>

namespace
{
  // Map.dbc column 2. Blizzard call it InstanceType; DBC.h calls it AreaType. 0 is the open world,
  // and everything else is an instance of one kind or another.
  int constexpr MAP_INSTANCE_TYPE_WORLD = 0;
  int constexpr MAP_INSTANCE_TYPE_PARTY = 1;
  int constexpr MAP_INSTANCE_TYPE_RAID = 2;
  int constexpr MAP_INSTANCE_TYPE_BATTLEGROUND = 3;
  int constexpr MAP_INSTANCE_TYPE_ARENA = 4;

  std::string toLower(std::string text)
  {
    std::transform(text.begin(), text.end(), text.begin()
      , [] (unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return text;
  }
}

namespace Noggit::Ui::Tools
{
  LightBrowser::LightBrowser(LightEditor* editor, QWidget* parent)
    : QWidget(parent)
    , _editor(editor)
  {
    setWindowTitle("Light browser -- all maps");
    setWindowFlags(Qt::Window);
    resize(720, 640);

    auto* const layout = new QVBoxLayout(this);

    auto* const search_layout = new QHBoxLayout();
    search_layout->addWidget(new QLabel("Search:", this));

    _search = new QLineEdit(this);
    _search->setPlaceholderText("light id, light name or map name -- e.g. \"stormwind\" or \"2541\"");
    search_layout->addWidget(_search);

    layout->addLayout(search_layout);

    _tree = new QTreeWidget(this);
    _tree->setColumnCount(4);
    _tree->setHeaderLabels({"Light", "Position", "Inner / outer radius", "Map"});
    _tree->setUniformRowHeights(true);
    _tree->setSelectionMode(QAbstractItemView::SingleSelection);
    _tree->header()->setStretchLastSection(true);
    layout->addWidget(_tree, 1);

    _status = new QLabel(this);
    _status->setWordWrap(true);
    layout->addWidget(_status);

    _deep_copy_chk = new QCheckBox("Paste with independent colours", this);
    _deep_copy_chk->setChecked(true);
    _deep_copy_chk->setToolTip("On: the pasted light gets its own LightParams, LightIntBand and "
                              "LightFloatBand rows, so editing its colours affects nothing else.\n"
                              "Off: it shares the source's parameter rows, so editing its colours "
                              "also edits every other light that uses them -- including the ones "
                              "on the map it came from.");
    layout->addWidget(_deep_copy_chk);

    auto* const button_layout = new QHBoxLayout();

    _copy_button = new QPushButton("Copy to clipboard", this);
    _copy_button->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::copy));
    _copy_button->setEnabled(false);
    button_layout->addWidget(_copy_button);

    _paste_button = new QPushButton("Paste into this map", this);
    _paste_button->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::paste));
    _paste_button->setEnabled(false);
    button_layout->addWidget(_paste_button);

    auto* const close_button = new QPushButton("Close", this);
    button_layout->addWidget(close_button);

    layout->addLayout(button_layout);

    loadEntries();
    rebuildTree();

    // Rebuilt rather than hidden row by row. The tree is three levels deep and hiding a leaf leaves
    // its map and category rows behind, so a search for one light would still show the twenty empty
    // continents above it. A rebuild of a few thousand rows is a few milliseconds and happens only
    // while someone is typing.
    connect(_search, &QLineEdit::textChanged, this, [this] (QString const&) { rebuildTree(); });

    connect(_tree, &QTreeWidget::currentItemChanged, this
      , [this] (QTreeWidgetItem*, QTreeWidgetItem*)
        {
          bool const has_light = selectedLightId() != 0;

          _copy_button->setEnabled(has_light);
          _paste_button->setEnabled(has_light);
        });

    connect(_tree, &QTreeWidget::itemDoubleClicked, this
      , [this] (QTreeWidgetItem*, int)
        {
          int const light_id = selectedLightId();

          if (light_id)
          {
            emit pasteLightRequested(light_id, _deep_copy_chk->isChecked());
          }
        });

    connect(_copy_button, &QPushButton::clicked, this, [this] ()
      {
        int const light_id = selectedLightId();

        if (light_id)
        {
          emit copyLightRequested(light_id);
          _status->setText(QString("Light %1 is on the clipboard. It stays there across map loads, "
                                   "so you can open another map and paste it there.").arg(light_id));
        }
      });

    connect(_paste_button, &QPushButton::clicked, this, [this] ()
      {
        int const light_id = selectedLightId();

        if (light_id)
        {
          emit pasteLightRequested(light_id, _deep_copy_chk->isChecked());
        }
      });

    connect(close_button, &QPushButton::clicked, this, [this] () { close(); });
  }

  void LightBrowser::loadEntries()
  {
    _entries.clear();

    // The whole of Light.dbc, with no map filter. That single missing filter is the entire
    // difference between this window and the panel's list -- gLightDB is one global opened once
    // for every map (DBC.cpp), so every map's lights have always been in memory.
    for (DBCFile::Iterator i = gLightDB.begin(); i != gLightDB.end(); ++i)
    {
      Entry entry;

      entry.light_id = static_cast<int>(i->getUInt(LightDB::ID));
      entry.map_id = i->getInt(LightDB::Map);

      // The DBC stores positions and radii multiplied by 36. Divided here for display only; the
      // copy path goes through Noggit::lightSnapshotFromDbc, which owns that constant.
      float constexpr LIGHT_DBC_SCALE = 36.0f;

      entry.x = i->getFloat(LightDB::PositionX) / LIGHT_DBC_SCALE;
      entry.y = i->getFloat(LightDB::PositionY) / LIGHT_DBC_SCALE;
      entry.z = i->getFloat(LightDB::PositionZ) / LIGHT_DBC_SCALE;
      entry.inner_radius = i->getFloat(LightDB::RadiusInner) / LIGHT_DBC_SCALE;
      entry.outer_radius = i->getFloat(LightDB::RadiusOuter) / LIGHT_DBC_SCALE;

      entry.global = (entry.x == 0.0f && entry.y == 0.0f && entry.z == 0.0f);

      // Names come from the same CSV the panel uses, through the same function, so the two windows
      // cannot disagree. That CSV stops at id 418 -- 378 rows of datamined Blizzard names -- and
      // Light.dbc runs to 2538, so most rows here fall back to "Unnamed Light" and are found by id
      // or by map name instead. There is no zone-name source in 3.3.5 to do better with: naming a
      // light by the area it sits in would need WorldMapArea.dbc, which this tree does not load.
      std::string const name = lightDisplayName(entry.light_id, entry.global);

      std::stringstream label;
      label << entry.light_id << " - " << name;
      entry.label = label.str();

      std::stringstream search;
      search << entry.light_id << " " << name << " " << MapDB::getMapName(entry.map_id) << " "
             << entry.map_id;
      entry.search_key = toLower(search.str());

      _entries.push_back(entry);
    }

    std::sort(_entries.begin(), _entries.end()
      , [] (Entry const& a, Entry const& b) { return a.light_id < b.light_id; });
  }

  void LightBrowser::rebuildTree()
  {
    _tree->clear();

    std::string const filter (toLower(_search->text().toStdString()));

    int const current_map_id = static_cast<int>(_editor->_map_view->getWorld()->getMapID());

    // Category order is fixed and deliberate: the map you are editing first, then the places a
    // custom zone is most likely to want lighting from.
    std::array<MapCategory, 7> constexpr ORDER
      { MapCategory::CurrentMap
      , MapCategory::Continent
      , MapCategory::Dungeon
      , MapCategory::Raid
      , MapCategory::Battleground
      , MapCategory::Arena
      , MapCategory::Other
      };

    auto const category_name = [] (MapCategory category) -> char const*
    {
      switch (category)
      {
        case MapCategory::CurrentMap:    return "Current map";
        case MapCategory::Continent:     return "Continents";
        case MapCategory::Dungeon:       return "Dungeons";
        case MapCategory::Raid:          return "Raids";
        case MapCategory::Battleground:  return "Battlegrounds";
        case MapCategory::Arena:         return "Arenas";
        default:                         return "Other maps";
      }
    };

    auto const category_of = [current_map_id] (int map_id) -> MapCategory
    {
      if (map_id == current_map_id)
      {
        return MapCategory::CurrentMap;
      }

      try
      {
        switch (gMapDB.getByID(static_cast<unsigned int>(map_id)).getInt(MapDB::AreaType))
        {
          case MAP_INSTANCE_TYPE_WORLD:        return MapCategory::Continent;
          case MAP_INSTANCE_TYPE_PARTY:        return MapCategory::Dungeon;
          case MAP_INSTANCE_TYPE_RAID:         return MapCategory::Raid;
          case MAP_INSTANCE_TYPE_BATTLEGROUND: return MapCategory::Battleground;
          case MAP_INSTANCE_TYPE_ARENA:        return MapCategory::Arena;
          default:                             return MapCategory::Other;
        }
      }
      catch (...)
      {
        // A Light.dbc row naming a map that is not in Map.dbc. It exists in stock 3.3.5 data and
        // is not an error to report at the user -- it goes in "Other maps" and stays selectable,
        // because copying its colours is still perfectly reasonable.
        return MapCategory::Other;
      }
    };

    // All seven category rows are created up front, in ORDER, and the empty ones are removed at
    // the end. Created lazily they would appear in whatever order the lights happened to mention
    // them -- which is light-id order -- so "Current map" would sit wherever its lowest light id
    // fell rather than at the top, where the whole point is that it is first.
    std::map<MapCategory, QTreeWidgetItem*> category_nodes;

    for (MapCategory const category : ORDER)
    {
      auto* const node = new QTreeWidgetItem(_tree);
      node->setText(0, category_name(category));
      node->setFirstColumnSpanned(true);
      category_nodes[category] = node;
    }

    std::map<MapCategory, std::map<int, QTreeWidgetItem*>> nodes;

    int shown = 0;

    for (Entry const& entry : _entries)
    {
      if (!filter.empty() && entry.search_key.find(filter) == std::string::npos)
      {
        continue;
      }

      MapCategory const category = category_of(entry.map_id);

      QTreeWidgetItem* const category_node = category_nodes[category];

      QTreeWidgetItem*& map_node = nodes[category][entry.map_id];

      if (!map_node)
      {
        map_node = new QTreeWidgetItem(category_node);
        map_node->setText(0, QString("%1 (%2)")
          .arg(QString::fromStdString(MapDB::getMapName(entry.map_id)))
          .arg(entry.map_id));
        map_node->setFirstColumnSpanned(true);
      }

      auto* const item = new QTreeWidgetItem(map_node);

      item->setText(0, QString::fromStdString(entry.label));
      item->setText(1, entry.global
        ? QString("global -- whole map")
        : QString("%1, %2, %3").arg(entry.x, 0, 'f', 1)
                               .arg(entry.y, 0, 'f', 1)
                               .arg(entry.z, 0, 'f', 1));
      item->setText(2, entry.global
        ? QString("-")
        : QString("%1 / %2").arg(entry.inner_radius, 0, 'f', 0)
                            .arg(entry.outer_radius, 0, 'f', 0));
      item->setText(3, QString::fromStdString(MapDB::getMapName(entry.map_id)));

      if (entry.global)
      {
        item->setIcon(0, Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::sun));
      }

      // The light id, on the same role the panel's list uses, so selectedLightId can read it back
      // without caring which of the three levels the click landed on.
      item->setData(0, Qt::UserRole + 1, QVariant(entry.light_id));

      ++shown;
    }

    // Alphabetical by map name, and only one level down: the leaves under each map are already in
    // ascending light-id order from the sort in loadEntries, and sorting them here would order them
    // as strings, putting light 1000 before light 2.
    for (auto const& category : category_nodes)
    {
      category.second->sortChildren(0, Qt::AscendingOrder);
    }

    for (auto const& category : category_nodes)
    {
      if (category.second->childCount() == 0)
      {
        // Deleting a QTreeWidgetItem detaches it from its view, so an empty category simply is not
        // there rather than being an empty row the user can click.
        delete category.second;
      }
    }

    // Expanded only when the result set is small enough to be worth expanding. With no filter this
    // is 2538 lights across ~70 maps and expanding everything would put the user at the top of a
    // list they then have to scroll through; with a filter it is usually a handful and collapsing
    // them would hide the answer they just searched for.
    if (shown > 0 && shown <= 60)
    {
      _tree->expandAll();
    }
    else
    {
      // Only the current map's own lights, which is the one group someone opening this window
      // with no search term is most likely to want to see without a click.
      for (int i = 0; i < _tree->topLevelItemCount(); ++i)
      {
        if (_tree->topLevelItem(i)->text(0) == QString(category_name(MapCategory::CurrentMap)))
        {
          _tree->topLevelItem(i)->setExpanded(true);
        }
      }
    }

    _status->setText(filter.empty()
      ? QString("%1 lights in Light.dbc, across every map.").arg(shown)
      : QString("%1 of %2 lights match.").arg(shown).arg(_entries.size()));

    _tree->resizeColumnToContents(0);
    _tree->resizeColumnToContents(1);
    _tree->resizeColumnToContents(2);
  }

  int LightBrowser::selectedLightId() const
  {
    QTreeWidgetItem const* const item = _tree->currentItem();

    if (!item)
    {
      return 0;
    }

    // A category or map row carries no id, which is exactly how "nothing selectable is selected"
    // is spelled -- the buttons stay disabled rather than acting on whatever was selected last.
    return item->data(0, Qt::UserRole + 1).toInt();
  }
}
